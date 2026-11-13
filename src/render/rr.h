/*
 * rr.h: the renderer, the same for every GPU API. It knows textures, render
 * targets, depth, stencil, blend state, geometry and shaders, and nothing of
 * any console. The engine fills a frame on the game thread: what changed in
 * its textures, the words its draws read, their uniforms, and passes of
 * draws. A backend runs the frame on the render thread, in order, and shows
 * it. Everything runs on the render thread but rr_open, which runs on the
 * system thread after the window opens.
 *
 * Targets. The window target is the window itself: the window's size,
 * multisampled (rr_samples) with depth and stencil, cleared when its pass
 * begins and resolved straight into the window's drawable when it ends.
 * With no drawable (a window that is gone or hidden, or none: headless, but
 * for RR_DRAW) the window's pass does not run; and when the frame has no pass into the picture
 * target, no pass but a texture's does (what only the window's pass reads is
 * left out with it). A frame that has one is an interlaced picture, state
 * that builds up from one picture to the next: all its passes but the
 * window's run on every picture, drawable or not. The frame target and the two extra targets
 * are made the same way, multisampled with depth and stencil, but resolved
 * into textures of their own (RR_TEX_FRAME, RR_TEX_EXTRA0, RR_TEX_EXTRA1)
 * for a later pass to read. The picture target is the window's size, one
 * sample a pixel, and keeps what was drawn into it from one frame to the
 * next (RR_TEX_PICTURE). A side target (RR_TARGET_SIDE, the pass's `layer` of
 * RR_SIDES) is a window-sized target made the same way with the pass's own
 * `samples`, resolved into its own texture (RR_TEX_SIDE0 - layer); with
 * `samples` 1 it is not multisampled and is drawn straight into its texture.
 * It is cleared to the pass's colour, turned like the other window-sized
 * targets, and run again for a read like the extra ones. A target is made
 * when passes draw into it, and let go of after some frames in which none does.
 * A texture target is one of the frame's textures made with RR_TEX_TARGET.
 * A pass into the picture or a texture clears it to the pass's colour, or
 * keeps what it held (`load`; a target just made, and neither written nor
 * drawn into since, is cleared; a write keeps its texels, the frame's order
 * holding); a colour texture made with RR_TEX_MIPS has its smaller levels
 * made from the first when a pass into it asks (`mips`). A
 * texture made with RR_TEX_ARRAY has `layers` layers; a pass draws into one
 * of them (`layer`), and makes that layer's smaller levels. A texture just
 * made reads 0 in every texel, level and layer until it is written or drawn
 * into, on every backend; so does a binding with no texture.
 *
 * Orientation. Where the display is turned from the picture (a phone held
 * sideways), the window and every window-sized target take the display's own
 * extent and are drawn turned (the uniform block's code, below): the backend
 * is told no more than that, and turns each viewport and scissor with the
 * draws. Their pixels line up with one another, so a pass reads another's
 * target at its own pixel. rr_read gives the picture unturned.
 *
 * Reading. Nothing of a frame is kept for rr_read but the frame itself: the
 * backend keeps a reference to the last frame it ran (its owner leaves it
 * whole until a later frame has been run), and rr_read runs that frame's
 * passes again, the window's into a texture of the window's size, and reads
 * that; of a frame with a pass into the picture target only the window's pass
 * is run again (the picture is already that frame's).
 *
 * Spaces. A draw in RR_SPACE_VIEW is in the frame's own units (width x
 * height), shown at its aspect, centred in the target and as large as fits
 * (rr_view): the viewport. With no window and no RR_SIZE, the window-sized
 * targets are the units themselves and are filled; a texture target is
 * placed the same way with a window or without. RR_SPACE_TARGET covers the
 * whole target.
 *
 * Programs. A program is a vertex and a fragment stage, compiled into the
 * backend (rr_spv.h, rr_msl.h, rr_dxil.h, from src/render/shaders). It reads
 * the draw's uniform block (binding 0), the frame's words (binding 1) and up
 * to nine textures (bindings 2-10): binding 2 sampled at its texels, bindings
 * 3 and 4 with their levels (a sampler of linear, anisotropic filtering is set for a game's own
 * shader; the engine's programs read them with texelFetch at a level they choose): 3 the
 * texture repeating, 4 transparent outside it; bindings 5-7 integer
 * textures read at their texels; bindings 8, 9 and 10 arrays of layers,
 * filtered: 8 repeating, 9 and 10 transparent outside; a texture of
 * another kind than its binding's reads as nothing there. It
 * draws `vertices` vertices of each of `instances` instances, with nothing
 * in vertex buffers, as triangles. A game's shader takes the place of a
 * program's fragment stage.
 *
 *   RR_SIZE=WxH   with no window: the target's size (default: the frame's units)
 *   RR_DRAW=1     with no window: every frame's passes run as for a window, the window's
 *                 into a texture kept for it and never read or shown (to time the drawing
 *                 without a display); read once, at rr_open; a window ignores it
 */
#ifndef RR_H
#define RR_H

#include <stddef.h>
#include <stdint.h>

#define RR_SIDES 8              /* the most side targets a frame draws into */
#define RR_PROGRAMS 21          /* programs compiled into the backend */
#define RR_CLIP_PROGRAM 17      /* the engine's instanced mesh program (programs.h P_LIST_MESH): its vertex stage writes clip distances, which a Vulkan device without shaderClipDistance lacks: it is then left out and its draws skipped */
#define RR_SHOW 18              /* a texture copied a pixel for a pixel (binding 2, the target's size; i[0].y mirrored, then i[0].x quarter turns clockwise: the engine sets neither, the backend does where it gives an interlaced picture back unturned) */
#define RR_MIPS 19              /* a level made from the one before (binding 2): each texel the mean of four, rounded half up in the shader, the same on every GPU */
#define RR_MIPS_LAYER 20        /* the same for a layer of an array (binding 2, an array of one layer) */
#define RR_TEX_SLOTS 11         /* bindings 2, 3, 4: colour textures; 5, 6, 7: integer textures; 8, 9, 10: colour arrays; 11, 12: colour textures */

/* texture formats */
enum { RR_R8U, RR_R32U, RR_BGRA8 };
#define RR_TEX_TARGET 1         /* a texture drawn into */
#define RR_TEX_MIPS 2           /* with every smaller level, down to 1 x 1 (colour only) */
#define RR_TEX_ARRAY 4          /* an array of layers (colour only) */
/* textures that are no texture of the frame's: the targets, resolved (the window's own is no texture to read) */
#define RR_TEX_EXTRA0 (-1)
#define RR_TEX_EXTRA1 (-2)
#define RR_TEX_FRAME (-3)
#define RR_TEX_PICTURE (-4)
#define RR_TEX_SIDE0 (-5)       /* side target k's: RR_TEX_SIDE0 - k, k < RR_SIDES */

/* pass targets */
enum { RR_TARGET_FRAME, RR_TARGET_EXTRA0, RR_TARGET_EXTRA1, RR_TARGET_TEXTURE, RR_TARGET_PICTURE, RR_TARGET_WINDOW, RR_TARGET_SIDE };

/* blend states: what a draw does to the colour there; rgba the target's, s the draw's */
enum {
    RR_BLEND_REPLACE,           /* rgba = s */
    RR_BLEND_NOCOLOUR,          /* nothing written */
    RR_BLEND_OVER,              /* rgba = s + rgba * (1 - s.a): premultiplied */
    RR_BLENDS
};
/* depth: off; tested "nearer or equal" and written; or tested the same and not written (RR_DEPTH_KEEP). Nearer is less,
   unless the pass says `depth_greater` (below): then nearer is greater. Of equal depths the later draw wins either way */
enum { RR_DEPTH_OFF, RR_DEPTH_LEQUAL, RR_DEPTH_KEEP, RR_DEPTHS };
/* which triangles a draw leaves out, by their winding as the draw's view has them (x right, y down, in the draw's own
   units), whatever the backend's clip space and the display's turns and mirror do to it: RR_CULL_BACK those whose
   winding is not the front's, RR_CULL_FRONT those whose is */
enum { RR_CULL_NONE, RR_CULL_BACK, RR_CULL_FRONT };
/* stencil, with the draw's masks and reference:
   EQUAL     drawn where (stencil & read_mask) == ref
   EQUAL_INC the same, and where drawn or hidden by depth, stencil + 1 under write_mask
   SET       stencil = ref under write_mask */
enum { RR_STENCIL_OFF, RR_STENCIL_EQUAL, RR_STENCIL_EQUAL_INC, RR_STENCIL_SET, RR_STENCILS };
enum { RR_SPACE_VIEW, RR_SPACE_TARGET };

/* a draw's uniform block: 256 bytes. f[0] is the backend's: the target's
   width, height (unturned: see below), samples a pixel, and a code, a whole
   number: bit 0 set if the API's clip y is down (else up), bits 1-2 the
   quarter turns clockwise and bit 3 the mirroring, left to right first, that
   a window-sized target is drawn with where the display is turned from the
   picture (common.glsl oriented); the rest is the program's. A texture
   target is never turned. A window-sized target's own size is the turned
   one: width and height in f[0] are the picture's orientation's, which
   normalise the units, and the backend turns the draw's viewport and
   scissor with it. */
typedef struct rr_uniform {
    float f[8][4];
    int32_t i[8][4];
} rr_uniform;

typedef struct rr_cmd {
    uint8_t program, blend, depth, stencil;
    uint8_t read_mask, write_mask, ref;
    uint8_t space, scissor;     /* scissor: kept to the rectangle in f[2] (x0, y0, x1, y1, the space's units) */
    uint8_t cull, front_ccw;    /* RR_CULL_*, and whether the front is counter-clockwise (1) or clockwise (0) in the draw's view */
    float bias, slope;          /* depth bias, nearer when positive, whatever the depth holds: `bias` whole steps of the depth's
                                   smallest (rounded to a whole number), and `slope` times the triangle's depth slope a pixel,
                                   as a GPU's polygon offset; the backend gives it the sign that is nearer for the pass's
                                   compare. Steps are those of the format the pass's depth has: dependable only in a pass
                                   with `depth_own` (float, the same everywhere); the frame's own differ by backend (24 bits
                                   on Vulkan and Direct3D 12, float on Metal) */
    uint16_t shader;            /* a game's shader in place of the fragment stage; 0 none */
    uint32_t uniform;           /* its block in the frame's */
    int32_t tex[RR_TEX_SLOTS];  /* the frame's textures, or RR_TEX_EXTRA*; 0 none */
    uint32_t vertices, instances;
} rr_cmd;

typedef struct rr_pass {
    uint8_t target;
    uint8_t load, mips;         /* keep what the target held; then make a texture's smaller levels */
    uint8_t depth_own;          /* RR_TARGET_SIDE's: its depth is 32-bit float, the clip z the draws give kept as it is (the
                                   whole range 0 to 1), without stencil: a pass of draws that use no stencil. Only where
                                   rr_depth_own() */
    uint8_t depth_greater;      /* with depth_own: nearer is greater or equal (else less or equal) */
    float depth_clear;          /* with depth_own: the depth it is cleared to (the farthest: 1 for less, 0 for greater) */
    uint32_t texture, layer;    /* RR_TARGET_TEXTURE's, and its layer if an array; RR_TARGET_SIDE's, which of RR_SIDES */
    uint32_t samples;           /* RR_TARGET_SIDE's samples a pixel (1, 2, 4, 8, 16: at most rr_samples_at_most's) */
    float clear[4];             /* the colour it is cleared to */
    size_t first, count;        /* its draws */
} rr_pass;

/* a texture made (or made again, its contents lost) or written: a write's
   texels are rows of w texels, tightly packed, at `at` in the upload bytes */
enum { RR_TEX_CREATE, RR_TEX_WRITE };
typedef struct rr_tex_op {
    uint32_t id;
    uint8_t op, format, flags;
    uint32_t x, y, w, h;        /* a creation's size is w x h */
    uint32_t layers;            /* an array's, at its creation */
    size_t at;
} rr_tex_op;

/* a game's shader made since the last frame: the backend's own compiled form;
   as many in a frame as the game loads (all of its init is one frame) */
typedef struct { uint32_t id; const void *code; size_t size; } rr_shader_load;

typedef struct rr_frame {
    rr_tex_op *texops;
    size_t ntexops, cap_texops;
    uint8_t *upload;
    size_t upload_bytes, upload_cap;
    rr_shader_load *shaders;
    uint32_t nshaders;
    size_t cap_shaders;
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

/* the samples a pixel of the window, frame and extra targets have: 8 where the
   device has them for the formats the screens use, else 4; fixed when the
   renderer opens (after it, any thread) */
int  rr_samples(void);
/* the largest count of samples a pixel the device supports (for those formats)
   that is not above n; 1 always is */
int  rr_samples_at_most(int n);
/* whether a side target can have a depth of its own (rr_pass.depth_own): a 32-bit float depth, no stencil. Metal and
   Direct3D 12 always (their frame depth is, or must be able to be, that format); a Vulkan device may lack it. Fixed when the
   renderer opens */
int  rr_depth_own(void);
/* the file extension of this backend's compiled shaders */
const char *rr_shader_ext(void);
int  rr_open(void);             /* 0 when it cannot run here */
void rr_close(void);
const char *rr_name(void);
/* runs the frame and shows it; 1 if a display paced it, 0 if nothing waited */
int  rr_draw(rr_frame *f);
/* the window went or came between frames (host_window_watch): anything kept
   on a window that went let go now, not at the next frame */
void rr_window(void);
/* the last frame run, as the window shows it (or would), 0xFFRRGGBB, top row
   first; 0 when none. *w, *h its size; argb NULL only asks the size. It runs
   the frame's passes again: nothing is kept on a frame nobody reads */
int  rr_read(uint32_t *argb, int *w, int *h);

#endif
