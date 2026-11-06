/*
 * resample.c: a windowed-sinc resampler (resample.h). Each output frame is
 * the input around its time, weighted by a sinc low-passed at the input's
 * Nyquist (raising the rate: the images above it go) or 95% of the output's
 * (lowering it), shaped by a Kaiser window (beta 8.6),
 * 32 taps; the taps come from a table of 512 phases, interpolated
 * linearly between the two nearest. The output's time is kept in input
 * frames as a whole and a 32-bit fraction (an integer: no double's
 * arithmetic per frame).
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "resample.h"

#define TAPS 32
#define PHASES 512
#define PHASE_BITS 9              /* (PHASES = 1 << PHASE_BITS) */
#define QUEUE 16384               /* input frames held (a power of 2) */

struct Resampler {
    double base_step;              /* input frames per output frame */
    uint64_t step;                 /* the same, as adjusted, in 2^-32 frames */
    uint64_t pos;                  /* the next output's time, in 2^-32 input frames past q[at] */
    float table[(PHASES + 1) * TAPS];
    float q[QUEUE * 2];             /* input, left and right */
    int at, n;                     /* the oldest frame kept; frames kept */
};

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    int k;
    for (k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

Resampler *resample_new(double in_rate, double out_rate)
{
    Resampler *r = (Resampler *)calloc(1, sizeof *r);
    double cut = in_rate < out_rate ? 0.5 : 0.475 * out_rate / in_rate;   /* cycles per input frame */
    const double beta = 8.6, i0b = bessel_i0(beta);
    int p, k;
    if (!r)
        return NULL;
    r->base_step = in_rate / out_rate;
    r->step = (uint64_t)llround(r->base_step * 4294967296.0);
    for (p = 0; p <= PHASES; p++) {
        double frac = (double)p / PHASES, sum = 0;
        float *t = &r->table[p * TAPS];
        for (k = 0; k < TAPS; k++) {
            /* tap k weighs input frame (k - TAPS/2 + 1) relative to the
               frame before the output's time, the output frac past it */
            double x = (k - TAPS / 2 + 1) - frac, w, s, u = x / (TAPS / 2);
            s = x == 0 ? 2 * cut : sin(2 * M_PI * cut * x) / (M_PI * x);
            w = fabs(u) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - u * u)) / i0b;
            t[k] = (float)(s * w);
            sum += s * w;
        }
        for (k = 0; k < TAPS; k++)            /* unity gain at DC */
            t[k] = (float)(t[k] / sum);
    }
    return r;
}

void resample_free(Resampler *r) { free(r); }

void resample_adjust(Resampler *r, double adjust)
{
    r->step = (uint64_t)llround(r->base_step * (1 + adjust) * 4294967296.0);
}

int resample_queued(const Resampler *r) { return r->n; }

int resample_push(Resampler *r, const int16_t *lr, int n)
{
    int i;
    if (n > QUEUE - r->n)
        n = QUEUE - r->n;
    for (i = 0; i < n; i++) {
        int j = (r->at + r->n + i) & (QUEUE - 1);
        r->q[2 * j] = lr[2 * i] * (1.0f / 32768.0f);    /* (a power of 2: the quotient's value) */
        r->q[2 * j + 1] = lr[2 * i + 1] * (1.0f / 32768.0f);
    }
    r->n += n;
    return n;
}

int resample_pull(Resampler *r, float *lr, int n)
{
    int o;
    for (o = 0; o < n; o++) {
        int whole = (int)(r->pos >> 32), p, k;
        uint32_t frac = (uint32_t)r->pos;
        float a, l = 0, rr = 0;
        const float *t0, *t1;
        /* the taps reach TAPS/2 - 1 frames back and TAPS/2 ahead of the
           frame before the output's time (kept at TAPS/2 - 1 in) */
        if (whole + TAPS / 2 + TAPS / 2 > r->n)
            break;
        /* the phase, and the fraction past it (its 23 bits, exact in a float) */
        p = (int)(frac >> (32 - PHASE_BITS));
        a = (float)((frac << PHASE_BITS) >> (32 - 23)) * (1.0f / 8388608.0f);
        t0 = &r->table[p * TAPS];
        t1 = &r->table[(p + 1) * TAPS];
        for (k = 0; k < TAPS; k++) {
            int j = (r->at + whole + k) & (QUEUE - 1);
            float w = t0[k] + (t1[k] - t0[k]) * a;
            l += r->q[2 * j] * w;
            rr += r->q[2 * j + 1] * w;
        }
        lr[2 * o] = l;
        lr[2 * o + 1] = rr;
        r->pos += r->step;
        whole = (int)(r->pos >> 32);
        if (whole > 0) {                     /* frames no output needs again */
            r->at = (r->at + whole) & (QUEUE - 1);
            r->n -= whole;
            r->pos &= 0xFFFFFFFFu;
        }
    }
    return o;
}
