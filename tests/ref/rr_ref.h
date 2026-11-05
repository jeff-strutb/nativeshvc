/*
 * rr_ref.h: sharp bilinear and overlays drawn on the CPU (rr_size.h's
 * definition), the reference every renderer is held to, for the checks only.
 */
#ifndef NSHVC_RR_REF_H
#define NSHVC_RR_REF_H

#include "rr_size.h"

static inline uint32_t rr_texel(const uint32_t *p, int w, int h, int x, int y)
{
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return p[(size_t)y * (size_t)w + (size_t)x];
}

/* The blend of a and b, b's weight t (rr_size.h rr_mix_w): each channel
   a + floor(((b - a) W + 128) / 256). */
static inline uint32_t rr_mix(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0xFF000000u;
    int s, W = rr_mix_w(t);
    for (s = 0; s < 24; s += 8) {
        int ca = (int)((a >> s) & 0xFF), cb = (int)((b >> s) & 0xFF);
        o |= (uint32_t)(ca + (((cb - ca) * W + 128) >> 8)) << s;
    }
    return o;
}

/* The reference: w x h pixels drawn over rectangle d of the tw x th
   target, as rr_image draws them (the software backend's drawing, and what
   check.c rr_check holds the GPU backends to). */
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

/* The reference overlay (rr.h rr_overlay), the software backend's drawing
   and what the check holds the GPU backends to: each target pixel whose
   centre falls in the parallelogram samples the image bilinear (texel
   centres at i + 0.5, edges clamped), its alpha times the constant alpha,
   and is blended over. */
static inline void rr_ref_overlay(uint32_t *dst, int tw, int th, const uint32_t *src, int w, int h, const float q[6],
                                  float alpha)
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
            a = out[3] / 255.0f * alpha;
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
