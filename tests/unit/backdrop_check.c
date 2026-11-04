/*
 * backdrop_check.c: the pause's backdrop (src/front/backdrop.c) against
 * what it must be: a frame of one colour the colour with the tint laid
 * over it, exactly (rounded); a blur of 0 the frame itself at a cell a
 * pixel; a white square of 10 x 10 cells on black spread symmetrically,
 * its light kept (the cells' sum 100 x 255, within rounding), its spread's
 * standard deviation the square's with three boxes of 4 cells either side
 * (sqrt((10^2 - 1) / 12 + 3 * ((2 * 4 + 1)^2 - 1) / 12) = 5.31 cells); the cells' count the
 * frame's size over the cell, rounded up.
 *
 *   backdrop_check            exit 0 when every check holds
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "backdrop.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    enum { TW = 1203, TH = 801 };
    uint32_t *f = (uint32_t *)malloc((size_t)TW * TH * 4), *b;
    int w, h, cell, i, x, y, bad = 0;
    double sum = 0, mx = 0, my = 0, vx = 0, vy = 0;

    for (i = 0; i < TW * TH; i++)
        f[i] = 0xFF4080C0u;
    b = backdrop_make(f, TW, TH, 24, 0x80000000u, &w, &h, &cell);
    for (i = 0; i < w * h; i++)
        bad += b[i] != 0xFF204060u;
    check(cell == 6 && w == (TW + 5) / 6 && h == (TH + 5) / 6, "radius 24: cells of 6, the frame's size over 6 rounded up");
    check(!bad, "one colour (40 80 C0) under black at half: 20 40 60 everywhere");
    free(b);

    for (i = 0; i < TW * TH; i++)
        f[i] = (uint32_t)(0xFF000000u | (unsigned)(i * 2654435761u >> 8));
    b = backdrop_make(f, TW, TH, 0, 0, &w, &h, &cell);
    bad = 0;
    for (i = 0; i < TW * TH; i++)
        bad += b[i] != f[i];
    check(cell == 1 && w == TW && h == TH && !bad, "no blur and no tint: the frame itself");
    free(b);

    for (i = 0; i < TW * TH; i++)
        f[i] = 0xFF000000u;
    for (y = 366; y < 426; y++)                         /* cells (95-104, 61-70) */
        for (x = 570; x < 630; x++)
            f[(size_t)y * TW + x] = 0xFFFFFFFFu;
    b = backdrop_make(f, TW, TH, 24, 0, &w, &h, &cell);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double v = (double)(b[y * w + x] & 0xFF);
            sum += v;
            mx += v * x;
            my += v * y;
        }
    mx /= sum;
    my /= sum;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double v = (double)(b[y * w + x] & 0xFF);
            vx += v * (x - mx) * (x - mx);
            vy += v * (y - my) * (y - my);
        }
    printf("     the square's light: sum %.1f (want 25500), centre (%.3f, %.3f), deviation %.3f %.3f cells\n", sum,
           mx, my, sqrt(vx / sum), sqrt(vy / sum));
    check(fabs(sum - 25500) < 0.01 * 25500, "its light kept, within 1%");
    check(fabs(mx - 99.5) < 0.01 && fabs(my - 65.5) < 0.01, "spread about its own centre");
    check(fabs(sqrt(vx / sum) - 5.31) < 0.05 && fabs(sqrt(vy / sum) - 5.31) < 0.05,
          "spread as three boxes of 4 either side: deviation 5.31 cells");
    bad = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < 100; x++)
            bad += b[y * w + x] != b[y * w + 199 - x];
    for (y = 0; y < 66; y++)
        for (x = 0; x < w; x++)
            bad += b[y * w + x] != b[(131 - y) * w + x];
    check(!bad, "symmetric across and down");
    free(b);
    free(f);
    printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
