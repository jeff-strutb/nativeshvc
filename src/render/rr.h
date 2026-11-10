/*
 * rr.h: the renderer, the same for every GPU API. It knows textures, render
 * targets, depth, stencil, blend state, geometry and shaders, and nothing of
 * any console. The engine fills a frame on the game thread: what changed in
 * its textures, the words its draws read, their uniforms, and passes of
 * draws. A backend runs the frame on the render thread, in order, and shows
 * it. Everything runs on the render thread but rr_open, which runs on the
 * system thread after the window opens.
 *
 * Targets. The frame target and the two extra targets are the window's size,
 * multisampled (RR_SAMPLES), with depth and stencil; each is cleared when its
 * pass begins and resolved when it ends: the frame into the output, an extra
 * target into its texture (RR_TEX_EXTRA0, RR_TEX_EXTRA1). The output is the
 * window's picture, one sample a pixel: drawn into after the frame, then
 * shown, and kept for rr_read. A texture target is one of the frame's
 * textures made with RR_TEX_TARGET, cleared to 0.
 *
 * Spaces. A draw in RR_SPACE_VIEW is in the frame's own units (width x
 * height), shown at its aspect, centred in the target and as large as fits
 * (rr_view): the viewport. RR_SPACE_TARGET covers the whole target.
 *
 * Programs. A program is a vertex and a fragment stage, compiled into the
 * backend (rr_spv.h, rr_msl.h, rr_dxil.h, from src/render/shaders). It reads
 * the draw's uniform block (binding 0), the frame's words (binding 1) and up
 * to five textures (bindings 2-6), and draws `vertices` vertices of each of
 * `instances` instances, with nothing in vertex buffers. A game's shader
 * takes the place of a program's fragment stage.
 *
 *   RR_SIZE=WxH   with no window: the target's size (default: the frame's units)
 */
#ifndef RR_H
#define RR_H

#include <stddef.h>
#include <stdint.h>

#define RR_SAMPLES 4            /* the frame's and extra targets' samples a pixel */
#define RR_PROGRAMS 11          /* programs compiled into the backend */
#define RR_SHOW 10              /* the backend's own: the output copied to the window */
#define RR_TEX_SLOTS 5          /* bindings 2, 3: colour textures; 4, 5, 6: integer textures */

/* texture formats */
enum { RR_R8U, RR_R32U, RR_BGRA8 };
#define RR_TEX_TARGET 1         /* a texture drawn into */
/* textures that are no texture of the frame's: an extra target, resolved */
#define RR_TEX_EXTRA0 (-1)
#define RR_TEX_EXTRA1 (-2)

/* pass targets */
enum { RR_TARGET_FRAME, RR_TARGET_EXTRA0, RR_TARGET_EXTRA1, RR_TARGET_TEXTURE, RR_TARGET_OUTPUT };

/* blend states: what a draw does to the colour there; a, rgb the target's, s the draw's */
enum {
    RR_BLEND_REPLACE,           /* rgba = s */
    RR_BLEND_NOCOLOUR,          /* nothing written */
    RR_BLEND_ADD_BY_A,          /* rgb += s.rgb * a */
    RR_BLEND_RSUB_BY_A,         /* rgb -= s.rgb * a */
    RR_BLEND_MIX_BY_A,          /* rgb = rgb * (1 - a) + s.rgb * a */
    RR_BLEND_RMIX_BY_A,         /* rgb = rgb * (1 - a) - s.rgb * a */
    RR_BLEND_A_ZERO,            /* a = 0 */
    RR_BLEND_RGB_ZERO_A_ADD,    /* rgb = 0; a += s.a * a */
    RR_BLEND_A_ADD,             /* a += s.a * a */
    RR_BLEND_RGB_SCALE,         /* rgb *= the draw's constant */
    RR_BLEND_OVER,              /* rgba = s + rgba * (1 - s.a): premultiplied */
    RR_BLENDS
};
/* depth: off, or tested less or equal and written */
enum { RR_DEPTH_OFF, RR_DEPTH_LEQUAL, RR_DEPTHS };
/* stencil, with the draw's masks and reference:
   EQUAL     drawn where (stencil & read_mask) == ref
   EQUAL_INC the same, and where drawn or hidden by depth, stencil + 1 under write_mask
   SET       stencil = ref under write_mask */
enum { RR_STENCIL_OFF, RR_STENCIL_EQUAL, RR_STENCIL_EQUAL_INC, RR_STENCIL_SET, RR_STENCILS };
enum { RR_TRIANGLES, RR_LINES };
enum { RR_SPACE_VIEW, RR_SPACE_TARGET };

/* a draw's uniform block: 256 bytes. f[0] is the backend's: the target's
   width, height, samples a pixel, and the sign that turns a downward y into
   the API's clip space; the rest is the program's */
typedef struct rr_uniform {
    float f[8][4];
    int32_t i[8][4];
} rr_uniform;

typedef struct rr_cmd {
    uint8_t program, blend, depth, stencil;
    uint8_t read_mask, write_mask, ref, topology;
    uint8_t space, scissor;     /* scissor: kept to the rectangle in f[2] (x0, y0, x1, y1, the space's units) */
    uint16_t shader;            /* a game's shader in place of the fragment stage; 0 none */
    float constant;             /* RR_BLEND_RGB_SCALE's */
    uint32_t uniform;           /* its block in the frame's */
    int32_t tex[RR_TEX_SLOTS];  /* the frame's textures, or RR_TEX_EXTRA*; 0 none */
    uint32_t vertices, instances;
} rr_cmd;

typedef struct rr_pass {
    uint8_t target;
    uint32_t texture;           /* RR_TARGET_TEXTURE's */
    float clear[4];             /* the frame's and extra targets' colour when cleared */
    size_t first, count;        /* its draws */
} rr_pass;

/* a texture made (or made again, its contents lost) or written: a write's
   texels are rows of w texels, tightly packed, at `at` in the upload bytes */
enum { RR_TEX_CREATE, RR_TEX_WRITE };
typedef struct rr_tex_op {
    uint32_t id;
    uint8_t op, format, flags;
    uint32_t x, y, w, h;        /* a creation's size is w x h */
    size_t at;
} rr_tex_op;

/* a game's shader made since the last frame: the backend's own compiled form */
#define RR_SHADER_LOADS 16
typedef struct { uint32_t id; const void *code; size_t size; } rr_shader_load;

typedef struct rr_frame {
    rr_tex_op *texops;
    size_t ntexops, cap_texops;
    uint8_t *upload;
    size_t upload_bytes, upload_cap;
    rr_shader_load shaders[RR_SHADER_LOADS];
    uint32_t nshaders;
    uint32_t *data;             /* binding 1's words */
    size_t data_words, data_cap;
    rr_uniform *uniforms;
    size_t nuniforms, cap_uniforms;
    rr_pass *passes;
    size_t npasses, cap_passes;
    rr_cmd *cmds;
    size_t ncmds, cap_cmds;
    int width, height;          /* RR_SPACE_VIEW's units */
    float aspect;               /* the shape they are shown at, width over height; <= 0 the whole target */
    uint64_t update;
    float between;
} rr_frame;

/* the file extension of this backend's compiled shaders */
const char *rr_shader_ext(void);
int  rr_open(void);             /* 0 when it cannot run here */
void rr_close(void);
const char *rr_name(void);
/* runs the frame and shows it; 1 if a display paced it, 0 if nothing waited */
int  rr_draw(rr_frame *f);
/* the last output, 0xFFRRGGBB, top row first; 0 when none. *w, *h its size;
   argb NULL only asks the size */
int  rr_read(uint32_t *argb, int *w, int *h);

#endif
