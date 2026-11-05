/*
 * rr_soft.c: the renderer backend in software, the reference the GPU
 * backends are checked against: sharp bilinear computed as their shaders
 * compute it (rr_size.h), in floats, with the bilinear blend of 8-bit
 * channels rounded to the nearest. The target is shown through the host's
 * picture call (host_present), one target pixel per window pixel.
 */
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_ref_mode7.h"
#include "rr_compose.h"
#include "backdrop.h"
#include "host.h"

void host_window_pixels(int *w, int *h);
__attribute__((weak)) void host_window_pixels(int *w, int *h) { *w = *h = 0; }

static uint32_t *s_px;
static int s_w, s_h;

static void size_to(int w, int h)
{
    if (w == s_w && h == s_h && s_px)
        return;
    free(s_px);
    s_px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    s_w = w;
    s_h = h;
}

int rr_open(void)
{
    int w, h;
    host_window_pixels(&w, &h);
    if (w < 1 || h < 1)
        rr_headless_size(&w, &h);
    size_to(w, h);
    return s_px != NULL;
}

void rr_close(void)
{
    free(s_px);
    s_px = NULL;
    s_w = s_h = 0;
}

const char *rr_name(void) { return "soft"; }

void rr_target(int *w, int *h)
{
    int ww, wh;
    host_window_pixels(&ww, &wh);
    if (ww > 0 && wh > 0)
        size_to(ww, wh);
    *w = s_w;
    *h = s_h;
}

void rr_begin(uint32_t argb)
{
    size_t i, n = (size_t)s_w * (size_t)s_h;
    for (i = 0; i < n; i++)
        s_px[i] = argb | 0xFF000000u;
}


/* the C build of the compositor (rr_compose.h): the picture, into s_pic */
static uint32_t *s_pic;
static size_t s_pic_cap;

static uint32_t *pic_of(const uint32_t *desc, int w, int h, const uint32_t wp[3])
{
    size_t n = (size_t)w * (size_t)h;
    int x, y;
    if (n > s_pic_cap) {
        free(s_pic);
        s_pic = (uint32_t *)malloc(n * 4);
        s_pic_cap = s_pic ? n : 0;
    }
    if (!s_pic)
        return NULL;
    rr_cp_fd = desc;
    cp_fdb = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            s_pic[(size_t)y * (size_t)w + (size_t)x] =
                wp[0] ? cp_woven((uint32_t)x, (uint32_t)y, wp[0], wp[1], wp[2]) : rr_cp_pixel((uint32_t)x, (uint32_t)y);
    cp_fdb = 0;
    return s_pic;
}

int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    static const uint32_t none[3] = { 0, 0, 0 };
    uint32_t *p = pic_of(desc, w, h, none);
    (void)words;
    if (!p)
        return 0;
    rr_ref_image(s_px, s_w, s_h, p, w, h, d);
    return 1;
}

int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4])
{
    uint32_t wp[3] = { (uint32_t)ww, base0, base1 };
    uint32_t *p = pic_of(desc, ww, wh, wp);
    (void)words;
    if (!p)
        return 0;
    rr_ref_image(s_px, s_w, s_h, p, ww, wh, d);
    return 1;
}

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    rr_ref_image(s_px, s_w, s_h, argb, w, h, d);
}


/* the picture and its records by the C build of the compositor, presented
   by the reference's drawing */
int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s)
{
    static const uint32_t none[3] = { 0, 0, 0 };
    static RrM7Px *rec;
    static size_t rec_cap;
    RrMode7 lm = *m;
    size_t n = (size_t)m->pw * (size_t)m->ph;
    uint32_t *p = pic_of(desc, m->pw, m->ph, none);
    int x, y;
    (void)words;
    if (!p)
        return 0;
    if (n > rec_cap) {
        free(rec);
        rec = (RrM7Px *)malloc(n * sizeof *rec);
        rec_cap = rec ? n : 0;
    }
    if (!rec)
        return 0;
    rr_cp_fd = desc;
    cp_fdb = 0;
    for (y = 0; y < m->ph; y++)
        for (x = 0; x < m->pw; x++) {
            RrM7Px *o = &rec[(size_t)y * (size_t)m->pw + (size_t)x];
            cp_record((uint32_t)x, (uint32_t)y);
            o->w0 = cp_r0;
            o->w1 = cp_r1;
            o->w2 = cp_r2;
            o->w3 = cp_r3;
        }
    lm.px = rec;
    lm.pic = p;
    rr_ref_mode7(s_px, s_w, s_h, &lm, d, s);
    return 1;
}

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    rr_ref_mode7(s_px, s_w, s_h, m, d, s);
}

void rr_overlay(const uint32_t *argb, int w, int h, const float q[6], float alpha)
{
    rr_ref_overlay(s_px, s_w, s_h, argb, w, h, q, alpha);
}

void rr_overlay_quads(const uint32_t *argb, int w, int h, int n, const float *uv, const float *q, float alpha)
{
    int j, x, y;
    for (j = 0; j < n; j++) {
        int u0 = (int)uv[4 * j], v0 = (int)uv[4 * j + 1], sw = (int)uv[4 * j + 2] - u0, sh = (int)uv[4 * j + 3] - v0;
        uint32_t *part;
        if (sw <= 0 || sh <= 0 || !(part = (uint32_t *)malloc((size_t)sw * (size_t)sh * 4)))
            continue;
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++)
                part[y * sw + x] = argb[(size_t)(v0 + y) * (size_t)w + (size_t)(u0 + x)];
        rr_ref_overlay(s_px, s_w, s_h, part, sw, sh, q + 6 * j, alpha);
        free(part);
    }
}

int rr_picture(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[3], uint32_t *argb)
{
    uint32_t *p = pic_of(desc, w, h, wp);
    (void)words;
    if (!p)
        return 0;
    memcpy(argb, p, (size_t)w * (size_t)h * 4);
    return 1;
}

void rr_size(int w, int h) { size_to(w, h); }

/* the backdrop by its definition (backdrop.c), over the frame */
static uint32_t *s_bd;
static int s_bdw, s_bdh;

int rr_backdrop_make(float radius_px, uint32_t tint)
{
    int cell;
    free(s_bd);
    s_bd = backdrop_make(s_px, s_w, s_h, radius_px, tint, &s_bdw, &s_bdh, &cell);
    return s_bd != NULL;
}

void rr_backdrop_draw(const float q[6])
{
    if (s_bd)
        rr_ref_overlay(s_px, s_w, s_h, s_bd, s_bdw, s_bdh, q, 1);
}

void rr_present(void)
{
    host_present(s_px, s_w, s_h, (float)s_w / (float)s_h);
}

int rr_read(uint32_t *argb)
{
    memcpy(argb, s_px, (size_t)s_w * (size_t)s_h * 4);
    return 1;
}
