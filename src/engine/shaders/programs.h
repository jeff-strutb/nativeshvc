/* programs.h: the engine's programs on the renderer (rr.h), for C and GLSL
 * alike: their numbers, and the stencil bits the picture uses. Each
 * program's uniforms are described at its source. */
#ifndef ENGINE_PROGRAMS_H
#define ENGINE_PROGRAMS_H

#define P_FILL 0        /* rectangles of one colour */
#define P_TILES 1       /* a tile layer: an instance a tile's 8 x 8 texels */
#define P_BLOCKS 2      /* a layer under mosaic: an instance a block of one texel */
#define P_MODE7 3       /* Mode 7's plane, textured through its matrix */
#define P_SPRITES 4     /* sprites: an instance each */
#define P_IMAGE 5       /* an image as a layer */
#define P_LIST 6        /* a draw list's triangles and lines */
#define P_COMBINE 7     /* the colour maths: another target, or a colour, onto the frame */
#define P_PLANE 8       /* Mode 7's plane drawn from its map and tiles into a texture */
#define P_OVERLAY 9     /* an overlay in window pixels */

/* the stencil's bits in a screen's target */
#define SB_SPRITE 1     /* a sprite's pixel is here: those after it are hidden */
#define SB_STRIPE 2     /* a hires row's even half-column: the sub screen's */
#define SB_COLOUR 4     /* inside the colour window */
#define SB_LAYER0 8     /* inside layer n's window: SB_LAYER0 << n, BG1-4 and sprites */

#endif
