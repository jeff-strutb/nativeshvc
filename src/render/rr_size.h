/*
 * rr_size.h: what every backend shares: the headless target size
 * (RR_SIZE=WxH, else 256 x 224) and sharp bilinear, the one definition the
 * backends' shaders and the software backend all compute.
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

static inline uint32_t rr_texel(const uint32_t *p, int w, int h, int x, int y)
{
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return p[(size_t)y * (size_t)w + (size_t)x];
}

static inline uint32_t rr_mix(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0xFF000000u;
    int s;
    for (s = 0; s < 24; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        o |= (uint32_t)(int)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return o;
}

/* The reference: w x h pixels drawn over rectangle d of the tw x th
   target, as rr_image draws them (the software backend's drawing, and what
   rr_check holds the GPU backends to). */
static inline void rr_ref_image(uint32_t *dst, int tw, int th, const uint32_t *src, int w, int h, const int d[4])
{
    int x, y, x0 = d[0], y0 = d[1], x1 = d[2], y1 = d[3];
    float dx, dy;
    if (w <= 0 || h <= 0 || x1 <= x0 || y1 <= y0)
        return;
    dx = (float)w / (float)(x1 - x0);
    dy = (float)h / (float)(y1 - y0);
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < th; y++) {
        int ty;
        float fy = rr_sharp(((float)(y - y0) + 0.5f) * dy, dy, &ty);
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < tw; x++) {
            int tx;
            float fx = rr_sharp(((float)(x - x0) + 0.5f) * dx, dx, &tx);
            uint32_t top = rr_mix(rr_texel(src, w, h, tx, ty), rr_texel(src, w, h, tx + 1, ty), fx);
            uint32_t bot = rr_mix(rr_texel(src, w, h, tx, ty + 1), rr_texel(src, w, h, tx + 1, ty + 1), fx);
            dst[(size_t)y * (size_t)tw + (size_t)x] = rr_mix(top, bot, fy);
        }
    }
}

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

/* The reference overlay (rr.h rr_overlay), the software backend's drawing
   and what the check holds the GPU backends to: each target pixel whose
   centre falls in the parallelogram samples the image bilinear (texel
   centres at i + 0.5, edges clamped) and is blended over. */
static inline void rr_ref_overlay(uint32_t *dst, int tw, int th, const uint32_t *src, int w, int h, const float q[6])
{
    float ax = q[2] - q[0], ay = q[3] - q[1], bx = q[4] - q[0], by = q[5] - q[1];
    float det = ax * by - ay * bx, minx, maxx, miny, maxy;
    int x, y, x0, x1, y0, y1;
    if (w <= 0 || h <= 0 || det == 0)
        return;
    minx = maxx = q[0];
    miny = maxy = q[1];
    {
        float cx[3] = { q[2], q[4], q[2] + bx }, cy[3] = { q[3], q[5], q[3] + by };
        int k;
        for (k = 0; k < 3; k++) {
            if (cx[k] < minx) minx = cx[k];
            if (cx[k] > maxx) maxx = cx[k];
            if (cy[k] < miny) miny = cy[k];
            if (cy[k] > maxy) maxy = cy[k];
        }
    }
    x0 = (int)floorf(minx);
    x1 = (int)ceilf(maxx);
    y0 = (int)floorf(miny);
    y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - q[0], py = (float)y + 0.5f - q[1];
            float u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
            float sx, sy, fx, fy, a, out[4], c[4][4];
            int ix, iy, k, ch;
            uint32_t d0;
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            sx = u * (float)w - 0.5f;
            sy = v * (float)h - 0.5f;
            ix = (int)floorf(sx);
            iy = (int)floorf(sy);
            fx = sx - (float)ix;
            fy = sy - (float)iy;
            for (k = 0; k < 4; k++) {
                uint32_t t = rr_texel(src, w, h, ix + (k & 1), iy + (k >> 1));
                for (ch = 0; ch < 4; ch++)
                    c[k][ch] = (float)((t >> (ch * 8)) & 0xFF);
            }
            for (ch = 0; ch < 4; ch++)
                out[ch] = (c[0][ch] * (1 - fx) + c[1][ch] * fx) * (1 - fy) + (c[2][ch] * (1 - fx) + c[3][ch] * fx) * fy;
            a = out[3] / 255.0f;
            d0 = dst[(size_t)y * (size_t)tw + (size_t)x];
            {
                uint32_t o = 0xFF000000u;
                for (ch = 0; ch < 3; ch++) {
                    float dv = (float)((d0 >> (ch * 8)) & 0xFF);
                    o |= (uint32_t)(int)(out[ch] * a + dv * (1 - a) + 0.5f) << (ch * 8);
                }
                dst[(size_t)y * (size_t)tw + (size_t)x] = o;
            }
        }
}

#endif
