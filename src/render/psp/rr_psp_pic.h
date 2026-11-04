/* rr_psp_pic.h: the console's picture on the GE (rr_psp_pic.c). */
#ifndef PORT_RR_PSP_PIC_H
#define PORT_RR_PSP_PIC_H

#include <stddef.h>
#include <stdint.h>

#include "host_psp.h"

/* the picture of a frame's description drawn into VRAM, pw x ph, its rows
   rr_psp_picture_stride pixels apart, 16 bits a pixel (the GE's 5551: red
   the low 5 bits); 0 when the frame has what is not drawn on the GE (the
   caller draws it). A picture pixel's channel is a 5-bit value v made
   8 bits as (v << 3 | v >> 2), which is what the GE makes of 5 bits read
   (and 5 bits are what it keeps of 8 written): so 16 bits hold it exactly,
   and a picture 512 wide fits where one of 32 bits 256 wide would */
int rr_psp_picture(const uint32_t *desc, size_t words, int *pw, int *ph);
/* the picture, uncached */
const uint16_t *rr_psp_picture_pixels(void);
#define rr_psp_picture_stride 512
/* a picture pixel as 0xFFRRGGBB */
static inline uint32_t rr_psp_picture_argb(uint16_t p)
{
    uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
/* VRAM past the screen's buffers: the picture (512 x 240, 16 bits), then
   what the picture's drawing uses only while drawing it (offsets from
   VRAM's start) */
#define RR_PSP_V_PICTURE HOST_PSP_VRAM_USED
#define RR_PSP_V_SCRATCH (RR_PSP_V_PICTURE + 512 * 240 * 2)
#define RR_PSP_V_END (2 * 1024 * 1024)

#endif
