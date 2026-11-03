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
#include "rr_size.h"
#include "rr_mode7.h"
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


int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    (void)desc;
    (void)words;
    (void)w;
    (void)h;
    (void)d;
    return 0;   /* no compositor: the caller draws the picture */
}


int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4])
{
    (void)desc;
    (void)words;
    (void)ww;
    (void)wh;
    (void)base0;
    (void)base1;
    (void)d;
    return 0;   /* no compositor */
}

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    rr_ref_image(s_px, s_w, s_h, argb, w, h, d);
}


int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s)
{
    (void)desc;
    (void)words;
    (void)m;
    (void)d;
    (void)s;
    return 0;   /* no compositor: the caller makes the records and picture */
}

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    rr_ref_mode7(s_px, s_w, s_h, m, d, s);
}

void rr_overlay(const uint32_t *argb, int w, int h, const float q[6])
{
    rr_ref_overlay(s_px, s_w, s_h, argb, w, h, q);
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
