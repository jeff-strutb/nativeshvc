/*
 * pace_model.c: rr_pace (src/render/rr_pace.h) against a model of a
 * display that shows pictures in order, one a refresh: a picture ready at
 * r is on the glass at the first refresh at least the lead after r, or the
 * refresh after the last picture's, whichever is later; the backend hears
 * of it a millisecond after. The console's picture is drawn a frame's run
 * (0.5 to 5.5 ms) and a GPU time (0.5 ms, or 5 ms in alternate 1000-frame
 * stretches in some cases) after its frame starts (each 1/60.0988 s, or
 * later should the picture before have been sent late); then it is sent,
 * ready after the copy to the display's image (50 to 150 us).
 *
 * For each display (60.00, 59.94, 120 and 50 Hz), console (NTSC, PAL) and
 * lead (3.5 ms, 20 ms), the picture sent once drawn or steadily (no sooner
 * than the longest run and drawing of the last 128 frames after its
 * frame's start, as the front end sends it), over 20,000 frames, it holds that:
 *
 *   - the lead is measured within 60 frames, to within 0.25 ms
 *   - after that, no picture waits behind another (on the glass later than
 *     its own first refresh)
 *   - no picture held back that would not have waited
 *   - with the compositor keeping pictures a refresh longer for a while
 *     (frames 5000-8000, 12000-12500), the lead moved, not measured again;
 *     no more held back needlessly than one each 60 frames while deeper
 *     (to tell it from a queue) and three each change, nor waiting than
 *     60 each time it goes back (until the next is held back) and three
 *     each change
 *   - sent steadily, the pictures held back no more than the console's
 *     extra frames over the refreshes, one each GPU step, and sixteen
 *     while the lead is measured
 *   - no picture waits at the end: the queue empty
 *
 * Exit 0 when every case holds.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rr_pace.h"

#define FRAMES 20000

static unsigned long rng = 12345;
static double urand(void)
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(rng >> 11) / 9007199254740992.0;
}

typedef struct {
    unsigned long id;
    double sent, ready, glass;
} Shown;

/* the compositor keeping pictures a refresh longer: from frame 5000 to
   8000, and 12000 to 12500 */
static int deeper(int i) { return (i >= 5000 && i < 8000) || (i >= 12000 && i < 12500); }

static int run(const char *name, double refresh, double frame, double lead, int steps, int steady, int depth)
{
    RrPace p = { 0 };
    static Shown q[FRAMES];
    double phase = urand() * refresh, last_glass = 0, free_at = 0;
    int nq = 0, fed = 0, waited = 0, held = 0, needless = 0, i, ok, nsteps = 0, pinned_at = -1;
    int last_waited = 0;
    double longest = 0, runs[128];
    int unpinned = 0;
    double crossings;
    for (i = 0; i < FRAMES; i++) {
        double now = i * frame, at;
        double gpu = 0.00005 + urand() * 0.0001;   /* the copy */
        unsigned long id;
        if (now < free_at)
            now = free_at;
        {
            /* the frame's run and drawing; sent no sooner than the longest
               of the last 128 after its start, as the front end sends it */
            double start = now, run = 0.0005 + urand() * 0.005 + (steps && i % 2000 >= 1000 ? 0.005 : 0.0005);
            now += run;
            if (steady) {
                int k;
                runs[i % 128] = run;
                longest = 0;
                for (k = 0; k < 128 && k <= i; k++)
                    longest = runs[k] > longest ? runs[k] : longest;
                if (now < start + longest)
                    now = start + longest;
            }
        }
        if (steps && i % 1000 == 0 && i)
            nsteps++;
        /* what has reached the glass by now, heard of */
        while (fed < nq && q[fed].glass + 0.001 <= now) {
            rr_pace_shown(&p, q[fed].id, q[fed].sent, q[fed].ready, q[fed].glass);
            fed++;
        }
        if (pinned_at < 0 && rr_pace_pinned(&p))
            pinned_at = i;
        else if (pinned_at >= 0 && !rr_pace_pinned(&p))
            unpinned++;
        id = rr_pace_send(&p, now, &at);
        if (!id) {
            /* held back: would it have waited? */
            double ready = now + gpu, ld = lead + (depth && deeper(i) ? refresh : 0);
            double own = phase + ceil((ready + ld - phase) / refresh) * refresh;
            held++;
            if (pinned_at >= 0 && !(last_glass > 0 && own < last_glass + refresh * 0.5))
                needless++;
            continue;
        }
        free_at = at;
        {
            double ld = lead + (depth && deeper(i) ? refresh : 0);
            double ready = at + gpu, own = phase + ceil((ready + ld - phase) / refresh) * refresh;
            double g = own;
            if (last_glass > 0 && g < last_glass + refresh * 0.5)
                g = last_glass + refresh;
            last_waited = g > own + refresh * 0.5;
            if (pinned_at >= 0)
                waited += last_waited;
            q[nq].id = id;
            q[nq].sent = at;
            q[nq].ready = ready;
            q[nq].glass = g;
            nq++;
            last_glass = g;
        }
    }
    crossings = refresh > frame ? FRAMES * (1 - frame / refresh) : 0;
    {
        /* the compositor deeper: a picture held back each 60 to tell it
           from a queue (3500 frames deeper), and up to 60 waiting each
           time it goes back (twice), and three at each change */
        int lim = depth ? 3500 / 60 + 2 * 60 + 3 * 4 : 0;
        /* sent steadily, no more held back than the console's extra
           frames over the refreshes, and a few around each GPU step */
        int held_ok = (!steady || held <= (int)ceil(crossings) + nsteps + 16 + lim) && !unpinned;
        int lead_ok = pinned_at >= 0 && pinned_at <= 60 && p.lo < lead + 1e-9 && lead <= p.hi + 1e-9 &&
                      p.hi - p.lo <= RR_PACE_PIN + 1e-9;
        ok = lead_ok && held_ok && waited <= lim && needless <= lim && !last_waited;
        printf("%s %-17s lead %4.1f ms%s%s%s: measured by frame %d (%.2f-%.2f ms); %d waited, %d held back, %d of "
               "them needlessly (allowed %d each; %.1f came due together)\n",
               ok ? "ok  " : "FAIL", name, lead * 1e3, steps ? ", GPU steps" : "", steady ? ", steady" : "",
               depth ? ", compositor deeper" : "", pinned_at, p.lo * 1e3, p.hi * 1e3,
               waited, held, needless, lim, crossings);
    }
    return !ok;
}

int main(void)
{
    int fails = 0;
    static const struct {
        const char *name;
        double hz, frame;
    } c[] = {
        { "NTSC on 60.00 Hz", 60.0, 0.016639267 },
        { "NTSC on 59.94 Hz", 60000.0 / 1001.0, 0.016639267 },
        { "NTSC on 120 Hz", 120.0, 0.016639267 },
        { "NTSC on 50 Hz", 50.0, 0.016639267 },
        { "PAL on 60.00 Hz", 60.0, 0.019997194 },
        { "PAL on 50.00 Hz", 50.0, 0.019997194 },
    };
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        int steady;
        for (steady = 0; steady < 2; steady++) {
            fails += run(c[i].name, 1.0 / c[i].hz, c[i].frame, 0.0035, 0, steady, 0);
            fails += run(c[i].name, 1.0 / c[i].hz, c[i].frame, 0.0035, 1, steady, 0);
            fails += run(c[i].name, 1.0 / c[i].hz, c[i].frame, 0.020, 1, steady, 0);
        }
        fails += run(c[i].name, 1.0 / c[i].hz, c[i].frame, 0.016, 0, 1, 1);
        fails += run(c[i].name, 1.0 / c[i].hz, c[i].frame, 0.016, 1, 1, 1);
    }
    printf("pace_model: %d case(s) failed\n", fails);
    return fails ? 1 : 0;
}
