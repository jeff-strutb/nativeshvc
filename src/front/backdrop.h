/*
 * backdrop.h: the picture behind the game's overlays while it is paused
 * (nshvc_ext_pause): a frame blurred and tinted (backdrop.c).
 */
#ifndef NSHVC_BACKDROP_H
#define NSHVC_BACKDROP_H

#include <stdint.h>

/* frame (tw x th, 0xAARRGGBB) blurred by radius_px pixels and tint
   (0xAARRGGBB, its alpha how much) laid over it, opaque, at a cell
   *cell pixels square: w x h cells, to be drawn back over the frame's
   whole size (bilinear); malloc'd, NULL when out of memory */
uint32_t *backdrop_make(const uint32_t *frame, int tw, int th, float radius_px, uint32_t tint, int *w, int *h,
                        int *cell);

#endif
