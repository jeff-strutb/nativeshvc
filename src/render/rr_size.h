/*
 * rr_size.h: what every backend shares: the headless target size
 * (RR_SIZE=WxH, else 256 x 224) and sharp bilinear, the one definition the
 * backends' shaders compute and the checks' reference (tests/ref/rr_ref.h)
 * draws by.
 */
#ifndef PORT_RR_SIZE_H
#define PORT_RR_SIZE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline void rr_headless_size(int *w, int *h)
{
    const char *e = getenv("RR_SIZE");
    int a, b;
    *w = 256;
    *h = 224;
    if (e && sscanf(e, "%dx%d", &a, &b) == 2 && a > 0 && b > 0 && a <= 16384 && b <= 16384) {
        *w = a;
        *h = b;
    }
}

/* Sharp bilinear along one axis: for the sample point p (in texels, the
   texel centre at i + 0.5) and d texels to one target pixel, the texel i
   and the weight of texel i + 1 (0 inside a texel, a ramp one target pixel
   wide across its edge). */
static inline float rr_sharp(float p, float d, int *i)
{
    float px = p - 0.5f, fl = floorf(px), f = px - fl;
    if (d < 1e-5f)
        d = 1e-5f;
    f = (f - 0.5f) / d + 0.5f;
    *i = (int)fl;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* A blend's weight t in whole 256ths (W, t's nearest): of a and b,
   each channel a + floor(((b - a) W + 128) / 256), integers throughout, so
   a GPU with no shaders (the PSP's GE: a table of b - a) makes it exactly
   for any pair of values, and a shader in floats exactly too. */
static inline int rr_mix_w(float t) { return (int)floorf(t * 256.0f + 0.5f); }

/* The standard sample positions (Direct3D's and Vulkan's
   standardSampleLocations) of 4, 8 and 16 samples a pixel, in 1/16 of a
   pixel from its centre: what the Mode 7 plane is supersampled at where it
   is minified (rr_mode7.h). */
static inline const int8_t (*rr_samples(int n))[2]
{
    static const int8_t p4[4][2] = { { -2, -6 }, { 6, -2 }, { -6, 2 }, { 2, 6 } };
    static const int8_t p8[8][2] = { { 1, -3 }, { -1, 3 }, { 5, 1 }, { -3, -5 }, { -5, 5 }, { -7, -1 }, { 3, 7 }, { 7, -7 } };
    static const int8_t p16[16][2] = { { 1, 1 }, { -1, -3 }, { -3, 2 }, { 4, -1 }, { -5, -2 }, { 2, 5 }, { 5, 3 }, { 3, -5 },
                                       { -2, 6 }, { 0, -7 }, { -4, -6 }, { -6, 4 }, { -8, 0 }, { 7, -4 }, { 6, 7 }, { -7, -8 } };
    return n >= 16 ? p16 : n >= 8 ? p8 : p4;
}

#endif
