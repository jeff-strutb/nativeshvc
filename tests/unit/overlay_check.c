/*
 * overlay_check.c: the renderer's overlays (rr.h rr_overlay) against the
 * reference drawing (rr_size.h rr_ref_overlay): a picture scaled over the
 * target, then images with alpha turned, scaled and faded over it, read
 * back and compared. Run with each RENDER's build; exit 0 when every pixel
 * is within 2 of the reference in every channel (the GPUs' filtering
 * rounds), 1 otherwise, 2 when the renderer cannot open.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rr.h"
#include "rr_size.h"

int main(void)
{
    enum { PW = 256, PH = 224, OW = 40, OH = 24 };
    static uint32_t pic[PW * PH], img[OW * OH];
    uint32_t *got, *want;
    int tw, th, i, x, y, bad = 0, worst = 0, ties = 0;
    const int d[4] = { 0, 0, 0, 0 };
    int dd[4];
    float q[4][6];
    for (i = 0; i < PW * PH; i++)
        pic[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8 & 0xFFFFFF);
    for (y = 0; y < OH; y++)
        for (x = 0; x < OW; x++) {
            uint32_t a = (uint32_t)(255 * x / (OW - 1));
            img[y * OW + x] = a << 24 | (uint32_t)(x * 6) << 16 | (uint32_t)(y * 10) << 8 | 0x40;
        }
    (void)d;
    if (!rr_open()) {
        fprintf(stderr, "overlay_check: %s cannot open\n", rr_name());
        return 2;
    }
    rr_target(&tw, &th);
    dd[0] = 0;
    dd[1] = 0;
    dd[2] = tw;
    dd[3] = th;
    {
        /* four placements: as it is, scaled up, turned 30 degrees, turned
           and scaled across the target's edge */
        float cx[4] = { 100.3f, 300.7f, 520.2f, 980.4f }, cy[4] = { 90.6f, 200.2f, 400.9f, 700.3f };
        float sc[4] = { 1.0f, 3.3f, 2.1f, 4.7f }, an[4] = { 0, 0, 0.5236f, -1.1f };
        for (i = 0; i < 4; i++) {
            float hw = OW * sc[i] / 2, hh = OH * sc[i] / 2, c = cosf(an[i]), s = sinf(an[i]);
            q[i][0] = cx[i] + (-hw * c + hh * s);
            q[i][1] = cy[i] + (-hw * s - hh * c);
            q[i][2] = cx[i] + (hw * c + hh * s);
            q[i][3] = cy[i] + (hw * s - hh * c);
            q[i][4] = cx[i] + (-hw * c - hh * s);
            q[i][5] = cy[i] + (-hw * s + hh * c);
        }
    }
    rr_begin(0xFF000000u);
    rr_image(pic, PW, PH, dd);
    for (i = 0; i < 4; i++)
        rr_overlay(img, OW, OH, q[i]);
    rr_present();
    got = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    want = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    if (!got || !want || !rr_read(got))
        return 2;
    for (i = 0; i < tw * th; i++)
        want[i] = 0xFF000000u;
    rr_ref_image(want, tw, th, pic, PW, PH, dd);
    for (i = 0; i < 4; i++)
        rr_ref_overlay(want, tw, th, img, OW, OH, q[i]);
    for (i = 0; i < tw * th; i++) {
        int sh, w = 0, k, tie = 0;
        /* a pixel whose centre lies on an overlay's edge (within 0.01 of a
           pixel) is covered or not by the rounding of its edge: either is
           right */
        for (k = 0; k < 4; k++) {
            float ax = q[k][2] - q[k][0], ay = q[k][3] - q[k][1], bx = q[k][4] - q[k][0], by = q[k][5] - q[k][1];
            float px = (float)(i % tw) + 0.5f - q[k][0], py = (float)(i / tw) + 0.5f - q[k][1];
            float det = ax * by - ay * bx, u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
            float eu = 0.01f / sqrtf(ax * ax + ay * ay), ev = 0.01f / sqrtf(bx * bx + by * by);
            if ((fabsf(u) < eu || fabsf(u - 1) < eu) && v > -ev && v < 1 + ev)
                tie = 1;
            if ((fabsf(v) < ev || fabsf(v - 1) < ev) && u > -eu && u < 1 + eu)
                tie = 1;
        }
        if (tie) {
            ties++;
            continue;
        }
        for (sh = 0; sh < 24; sh += 8) {
            int a = (int)((got[i] >> sh) & 0xFF) - (int)((want[i] >> sh) & 0xFF);
            if (a < 0)
                a = -a;
            if (a > w)
                w = a;
        }
        if (w > worst)
            worst = w;
        if (w > 2) {
            if (!bad)
                printf("first at (%d, %d): got %08x want %08x\n", i % tw, i / tw, got[i], want[i]);
            bad++;
        }
    }
    printf("overlay_check: %s, %dx%d: %d pixels off by more than 2 (largest %d); %d on an edge not judged\n", rr_name(),
           tw, th, bad, worst, ties);
    return bad ? 1 : 0;
}
