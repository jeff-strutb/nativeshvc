/*
 * rr.h: the renderer backend interface, neutral between graphics APIs.
 *
 * A frame is drawn into a target the size of the window's drawable, one
 * target pixel per window pixel: pictures of the game's pixels go onto it
 * where the port places them, scaled with sharp bilinear filtering (whole
 * pixels at any size, blended only across the one target pixel a pixel's
 * edge falls in), then the target is shown. A backend (Metal, OpenGL,
 * Vulkan, the PSP's GE, or none with no picture) only has to do that; the
 * CPU's drawing of it, for the checks, is tests/ref/rr_soft.c.
 *
 *   RR_SIZE=WxH   headless: the target's size (a window sets it otherwise)
 */
#ifndef PORT_RR_H
#define PORT_RR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the backend, ready to draw; 0 when it cannot run here */
int  rr_open(void);
void rr_close(void);
/* the name of this backend ("metal", "gl", "vulkan", "soft", "null") */
const char *rr_name(void);
/* the target's size in pixels, for the frame about to be drawn */
void rr_target(int *w, int *h);
/* the frame starts: the whole target cleared to argb (0xAARRGGBB) */
void rr_begin(uint32_t argb);
/* w x h pixels (0xAARRGGBB, top row first) drawn over the target's
   rectangle d (x0, y0, x1, y1, target pixels, top left origin): sharp
   bilinear, or one to one when d is w x h */
void rr_image(const uint32_t *argb, int w, int h, const int d[4]);
/* the console's picture, w x h, drawn by the backend's compositor from the
   frame's description (ppu.c ppu_compose, render/compose_body.h: words of
   it), then shown over target rectangle d as rr_image shows a picture;
   0 when the backend has none (the caller draws the picture and calls
   rr_image) */
int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4]);
/* an interlaced frame's woven picture, ww x wh, by the compositor: desc
   holds the finished frame's description and the one before's, each row
   parity's rows from the one at word base0 (even rows) or base1 (odd)
   (ppu.h ppu_woven) */
int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4]);
/* An image with alpha (0xAARRGGBB, the alpha straight) over the frame,
   blended (src alpha, 1 - src alpha), filtered bilinear, onto the
   parallelogram q: its top left (q[0], q[1]), top right (q[2], q[3]) and
   bottom left (q[4], q[5]) corners in target pixels; the filtered alpha
   times the constant alpha (0-1) on the GPU. For the game's controls on a
   touch screen (nshvc_overlay_place). At most RR_OVERLAYS a frame. */
#define RR_OVERLAYS 18                  /* the game's 16, the meter, the pause's backdrop */
void rr_overlay(const uint32_t *argb, int w, int h, const float q[6], float alpha);
/* n parts of one such image, each its texels uv (u0, v0, u1, v1: a
   rectangle of them) onto its parallelogram q (6 floats each, as above):
   one image of the RR_OVERLAYS (the frame-rate meter's text, its font's
   glyphs) */
void rr_overlay_quads(const uint32_t *argb, int w, int h, int n, const float *uv, const float *q, float alpha);

/* The GPU the game may draw with (nshvc.h nshvc_ext_gpu_device), filling
   *loan: its NSHVC_GPU_*; the front end's default 0, none. */
struct NshvcGpuLoan;
int rr_gpu_device(struct NshvcGpuLoan *loan);
/* VRAM the game drew that the GE may read as a picture or an overlay's image
   (nshvc_ext_canvas_screen_ge): fatal when it cannot; the front end's default
   0, no GE */
#define RR_GE_PSM_8888 3                /* the GE's 32-bit pixels (GU_PSM_8888) */
int rr_ge_picture(const char *what, const void *vram, int stride, int w, int h, int psm);
/* An overlay from an image the GPU holds (nshvc_ext_overlay_gpu), as
   rr_overlay's: a texture, or the GE's VRAM (image its address, view its
   stride). The image is sampled as it stands; event, value: its fence (0:
   none), waited on before. */
void rr_overlay_gpu(uint64_t image, uint64_t view, int w, int h, const float q[6], float alpha, uint64_t event,
                    uint64_t value);
/* A screen canvas in a texture (compose_body.h CP_CVGPU): a backend's own
   description, with a canvas whose texels the GPU holds, holds them w x h x 4
   bytes past its last word, at word CP_CV (the backend copying them there on
   the GPU, once its fence is passed, before the compositor runs): so its
   shaders read them as any full-colour canvas's. gen: compose_body.h CP_CVGEN.
   0 when the description has none. */
typedef struct {
    uint64_t image, event, value;
    uint32_t w, h, gen;
} RrCvGpu;
static inline int rr_cv_gpu(const uint32_t *desc, RrCvGpu *g)
{
    if (!(desc[22] & 0x40u))            /* CP_CVSCREEN */
        return 0;
    g->image = desc[38] | (uint64_t)desc[39] << 32;
    g->event = desc[40] | (uint64_t)desc[41] << 32;
    g->value = desc[42] | (uint64_t)desc[43] << 32;
    g->w = desc[23];                    /* CP_CVW */
    g->h = desc[24];                    /* CP_CVH */
    g->gen = desc[37];
    return 1;
}

/* A frame with Mode 7 or a layer's canvas presented over the target's
   rectangle d (rr_mode7.h):
   its record m and the samples s rr_m7_tables made at d's size. Exactly
   the reference's drawing (tests/ref/rr_ref_mode7.h), but that the blends
   round as the GPU's (within 1
   a channel, as rr_image's). */
struct RrMode7;
struct RrM7Samples;
/* 1 when the backend presents such a frame at the target's resolution
   (rr_mode7, rr_compose_mode7: every backend's, the front end's default;
   the PSP's a frame with a canvas and no Mode 7 line); 0 when it presents
   it as any frame, the picture at the console's resolution scaled
   (rr_compose, rr_image). what: the frame's (ppu.c ppu_frame_presents),
   1 a Mode 7 line, 2 a canvas, 4 a canvas on a pseudo-hires line. */
int rr_mode7_window(int what);
/* 1 when the backend presents a Mode 7 line at the console's resolution,
   scaled as any picture, in a frame presented at the target's (a canvas
   frame's): its records' Mode 7 pixels the picture's own (ppu.c
   ppu_set_mode7_plain); the front end's default 0 */
int rr_mode7_plain(void);
/* the largest screen the platform draws on its GPU (nshvc_ext_screen):
   width and height; the front end's default the contract's (8192) */
void rr_screen_max(int *w, int *h);
/* the most texels a canvas (nshvc_ext_canvas_chars, _screen) spans on its
   GPU, across and down: a character canvas's screen times k, a screen
   canvas's texels (at a whole k, its screen times k too); the front end's
   default none (0) */
void rr_canvas_max(int *w, int *h);
/* 1 when the backend reads a screen canvas's texels from the frame's
   description; 0 from the record (RrMode7.cv: rr_compose_mode7), so they are
   copied into no description (ppu.h ppu_set_compose_canvas); the front end's
   default 1 */
int rr_compose_canvas(void);
void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s);
/* a frame presenting Mode 7 or a canvas: its picture and records drawn by
   the backend's compositor from its description (ppu.c ppu_compose_m7),
   then presented as rr_mode7 presents m (m's tables, not its records or
   picture); 0 when the backend has no compositor */
int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s);

/* The console's picture, w x h, drawn by the compositor from the frame's
   description (as rr_compose draws it; wp an interlaced frame's weave as
   rr_compose_woven's: ww, base0, base1, else all 0) apart from the frame
   being drawn, and read back into argb (0xAARRGGBB, top row first): the
   console's picture as the GPU makes it (nshvc_picture). 0 when it cannot. */
int rr_picture(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[3], uint32_t *argb);
/* With no window: the target w x h from the next frame (rr_target's;
   RR_SIZE's until then). With a window, its size stands: fatal. */
void rr_size(int w, int h);

/* The pause's backdrop: the last frame presented blurred and tinted by the
   GPU and kept: at cells of radius_px / 4 pixels square (at least 1), each
   the mean of its pixels; three box blurs across and down, each 4 cells
   either side (none at radius_px 0); then tint (0xAARRGGBB) laid over at
   its alpha. A platform without a compositor's arithmetic makes it as its
   GPU makes a blur of that spread. Drawn over parallelogram q as an
   overlay of its cells is, bilinear (rr_backdrop_draw). 0 when it cannot. */
int rr_backdrop_make(float radius_px, uint32_t tint);
void rr_backdrop_draw(const float q[6]);

/* the frame done: shown in the window, when there is one */
void rr_present(void);
/* the last presented frame, read back: rr_target's w x h pixels into argb;
   0 when this backend keeps none */
int  rr_read(uint32_t *argb);

#ifdef __cplusplus
}
#endif
#endif
