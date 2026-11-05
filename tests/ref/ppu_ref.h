/*
 * ppu_ref.h: a frame drawn on the CPU from its record (ppu_ref.c), the
 * reference the GPU compositors are held to, for the checks only.
 */
#ifndef NSHVC_PPU_REF_H
#define NSHVC_PPU_REF_H

#include <stddef.h>
#include <stdint.h>

#include "ppu.h"

/* a frame drawn from its record: ppu_frame_width(f) x f->screen_height
   pixels, 0xFFRRGGBB: f->width, twice that when a line is hires */
void ppu_draw_frame(const PpuFrame *f, uint32_t *out);
/* the last finished frame, drawn from its record when first asked for; an
   interlaced one woven with the frame before (twice f->screen_height rows,
   as wide as the wider of the two); before the first frame, a zeroed
   256 x 224 */
const uint32_t *ppu_picture(int *width, int *height);

/* The presentation of a frame with Mode 7 or a layer's canvas
   (render/rr_mode7.h): m set to frame f's record for rr_mode7, pic its
   picture (ppu_draw_frame's, one row a line: not an interlaced weave); valid
   until the next call. 0 when there is no memory for it. */
struct RrMode7;
int ppu_mode7(const PpuFrame *f, const uint32_t *pic, struct RrMode7 *m);
/* frame f presented at ow x oh pixels into out (0xFFRRGGBB): rr_ref_mode7 of
   ppu_mode7's record, as the window shows it. Every output pixel is pic
   scaled with sharp bilinear exactly as rr_ref_image scales it, but that a
   picture pixel on a Mode 7 line is composed again by the console's rules
   for that pixel with the Mode 7 plane under the output pixel, filtered by
   the pixel's footprint on the plane (its texels' edges covered where
   magnified, supersampled where minified; a Mode 7 layer under mosaic keeps
   its block's texel), and a picture pixel of a layer's canvas composed again
   with the canvas's texel under the output pixel's centre. */
void ppu_present(const PpuFrame *f, const uint32_t *pic, uint32_t *out, int ow, int oh);

/* line of frame f drawn as it is recorded, from the live VRAM and sprite
   table (ppu.h ppu_line_hook), 2 f->width pixels onto its row of
   ppu_ref_lines() */
void ppu_ref_line(const PpuFrame *f, unsigned line, const uint16_t *live_vram, const uint8_t *live_table);
const uint32_t *ppu_ref_lines(void);

#endif
