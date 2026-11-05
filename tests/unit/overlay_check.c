/*
 * overlay_check.c: the renderer's overlays (rr.h rr_overlay) against the
 * reference drawing (rr_size.h rr_ref_overlay): a picture scaled over the
 * target, then images with alpha turned, scaled and faded over it, read
 * back and compared. Run with each RENDER's build; exit 0 when every pixel
 * is within 2 of the reference in every channel (the GPUs' filtering
 * rounds), 1 otherwise, 2 when the renderer cannot open.
 * Each platform's overlays are its own GPU's filtering and blend. The
 * PSP's GE filters in sixteenths of a texel (its fraction truncated: off
 * by up to a sixteenth of the step across and of the step down) and blends
 * by weights that sum to 255/256: there each overlay covering a pixel adds
 * 1 + (the spread of its four texels' channel and alpha) / 8 to the 2,
 * and its vertices are in sixteenths of a pixel (an edge's pixels within
 * 1/16 of it not judged).
 */
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rr.h"
#include "rr_ref.h"

/* got against want, overlays q (n of them, their images im, OW x OH):
   the pixels off by more than the bound (2, or the GE's), the largest, and
   those on an edge not judged */
enum { OW = 40, OH = 24 };
static int compare(const uint32_t *got, const uint32_t *want, int tw, int th, float (*q)[6], const uint32_t *const *im,
                   int n, int ge, int *worstp, int *tiesp)
{
    int i, bad = 0, worst = 0, ties = 0;
    for (i = 0; i < tw * th; i++) {
        int sh, w = 0, k, tie = 0, over = 0;
        /* a pixel whose centre lies on an overlay's edge (within 0.01 of a
           pixel; the GE's vertices are in sixteenths of a pixel, so within
           1/16 there) is covered or not by the rounding of its edge: either
           is right */
        float ew = ge ? 1.0f / 16.0f : 0.01f;
        for (k = 0; k < n; k++) {
            float ax = q[k][2] - q[k][0], ay = q[k][3] - q[k][1], bx = q[k][4] - q[k][0], by = q[k][5] - q[k][1];
            float px = (float)(i % tw) + 0.5f - q[k][0], py = (float)(i / tw) + 0.5f - q[k][1];
            float det = ax * by - ay * bx, u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
            float eu = ew / sqrtf(ax * ax + ay * ay), ev = ew / sqrtf(bx * bx + by * by);
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
            int a = (int)((got[i] >> sh) & 0xFF) - (int)((want[i] >> sh) & 0xFF), allow = 2;
            if (a < 0)
                a = -a;
            if (ge)
                for (k = 0; k < n; k++) {
                    /* (the GE: overlay k's texels under the pixel, rr_ref_overlay's) */
                    float ax = q[k][2] - q[k][0], ay = q[k][3] - q[k][1], bx = q[k][4] - q[k][0], by = q[k][5] - q[k][1];
                    float px = (float)(i % tw) + 0.5f - q[k][0], py = (float)(i / tw) + 0.5f - q[k][1];
                    float det = ax * by - ay * bx, u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
                    int ix, iy, t, lo = 255, hi = 0, alo = 255, ahi = 0;
                    if (u < 0 || u >= 1 || v < 0 || v >= 1)
                        continue;
                    ix = (int)floorf(u * OW - 0.5f);
                    iy = (int)floorf(v * OH - 0.5f);
                    for (t = 0; t < 4; t++) {
                        uint32_t c = rr_texel(im[k], OW, OH, ix + (t & 1), iy + (t >> 1));
                        int cv = (int)((c >> sh) & 0xFF), av = (int)(c >> 24);
                        lo = cv < lo ? cv : lo;
                        hi = cv > hi ? cv : hi;
                        alo = av < alo ? av : alo;
                        ahi = av > ahi ? av : ahi;
                    }
                    allow += 1 + (hi - lo + ahi - alo + 7) / 8;
                }
            if (a > allow)
                over = 1;
            if (a > w)
                w = a;
        }
        if (w > worst)
            worst = w;
        if (over) {
            if (!bad)
                printf("first at (%d, %d): got %08x want %08x\n", i % tw, i / tw, got[i], want[i]);
            bad++;
        }
    }
    *worstp = worst > *worstp ? worst : *worstp;
    *tiesp += ties;
    return bad;
}

int main(void)
{
    enum { PW = 256, PH = 224 };
    static uint32_t pic[PW * PH], img[OW * OH], chk[OW * OH];
    const uint32_t *im[5] = { img, img, img, img, chk };
    static const float fade[5] = { 1, 0.5f, 1, 0.25f, 0.75f };   /* (the constant alpha) */
    uint32_t *got, *want;
    int tw, th, i, x, y, bad = 0, worst = 0, ties = 0, ge;
    const int d[4] = { 0, 0, 0, 0 };
    int dd[4];
    float q[5][6];
    for (i = 0; i < PW * PH; i++)
        pic[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8 & 0xFFFFFF);
    for (y = 0; y < OH; y++)
        for (x = 0; x < OW; x++) {
            uint32_t a = (uint32_t)(255 * x / (OW - 1));
            img[y * OW + x] = a << 24 | (uint32_t)(x * 6) << 16 | (uint32_t)(y * 10) << 8 | 0x40;
            /* hard edges: a checker of 3-texel squares, opaque white and
               black, every third clear */
            chk[y * OW + x] = ((x / 3 + y / 3) % 3 == 2 ? 0u : 0xFF000000u) | ((x / 3 + y / 3) & 1 ? 0xFFFFFFu : 0u);
        }
    (void)d;
    if (!rr_open()) {
        fprintf(stderr, "overlay_check: %s cannot open\n", rr_name());
        return 2;
    }
    rr_target(&tw, &th);
    ge = !strcmp(rr_name(), "psp");
    dd[0] = 0;
    dd[1] = 0;
    dd[2] = tw;
    dd[3] = th;
    {
        /* five placements: as it is, scaled up, turned 30 degrees, turned
           and scaled across the target's edge; the checker turned and
           scaled */
        float cx[5] = { 100.3f, 300.7f, 520.2f, 980.4f, 180.6f }, cy[5] = { 90.6f, 200.2f, 400.9f, 700.3f, 150.3f };
        float sc[5] = { 1.0f, 3.3f, 2.1f, 4.7f, 2.7f }, an[5] = { 0, 0, 0.5236f, -1.1f, 0.3f };
        for (i = 0; i < 5; i++) {
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
    if (!ge)
        rr_image(pic, PW, PH, dd);   /* (the PSP draws no picture made on the CPU: black under them) */
    for (i = 0; i < 5; i++)
        rr_overlay(im[i], OW, OH, q[i], fade[i]);
    rr_present();
    got = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    want = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    if (!got || !want || !rr_read(got))
        return 2;
    for (i = 0; i < tw * th; i++)
        want[i] = 0xFF000000u;
    if (!ge)
        rr_ref_image(want, tw, th, pic, PW, PH, dd);
    for (i = 0; i < 5; i++)
        rr_ref_overlay(want, tw, th, im[i], OW, OH, q[i], fade[i]);
    bad = compare(got, want, tw, th, q, im, 5, ge, &worst, &ties);
    {
        /* parts of one image as quads (rr_overlay_quads): img and the
           checker side by side, each with a border of its own edge texels
           (so the filter at a part's edge reads as the reference's clamp),
           drawn over two of the placements, against each part drawn alone */
        enum { AW = 2 * (OW + 2), AH = OH + 2 };
        static uint32_t atlas[AW * AH];
        float uv[2][4], q2[2][6];
        const uint32_t *im2[2] = { img, chk };
        int p, k;
        for (p = 0; p < 2; p++)
            for (y = 0; y < AH; y++)
                for (x = 0; x < OW + 2; x++) {
                    int sx = x - 1 < 0 ? 0 : x - 1 >= OW ? OW - 1 : x - 1, sy = y - 1 < 0 ? 0 : y - 1 >= OH ? OH - 1 : y - 1;
                    atlas[y * AW + p * (OW + 2) + x] = im2[p][sy * OW + sx];
                }
        for (p = 0; p < 2; p++) {
            uv[p][0] = (float)(p * (OW + 2) + 1);
            uv[p][1] = 1;
            uv[p][2] = uv[p][0] + OW;
            uv[p][3] = 1 + OH;
            for (k = 0; k < 6; k++)
                q2[p][k] = q[p ? 3 : 1][k];
        }
        rr_begin(0xFF000000u);
        if (!ge)
            rr_image(pic, PW, PH, dd);
        rr_overlay_quads(atlas, AW, AH, 2, &uv[0][0], &q2[0][0], 1);
        rr_present();
        if (!rr_read(got))
            return 2;
        for (i = 0; i < tw * th; i++)
            want[i] = 0xFF000000u;
        if (!ge)
            rr_ref_image(want, tw, th, pic, PW, PH, dd);
        for (p = 0; p < 2; p++)
            rr_ref_overlay(want, tw, th, im2[p], OW, OH, q2[p], 1);
        k = compare(got, want, tw, th, q2, im2, 2, ge, &worst, &ties);
        printf("overlay_check: parts as quads: %d pixels off\n", k);
        bad += k;
    }
    printf("overlay_check: %s, %dx%d: %d pixels off by more than %s (largest %d); %d on an edge not judged\n", rr_name(),
           tw, th, bad, ge ? "the GE's bound" : "2", worst, ties);
    return bad ? 1 : 0;
}
