/* programs.h: the engine's programs on the renderer (rr.h), for C and GLSL
 * alike: their numbers, and the stencil bits the picture uses. Each
 * program's uniforms are described at its source. */
#ifndef ENGINE_PROGRAMS_H
#define ENGINE_PROGRAMS_H

#define P_FILL 0        /* rectangles of one colour */
#define P_TILES 1       /* a tile layer's mixed tiles (some texels 0): an instance a tile's 8 x 8 texels */
#define P_BLOCKS 2      /* a layer under mosaic: an instance a block of one texel */
#define P_MODE7 3       /* Mode 7's plane, textured through its matrix */
#define P_SPRITES 4     /* sprites: an instance each */
#define P_IMAGE 5       /* an image as a layer */
#define P_LIST 6        /* a draw list's triangles */
#define P_LINES 7       /* a draw list's lines, as quads a list pixel wide */
#define P_FIELD 8       /* rectangles of a texture copied pixel for pixel: an interlaced picture's field */
#define P_PLANE 9       /* Mode 7's plane drawn from its map and tiles into a texture */
#define P_TINT 10       /* Mode 7's plane (or its tile 0) in colour, with its smaller levels */
#define P_OVERLAY 11    /* an overlay in window pixels */
#define P_PLACED 12     /* a draw list drawn into a side target, placed where its layer is */
#define P_TILES_SOLID 13 /* a tile layer's solid tiles (no texel 0): tiles.vert, a fragment stage that rejects nothing */

/* the stencil's bits in a screen's target */
#define SB_SPRITE 1     /* a sprite's pixel is here: those after it are hidden */
#define SB_STRIPE 2     /* a hires row's even half-column: the sub screen's */
#define SB_GROUP 4      /* the rows of one state, drawn together */
#define SB_LAYER0 8     /* inside layer n's window: SB_LAYER0 << n, BG1-4 and sprites */

#endif
