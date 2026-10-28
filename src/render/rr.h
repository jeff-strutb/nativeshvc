/*
 * rr.h: the renderer backend interface, neutral between graphics APIs (the
 * same design as the Boss Rally port's).
 *
 * A frame is drawn into a target the size of the window's drawable, one
 * target pixel per window pixel: pictures of the game's pixels go onto it
 * where the port places them, scaled with sharp bilinear filtering (whole
 * pixels at any size, blended only across the one target pixel a pixel's
 * edge falls in), then the target is shown. A backend (Metal, OpenGL,
 * Vulkan, the software one, or the null one) only has to do that.
 *
 *   RR_SIZE=WxH   headless: the target's size (a window sets it otherwise)
 */
#ifndef PORT_RR_H
#define PORT_RR_H

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
/* An image with alpha (0xAARRGGBB, the alpha straight) over the frame,
   blended (src alpha, 1 - src alpha), filtered bilinear, onto the
   parallelogram q: its top left (q[0], q[1]), top right (q[2], q[3]) and
   bottom left (q[4], q[5]) corners in target pixels. For the game's
   controls on a touch screen (nshvc_overlay_place). At most RR_OVERLAYS a
   frame. */
#define RR_OVERLAYS 16
void rr_overlay(const uint32_t *argb, int w, int h, const float q[6]);

/* A frame with Mode 7 or a layer's canvas presented over the target's
   rectangle d (rr_mode7.h):
   its record m and the samples s rr_m7_tables made at d's size. Exactly
   rr_ref_mode7's drawing, but that the blends round as the GPU's (within 1
   a channel, as rr_image's). */
struct RrMode7;
struct RrM7Samples;
void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s);

/* the frame done: shown in the window, when there is one */
void rr_present(void);
/* the last presented frame, read back: rr_target's w x h pixels into argb;
   0 when this backend keeps none */
int  rr_read(uint32_t *argb);

#ifdef __cplusplus
}
#endif
#endif
