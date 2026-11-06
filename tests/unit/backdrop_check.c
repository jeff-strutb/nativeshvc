/*
 * backdrop_check.c: the pause's backdrop (rr_backdrop_make, rr_backdrop_draw)
 * as a renderer makes it, against what it must be. The frame laid on the
 * target with overlays, presented, the backdrop made from it, drawn over
 * the whole target and read back:
 *
 *   a frame of one colour: that colour with the tint laid over it, exactly
 *   (within rounding: 1);
 *   no blur and no tint: the frame itself;
 *   a white square of 10 x 10 cells on black (a cell the blur's radius / 4):
 *   spread symmetrically, its light kept within 1%, its
 *   standard deviation the square's with three boxes of 4 cells either side,
 *   sqrt((10^2 - 1) / 12 + 3 * ((2 * 4 + 1)^2 - 1) / 12) = 5.31 cells, and
 *   the cells' bilinear magnification's 1/6 cell^2 with them: within 3%.
 *
 *   RR_SIZE=1203x801 build/null-RENDER/tests/backdrop_check
 *
 * Run with each RENDER's build. Exit 0 when every check holds.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static int tw, th;
static uint32_t *got;

/* the frame (w x h, 0xFFRRGGBB) laid over the target and presented */
static void frame_of(const uint32_t *px, int w, int h)
{
    float q[6] = { 0, 0, (float)tw, 0, 0, (float)th };
    rr_begin(0xFF000000u);
    rr_overlay(px, w, h, q, 1);
    rr_present();
}

/* the backdrop made from the frame presented, drawn and read back */
static int backdrop(float radius, uint32_t tint)
{
    float q[6] = { 0, 0, (float)tw, 0, 0, (float)th };
    if (!rr_backdrop_make(radius, tint))
        return 0;
    rr_begin(0xFF000000u);
    rr_backdrop_draw(q);
    rr_present();
    return rr_read(got);
}

int main(void)
{
    const uint32_t c = 0xFF4080C0u, tint = 0x80102030u;
    uint32_t *f;
    int i, x, y, k, worst = 0, slack = 1;
    if (!rr_open()) {
        fprintf(stderr, "backdrop_check: %s cannot open\n", rr_name());
        return 2;
    }
    rr_target(&tw, &th);
    got = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    f = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    if (!got || !f)
        return 2;

    /* one colour under the tint */
    frame_of(&c, 1, 1);
    if (!backdrop(16, tint)) {
        printf("FAIL the backdrop could not be made\n");
        return 1;
    }
    {
        float ta = (float)(tint >> 24) / 255.0f;
        for (i = 0; i < tw * th; i++)
            for (k = 0; k < 24; k += 8) {
                int want = (int)((float)(c >> k & 0xFF) * (1 - ta) + (float)(tint >> k & 0xFF) * ta + 0.5f);
                int d = abs((int)(got[i] >> k & 0xFF) - want);
                worst = d > worst ? d : worst;
            }
        printf("     one colour under the tint: largest difference %d\n", worst);
        check(worst <= slack, "a frame of one colour is the colour with the tint over it");
    }

    /* no blur, no tint: the frame */
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++)
            f[y * tw + x] = 0xFF000000u | (uint32_t)(x * 255 / tw) << 16 | (uint32_t)(y * 255 / th) << 8 | 0x40u;
    frame_of(f, tw, th);
    if (backdrop(0, 0)) {
        worst = 0;
        for (i = 0; i < tw * th; i++)
            for (k = 0; k < 24; k += 8) {
                int d = abs((int)(got[i] >> k & 0xFF) - (int)(f[i] >> k & 0xFF));
                worst = d > worst ? d : worst;
            }
        printf("     no blur, no tint: largest difference %d\n", worst);
        check(worst <= 2, "no blur and no tint is the frame itself");
    }

    /* the white square */
    {
        int radius = 16, cell = radius / 4, side = 10 * cell, x0 = (tw - side) / 2, y0 = (th - side) / 2;
        double sum = 0, mx = 0, my = 0, vx = 0, vy = 0, want = 255.0 * side * side, sdx, sdy, cx, cy;
        double target = sqrt((10.0 * 10.0 - 1) / 12 + 3 * (9.0 * 9.0 - 1) / 12 + 1.0 / 6);
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                f[y * tw + x] = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side ? 0xFFFFFFFFu : 0xFF000000u;
        frame_of(f, tw, th);
        if (backdrop((float)radius, 0)) {
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++) {
                    double v = (double)(got[y * tw + x] & 0xFF);
                    sum += v;
                    mx += v * (x + 0.5);
                    my += v * (y + 0.5);
                }
            mx /= sum;
            my /= sum;
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++) {
                    double v = (double)(got[y * tw + x] & 0xFF);
                    vx += v * (x + 0.5 - mx) * (x + 0.5 - mx);
                    vy += v * (y + 0.5 - my) * (y + 0.5 - my);
                }
            sdx = sqrt(vx / sum) / cell;
            sdy = sqrt(vy / sum) / cell;
            cx = x0 + side / 2.0;
            cy = y0 + side / 2.0;
            printf("     the square: light %.4f of its own, centre off by (%.2f, %.2f) pixels, deviation %.3f x %.3f cells (%.3f)\n",
                   sum / want, mx - cx, my - cy, sdx, sdy, target);
            check(fabs(sum / want - 1) < 0.01, "the square's light is kept");
            check(fabs(mx - cx) < cell && fabs(my - cy) < cell, "the spread is about the square's centre");
            check(fabs(sdx / target - 1) < 0.03 && fabs(sdy / target - 1) < 0.03,
                  "the spread's deviation is the boxes'");
        }
    }
    printf("backdrop_check: %s, %dx%d: %d check(s) failed\n", rr_name(), tw, th, fails);
    return fails ? 1 : 0;
}
