/*
 * backdrop.c: a frame blurred and tinted (backdrop.h). The blur is made at
 * cells a quarter of its radius square (each the mean of its pixels): three
 * box blurs across and down, each four cells either side, which come to
 * near a Gaussian of the radius; the tint is laid over the result. Small
 * enough to be scaled back over the frame bilinear with no edge showing:
 * a cell is at most a quarter of the blur.
 */
#include <stdlib.h>

#include "backdrop.h"

#define REACH 4                     /* a box's cells either side */

/* one box blur along a line of n cells (3 floats each), stride st cells */
static void box_line(float *c, int n, int st, int r, float *tmp)
{
    int i, k;
    for (k = 0; k < 3; k++) {
        float sum = 0;
        for (i = -r; i <= r; i++)
            sum += c[(size_t)(i < 0 ? 0 : i >= n ? n - 1 : i) * st * 3 + k];
        for (i = 0; i < n; i++) {
            int lo = i - r < 0 ? 0 : i - r, hi = i + r + 1 >= n ? n - 1 : i + r + 1;
            tmp[i] = sum / (float)(2 * r + 1);
            sum += c[(size_t)hi * st * 3 + k] - c[(size_t)lo * st * 3 + k];
        }
        for (i = 0; i < n; i++)
            c[(size_t)i * st * 3 + k] = tmp[i];
    }
}

uint32_t *backdrop_make(const uint32_t *f, int tw, int th, float radius_px, uint32_t tint, int *w_out, int *h_out,
                        int *cell_out)
{
    float *c, *tmp, ta = (float)(tint >> 24) / 255.0f;
    int cell = (int)(radius_px / REACH), w, h, x, y, k, pass, r = radius_px > 0 ? REACH : 0;
    uint32_t *out;
    if (cell < 1)
        cell = 1;
    w = (tw + cell - 1) / cell;
    h = (th + cell - 1) / cell;
    c = (float *)calloc((size_t)w * (size_t)h * 3, sizeof *c);
    tmp = (float *)malloc((size_t)(w > h ? w : h) * sizeof *tmp);
    out = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!c || !tmp || !out) {
        free(c);
        free(tmp);
        free(out);
        return NULL;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float sum[3] = { 0, 0, 0 };
            int n = 0, yy, xx;
            for (yy = y * cell; yy < (y + 1) * cell && yy < th; yy++)
                for (xx = x * cell; xx < (x + 1) * cell && xx < tw; xx++, n++) {
                    uint32_t p = f[(size_t)yy * (size_t)tw + (size_t)xx];
                    sum[0] += (float)(p >> 16 & 0xFF);
                    sum[1] += (float)(p >> 8 & 0xFF);
                    sum[2] += (float)(p & 0xFF);
                }
            for (k = 0; k < 3; k++)
                c[((size_t)y * w + x) * 3 + k] = sum[k] / (float)n;
        }
    if (r)
        for (pass = 0; pass < 3; pass++) {
            for (y = 0; y < h; y++)
                box_line(c + (size_t)y * w * 3, w, 1, r, tmp);
            for (x = 0; x < w; x++)
                box_line(c + (size_t)x * 3, h, w, r, tmp);
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t o = 0xFF000000u;
            for (k = 0; k < 3; k++) {
                float t = (float)(tint >> (16 - 8 * k) & 0xFF);
                float v = c[((size_t)y * w + x) * 3 + k] * (1 - ta) + t * ta;
                o |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f) << (16 - 8 * k);
            }
            out[(size_t)y * w + x] = o;
        }
    free(c);
    free(tmp);
    *w_out = w;
    *h_out = h;
    *cell_out = cell;
    return out;
}
