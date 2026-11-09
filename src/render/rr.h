/*
 * rr.h: the renderer interface, the same for every GPU API. A backend applies
 * a packet's changes to its store of tiles, maps and images, draws the
 * picture's description (picture_body.h) at the picture's own size, then at
 * the window's pixels, and shows it. Everything runs on the render thread but
 * rr_open, which runs on the system thread after the window opens.
 *
 *   RR_SIZE=WxH   with no window: the target's size (default: the picture's)
 */
#ifndef RR_H
#define RR_H

#include <stdint.h>
#include "packet.h"

/* the file extension of this backend's compiled shaders */
const char *rr_shader_ext(void);
int  rr_open(void);             /* 0 when it cannot run here */
void rr_close(void);
const char *rr_name(void);
/* draws the packet; 1 if a display paced it, 0 if nothing waited */
int  rr_draw(eng_packet *p);
/* the last picture at its own size, 0xFFRRGGBB, top row first; 0 when none.
   *w, *h its size; argb NULL only asks the size */
int  rr_read(uint32_t *argb, int *w, int *h);
/* the same for the last offscreen window pass, 0xFFRRGGBB; 0 when this backend keeps none */
int  rr_read_window(uint32_t *argb, int *w, int *h);

#endif
