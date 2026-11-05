/*
 * rr_none.c: no renderer: a headless console with no GPU. It runs, and has
 * no picture: every call that would draw one says so. (The test programs
 * bring the reference's, tests/ref/rr_soft.c, in its place.)
 */
#include "nshvc/nshvc.h"
#include "rr.h"

int rr_open(void) { return 0; }
void rr_close(void) {}
const char *rr_name(void) { return "none"; }
void rr_target(int *w, int *h) { *w = *h = 0; }
void rr_begin(uint32_t argb) { (void)argb; }
void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    (void)argb;
    (void)d;
    nshvc_fatal("rr_image(%dx%d): no renderer", w, h);
}
int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    (void)desc; (void)words; (void)w; (void)h; (void)d;
    return 0;
}
int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1, const int d[4])
{
    (void)desc; (void)words; (void)ww; (void)wh; (void)base0; (void)base1; (void)d;
    return 0;
}
void rr_overlay(const uint32_t *argb, int w, int h, const float q[6], float alpha)
{
    (void)argb; (void)w; (void)h; (void)q; (void)alpha;
}
void rr_overlay_quads(const uint32_t *argb, int w, int h, int n, const float *uv, const float *q, float alpha)
{
    (void)argb; (void)w; (void)h; (void)n; (void)uv; (void)q; (void)alpha;
}
void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s) { (void)m; (void)d; (void)s; }
int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    (void)desc; (void)words; (void)m; (void)d; (void)s;
    return 0;
}
int rr_picture(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[3], uint32_t *argb)
{
    (void)desc; (void)words; (void)w; (void)h; (void)wp; (void)argb;
    return 0;
}
void rr_size(int w, int h) { (void)w; (void)h; }
int rr_backdrop_make(float radius_px, uint32_t tint) { (void)radius_px; (void)tint; return 0; }
void rr_backdrop_draw(const float q[6]) { (void)q; }
void rr_present(void) {}
int rr_read(uint32_t *argb) { (void)argb; return 0; }
