/* picture.h: what picture.c (the game's picture calls and its objects) and
 * compose.c (the picture's capabilities as the renderer's draws) share. */
#ifndef ENGINE_PICTURE_H
#define ENGINE_PICTURE_H

#include <stdint.h>
#include "nshvc/nshvc.h"
#include "rr.h"

#define ENG_OBJECTS 4096
#define ENG_RGB 0x1000000u      /* a palette entry past 15 bits: ENG_RGB | 0xRRGGBB */

/* objects, each a texture */
typedef struct { uint32_t tex, count, bpp, tpp, cols, gen; } eng_tiles;   /* R8U: `cols` tiles a row, 8 * tpp texels a side */
typedef struct { uint32_t tex, w, h, gen; } eng_map;                      /* R32U: tile | palette << 16 | priority << 19 | hflip << 20 | vflip << 21 */
typedef struct { uint32_t tex, w, h, format; } eng_image;                 /* BGRA8, or R8U indexed */
extern eng_tiles eng_tiles_of[ENG_OBJECTS];
extern eng_map eng_maps_of[ENG_OBJECTS];
extern eng_image eng_images_of[ENG_OBJECTS];
extern uint32_t eng_ntiles, eng_nmaps, eng_nimages;

/* a draw list: filled by the game or by a job given it */
typedef struct {
    uint32_t first, count, lines;
    uint32_t image;             /* RGBA, or 0 */
    uint32_t shader;
    float inputs[8];
} eng_list_cmd;
typedef struct {
    uint32_t w, h, rule;
    nshvc_vertex *v;
    size_t nv, cap_v;
    eng_list_cmd *cmd;
    size_t nc, cap_c;
} eng_list;
#define ENG_LISTS 256
extern eng_list eng_lists_of[ENG_LISTS];
extern uint32_t eng_nlists;

/* the picture's settings as they stand on a row, as line changes make them */
typedef struct {
    int m, b3, hires, bright, blank, sprites_main, sprites_sub;
    nshvc_layer l[4];
    nshvc_mode7 m7;
    nshvc_windows w;
    nshvc_blend b;
    int anchor[4];              /* the line each layer's mosaic started at */
    int pal;                    /* the palette's version */
} eng_state;

typedef struct { int x0; uint32_t state; } eng_part;   /* from screen column x0 on */
typedef struct { uint32_t first, count; } eng_row;

typedef struct { uint32_t image; float corners[6]; float alpha; } eng_overlay;

/* the picture as draw left it */
typedef struct {
    int w, h, ox, oy, lines, interlace;
    int show_l, show_r, show_t, show_b;
    nshvc_limits limits;
    const eng_row *rows;        /* h of them */
    const eng_part *parts;
    const eng_state *states;
    const uint32_t *palettes;   /* 256 0xFFRRGGBB a version */
    int npalettes;
    uint32_t palette_tex;
    const nshvc_sprite *sprites;
    uint32_t nsprites, sprite_first;
    const eng_overlay *overlays;
} eng_picture;

/* a texture's id, never used before */
uint32_t eng_texture_id(void);
/* records a texture's creation, or a write of its texels (returned: w * h *
   bytes a texel to fill) in the frame being filled */
void eng_texture_create(rr_frame *f, uint32_t id, uint32_t w, uint32_t h, int format, int flags);
uint8_t *eng_texture_write(rr_frame *f, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int bytes);

/* the game thread: the frame being filled */
void eng_picture_init(void);
void eng_picture_begin(void);           /* draw is about to run */
rr_frame *eng_picture_end(void);        /* draw has returned: the finished frame */

/* the picture's draws, into f (compose.c) */
void eng_compose(rr_frame *f, const eng_picture *p);

#endif
