/*
 * rr_pace.h: which pictures go to a display that shows them in order, one
 * a refresh (a swap chain synced to the display), so that none waits
 * behind another. The console keeps its own time (a frame each
 * 1/60.0988 s) whatever the display's refresh: on a display a little
 * slower (60.00 Hz) two pictures now and then come due on the same
 * refresh, and sending both puts every later picture a refresh behind for
 * good (the swap chain fills, and its wait paces the console). The second
 * is held back instead; the display shows the newest it can.
 *
 * A picture ready (drawn) at t is on the glass at the first refresh at
 * least the lead L after t, or a refresh after the picture before it,
 * whichever is later. The refreshes are learned from the pictures on the
 * glass (rr_pace_shown): their spacing, the least gap seen between two,
 * and where they fall. The lead shows only in a picture that waited
 * behind none, so it is measured: at the start (and should the display or
 * the lead change) pictures go one at a time, each once the one before is
 * on the glass, each timed to be ready the middle of the lead's range
 * before a refresh; on the glass at that refresh the lead is at most that,
 * a refresh later at least, and seven or eight of them pin it within a
 * quarter of a millisecond. Then a picture whose first refresh is the last
 * one sent's is held back; and should one still have waited (on the glass
 * a refresh after its first for certain), the next is held back to empty
 * the queue. A compositor (the Mac's window server) now and then keeps
 * pictures a refresh longer for a while, and back: a picture still waiting
 * just after one was held back, or one on the glass sooner than the lead
 * allows, moves the lead by whole refreshes (where in the refresh it
 * falls stays).
 * The backend gives the time a picture will be ready to the microsecond
 * (its drawing done, only the copy to the display's image left), and one
 * that would be ready within the lead's doubt of a deadline is sent just
 * after it, so that each picture's refresh is certain.
 *
 * Times in seconds, on the clock of the backend's glass times. One
 * definition for the backends that present through a synced swap chain
 * (Metal, Vulkan in FIFO).
 */
#ifndef RR_PACE_H
#define RR_PACE_H

#include <math.h>

#define RR_PACE_PIN 0.00025     /* the lead pinned within this */
#define RR_PACE_MARGIN 0.00015  /* and a picture's refresh certain this past a deadline */

typedef struct RrPace {
    double refresh;     /* the display's refresh, 0 not yet seen */
    double anchor;      /* a refresh: the last picture's time on the glass */
    double lo, hi;      /* the lead: more than lo, at most hi (hi 0: none) */
    double work, doubt; /* sent to ready: the median of the last 16, and
                           how far the rest but the two outliers stray */
    double works[16];
    double last_first;  /* the first refresh of the last picture sent */
    unsigned long sent; /* pictures sent */
    unsigned long seen; /* the last one rr_pace_shown had */
    unsigned long alone;      /* the last picture sent with none before it unseen */
    unsigned long hold_seen;  /* waits seen up to this one: emptied already */
    unsigned long after_hold; /* the first picture sent after a hold, 0 none */
    unsigned long shifted;    /* the picture the lead was last shifted at */
    double least;             /* the lead's lo at its least depth, 0 none */
    unsigned long tried;      /* the last picture held back to try it */
    int drain;          /* hold the next picture back */
} RrPace;

/* the first refresh a picture ready at t is due on */
static inline double rr_pace_first(const RrPace *p, double t, double lead)
{
    return p->anchor + ceil((t + lead - p->anchor) / p->refresh - 1e-9) * p->refresh;
}

/* the lead pinned */
static inline int rr_pace_pinned(const RrPace *p)
{
    return p->hi > 0 && p->hi - p->lo <= RR_PACE_PIN;
}

/* a picture drawn, ready to go at now: 0 to hold it back, else its number
   (from 1, for rr_pace_shown) with *at the time to send it (now, or a
   little later: while the lead is measured, or off a deadline) */
static inline unsigned long rr_pace_send(RrPace *p, double now, double *at)
{
    *at = now;
    if (p->refresh > 0 && p->hi > 0 && !rr_pace_pinned(p)) {
        /* measuring: one at a time, ready the middle of the lead's range
           before a refresh */
        double mid = (p->lo + p->hi) / 2, t;
        p->drain = 0;
        if (p->seen != p->sent)
            return 0;
        t = rr_pace_first(p, now + p->work, mid) - mid - p->work;
        *at = t;
        p->alone = p->sent + 1;
        p->last_first = 0;
        return ++p->sent;
    }
    if (rr_pace_pinned(p)) {
        /* ready at t: due on the first refresh whose deadline (the lead
           before it) is at or after t; within the lead's doubt and the
           margin of a deadline the refresh is not certain, so it is sent
           just after that deadline instead */
        double mid = (p->lo + p->hi) / 2, half = (p->hi - p->lo) / 2 + p->doubt + RR_PACE_MARGIN;
        double t = now + p->work, first = rr_pace_first(p, t, mid), dl = first - mid;
        if (dl - t < half) {
            t = dl + half;
            first += p->refresh;
        } else if (t - (dl - p->refresh) < half)
            t = dl - p->refresh + half;
        if (p->last_first > 0 && first < p->last_first + p->refresh * 0.5) {
            p->after_hold = p->sent + 1;
            return 0;                   /* due with the last: it would wait */
        }
        /* held back to empty the queue, a picture that would have gone
           (one due with the last empties nothing); and the lead a refresh
           or more over its least, the compositor keeping pictures longer
           or pictures waiting behind one (the two look alike, should it
           have gone back to its least depth and the queue kept them), one
           held back each 60 to tell: the next on the glass sooner should
           they have waited */
        if (p->drain || (p->lo > p->least + p->refresh * 0.5 && p->sent - p->tried >= 60)) {
            if (!p->drain)
                p->tried = p->sent;
            p->drain = 0;
            p->after_hold = p->sent + 1;
            return 0;
        }
        *at = t - p->work;
        p->last_first = first;
    }
    if (p->seen == p->sent)
        p->alone = p->sent + 1;
    return ++p->sent;
}

/* the greatest common measure of two gaps, to a fifth of a millisecond */
static inline double rr_pace_measure(double a, double b)
{
    while (b > 0.0002) {
        double r = fmod(a, b);
        if (b - r < 0.0002)
            r = 0;
        a = b;
        b = r;
    }
    return a;
}

/* forget the lead: measure it again, from at most hi (a picture's time
   from ready to the glass, which may have waited) */
static inline void rr_pace_remeasure(RrPace *p, double hi)
{
    p->hi = hi;
    p->lo = 0;
    p->least = 0;
    p->last_first = 0;
}

/* the lead k whole refreshes more (or fewer) */
static inline void rr_pace_shift(RrPace *p, unsigned long n, double k)
{
    p->shifted = n;
    p->lo += k * p->refresh;
    p->hi += k * p->refresh;
    if (p->last_first > 0)
        p->last_first += k * p->refresh;
    if (p->lo < 0)
        rr_pace_remeasure(p, p->hi);
}

/* picture n (sent at sent, ready at ready) on the glass at glass, or
   never (glass 0) */
static inline void rr_pace_shown(RrPace *p, unsigned long n, double sent, double ready, double glass)
{
    double w = ready - sent, l = glass - ready;
    if (n <= p->seen)
        return;
    p->seen = n;
    if (glass <= 0)
        return;
    if (p->anchor > 0) {
        double d = glass - p->anchor;
        if (d > 0.001 && (p->refresh == 0 || d < p->refresh * 0.75)) {
            p->refresh = d;             /* a display (or a refresh) new to us */
            if (p->hi > 0)
                rr_pace_remeasure(p, p->hi);
        } else if (p->refresh > 0 && d > 0.001) {
            /* a whole number of refreshes: the spacing to a microsecond */
            double k = floor(d / p->refresh + 0.5);
            if (k >= 1 && fabs(d - k * p->refresh) < 0.0002)
                p->refresh += (d / k - p->refresh) * 0.05;
            else if (fabs(d - k * p->refresh) > 0.001) {
                /* not a whole number: the refresh is the two gaps' common
                   measure (pictures two refreshes apart, then three) */
                double g = rr_pace_measure(p->refresh, d);
                p->refresh = g > 0.001 ? g : 0;
                if (g > 0.001 && p->hi > 0)
                    rr_pace_remeasure(p, p->hi);
                else if (g <= 0.001)
                    p->hi = 0;
            }
        }
    }
    p->anchor = glass;
    if (w >= 0) {
        double v[16];
        int k, j, m = n < 16 ? (int)n : 16;
        p->works[n % 16] = w;
        for (k = 0; k < m; k++) {       /* the last m, in order */
            double x = p->works[(n - (unsigned long)k) % 16];
            for (j = k; j > 0 && v[j - 1] > x; j--)
                v[j] = v[j - 1];
            v[j] = x;
        }
        p->work = v[m / 2];
        p->doubt = m >= 4 ? fmax(v[m - 2] - p->work, p->work - v[1]) : 0;
    }
    if (l <= 0 || p->refresh == 0)
        return;
    if (p->hi == 0) {                   /* the first: the lead within a refresh */
        rr_pace_remeasure(p, l);
        return;
    }
    /* on the glass later than the lead allows with none before it, or
       sooner than it allows: the display keeps whole refreshes more or
       fewer (its compositor's own depth changes, the place in the refresh
       its deadline falls does not); while measuring, from this picture */
    if (n == p->alone && l - p->refresh >= p->hi) {
        if (!rr_pace_pinned(p)) {
            rr_pace_remeasure(p, l);
            return;
        }
        rr_pace_shift(p, n, floor((l - p->hi) / p->refresh));
    } else if (l <= p->lo) {
        if (!rr_pace_pinned(p)) {
            rr_pace_remeasure(p, l);
            return;
        }
        rr_pace_shift(p, n, -ceil((p->lo - l) / p->refresh + 1e-9));
    }
    if (l < p->hi)
        p->hi = l;                      /* every picture: the lead at most this */
    if (n == p->alone && l - p->refresh > p->lo)
        p->lo = l - p->refresh;         /* none before it: the lead more than this */
    if (p->hi <= p->lo)                 /* the lead changed otherwise */
        rr_pace_remeasure(p, l);
    if (!rr_pace_pinned(p))
        return;
    if (p->least == 0 || p->lo < p->least - p->refresh * 0.5)
        p->least = p->lo;
    /* on the glass a refresh after its first for certain: it waited behind
       another, and so does every picture sent since; one sent just after a
       hold waiting means the display keeps whole refreshes more */
    if (l - p->refresh >= p->hi + 0.00002 && n > p->hold_seen) {
        if (n == p->after_hold)
            rr_pace_shift(p, n, floor((l - p->hi) / p->refresh));
        else {
            p->drain = 1;
            p->hold_seen = p->sent;
        }
    }
}

#endif
