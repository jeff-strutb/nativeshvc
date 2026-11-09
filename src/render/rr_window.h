/* rr_window.h: a description's window words (picture_body.h PB_WW and on),
 * filled by the render thread once it knows the window's size. */
#ifndef RR_WINDOW_H
#define RR_WINDOW_H

#include <stdint.h>
#include "picture_defs.h"

static inline int32_t rr_floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (int32_t)((a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q);
}
static inline int32_t rr_round(int64_t n, int64_t d) { return rr_floordiv(2 * n + d, 2 * d); }

/* the standard sample positions of 4, 8 and 16 samples, in 1/16 of a pixel */
static const int8_t rr_s4[4][2] = { { -2, -6 }, { 6, -2 }, { -6, 2 }, { 2, 6 } };
static const int8_t rr_s8[8][2] = { { 1, -3 }, { -1, 3 }, { 5, 1 }, { -3, -5 }, { -5, 5 }, { -7, -1 }, { 3, 7 }, { 7, -7 } };
static const int8_t rr_s16[16][2] = { { 1, 1 }, { -1, -3 }, { -3, 2 }, { 4, -1 }, { -5, -2 }, { 2, 5 }, { 5, 3 }, { 3, -5 },
                                      { -2, 6 }, { 0, -7 }, { -4, -6 }, { -6, 4 }, { -8, 0 }, { 7, -4 }, { 6, 7 }, { -7, -8 } };

static inline void rr_window_words(uint32_t *d, int ow, int oh)
{
    int64_t pw = d[PB_PW], ph = d[PB_H], cpp = d[PB_PW] == d[PB_W] ? 1 : 2;
    int64_t dcol = 16 * (int64_t)ow * cpp, drow = 16 * (int64_t)oh;
    int k;
    d[PB_WW] = (uint32_t)ow;
    d[PB_WH] = (uint32_t)oh;
    d[PB_STEPU] = (uint32_t)rr_round(16 * pw * 1024, dcol);
    d[PB_STEPV] = (uint32_t)rr_round(16 * ph * 1024, drow);
    for (k = 0; k < 28; k++) {
        const int8_t *s = k < 4 ? rr_s4[k] : k < 12 ? rr_s8[k - 4] : rr_s16[k - 12];
        d[PB_DU + k] = (uint32_t)rr_round((int64_t)s[0] * pw * 1024, dcol);
        d[PB_DV + k] = (uint32_t)rr_round((int64_t)s[1] * ph * 1024, drow);
    }
}

#endif
