/*
 * rr_psp_pic.c: the console's picture on the PSP's GE, from a frame's
 * description (ppu.c ppu_compose), by compose_body.h's rules: what the CPU
 * decides (map entries, windows, depths, palettes) is the rules' own C
 * build (rr_compose.h), what each pixel becomes is the GE's.
 *
 *   tiles     each VRAM copy's tiles as the GE's indexed textures (2 and 4
 *             bits a pixel as 4-bit indices, 8 as 8-bit), decoded again
 *             only where the copy changed
 *   colours   the palette times 4 (each channel's 5 bits, the low 2 clear),
 *             a tile's colour 0 0xFFFFFF, which the colour test drops
 *   screens   in stripes of rows: the main screen, then the sub, a band of
 *             rows with one state at a time: the backdrop (the sub's the
 *             fixed colour), each background's tiles at their depth for
 *             their priority (cp_zbg), the windows scissored spans; the
 *             sprites drawn a row at a time into a buffer of their own in
 *             reverse order (the first on top), then onto the screens at
 *             their priority's depth (cp_zobj)
 *   maths     the main screen's stencil marks colour maths (its layer's
 *             bit), the colour window's clip and prevent regions, and
 *             halving; the sub screen (its backdrop the fixed colour) or the
 *             fixed colour added or subtracted by factors 255, which pass a
 *             value through exactly; halving a blend by 128 (4k to 2k
 *             exactly for every k to 62)
 *   picture   each channel through a palette lookup: v >> 2 to at most 31,
 *             brightness, 5 bits to 8 (ppu.c finish), added into the
 *             picture (0 + x by factors 255: exact)
 *
 * Every step is exact, so the picture is ppu_draw_frame's, pixel for pixel.
 * A frame with what is not drawn here (hires, Mode 7, offset-per-tile,
 * mosaic, a canvas, the screen extensions) is left to the CPU (0).
 */
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "nshvc/nshvc.h"
#include "rr_compose.h"
#include "rr_size.h"
#include "rr_psp_pic.h"
#include "rr_mode7.h"
#include "host_psp.h"

/* compose_body.h's own words, for the rules' state (rr_compose.h leaves
   them undefined) */
#define U32 uint32_t
#define I32 int32_t
#define U_(x) ((uint32_t)(x))
#define I_(x) ((int32_t)(x))
#define FD_(i) rr_cp_fd[cp_fdb + (i)]

#define PIC_W 256
#define PIC_H 240
#define BAND 32                              /* rows a stripe */
/* VRAM past the screen's buffers: the picture, then a stripe's main and sub
   screens, sprites and depth */
#define V_PIC RR_PSP_V_PICTURE
#define V_MAIN RR_PSP_V_SCRATCH
#define V_SUB (V_MAIN + (unsigned)(s_pw * s_band * 4))
#define V_OBJ (V_SUB + (unsigned)(s_pw * s_band * 4))
#define V_Z (V_OBJ + (unsigned)(s_pw * s_band * 4))
#define V_SUB2 (V_Z + (unsigned)(s_pw * s_band * 4))   /* a hires line's sub screen as its even pixels' own */
/* a stripe's buffers' row (pixels) and rows: PIC_W and BAND; a canvas
   frame wider (to 512 pixels), 512 and CV_BAND: its main and sub screens,
   sprites and depth (16 bits a pixel, the last: no hires line's second
   sub screen after it) in the VRAM past the picture */
#define CV_BAND 24
static int s_pw = PIC_W, s_band = BAND;
/* a stripe buffer's rows as a texture's height: the GE takes a texture's
   size as a power of 2 (sceGuTexImage's log2), so the one at s_band or
   above (24 read as 16 would clamp rows 16-23 to row 15) */
#define BAND_TEX (s_band > 16 ? 32 : 16)

/* the main screen's stencil */
#define F_MATH 0x01
#define F_CLIP 0x02
#define F_PREVENT 0x04
#define F_HALF 0x08
#define F_BACK 0x80                          /* the sub screen's: its backdrop */
#define F_W 0x10                             /* a canvas frame's: the canvas, opaque, would be on top here */
#define F_DIR 0x20                           /* a full-colour canvas's: its texel a colour, on top of the main screen */
#define F_HIDE 0x40                          /* a screen's, while a layer is drawn on a band whose rows' windows
                                                differ: the layer's window hides it here (hide_mask) */

#define CLEAR 0x00FFFFFFu                    /* a tile's colour 0 */

static unsigned int __attribute__((aligned(64))) s_list[256 * 1024];
#define LIST_ROOM (64 * 1024)                /* bytes kept free: far more than any one step adds (a bucket's
                                                tiles: 256 sprites, 5 KB) */

/* the picture drawn into: its first byte past V_PIC, its rows' distance
   (pixels); the rows' distance its readers see (a woven picture's 256) */
int rr_psp_picture_stride = 512;
static unsigned s_pic_off;
static int s_pic_stride = 512;

static unsigned s_target_off;                /* the buffer drawn into (target()) */
static int s_target_w, s_target_psm;

/* the buffer drawn into: a stripe's (32 bits a pixel), or the picture (16) */
static void target(unsigned off, int w)
{
    s_target_off = off;
    s_target_w = w;
    s_target_psm = off >= V_PIC && off < V_PIC + 512 * 240 * 2 ? GU_PSM_5551 : GU_PSM_8888;
    sceGuDrawBufferList(s_target_psm, (void *)off, w);
}

/* the list run and begun again before it could fill: the GE's state
   carries over (its registers are its own) but the buffer drawn into,
   which sceGuStart sets again to the GU's own (rr_psp's screen) */
/* The picture's list is a call list run by a list of its own (list_run):
   a direct list asks the system for each draw (its stall address), a call
   list for none */
static unsigned int __attribute__((aligned(64))) s_run[64];

static void list_begin(void) { sceGuStart(GU_CALL, s_list); }

/* the list so far run to its end (the GE done with it) */
static void list_run(void)
{
    int bytes = sceGuFinish();
    sceKernelDcacheWritebackRange(s_list, (unsigned)(bytes + 64));
    sceGuStart(GU_DIRECT, s_run);
    sceGuCallList(s_list);
    sceGuFinish();
    sceGuSync(0, 0);
}

/* the buffer drawn into set again as it was (sceGuStart's own set) */
static void retarget(void) { sceGuDrawBufferList(s_target_psm, (void *)s_target_off, s_target_w); }

static int s_prebuild;                       /* a list made to keep (rr_psp_canvas's): its room its own */

static void room(void)
{
    if (s_prebuild)
        return;
    if (sceGuCheckList() > (int)sizeof s_list - LIST_ROOM) {
        list_run();
        list_begin();
        retarget();
    }
}

static void *vram(unsigned off) { return (char *)sceGeEdramGetAddr() + off; }

/* ---- tiles ---- */
typedef struct {
    int used;
    uint32_t key;                            /* the copy's first word and page */
    uint32_t frame;                          /* the frame it was last brought up to date in */
    uint64_t gen;                            /* its copy's count of VRAM writes (CP_VGEN) then */
    uint16_t shadow[0x8000];                 /* the words its textures were decoded from */
    uint8_t *tex[5];                         /* 2, 4, 8 bits: 4096, 2048, 1024 tiles; then 2 and 4 bits
                                                split, each row's texels 0, 2, 4, 6, 1, 3, 5, 7 (a hires
                                                layer's screens each take half: tex_kind 3, 4) */
    uint8_t dirty[4096];                     /* this frame: the 8-word runs that changed */
} Atlas;

#define ATLASES 8
static Atlas *s_atlas[ATLASES];
static uint32_t s_frame;

static const int k_tiles[5] = { 4096, 2048, 1024, 4096, 2048 };

/* tile t of a kind (0: 2 bits, 1: 4, 2: 8; 3, 4: 2 and 4 split): its
   texture image (1,024 tiles each, 16 across), and its texel there */
static uint8_t *tile_image(Atlas *a, int kind, int t) { return a->tex[kind] + (size_t)(t >> 10) * (kind == 2 ? 128 * 512 : 64 * 512); }
static size_t kind_bytes(int kind) { return kind == 2 ? 128 * 512 : (size_t)64 * 512 * (size_t)(4 >> (kind % 3)); }
static int tile_u(int t) { return (t & 15) * 8; }
static int tile_v(int t) { return ((t >> 4) & 63) * 8; }

static void decode_tile(Atlas *a, int kind, int t)
{
    const uint16_t *w = a->shadow;
    static const uint8_t split[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    uint8_t *img = tile_image(a, kind, t);
    int bpp = 2 << (kind % 3), y, x, p, words = bpp * 4, u = tile_u(t), v = tile_v(t), sp = kind >= 3;
    unsigned at = (unsigned)t * (unsigned)words;
    for (y = 0; y < 8; y++) {
        uint8_t px[8];
        for (x = 0; x < 8; x++) {
            unsigned c = 0, bit = 7u - (unsigned)x;
            for (p = 0; p < bpp; p += 2) {
                unsigned ww = w[(at + (unsigned)p * 4u + (unsigned)y) & 0x7FFFu];
                c |= ((ww >> bit) & 1u) << p;
                c |= ((ww >> (bit + 8u)) & 1u) << (p + 1);
            }
            px[x] = (uint8_t)c;
        }
        if (sp) {
            uint8_t q[8];
            for (x = 0; x < 8; x++)
                q[x] = px[split[x]];
            memcpy(px, q, 8);
        }
        if (kind == 2) {
            memcpy(img + (size_t)(v + y) * 128 + (size_t)u, px, 8);
        } else {
            uint8_t *o = img + (size_t)(v + y) * 64 + (size_t)u / 2;
            for (x = 0; x < 8; x += 2)
                o[x / 2] = (uint8_t)(px[x] | px[x + 1] << 4);
        }
    }
}

/* the atlas for VRAM copy word `base` (a page's first word in the
   description's copies), up to date for this frame with `kind` decoded */
static Atlas *atlas(const uint16_t *words, uint32_t base, int kind)
{
    Atlas *a = NULL;
    int i, t, oldest = 0;
    for (i = 0; i < ATLASES; i++)
        if (s_atlas[i] && s_atlas[i]->used && s_atlas[i]->key == base) {
            a = s_atlas[i];
            break;
        }
    /* else an empty slot, or the one used longest ago (never one of this
       frame's: drawable() counted them) */
    for (i = 0; !a && i < ATLASES; i++) {
        if (!s_atlas[i] || !s_atlas[i]->used) {
            oldest = i;
            break;
        }
        if (s_atlas[i]->frame < s_atlas[oldest]->frame)
            oldest = i;
    }
    if (!a) {
        if (!s_atlas[oldest]) {
            s_atlas[oldest] = (Atlas *)memalign(64, sizeof(Atlas));
            if (!s_atlas[oldest])
                nshvc_fatal("no memory for the tiles");
            memset(s_atlas[oldest], 0, sizeof(Atlas));
        }
        a = s_atlas[oldest];
        for (i = 0; i < 5; i++) {
            free(a->tex[i]);
            a->tex[i] = NULL;
        }
        a->used = 1;
        a->key = base;
        a->frame = 0;
    }
    if (a->frame != s_frame) {
        /* this frame's words against those decoded: the runs that changed,
           decoded again in each kind already made; none when its copy's
           count of VRAM writes is the one they were last checked against
           (the same writes, the same words) */
        const uint16_t *src = words + base;
        U32 v = base / FD_(CP_VCOPY);
        uint64_t gen = FD_(CP_VGEN + 2u * v) | (uint64_t)FD_(CP_VGEN + 2u * v + 1u) << 32;
        int any = 0;
        if (a->frame && gen == a->gen)
            ;
        else if (memcmp(src, a->shadow, sizeof a->shadow) != 0)
            for (i = 0; i < 4096; i++) {
                a->dirty[i] = memcmp(src + i * 8, a->shadow + i * 8, 16) != 0;
                any |= a->dirty[i];
            }
        if (any) {
            memcpy(a->shadow, src, sizeof a->shadow);
            for (i = 0; i < 5; i++) {
                int words_a = 8 << (i % 3);  /* 8, 16, 32 words a tile */
                if (!a->tex[i])
                    continue;
                for (t = 0; t < k_tiles[i]; t++) {
                    int r, d = 0;
                    for (r = 0; r < words_a / 8; r++)
                        d |= a->dirty[t * (words_a / 8) + r];
                    if (d)
                        decode_tile(a, i, t);
                }
                sceKernelDcacheWritebackRange(a->tex[i], (unsigned)kind_bytes(i));
            }
        }
        a->frame = s_frame;
        a->gen = gen;
    }
    if (!a->tex[kind]) {
        size_t bytes = kind_bytes(kind);
        a->tex[kind] = (uint8_t *)memalign(64, bytes);
        if (!a->tex[kind])
            nshvc_fatal("no memory for the tiles");
        for (t = 0; t < k_tiles[kind]; t++)
            decode_tile(a, kind, t);
        sceKernelDcacheWritebackRange(a->tex[kind], (unsigned)bytes);
    }
    return a;
}

/* ---- colours ---- */
/* a state's palette as the GE's: times 4, each palette's colour 0 CLEAR
   for 2 bits (every 4th), 4 (every 16th) and 8 (the first); and direct
   colour's 8 palettes */
typedef struct {
    uint32_t key, frame;
    uint32_t __attribute__((aligned(64))) c[3][256];
    uint32_t __attribute__((aligned(64))) direct[8][256];
    uint32_t __attribute__((aligned(64))) ext[2][256];  /* Mode 7's BG2 (EXTBG): its bytes of priority 0, 1 */
    uint32_t __attribute__((aligned(64))) f[256];       /* every colour, 0 too: an index band's (s_index) */
    int have_c, have_direct, have_ext;
} Clut;

/* An index band (rows whose states differ in their palettes alone, none in
   direct colour): its screens drawn in palette indices (an index in a
   pixel's red byte, s_iclut's), then each row's made its own palette's
   colours (index_band) */
static int s_index;
static Clut s_iclut;
/* a band whose rows' fixed colours differ (theirs alone, or with their
   palettes): the sub screen's backdrop and the maths' fixed colour each
   row's own */
static int s_fx;

#define CLUTS 16
static Clut *s_clut[CLUTS];

/* a 15-bit colour as the GE's, each channel times 4 (from s_t4: a load for
   a palette's 256, made with the lookups) */
static uint32_t s_t4[32768];
static uint32_t times4_of(unsigned c) { return 0xFF000000u | (c >> 10 & 31u) << 18 | (c >> 5 & 31u) << 10 | (c & 31u) << 2; }
static uint32_t times4(unsigned c) { return s_t4[c & 0x7FFFu]; }

/* A palette's 256 colours (times4) for the GE, each palette of the frame
   made once (an index band's rows' and the canvas's: a palette changed
   line by line has one a row): PALS at most a frame, in a table by the
   palette's word, the GE run to its end before more */
#define PALS 256
static uint32_t __attribute__((aligned(64))) s_palf[PALS][256];
static uint32_t s_palk[PALS], s_palfr[PALS], s_paln, s_palnf;

static const uint32_t *pal_f(uint32_t key)
{
    uint32_t at = cp_fdb + key, h, k;
    if (s_palnf != s_frame) {
        s_palnf = s_frame;
        s_paln = 0;
    }
    for (h = (at * 2654435761u) >> 24;; h = (h + 1u) & (PALS - 1u)) {
        if (s_palfr[h] != s_frame)
            break;
        if (s_palk[h] == at)
            return s_palf[h];
    }
    if (s_paln == PALS * 3 / 4) {
        /* (the table this frame's, three quarters full: the list run, the
           GE having read them, and begun again) */
        list_run();
        list_begin();
        retarget();
        for (k = 0; k < PALS; k++)
            s_palfr[k] = 0;
        s_paln = 0;
        h = (at * 2654435761u) >> 24;
    }
    for (k = 0; k < 256; k++)
        s_palf[h][k] = times4((FD_(key + (k >> 1)) >> (16u * (k & 1u))) & 0xFFFFu);
    sceKernelDcacheWritebackRange(s_palf[h], sizeof s_palf[h]);
    s_palk[h] = at;
    s_palfr[h] = s_frame;
    s_paln++;
    return s_palf[h];
}

/* (direct: 1 direct colour's tables too, -1 Mode 7 BG2's (ext), 2 the full
   table alone: an index band's rows; each table made when first asked) */
static Clut *clut(uint32_t key, int direct)
{
    Clut *c = NULL;
    int i, k;
    uint32_t at = cp_fdb + key;                /* (the palette's word in all the descriptions) */
    for (i = 0; i < CLUTS; i++)
        if (s_clut[i] && s_clut[i]->frame == s_frame && s_clut[i]->key == at) {
            c = s_clut[i];
            break;
        }
    if (!c) {
        for (i = 0; i < CLUTS && s_clut[i] && s_clut[i]->frame == s_frame; i++)
            ;
        if (i == CLUTS) {
            /* every slot this frame's (a palette changed line by line): the
               list run to its end, the GE having read the palettes, and
               begun again (room()'s), the slots free */
            list_run();
            list_begin();
            retarget();
            for (i = 0; i < CLUTS; i++)
                s_clut[i]->frame = 0;
            i = 0;
        }
        if (!s_clut[i] && !(s_clut[i] = (Clut *)memalign(64, sizeof(Clut))))
            nshvc_fatal("no memory for the palettes");
        c = s_clut[i];
        c->key = at;
        c->frame = s_frame;
        c->have_c = c->have_direct = c->have_ext = 0;
        for (k = 0; k < 256; k++)
            c->f[k] = times4((FD_(key + ((unsigned)k >> 1)) >> (16u * ((unsigned)k & 1u))) & 0xFFFFu);
        sceKernelDcacheWritebackRange(c->f, sizeof c->f);
    }
    if (direct != 2 && !c->have_c) {
        for (k = 0; k < 256; k++) {
            uint32_t v = c->f[k];
            c->c[0][k] = (k & 3) ? v : CLEAR;
            c->c[1][k] = (k & 15) ? v : CLEAR;
            c->c[2][k] = k ? v : CLEAR;
        }
        sceKernelDcacheWritebackRange(c, sizeof c->c);
        c->have_c = 1;
    }
    if (direct < 0 && !c->have_ext) {
        /* a byte's low 7 bits its colour, the high its priority */
        for (k = 0; k < 256; k++) {
            c->ext[0][k] = k < 128 ? c->c[2][k] : CLEAR;
            c->ext[1][k] = k >= 128 ? c->c[2][k & 127] : CLEAR;
        }
        sceKernelDcacheWritebackRange(c->ext, sizeof c->ext);
        c->have_ext = 1;
    }
    if (direct == 1 && !c->have_direct) {
        int p;
        for (p = 0; p < 8; p++)
            for (k = 0; k < 256; k++)
                c->direct[p][k] = k ? times4(cp_direct((unsigned)k, (unsigned)p)) : CLEAR;
        sceKernelDcacheWritebackRange(c->direct, sizeof c->direct);
        c->have_direct = 1;
    }
    return c;
}

/* the picture's lookups: channel k's (its byte) of a value 4 v at
   brightness b */
static uint32_t __attribute__((aligned(64))) s_to8[16][3][256];
/* the parity of a hires line's 512 pixels: 1 the odd (the alpha the
   picture's stencil) */
static uint32_t __attribute__((aligned(16))) s_parity[4] = { 0, 0xFF000000u, 0, 0xFF000000u };
static int s_to8_made;

static void make_to8(void)
{
    int b, k, n;
    for (b = 0; b < 16; b++)
        for (n = 0; n < 256; n++) {
            unsigned v = (unsigned)n >> 2;
            if (v > 31)
                v = 31;
            if (b != 15)
                v = (v * (unsigned)b + 7u) / 15u;
            v = v << 3 | v >> 2;
            for (k = 0; k < 3; k++)
                s_to8[b][k][n] = v << (8 * k);
        }
    sceKernelDcacheWritebackRange(s_to8, sizeof s_to8);
    sceKernelDcacheWritebackRange(s_parity, sizeof s_parity);
    for (n = 0; n < 32768; n++)
        s_t4[n] = times4_of((unsigned)n);
    for (n = 0; n < 256; n++) {
        uint32_t v = 0xFF000000u | (uint32_t)n;
        s_iclut.c[0][n] = (n & 3) ? v : CLEAR;
        s_iclut.c[1][n] = (n & 15) ? v : CLEAR;
        s_iclut.c[2][n] = n ? v : CLEAR;
        s_iclut.ext[0][n] = n < 128 ? s_iclut.c[2][n] : CLEAR;
        s_iclut.ext[1][n] = n >= 128 ? s_iclut.c[2][n & 127] : CLEAR;
    }
    sceKernelDcacheWritebackRange(&s_iclut, sizeof s_iclut);
    s_to8_made = 1;
}

/* ---- drawing ---- */
typedef struct {
    short u, v, x, y, z;
} TV;

typedef struct {
    uint32_t c;
    short x, y, z;
} CV;

static int s_y0;                             /* the stripe's first row */

static void scissor(int x0, int y0, int x1, int y1) { sceGuScissor(x0, y0, x1 - x0, y1 - y0); }

static void rect(int x0, int y0, int x1, int y1, uint32_t c)
{
    CV *v = (CV *)sceGuGetMemory(2 * sizeof(CV));
    v[0].c = c;
    v[0].x = (short)x0;
    v[0].y = (short)y0;
    v[0].z = 0;
    v[1].c = c;
    v[1].x = (short)x1;
    v[1].y = (short)y1;
    v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

/* a rectangle at depth z, of no colour (its stencil alone, where the
   depth test passes) */
static void zrect(int x0, int y0, int x1, int y1, uint32_t z)
{
    CV *v = (CV *)sceGuGetMemory(2 * sizeof(CV));
    v[0].c = v[1].c = 0;
    v[0].x = (short)x0;
    v[0].y = (short)y0;
    v[1].x = (short)x1;
    v[1].y = (short)y1;
    v[0].z = v[1].z = (short)z;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

/* a stripe buffer (main, sub or sprites) drawn over itself's rows y0-y1,
   columns x0-x1 as a texture */
static void stripe_tex(unsigned off, int x0, int y0, int x1, int y1)
{
    TV *v = (TV *)sceGuGetMemory(2 * sizeof(TV));
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, s_pw, BAND_TEX, s_pw, vram(off));
    sceGuTexFlush();
    v[0].u = (short)x0;
    v[0].v = (short)y0;
    v[0].x = (short)x0;
    v[0].y = (short)y0;
    v[0].z = 0;
    v[1].u = (short)x1;
    v[1].v = (short)y1;
    v[1].x = (short)x1;
    v[1].y = (short)y1;
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

/* a sprite of w x h texels from (u, v), flipped as asked, at (x, y), at
   depth z: the part at negative columns or rows left off, its texels with
   it (the GE steps a flipped sprite's texels down from the far edge's) */
static void put(TV *o, int x, int y, int w, int h, int u, int v, int hf, int vf, int z)
{
    int cx = x < 0 ? -x : 0, cy = y < 0 ? -y : 0;
    o[0].u = (short)(hf ? u + w - cx : u + cx);
    o[0].v = (short)(vf ? v + h - cy : v + cy);
    o[0].x = (short)(x + cx);
    o[0].y = (short)(y + cy);
    o[0].z = (short)z;
    o[1].u = (short)(hf ? u : u + w);
    o[1].v = (short)(vf ? v : v + h);
    o[1].x = (short)(x + w);
    o[1].y = (short)(y + h);
    o[1].z = (short)z;
}

/* the spans of columns x0-x1 (the picture's) where `keep(x)` holds: keep
   a function of the loaded state's windows (cp_win), so the same between
   the columns where either window's test can change (cp_win_in: its WH
   edges and the clamps at the console's columns 0 and 256, the map's
   edges, the columns'), and taken once each piece */
typedef int (*SpanFn)(uint32_t si, int n, int x);
static int spans(uint32_t si, int n, int x0, int x1, SpanFn keep, int out[][2])
{
    int p[16], np = 0, i, j, k = 0, on = 0, sl = (int)FD_(CP_SL), w;
    U32 f = cp_b(si, U_(CP_S_FIXED), 3u);
    for (w = 0; w < 2; w++) {
        int l = (int)cp_b(si, U_(CP_S_WH), 2u * (U32)w), r = (int)cp_b(si, U_(CP_S_WH), 2u * (U32)w + 1u);
        p[np++] = sl + l;
        p[np++] = sl + r + 1;
        if ((f >> (2u + (U32)w)) & 1u) {
            p[np++] = (int)cp_h(si, U_(CP_S_WCOL) + (U32)w, 0u);
            p[np++] = (int)cp_h(si, U_(CP_S_WCOL) + (U32)w, 1u) + 1;
        }
        if (f & 2u) {
            p[np++] = (int)cp_wmap(si, l);
            p[np++] = (int)cp_wmap(si, r + 1);
        }
    }
    p[np++] = sl;
    p[np++] = sl + 256;
    /* the pieces' starts within x0-x1, in order */
    for (i = 1; i < np; i++)
        for (j = i; j > 0 && p[j - 1] > p[j]; j--) {
            int t = p[j];
            p[j] = p[j - 1];
            p[j - 1] = t;
        }
    for (i = -1; i < np; i++) {
        int a = i < 0 ? x0 : p[i], v;
        if (a < x0 || a >= x1 || (i >= 0 && a == x0) || (i > 0 && p[i - 1] == a))
            continue;
        v = keep(si, n, a);
        if (v && !on) {
            out[k][0] = a;
            on = 1;
        } else if (!v && on) {
            out[k++][1] = a;
            on = 0;
        }
    }
    if (on)
        out[k++][1] = x1;
    return k;
}

static int s_which;                          /* the screen spans are for: 0 main, 1 sub */

/* layer n shows at column x of the screen s_which (cp_put's windows) */
static int shows(uint32_t si, int n, int x)
{
    U32 tmw = cp_b(si, U_(CP_S_TM), 3u), tsw = cp_b(si, U_(CP_S_TSW), 0u), wn = s_which ? tsw : tmw;
    if (!(((tmw | tsw) >> n) & 1u) || !((wn >> n) & 1u))
        return 1;
    return !cp_win(si, cp_sel(si, (U32)n), cp_logic(si, (U32)n), x - I_(FD_(CP_SL)));
}

static int region_of(uint32_t si, int which, int x)
{
    U32 cgwsel = cp_b(si, U_(CP_S_TSW), 1u), cw = cp_win(si, cp_sel(si, 5u), cp_logic(si, 5u), x - I_(FD_(CP_SL)));
    return (int)cp_region(cgwsel >> (which ? 4 : 6), cw);
}

static int clipped(uint32_t si, int n, int x) { (void)n; return region_of(si, 0, x); }
static int prevented(uint32_t si, int n, int x) { (void)n; return region_of(si, 1, x); }
static int hidden(uint32_t si, int n, int x) { return !shows(si, n, x); }

/* A band whose rows differ in their windows alone (their edges: WH, the
   columns, the map, the edges' flags; draw_picture's bands) is drawn as
   one: each layer over the whole band, kept off the pixels its window
   hides on each row by F_HIDE (hide_mask, then the stencil's test, sfunc),
   and the colour window's regions each row's own (row_spans). */
static int s_wv;                             /* the band's rows' windows differ */
static uint32_t row_state(int r);

/* layer n's (4 the sprites) F_HIDE over rows r0-r1, columns x0-x1, of the
   screen drawn (s_which): cleared, then set where each row's window hides
   it; the depth test left on, the stencil's operation the layers' */
static void hide_mask(int n, int r0, int r1, int x0, int x1)
{
    int r, k, ns, sp[130][2], nv = 0;
    U32 keep[CP_ST];
    CV *v;
    room();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_HIDE & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    scissor(x0, r0 - s_y0, x1, r1 - s_y0);
    rect(x0, r0 - s_y0, x1, r1 - s_y0, 0);
    v = (CV *)sceGuGetMemory((unsigned)(r1 - r0) * 4u * 2u * sizeof(CV));
    memcpy(keep, cp_st, sizeof keep);
    for (r = r0; r < r1; r++) {
        cp_load(row_state(r));               /* (cp_b's state: the row's own) */
        ns = spans(row_state(r), n, x0, x1, hidden, sp);
        if (ns > 4)
            nshvc_fatal("a window's row in %d spans", ns);
        for (k = 0; k < ns; k++, nv++) {
            v[2 * nv].c = v[2 * nv + 1].c = 0;
            v[2 * nv].x = (short)sp[k][0];
            v[2 * nv].y = (short)(r - s_y0);
            v[2 * nv + 1].x = (short)sp[k][1];
            v[2 * nv + 1].y = (short)(r + 1 - s_y0);
            v[2 * nv].z = v[2 * nv + 1].z = 0;
        }
    }
    memcpy(cp_st, keep, sizeof keep);
    if (nv) {
        sceGuStencilFunc(GU_ALWAYS, F_HIDE, 0xFF);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * nv, 0, v);
    }
    sceGuPixelMask(0);
    sceGuEnable(GU_DEPTH_TEST);
}

/* the stencil's test and value for a layer's pixels: always (or, s_wv,
   where F_HIDE is not), value ref */
static void sfunc(int ref)
{
    if (s_wv)
        sceGuStencilFunc(GU_EQUAL, ref, F_HIDE);
    else
        sceGuStencilFunc(GU_ALWAYS, ref, 0xFF);
}

/* layer n's spans on the band: its window's (one state), or (s_wv) the
   whole band, the mask made (hide_mask) */
static int layer_spans(uint32_t si, int n, int r0, int r1, int x0, int x1, int out[][2])
{
    if (!s_wv)
        return spans(si, n, x0, x1, shows, out);
    hide_mask(n, r0, r1, x0, x1);
    out[0][0] = x0;
    out[0][1] = x1;
    return 1;
}

/* layer n's map entry at tile (tx, ty): its map's, or (s_out: the columns
   drawn are outside the console's picture, draw_screen's pieces) its
   outside map's (nshvc_ext_layer_outside; cp_bg's) */
static int s_out;

static U32 bg_entry(uint32_t si, U32 n, U32 tx, U32 ty)
{
    U32 ob = FD_(CP_OUT), sc, off;
    if (!s_out)
        return cp_map_entry(si, n, tx, ty);
    sc = (FD_(ob) >> (4u + 3u * n)) & 7u;
    off = (ty & 31u) << 5 | (tx & 31u);
    if (sc >= 4u) {
        off = (ty & (sc == 4u ? 31u : 63u)) << 7 | (tx & 127u);
    } else {
        if ((tx & 32u) != 0u && (sc & 1u) != 0u)
            off += 0x400u;
        if ((ty & 32u) != 0u && (sc & 2u) != 0u)
            off += (sc & 1u) != 0u ? 0x800u : 0x400u;
        off &= 0xFFFu;
    }
    return (FD_(ob + 1u + n * 4096u + (off >> 1)) >> (16u * (off & 1u))) & 0xFFFFu;
}

/* whether layer n has an outside map this frame */
static int has_out(U32 n)
{
    U32 ob = FD_(CP_OUT);
    return ob != 0u && ((FD_(ob) >> n) & 1u) != 0u;
}

/* the tiles of background n over rows r0-r1 (the band's, dr0 the first's
   hardware row) and columns x0-x1, at their depths, by the texture they
   come from and their palette */
#define CELLS 256                            /* a band's tiles in one bucket at most: 33 x 6 */
static TV s_cell[2][16][8][CELLS * 2];       /* by priority, texture image (and 2 bits' palette), palette */
static int s_cells[2][16][8];

/* a layer's texture images and palettes (draw_bg, bg_flush) */
typedef struct {
    Atlas *a;
    Clut *c;
    int kind, tk, direct;                    /* the tiles' kind, the texture's (hires: split), direct colour */
} BgTex;

/* the cells made, drawn: a bucket a texture image and palette (out of
   line: a loop's registers its own) */
static __attribute__((noinline)) void bg_flush(const BgTex *g)
{
    int im, pl, p, kind = g->kind;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(kind == 2 ? GU_PSM_T8 : GU_PSM_T4, 0, 0, 0);
    for (im = 0; im < 16; im++)
        for (pl = 0; pl < 8; pl++)
            for (p = 0; p < 2; p++) {
                int cnt = s_cells[p][im][pl], timg = kind == 0 ? im / 4 : im;
                TV *v;
                if (!cnt)
                    continue;
                room();
                sceGuTexImage(0, 128, 512, 128, tile_image(g->a, g->tk, timg << 10));
                sceGuTexFlush();
                if (kind == 0) {
                    /* the window of 4 colours the palette starts at */
                    int b = (im & 3) * 8 + pl;
                    sceGuClutMode(GU_PSM_8888, 0, 0x0F, 0);
                    sceGuClutLoad(2, &g->c->c[0][b * 4]);
                } else if (kind == 1) {
                    sceGuClutMode(GU_PSM_8888, 0, 0x0F, (unsigned)pl);
                    sceGuClutLoad(32, g->c->c[1]);
                } else {
                    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
                    sceGuClutLoad(32, g->direct ? g->c->direct[pl] : g->c->c[2]);
                }
                v = (TV *)sceGuGetMemory(cnt * 2 * (int)sizeof(TV));
                memcpy(v, s_cell[p][im][pl], (size_t)cnt * 2 * sizeof(TV));
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, cnt * 2, 0, v);
            }
}

/* mosaic's colours: a run of rows' blocks' colour bytes, by layer and
   priority, a picture row a texture row (made on its first mosaic) */
static uint8_t (*s_mos)[2][PIC_H][256];

/* draw_bg's mosaic (out of line: its loop's registers its own) */
static __attribute__((noinline)) void bg_mosaic(uint32_t si, int n, int r0, int r1, int x0, int x1, const BgTex *g, U32 xs,
                                            U32 ys, U32 bpp, U32 nba, U32 hofs, U32 vofs)
{
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u;
    int sl = (int)FD_(CP_SL), kind = g->kind, direct = g->direct, pl;
    /* mosaic (cp_bg): a block of m columns, of the rows whose block
       starts on the same line, the layer's pixel at its first column
       and that line (offset-per-tile's scroll that column's): a sprite
       of the one texel, magnified (every pixel's position on it under
       the next texel's) */
    I32 m = (I32)(cp_b(si, U_(CP_S_INIDISP), 3u) >> 4) + 1, b0;
    int ra, rb, z[2] = { (int)cp_zbg(bgmode, (U32)n, 0u), (int)cp_zbg(bgmode, (U32)n, 1u) };
    int csh = bpp == 2u ? 3 : bpp == 4u ? 4 : 5;     /* (a tile's bytes, 8 bpp, a power of 2) */
    U32 last_tx = ~0u, last_ty = ~0u, last_e = 0;
    U32 big3 = (bgmode >> 6) & 1u, sx3 = big3 ? 4u : 3u, sy3 = big3 ? 4u : 3u;
    U32 h3 = cp_h(si, U_(CP_S_HOFS) + 1u, 0u), v3 = cp_h(si, U_(CP_S_VOFS) + 1u, 0u);
    /* a block's colour a byte (its palette's place and its colour, 0
       clear: a tile's colour 0 is), drawn a run of rows at a time, each
       priority a sprite of a byte a block, magnified; direct colour (its
       palette bits the tile's) a sprite a block */
    int bytes = !(kind == 2 && direct);
    b0 = cp_floor_to(x0 - sl, m);
    if (bytes) {
        if (!s_mos && !(s_mos = (uint8_t(*)[2][PIC_H][256])memalign(64, 4 * sizeof *s_mos)))
            nshvc_fatal("no memory for mosaic");
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(32, g->c->c[2]);
    }
    for (ra = r0; ra < r1; ra = rb) {
        U32 my = FD_(FD_(CP_ROWS) + 4u * (U32)ra + 3u);
        I32 bx;
        int nb = 0, q, done = 0;
        for (rb = ra + 1; rb < r1 && FD_(FD_(CP_ROWS) + 4u * (U32)rb + 3u) == my; rb++)
            ;
        if (bytes && !((mode == 2u || mode == 4u) && n < 2)) {
            /* one scroll for the run: a tile's entry, flips, palette,
               priority and its characters' rows of pixels taken as a block
               first meets it, each block then its pixel alone */
            U32 yy = (my + vofs) & 0x3FFu, mx = (1u << xs) - 1u, tcur = ~0u, base = 0, pr = 0, hf = 0;
            const uint8_t *row[2] = { NULL, NULL };
            uint8_t *o[2] = { s_mos[n][0][ra], s_mos[n][1][ra] };
            int nbk = (x1 - sl - b0 + m - 1) / m;
            memset(o[0], 0, (size_t)nbk);
            memset(o[1], 0, (size_t)nbk);
            for (bx = b0; nb < nbk; bx += m, nb++) {
                U32 xx = (U_(bx) + hofs) & 0x3FFu, px, c;
                if ((xx >> xs) != tcur) {
                    U32 e, py, ch, k;
                    tcur = xx >> xs;
                    e = bg_entry(si, (U32)n, tcur, yy >> ys);
                    hf = (e >> 14) & 1u;
                    py = yy & ((1u << ys) - 1u);
                    if ((e >> 15) & 1u)
                        py = (1u << ys) - 1u - py;
                    ch = (e & 0x3FFu) + (py >= 8u ? 16u : 0u);
                    pr = (e >> 13) & 1u;
                    base = kind == 0 ? (mode == 0u ? (U32)n * 32u : 0u) + ((e >> 10) & 7u) * 4u
                         : kind == 1 ? ((e >> 10) & 7u) * 16u : 0u;
                    for (k = 0; k < 2; k++) {
                        U32 ca = (nba + ((ch + k) & 0x3FFu) * (bpp * 4u)) & 0x7FFFu;
                        int t = (int)(ca >> csh);
                        const uint8_t *img = tile_image(g->a, kind, t);
                        row[k] = kind == 2 ? img + (tile_v(t) + (int)(py & 7u)) * 128 + tile_u(t)
                                           : img + (tile_v(t) + (int)(py & 7u)) * 64 + tile_u(t) / 2;
                    }
                }
                px = xx & mx;
                if (hf)
                    px = mx - px;
                c = kind == 2 ? row[px >> 3][px & 7u] : (U32)(row[px >> 3][(px & 7u) >> 1] >> ((px & 1u) * 4)) & 15u;
                if (c)
                    o[pr][nb] = (uint8_t)(base + c);
            }
            done = 1;
        }
        for (bx = b0; !done && bx + sl < x1; bx += m, nb++) {
            U32 hc = hofs, vc = vofs, xx, yy, e, ch, hf, vf, pal, prio, ca, px, py;
            int t, slot, b;
            TV *v;
            if ((mode == 2u || mode == 4u) && n < 2) {
                I32 col = (bx + I_(hofs & 7u)) >> 3;
                if (col > 0) {
                    U32 tx3 = (U_(col - 1) * 8u + (h3 & ~7u)) >> sx3, hv = cp_map_entry(si, 2u, tx3, v3 >> sy3), vv;
                    if (mode == 4u) {
                        if (hv & 0x8000u) {
                            if (hv & (0x2000u << n))
                                vc = hv & 0x3FFu;
                        } else if (hv & (0x2000u << n)) {
                            hc = (hofs & 7u) | (hv & 0x3F8u);
                        }
                    } else {
                        vv = cp_map_entry(si, 2u, tx3, (v3 + 8u) >> sy3);
                        if (hv & (0x2000u << n))
                            hc = (hofs & 7u) | (hv & 0x3F8u);
                        if (vv & (0x2000u << n))
                            vc = vv & 0x3FFu;
                    }
                }
            }
            xx = (U_(bx) + hc) & 0x3FFu;
            yy = (my + vc) & 0x3FFu;
            if ((xx >> xs) != last_tx || (yy >> ys) != last_ty) {
                last_tx = xx >> xs;
                last_ty = yy >> ys;
                last_e = bg_entry(si, (U32)n, last_tx, last_ty);
            }
            e = last_e;
            px = xx & ((1u << xs) - 1u);
            py = yy & ((1u << ys) - 1u);
            hf = (e >> 14) & 1u;
            vf = (e >> 15) & 1u;
            if (hf)
                px = (1u << xs) - 1u - px;
            if (vf)
                py = (1u << ys) - 1u - py;
            ch = e & 0x3FFu;
            if (px >= 8u)
                ch += 1u;
            if (py >= 8u)
                ch += 16u;
            pal = (e >> 10) & 7u;
            prio = (e >> 13) & 1u;
            ca = (nba + (ch & 0x3FFu) * (bpp * 4u)) & 0x7FFFu;
            t = (int)(ca >> csh);
            if (bytes) {
                /* the tile's pixel from its decoded image (draw_bg's atlas) */
                const uint8_t *img = tile_image(g->a, kind, t);
                int u = tile_u(t) + (int)(px & 7u), w = tile_v(t) + (int)(py & 7u);
                U32 c = kind == 2 ? img[w * 128 + u] : (U32)(img[w * 64 + u / 2] >> ((u & 1) * 4)) & 15u;
                U32 ci = !c ? 0u : kind == 0 ? (mode == 0u ? (U32)n * 32u : 0u) + pal * 4u + c : kind == 1 ? pal * 16u + c : c;
                s_mos[n][prio][ra][nb] = (uint8_t)ci;
                s_mos[n][prio ^ 1u][ra][nb] = 0;
                continue;
            }
            b = kind == 0 ? (int)((mode == 0u ? (U32)n * 8u : 0u) + pal) : (int)pal;
            slot = kind == 0 ? (t >> 10) * 4 + (b >> 3) : (t >> 10);
            pl = kind == 2 && !direct ? 0 : b & 7;
            if (s_cells[prio][slot][pl] >= CELLS) {
                /* a bucket full: those made drawn (a layer's blocks in any
                   order, their pixels apart, their depths the same) */
                bg_flush(g);
                memset(s_cells, 0, sizeof s_cells);
            }
            v = &s_cell[prio][slot][pl][2 * s_cells[prio][slot][pl]++];
            v[0].u = (short)(tile_u(t) + (int)(px & 7u));
            v[0].v = (short)(tile_v(t) + (int)(py & 7u));
            v[0].x = (short)(bx + sl);
            v[0].y = (short)(ra - s_y0);
            v[0].z = (short)z[prio];
            v[1].u = (short)(v[0].u + 1);
            v[1].v = (short)(v[0].v + 1);
            v[1].x = (short)(bx + sl + m);
            v[1].y = (short)(rb - s_y0);
            v[1].z = v[0].z;
        }
        for (q = 0; bytes && nb && q < 2; q++) {
            TV *v;
            room();
            v = (TV *)sceGuGetMemory(2 * sizeof(TV));
            sceKernelDcacheWritebackRange(s_mos[n][q][ra], 256);
            sceGuTexImage(0, 256, 1, 256, s_mos[n][q][ra]);
            sceGuTexFlush();
            v[0].u = 0;
            v[0].v = 0;
            v[0].x = (short)(b0 + sl);
            v[0].y = (short)(ra - s_y0);
            v[0].z = (short)z[q];
            v[1].u = (short)nb;
            v[1].v = 1;
            v[1].x = (short)(b0 + sl + nb * m);
            v[1].y = (short)(rb - s_y0);
            v[1].z = v[0].z;
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        }
    }
}

/* (phase: a hires layer's (modes 5, 6) texels the screen takes, 0 the even
   (the sub screen), 1 the odd (the main); -1 a layer of a screen's pixels) */
/* (out of line: its loops their own registers, not the caller's) */
static __attribute__((noinline)) void draw_bg(uint32_t si, int n, int r0, int r1, I32 dr0, int x0, int x1, int phase)
{
    const uint16_t *words = (const uint16_t *)(rr_cp_fd + cp_fdb + FD_(CP_VRAM));
    U32 ld = cp_st[U_(CP_S_LAYER) + (U32)n], bpp = (ld >> 1) & 15u, xs = (ld >> 5) & 7u, ys = (ld >> 8) & 7u;
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u, page = cp_b(si, U_(CP_S_PAGE), (U32)n);
    U32 hofs = (((n >> 1) == 0 ? cp_st[CP_S_HOFS] : cp_st[CP_S_HOFS + 1]) >> (16u * ((U32)n & 1u))) & 0xFFFFu;
    U32 vofs = (((n >> 1) == 0 ? cp_st[CP_S_VOFS] : cp_st[CP_S_VOFS + 1]) >> (16u * ((U32)n & 1u))) & 0xFFFFu;
    U32 nbaw = cp_b(si, U_(CP_S_NBA), n < 2 ? 0u : 1u), nba = ((nbaw >> (((U32)n & 1u) * 4u)) & 0x0Fu) << 12;
    int kind = bpp == 2u ? 0 : bpp == 4u ? 1 : 2, sl = (int)FD_(CP_SL), x, y, pl, direct, cx;
    int tk = phase >= 0 ? kind + 3 : kind;   /* the texture's kind: hires, the split images */
    Atlas *a = atlas(words, cp_sw(si, U_(CP_S_VBASE)) + (page << 15), tk);
    Clut *c;
    BgTex g;
    if (phase >= 0)
        xs = 4u;                             /* (hires: 16 texels a tile across) */
    direct = bpp == 8u && (cp_b(si, U_(CP_S_TSW), 1u) & 1u);
    c = s_index ? &s_iclut : clut(cp_sw(si, U_(CP_S_PAL)), direct);
    g.a = a;
    g.c = c;
    g.kind = kind;
    g.tk = tk;
    g.direct = direct;
    memset(s_cells, 0, sizeof s_cells);
    /* the cell at band column x, row y, w across: the map's tile at (xx,
       yy), its texels from nu across in the image (fu flipped) */
#define CELL(xx, yy, w, nu, fu)                                                                                    \
    do {                                                                                                           \
        U32 e = bg_entry(si, (U32)n, (xx) >> xs, (yy) >> ys);                                                      \
        U32 ch = e & 0x3FFu, hf = (e >> 14) & 1u, vf = (e >> 15) & 1u, pal = (e >> 10) & 7u, prio = (e >> 13) & 1u; \
        U32 ca;                                                                                                    \
        int t, slot, b;                                                                                            \
        TV *v;                                                                                                     \
        if (xs == 4u && ((((xx) >> 3) & 1u) ^ hf))                                                                 \
            ch += 1u;                                                                                              \
        if (ys == 4u && ((((yy) >> 3) & 1u) ^ vf))                                                                 \
            ch += 16u;                                                                                             \
        ca = (nba + (ch & 0x3FFu) * (bpp * 4u)) & 0x7FFFu;                                                         \
        t = (int)(ca / (bpp * 4u));                                                                                \
        /* a 2-bit tile's palette is its CLUT window (b), others' the CSA or a direct colour's palette */          \
        b = kind == 0 ? (int)((mode == 0u ? (U32)n * 8u : 0u) + pal) : (int)pal;                                   \
        slot = kind == 0 ? (t >> 10) * 4 + (b >> 3) : (t >> 10);                                                   \
        pl = kind == 2 && !direct ? 0 : b & 7;                                                                     \
        if (s_cells[prio][slot][pl] >= CELLS)                                                                      \
            break;                           /* (more than a band's tiles: cannot be) */                           \
        v = &s_cell[prio][slot][pl][2 * s_cells[prio][slot][pl]++];                                                \
        put(v, x, y - s_y0, (w), 8, tile_u(t) + (hf ? (fu) : (nu)), tile_v(t), (int)hf, (int)vf,                    \
            (int)cp_zbg(bgmode, (U32)n, prio));                                                                    \
    } while (0)
    /* the cells made, drawn (bg_flush) */
#define FLUSH() bg_flush(&g)
    if (phase < 0 && ((cp_b(si, U_(CP_S_INIDISP), 3u) >> n) & 1u) && (cp_b(si, U_(CP_S_INIDISP), 3u) >> 4)) {
        bg_mosaic(si, n, r0, r1, x0, x1, &g, xs, ys, bpp, nba, hofs, vofs);
    } else if ((mode == 2u || mode == 4u) && n < 2) {
        /* offset-per-tile (cp_bg): each tile column past the first its own
           scroll from BG3's map, its fine horizontal scroll the layer's
           (the cells' columns the same), so a column's cells down the band */
        U32 big3 = (bgmode >> 6) & 1u, sx3 = big3 ? 4u : 3u, sy3 = big3 ? 4u : 3u;
        U32 h3 = cp_h(si, U_(CP_S_HOFS) + 1u, 0u), v3 = cp_h(si, U_(CP_S_VOFS) + 1u, 0u);
        for (x = x0 - (int)((U_(x0 - sl) + hofs) & 7u); x < x1; x += 8) {
            I32 col = (I_(x - sl) + I_(hofs & 7u)) >> 3;
            U32 hc = hofs, vc = vofs;
            if (col > 0) {
                U32 bx = (U_(col - 1) * 8u + (h3 & ~7u)) >> sx3, hv = cp_map_entry(si, 2u, bx, v3 >> sy3), vv;
                if (mode == 4u) {
                    if (hv & 0x8000u) {
                        if (hv & (0x2000u << n))
                            vc = hv & 0x3FFu;
                    } else if (hv & (0x2000u << n)) {
                        hc = (hofs & 7u) | (hv & 0x3F8u);
                    }
                } else {
                    vv = cp_map_entry(si, 2u, bx, (v3 + 8u) >> sy3);
                    if (hv & (0x2000u << n))
                        hc = (hofs & 7u) | (hv & 0x3F8u);
                    if (vv & (0x2000u << n))
                        vc = vv & 0x3FFu;
                }
            }
            for (y = r0 - (int)((U_(dr0 + 1) + vc) & 7u); y < r1; y += 8) {
                U32 yy = (U_(dr0 + 1 + (y - r0)) + vc) & 0x3FFu, xx = (U_(x - sl) + hc) & 0x3FFu;
                CELL(xx, yy, 8, 0, 0);
            }
        }
    } else if (phase >= 0) {
        /* hires (cp_bg): column x's texel 2 x + phase (from the screen's
           left) of a line 512 across, the scroll twice the layer's; a cell
           an 8-texel character, the screen's half of it 4 columns across
           (the split image's even or odd half: flipped, the other half
           backwards). Mode 6's offset-per-tile a column of 16 texels. In
           runs of 128 columns, so a bucket holds a run's cells. */
        U32 big3 = (bgmode >> 6) & 1u, sy3 = big3 ? 4u : 3u;
        U32 h3 = cp_h(si, U_(CP_S_HOFS) + 1u, 0u), v3 = cp_h(si, U_(CP_S_VOFS) + 1u, 0u);
        int xs0 = x0 - (int)(((U_(2 * (x0 - sl)) + (hofs << 1)) & 7u) >> 1);
        for (cx = xs0; cx < x1; cx += 128) {
            if (cx > xs0) {
                FLUSH();
                memset(s_cells, 0, sizeof s_cells);
            }
            for (x = cx; x < cx + 128 && x < x1; x += 4) {
                U32 hc = hofs, vc = vofs, kc, xx;
                int opt_h = 0;
                if (mode == 6u && n == 0) {
                    I32 col = (I_(2 * (x - sl)) + I_((hofs << 1) & 7u)) >> 4;
                    if (col > 0) {
                        U32 bx = (U_(col - 1) * 8u + (h3 & ~7u)) >> 3;
                        U32 hv = cp_map_entry(si, 2u, bx, v3 >> sy3), vv = cp_map_entry(si, 2u, bx, (v3 + 8u) >> sy3);
                        if (hv & 0x2000u) {
                            hc = (hofs & 7u) | (hv & 0x3F8u);
                            opt_h = 1;
                        }
                        if (vv & 0x2000u)
                            vc = vv & 0x3FFu;
                    }
                }
                kc = opt_h ? ((hc << 1) & 7u) + (hc & ~7u) : hc << 1;
                xx = (U_(2 * (x - sl)) + kc) & 0x7FFu;
                for (y = r0 - (int)((U_(dr0 + 1) + vc) & 7u); y < r1; y += 8) {
                    U32 yy = (U_(dr0 + 1 + (y - r0)) + vc) & 0x3FFu;
                    CELL(xx, yy, 4, phase ? 4 : 0, phase ? 0 : 4);
                }
            }
        }
    } else {
        for (y = r0 - (int)((U_(dr0 + 1) + vofs) & 7u); y < r1; y += 8) {
            U32 yy = (U_(dr0 + 1 + (y - r0)) + vofs) & 0x3FFu;
            for (x = x0 - (int)((U_(x0 - sl) + hofs) & 7u); x < x1; x += 8) {
                U32 xx = (U_(x - sl) + hofs) & 0x3FFu;
                CELL(xx, yy, 8, 0, 0);
            }
        }
    }
#undef CELL
    FLUSH();
#undef FLUSH
}

/* ---- Mode 7 (cp_m7): each console pixel's byte of the plane, the CPU's,
   drawn as a texture of them ---- */
static uint8_t __attribute__((aligned(64))) s_m7[2][256 * 256];   /* by layer: a picture row a texture row */

/* layer n's (BG1, or EXTBG's BG2) bytes over rows r0-r1 of the band, its
   columns x0-x1: the pixel's texel by the line's matrix as the console
   finds it (ppu.c draw_m7), 0 where the wrap mode leaves it clear */
static void m7_fill(uint32_t si, int n, int r0, int r1, I32 dr0, int x0, int x1)
{
    const uint16_t *v = (const uint16_t *)(rr_cp_fd + cp_fdb + FD_(CP_VRAM)) + cp_sw(si, U_(CP_S_VBASE)) +
                        (cp_b(si, U_(CP_S_PAGE), 0u) << 15);
    U32 mosaic = cp_b(si, U_(CP_S_INIDISP), 3u), m7sel = cp_b(si, U_(CP_S_NBA), 2u);
    I32 msize = (mosaic & (1u << n)) ? (I32)(mosaic >> 4) + 1 : 1, sl = I_(FD_(CP_SL));
    I32 a = cp_s16(cp_h(si, U_(CP_S_M7AB), 0u)), b = cp_s16(cp_h(si, U_(CP_S_M7AB), 1u));
    I32 c = cp_s16(cp_h(si, U_(CP_S_M7CD), 0u)), d = cp_s16(cp_h(si, U_(CP_S_M7CD), 1u));
    I32 cx = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 0u))), cy = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 1u)));
    I32 hofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 0u))), vofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 1u)));
    I32 x0c = cp_clip10(hofs - cx), y0c = cp_clip10(vofs - cy);
    U32 wrap = m7sel & 0xC0u;
    int r, x;
    for (r = r0; r < r1; r++) {
        uint8_t *o = s_m7[n] + (size_t)r * 256;
        I32 y = msize > 1 ? I_(FD_(FD_(CP_ROWS) + 4u * (U32)r + 3u)) : dr0 + (r - r0) + 1;
        I32 ox, oy, tx, ty, sx, step;
        if (m7sel & 2u)
            y = 255 - y;
        ox = ((a * x0c) & ~63) + ((b * y) & ~63) + ((b * y0c) & ~63) + cx * 256;
        oy = ((c * x0c) & ~63) + ((d * y) & ~63) + ((d * y0c) & ~63) + cy * 256;
        /* the plane's position at the first column, then a column's step
           (the matrix's a, c; a flip steps back) */
        sx = msize > 1 ? cp_floor_to(x0 - sl, msize) : x0 - sl;
        step = (m7sel & 1u) ? -1 : 1;
        if (m7sel & 1u)
            sx = 255 - sx;
        tx = ox + a * sx;
        ty = oy + c * sx;
        if (msize == 1 && x1 > x0) {
            /* the plane repeating (its map's 128 tiles a row and column,
               wrapped), or the span's ends on it and so all of it (a
               line): its map's and characters' bytes read straight */
            I32 ex = tx + step * a * (x1 - 1 - x0), ey = ty + step * c * (x1 - 1 - x0);
            if (!(wrap & 0x80u) || (((tx | ty | ex | ey) >> 8) & ~0x3FF) == 0) {
                const uint8_t *vb = (const uint8_t *)v;
                I32 da = step * a, dc = step * c;
                for (x = x0; x < x1; x++) {
                    U32 tile = vb[((U_(ty >> 11) & 127u) << 8) | ((U_(tx >> 11) & 127u) << 1)];
                    o[x] = vb[(tile << 7 | (U_(ty >> 4) & 0x70u) | (U_(tx >> 7) & 0x0Eu)) + 1u];
                    tx += da;
                    ty += dc;
                }
                continue;
            }
        }
        for (x = x0; x < x1; x++) {
            I32 px, py;
            U32 tile;
            if (msize > 1 && x > x0) {
                I32 s2 = cp_floor_to(x - sl, msize);
                if (m7sel & 1u)
                    s2 = 255 - s2;
                tx = ox + a * s2;
                ty = oy + c * s2;
            }
            px = tx >> 8;
            py = ty >> 8;
            if (((px | py) & ~0x3FF) != 0) {
                if (wrap == 0x80u) {
                    o[x] = 0;
                    goto next;
                }
                tile = wrap == 0xC0u ? 0u : v[((py >> 3) & 127) << 7 | ((px >> 3) & 127)] & 0xFFu;
            } else {
                tile = v[(py >> 3) << 7 | (px >> 3)] & 0xFFu;
            }
            o[x] = (uint8_t)(v[(tile * 64u + U_(py & 7) * 8u + U_(px & 7)) & 0x7FFFu] >> 8);
        next:
            if (msize == 1) {
                tx += step * a;
                ty += step * c;
            }
        }
    }
}

/* layer n's bytes (m7_fill) over the band's rows r0-r1, columns x0-x1, at
   its depth: BG1's colours (direct where CGWSEL says), EXTBG's BG2 its
   two priorities' each at its own */
static void draw_m7(uint32_t si, int n, int r0, int r1, int x0, int x1)
{
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u);
    int direct = n == 0 && (cp_b(si, U_(CP_S_TSW), 1u) & 1u), p;
    Clut *c = s_index ? &s_iclut : clut(cp_sw(si, U_(CP_S_PAL)), n ? -1 : direct);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, 256, 256, 256, s_m7[n]);
    sceGuTexFlush();
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    for (p = 0; p < (n ? 2 : 1); p++) {
        TV *v;
        room();
        sceGuClutLoad(32, n ? c->ext[p] : direct ? c->direct[0] : c->c[2]);
        v = (TV *)sceGuGetMemory(2 * sizeof(TV));
        put(v, x0, r0 - s_y0, x1 - x0, r1 - r0, x0, r0, 0, 0, (int)cp_zbg(bgmode, (U32)n, (U32)p));
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
}

/* the sprites of row r's part p (columns x0-x1) into the sprite buffer, in
   reverse order so the first is on top; each pixel's stencil 1 | priority
   << 1 | maths (palettes 4-7) << 3 (cp_obj) */
/* a row's sprite tiles, in order, a draw for each run of them with the
   same stencil, palette and texture image (state sent as it changes) */
#define OB_MAX 128
static TV s_ob[OB_MAX][2];
static int s_ob_n;

static void ob_flush(void)
{
    TV *v;
    if (!s_ob_n)
        return;
    room();
    v = (TV *)sceGuGetMemory(s_ob_n * 2 * (int)sizeof(TV));
    memcpy(v, s_ob, (size_t)s_ob_n * 2 * sizeof(TV));
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, s_ob_n * 2, 0, v);
    s_ob_n = 0;
}

/* The rows from r (before s1) whose sprites draw together (draw_obj_rows):
   each a part alone, of the same state and index band (ix: each row's),
   its sprites the same as r's (their X, name, columns, flips, palette,
   priority, kept) but their rows, each on by 1 or -1 a row (2 or -2 with
   OBJ interlace), the same each row (step[i]); the end of them */
static int obj_rows(int r, int s1, const int *ix, signed char *step)
{
    U32 ri = FD_(CP_ROWS) + 4u * (U32)r, part = FD_(ri), si = FD_(FD_(CP_PARTS) + 4u * part + 1u);
    U32 at = FD_(FD_(CP_PARTS) + 4u * part + 2u), n = FD_(FD_(CP_PARTS) + 4u * part + 3u), i;
    const uint32_t *st = &FD_(FD_(CP_STATES) + si * U_(CP_ST));
    int g = r + 1;
    if (n > 128u)
        return g;
    for (; g < s1; g++) {
        U32 rj = FD_(CP_ROWS) + 4u * (U32)g, pj = FD_(rj), sj = FD_(FD_(CP_PARTS) + 4u * pj + 1u);
        U32 atj = FD_(FD_(CP_PARTS) + 4u * pj + 2u);
        if (FD_(rj + 1u) != 1u || FD_(FD_(CP_PARTS) + 4u * pj + 3u) != n || ix[g - r] != ix[0] ||
            (sj != si && memcmp(&FD_(FD_(CP_STATES) + sj * U_(CP_ST)), st, CP_ST * sizeof(uint32_t))))
            break;
        for (i = 0; i < n; i++) {
            U32 e = FD_(CP_SPR) + (at + i) * U_(CP_SE), ej = FD_(CP_SPR) + (atj + i) * U_(CP_SE);
            U32 f = FD_(e + 1u), fj = FD_(ej + 1u);
            if (FD_(ej) != FD_(e) || FD_(ej + 2u) != FD_(e + 2u) || ((fj ^ f) & ~0xFFu))
                break;
            if (g == r + 1) {
                /* the step: the first two rows' */
                U32 u = ((fj & 0xFFu) - (f & 0xFFu)) & 0xFFu;
                step[i] = (signed char)(u == 1u ? 1 : u == 0xFFu ? -1 : u == 2u ? 2 : u == 0xFEu ? -2 : 0);
                if (!step[i])
                    break;
            } else if ((U32)(((int)(f & 0xFFu) + step[i] * (g - r)) & 0xFF) != (fj & 0xFFu)) {
                break;
            }
        }
        if (i < n)
            break;
    }
    return g;
}

/* One sprite of rows ga-gb (draw_obj_rows) at its entry e, its rows on by
   d a row; dbl: d 2 or -2 (its rows every other row of its tiles), a
   constant in each of the two made of this, so the other has none of it.
   cur: the stencil, palette and image last set. */
static inline __attribute__((always_inline)) void obj_sprite(Atlas *a, U32 e, int d, const int dbl, int ga, int gb,
                                                             U32 base, U32 gap, int sl, int cur[3])
{
    U32 f = FD_(e + 1u), name = FD_(e + 2u), col;
    I32 sxp = I_(FD_(e)), keep = I_((f >> 18) & 63u) - 1, first = sxp > -8 ? 0 : (-sxp - 8) / 8 + 1;
    U32 row0 = f & 0xFFu, cols = (f >> 8) & 15u, hflip = (f >> 12) & 1u, pal = (f >> 13) & 7u, prio = (f >> 16) & 3u;
    U32 cc;
    /* (rows stepping 2: the image read every other row, from the first
       of the rows' parity: the image's 2 | parity << 2) */
    int ra, L, ix = dbl ? 2 | (int)(row0 & 1u) << 2 : 0;
    for (ra = ga; ra < gb; ra += L) {
        U32 row = (row0 + (U32)(d * (ra - ga))) & 0xFFu, y = row & 7u;
        TV tv[16][2];
        int img[16], nt = 0, k, j;
        /* the rows from ra in this row of the sprite's tiles */
        L = d > 0 ? (8 - (int)y + dbl) >> dbl : ((int)y >> dbl) + 1;
        if (L > gb - ra)
            L = gb - ra;
        for (cc = 0; cc < cols; cc++) {
            U32 t, ca;
            int T, tv0, X = sxp + I_(cc) * 8, pa = 0, pb = 8;
            /* the console's limits on its own columns alone (cp_obj): a tile
               it keeps none of drawn past them only */
            if (FD_(CP_LIMITS) != 0u && !(keep >= 0 && I_(cc) - first < keep)) {
                if (X < 0)
                    pb = -X < 8 ? -X : 8;
                else if (X + 8 > 256)
                    pa = 256 - X > 0 ? 256 - X : 0;
                else
                    continue;
            }
            if (sl + X + pb <= 0)
                continue;
            col = hflip ? cols - 1u - cc : cc;
            t = (((name & 0xF0u) + ((row >> 3) << 4)) & 0xF0u) | ((name + col) & 0x0Fu);
            ca = (base + t * 16u + ((name & 0x100u) != 0u ? gap : 0u)) & 0x7FFFu;
            T = (int)(ca / 16u);
            img[nt] = T >> 10;
            tv0 = (tile_v(T) + (int)y) >> dbl;
            /* (columns pa-pb of the tile: its texels, flipped or not) */
            if (d > 0)
                put(tv[nt++], sl + X + pa, ra - s_y0, pb - pa, L, tile_u(T) + (hflip ? 8 - pb : pa), tv0, (int)hflip, 0, 0);
            else                             /* (its rows up the tile: flipped) */
                put(tv[nt++], sl + X + pa, ra - s_y0, pb - pa, L, tile_u(T) + (hflip ? 8 - pb : pa), tv0 - L + 1, (int)hflip, 1,
                    0);
        }
        if (!nt)
            continue;
        /* the sprite's tiles of each texture image they are in (they
           overlap nothing of each other: their columns are their own),
           after those before them */
        for (k = 0; k < 2; k++) {
            int ref = (int)(1u | prio << 1 | (pal >= 4u ? 8u : 0u)), im = k | ix;
            for (j = 0; j < nt; j++) {
                if (img[j] != k)
                    continue;
                if (ref != cur[0] || (int)pal != cur[1] || im != cur[2] || s_ob_n == OB_MAX) {
                    ob_flush();
                    room();
                    if (ref != cur[0])
                        sceGuStencilFunc(GU_ALWAYS, ref, 0xFF);
                    if ((int)pal != cur[1])
                        sceGuClutMode(GU_PSM_8888, 0, 0x0F, 8u + pal);
                    if (im != cur[2]) {
                        if (dbl)
                            sceGuTexImage(0, 256, 256, 256, tile_image(a, 1, k << 10) + (ix >> 2) * 64);
                        else
                            sceGuTexImage(0, 128, 512, 128, tile_image(a, 1, k << 10));
                        sceGuTexFlush();
                    }
                    cur[0] = ref;
                    cur[1] = (int)pal;
                    cur[2] = im;
                }
                s_ob[s_ob_n][0] = tv[j][0];
                s_ob[s_ob_n][1] = tv[j][1];
                s_ob_n++;
            }
        }
    }
}

/* The sprites of rows ga-gb's part `part` (each row's the same sprites,
   their rows on by step[i], 1 or -1, or 2 or -2, a row: obj_rows) into
   the sprite buffer, in reverse order so the first is on top (each row's
   order, so each pixel as its row's alone); each sprite in pieces as tall
   as its rows stay in one row of its tiles; each pixel's stencil 1 |
   priority << 1 | maths (palettes 4-7) << 3 (cp_obj). (A row alone: gb
   ga + 1.) A sprite whose rows step 2 is drawn from its tiles' rows of
   one parity: their image from its first row of that parity, read at
   twice its rows' stride, a texel row a picture row. */
static void draw_obj_rows(uint32_t si, uint32_t part, int ga, int gb, int x0, int x1, const signed char *step)
{
    const uint16_t *words = (const uint16_t *)(rr_cp_fd + cp_fdb + FD_(CP_VRAM));
    U32 at = FD_(FD_(CP_PARTS) + 4u * part + 2u), n = FD_(FD_(CP_PARTS) + 4u * part + 3u), i;
    U32 obsel = cp_b(si, U_(CP_S_INIDISP), 1u), page = cp_b(si, U_(CP_S_PAGE4), 0u);
    U32 base = (obsel & 7u) << 13, gap = (((obsel >> 3) & 3u) + 1u) << 12;
    int sl = (int)FD_(CP_SL), cur[3] = { -1, -1, -1 };
    Atlas *a;
    Clut *c;
    if (!n)
        return;
    a = atlas(words, cp_sw(si, U_(CP_S_VBASE)) + (page << 15), 1);
    c = s_index ? &s_iclut : clut(cp_sw(si, U_(CP_S_PAL)), 0);
    scissor(x0, ga - s_y0, x1, gb - s_y0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuClutMode(GU_PSM_8888, 0, 0x0F, 8);
    sceGuClutLoad(32, c->c[1]);
    for (i = n; i-- > 0;) {
        U32 e = FD_(CP_SPR) + (at + i) * U_(CP_SE);
        int d = step ? step[i] : 1;
        if (d == 2 || d == -2)
            obj_sprite(a, e, d, 1, ga, gb, base, gap, sl, cur);
        else
            obj_sprite(a, e, d, 0, ga, gb, base, gap, sl, cur);
    }
    ob_flush();
}

/* whether the frame is one drawn here: its features, and its VRAM pages
   each an atlas */
static int s_cv = -1;                        /* a canvas frame's: the canvas's layer (rr_psp_canvas) */

static int drawable(void)
{
    U32 r, ph = FD_(CP_PH), keys[ATLASES];
    int nkeys = 0;
    if ((FD_(CP_PW) != FD_(CP_SW) && FD_(CP_PW) != 2u * FD_(CP_SW)) || FD_(CP_SW) > (s_cv >= 0 ? 512u : (U32)PIC_W) || ph > PIC_H ||
        (FD_(CP_CVON) && s_cv < 0))
        return 0;
    for (r = 0; r < ph; r++) {
        U32 ri = FD_(CP_ROWS) + 4u * r, p0 = FD_(ri), np = FD_(ri + 1u), k;
        I32 dr = I_(FD_(ri + 2u));
        if (dr < 0 || dr >= I_(FD_(CP_LINES)))
            return 0;
        for (k = 0; k < np; k++) {
            U32 si = FD_(FD_(CP_PARTS) + 4u * (p0 + k) + 1u), n, mode, tmts;
            cp_load(si);
            if (cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u)
                continue;
            mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
            tmts = cp_b(si, U_(CP_S_TM), 1u) | cp_b(si, U_(CP_S_TM), 2u);
            /* (hires with the screen interlaced: its rows twice as many) */
            /* (the windows' columns, map and edges: cp_win's, spans' own) */
            if (((mode == 5u || mode == 6u) && (cp_b(si, U_(CP_S_NBA), 3u) & 0x01u)) || cp_b(si, U_(CP_S_TSW), 3u))
                return 0;
            for (n = 0; n < 5u; n++) {
                U32 key = cp_sw(si, U_(CP_S_VBASE)) + ((n < 4u ? cp_b(si, U_(CP_S_PAGE), n) : cp_b(si, U_(CP_S_PAGE4), 0u)) << 15);
                int j;
                /* (Mode 7's plane read from the VRAM's words: m7_fill) */
                if (!((tmts >> n) & 1u) || (n < 4u && (mode == 7u || !cp_bpp(mode, n))))
                    continue;
                /* any layer but hires (5, 6) under mosaic */
                if (n < 4u && !(cp_st[U_(CP_S_LAYER) + n] & 1u) && (mode == 5u || mode == 6u) &&
                    ((cp_b(si, U_(CP_S_INIDISP), 3u) >> n) & 1u) && (cp_b(si, U_(CP_S_INIDISP), 3u) >> 4))
                    return 0;
                for (j = 0; j < nkeys && keys[j] != key; j++)
                    ;
                if (j == nkeys) {
                    if (nkeys == ATLASES)
                        return 0;
                    keys[nkeys++] = key;
                }
            }
        }
    }
    return 1;
}

/* one screen of a band (which: 0 main, 1 sub): its backdrop, backgrounds
   and sprites */
/* (which: 0 the main screen, 1 the sub; 2, on a hires line, the sub as
   its even pixels' own (cp_pixel): its backdrop the main's colour, its
   pixels' maths bits as the main's are. hires: the line's, 1 for modes 5
   and 6 (their layers hires), 2 pseudo-hires, 0 none) */
static void draw_screen(uint32_t si, int which, int r0, int r1, I32 dr0, int x0, int x1, int hires)
{
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u;
    U32 on = cp_b(si, U_(CP_S_TM), which ? 2u : 1u), cgadsub = cp_b(si, U_(CP_S_TSW), 2u), n;
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    U32 back_math = (cgadsub >> 5) & 1u ? F_MATH : 0u;
    int sp[130][2], ns, k, p, m, own = which != 1;
    target(which == 0 ? V_MAIN : which == 1 ? V_SUB : V_SUB2, s_pw);
    scissor(x0, r0 - s_y0, x1, r1 - s_y0);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuPixelMask(0);
    /* (an index band's: index 0, the backdrop's; the sub screen's its fixed
       colour after, index_band) */
    sceGuClearColor(s_index ? 0xFF000000u : times4(which == 1 ? fixc : cp_pal(si, 0u)));
    /* the backdrop's bits: its maths where it is a pixel's own, F_BACK where
       the other screen's (and on a hires line the main screen is that too) */
    sceGuClearStencil(which == 1 ? F_BACK : which == 2 || hires ? F_BACK | back_math : back_math);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GREATER);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuEnable(GU_COLOR_TEST);
    sceGuColorFunc(GU_NOTEQUAL, CLEAR, 0xFFFFFF);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    s_which = which ? 1 : 0;
    if (mode == 7u) {
        U32 both = cp_b(si, U_(CP_S_TM), 1u) | cp_b(si, U_(CP_S_TM), 2u);
        for (n = 0; n < 2u; n++) {
            if ((n && !(cp_b(si, U_(CP_S_NBA), 3u) & 0x40u)) || !((both >> n) & 1u))
                continue;
            /* the bytes once a band (the main screen's turn), both screens';
               EXTBG's BG2 BG1's own (the same texel) unless their mosaic
               differs */
            if (!which) {
                U32 mos = cp_b(si, U_(CP_S_INIDISP), 3u);
                if (n && !(((mos ^ (mos >> 1)) & 1u) && (mos >> 4)) && ((both & 1u) != 0u))
                    memcpy(s_m7[1] + (size_t)r0 * 256, s_m7[0] + (size_t)r0 * 256, (size_t)(r1 - r0) * 256);
                else
                    m7_fill(si, (int)n, r0, r1, dr0, x0, x1);
                sceKernelDcacheWritebackRange(s_m7[n] + (size_t)r0 * 256, (unsigned)(r1 - r0) * 256u);
            }
            if (!((on >> n) & 1u))
                continue;
            ns = layer_spans(si, (int)n, r0, r1, x0, x1, sp);
            sfunc(own ? (int)((cgadsub >> n) & 1u) : 0);
            for (k = 0; k < ns; k++) {
                scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
                draw_m7(si, (int)n, r0, r1, sp[k][0], sp[k][1]);
            }
        }
    }
    for (n = 0; n < 4u && mode != 7u; n++) {
        if (!cp_bpp(mode, n) || !((on >> n) & 1u) || (int)n == s_cv)
            continue;
        ns = layer_spans(si, (int)n, r0, r1, x0, x1, sp);
        sfunc(own ? (int)((cgadsub >> n) & 1u) : 0);
        for (k = 0; k < ns; k++) {
            /* (with an outside map: its pieces left of, on and right of the
               console's picture, each from its map) */
            int sl = (int)FD_(CP_SL), cut[4] = { sp[k][0], sp[k][1], sp[k][1], sp[k][1] }, q, out = has_out(n);
            if (out) {
                cut[1] = sp[k][0] > sl ? sp[k][0] : sl < sp[k][1] ? sl : sp[k][1];
                cut[2] = sp[k][0] > sl + 256 ? sp[k][0] : sl + 256 < sp[k][1] ? sl + 256 : sp[k][1];
                if (cut[2] < cut[1])
                    cut[2] = cut[1];
            }
            for (q = 0; q < 3; q++) {
                if (cut[q + 1] <= cut[q])
                    continue;
                s_out = out && q != 1;
                scissor(cut[q], r0 - s_y0, cut[q + 1], r1 - s_y0);
                draw_bg(si, (int)n, r0, r1, dr0, cut[q], cut[q + 1], hires == 1 ? (which ? 0 : 1) : -1);
            }
            s_out = 0;
        }
    }
    if ((on >> 4) & 1u) {
        /* the sprite buffer at each priority's depth, its maths' bit its stencil's */
        sceGuDisable(GU_COLOR_TEST);
        ns = layer_spans(si, 4, r0, r1, x0, x1, sp);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        for (p = 0; p < 4; p++)
            for (m = 0; m < 2; m++) {
                U32 z = cp_zobj(mode, (U32)p);
                sceGuAlphaFunc(GU_EQUAL, 1 | p << 1 | m << 3, 0x0F);
                sfunc(own ? (int)(m && ((cgadsub >> 4) & 1u)) : 0);
                for (k = 0; k < ns; k++) {
                    TV *v;
                    room();
                    v = (TV *)sceGuGetMemory(2 * sizeof(TV));
                    scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
                    sceGuEnable(GU_TEXTURE_2D);
                    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
                    sceGuTexImage(0, s_pw, BAND_TEX, s_pw, vram(V_OBJ));
                    sceGuTexFlush();
                    v[0].u = (short)sp[k][0];
                    v[0].v = (short)(r0 - s_y0);
                    v[0].x = (short)sp[k][0];
                    v[0].y = (short)(r0 - s_y0);
                    v[0].z = (short)z;
                    v[1].u = (short)sp[k][1];
                    v[1].v = (short)(r1 - s_y0);
                    v[1].x = (short)sp[k][1];
                    v[1].y = (short)(r1 - s_y0);
                    v[1].z = (short)z;
                    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
                }
            }
        sceGuDisable(GU_ALPHA_TEST);
    }
    if (s_cv >= 0 && cp_bpp(mode, (U32)s_cv) && ((on >> s_cv) & 1u)) {
        /* a canvas frame: where the canvas's layer, opaque, would be the
           screen's top pixel (its depth over the depth there, where it may
           show), F_W */
        int z = (int)cp_zbg(bgmode, (U32)s_cv, (FD_(CP_CVSCREEN) >> 1) & 1u);
        sceGuDisable(GU_COLOR_TEST);
        ns = layer_spans(si, s_cv, r0, r1, x0, x1, sp);
        sceGuDepthMask(GU_TRUE);
        sfunc(F_W);
        sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_W & 0xFF) << 24);
        for (k = 0; k < ns; k++) {
            room();
            scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
            zrect(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0, (uint32_t)z);
        }
        sceGuPixelMask(0);
        sceGuDepthMask(GU_FALSE);
    }
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_COLOR_TEST);
    if (s_wv) {
        /* (F_HIDE cleared: the finish's tests not to see it) */
        sceGuDisable(GU_ALPHA_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_HIDE & 0xFF) << 24);
        scissor(x0, r0 - s_y0, x1, r1 - s_y0);
        rect(x0, r0 - s_y0, x1, r1 - s_y0, 0);
        sceGuPixelMask(0);
    }
}

/* (stripe rows' states, from the description: the row's part's) */
static uint32_t row_state(int r) { return FD_(FD_(CP_PARTS) + 4u * FD_(FD_(CP_ROWS) + 4u * (U32)r) + 1u); }
static uint32_t state_word(uint32_t si, int w) { return FD_(FD_(CP_STATES) + si * U_(CP_ST) + (U32)w); }

/* row r's fixed colour (15 bits) */
static U32 row_fixed(int r)
{
    U32 f = state_word(row_state(r), CP_S_FIXED);
    return (f & 0xFFu) | (f >> 8 & 0xFFu) << 5 | (f >> 16 & 0xFFu) << 10;
}

/* an index band's screens (each of `screens`) made colours, each row its
   own palette's (in place, the stencil kept); then (an index band's, or
   one whose rows' fixed colours differ) the sub screen's backdrop its fixed
   colour (F_BACK), each row's */
static void index_band(uint32_t si, int r0, int r1, int x0, int x1, int screens)
{
    const unsigned buf[3] = { V_MAIN, V_SUB, V_SUB2 };
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    int i, r;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    for (i = 0; i < screens; i++) {
        room();
        target(buf[i], s_pw);
        scissor(x0, r0 - s_y0, x1, r1 - s_y0);
        sceGuPixelMask(0xFF000000u);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexMode(GU_PSM_T32, 0, 0, 0);
        sceGuTexImage(0, s_pw, BAND_TEX, s_pw, vram(buf[i]));
        sceGuTexFlush();
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        for (r = r0; r < r1 && s_index; r++) {
            const uint32_t *f = pal_f(state_word(row_state(r), CP_S_PAL));
            TV *v;
            room();
            v = (TV *)sceGuGetMemory(2 * sizeof(TV));
            sceGuClutLoad(32, f);
            sceGuTexFlush();
            put(v, x0, r - s_y0, x1 - x0, 1, x0, r - s_y0, 0, 0, 0);
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        }
        if (buf[i] == V_SUB) {
            sceGuEnable(GU_STENCIL_TEST);
            sceGuStencilFunc(GU_EQUAL, F_BACK, F_BACK);
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
            for (r = r0; r < r1; r++) {
                room();
                rect(x0, r - s_y0, x1, r + 1 - s_y0, times4(s_fx ? row_fixed(r) : fixc));
            }
            sceGuDisable(GU_STENCIL_TEST);
        }
        sceGuPixelMask(0);
    }
}

/* The band's colour maths on a screen's pixels (own) against the other
   screen's (other), then its rows of the picture (out): 0 a pixel each; 1
   a line 512 across that is not hires, each pixel twice; 2 a hires line's
   even pixels (own the sub screen, other the main), the rows cleared and
   their odd pixels marked in the picture's stencil; 3 its odd pixels (own
   the main), its first black (cp_pixel) */
/* the colour window's region (fn: clipped, prevented) over rows r0-r1,
   columns x0-x1: rectangles, the state's spans (or, s_wv, each row's) */
static void region_rects(uint32_t si, int r0, int r1, int x0, int x1, SpanFn fn)
{
    int sp[130][2], ns, k, r;
    U32 keep[CP_ST];
    if (!s_wv) {
        ns = spans(si, 0, x0, x1, fn, sp);
        for (k = 0; k < ns; k++) {
            scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
            rect(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0, 0);
        }
        return;
    }
    scissor(x0, r0 - s_y0, x1, r1 - s_y0);
    memcpy(keep, cp_st, sizeof keep);
    for (r = r0; r < r1; r++) {
        room();
        cp_load(row_state(r));               /* (cp_b's state: the row's own) */
        ns = spans(row_state(r), 0, x0, x1, fn, sp);
        for (k = 0; k < ns; k++)
            rect(sp[k][0], r - s_y0, sp[k][1], r + 1 - s_y0, 0);
    }
    memcpy(cp_st, keep, sizeof keep);
}

static void finish_band(uint32_t si, int r0, int r1, int x0, int x1, unsigned own, unsigned other, int out)
{
    U32 cgwsel = cp_b(si, U_(CP_S_TSW), 1u), cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    U32 inidisp = cp_b(si, U_(CP_S_INIDISP), 0u), bright = inidisp & 0x0Fu;
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    int k, y0 = r0 - s_y0, y1 = r1 - s_y0;
    int sc = out ? 2 : 1;
    room();
    target(own, s_pw);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    /* clip and prevent: their bits set over the colour window's spans */
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_CLIP & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_CLIP, 0xFF);
    region_rects(si, r0, r1, x0, x1, clipped);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_PREVENT & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_PREVENT, 0xFF);
    region_rects(si, r0, r1, x0, x1, prevented);
    scissor(x0, y0, x1, y1);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    /* clipped: black */
    sceGuPixelMask(0xFF000000u);
    sceGuStencilFunc(GU_EQUAL, F_CLIP, F_CLIP);
    rect(x0, y0, x1, y1, 0);
    if (cgadsub & 0x3Fu) {
        /* halving: maths, not prevented or clipped, and (with the sub
           screen) the sub screen's pixel not its backdrop */
        if (cgadsub & 0x40u) {
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
            sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_HALF & 0xFF) << 24);
            /* tested masked (F_MATH alone of the three), written whole
               through the pixel mask (F_HALF alone) */
            sceGuStencilFunc(GU_EQUAL, F_MATH | F_HALF, F_MATH | F_PREVENT | F_CLIP);
            if (cgwsel & 2u) {
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_EQUAL, 0, F_BACK);
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
                stripe_tex(other, x0, y0, x1, y1);
                sceGuDisable(GU_ALPHA_TEST);
            } else {
                rect(x0, y0, x1, y1, 0);
            }
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
        }
        /* the other screen added or subtracted: maths and not prevented */
        sceGuPixelMask(0xFF000000u);
        sceGuStencilFunc(GU_EQUAL, F_MATH, F_MATH | F_PREVENT);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc((cgadsub & 0x80u) ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
        if (cgwsel & 2u) {
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            stripe_tex(other, x0, y0, x1, y1);
        } else {
            if (s_fx) {
                int r;
                for (r = r0; r < r1; r++) {
                    room();
                    rect(x0, r - s_y0, x1, r + 1 - s_y0, times4(row_fixed(r)));
                }
            } else {
                rect(x0, y0, x1, y1, times4(fixc));
            }
        }
        if (cgadsub & 0x40u) {
            sceGuStencilFunc(GU_EQUAL, F_HALF, F_HALF);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0, 0x808080);
            rect(x0, y0, x1, y1, 0);
        }
        sceGuDisable(GU_BLEND);
    }
    sceGuDisable(GU_STENCIL_TEST);
    sceGuPixelMask(0);
    /* the picture's rows: each channel through its lookup */
    target(V_PIC + s_pic_off, s_pic_stride);
    scissor(sc * x0, r0, sc * x1, r1);
    if (out != 3) {
        /* cleared, colour and stencil, by a rectangle (a clear covers the
           display's 480 columns, the picture's 512) */
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
        rect(sc * x0, r0, sc * x1, r1, 0);
        sceGuDisable(GU_STENCIL_TEST);
    }
    if (inidisp & 0x80u)
        return;                              /* forced blank: black */
    sceGuEnable(GU_TEXTURE_2D);
    if (out == 2) {
        /* the odd pixels marked: a texture of alternate alphas, repeated */
        TV *v = (TV *)sceGuGetMemory(2 * sizeof(TV));
        sceGuDisable(GU_BLEND);
        sceGuPixelMask(0x00FFFFFFu);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 4, 1, 4, s_parity);
        sceGuTexFlush();
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0xFF, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        put(v, 2 * x0, r0, 2 * (x1 - x0), r1 - r0, 2 * x0, 0, 0, 0, 0);
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuPixelMask(0);
    }
    if (out >= 2) {
        /* the line's own pixels of this pass, by the stencil */
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilFunc(GU_EQUAL, out == 3 ? 0xFF : 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    }
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexMode(GU_PSM_T32, 0, 0, 0);
    sceGuTexImage(0, s_pw, BAND_TEX, s_pw, vram(own));
    sceGuTexFlush();
    for (k = 0; k < 3; k++) {
        TV *v = (TV *)sceGuGetMemory(2 * sizeof(TV));
        sceGuClutMode(GU_PSM_8888, (unsigned)(8 * k), 0xFF, 0);
        sceGuClutLoad(32, s_to8[bright][k]);
        v[0].u = (short)x0;
        v[0].v = (short)y0;
        v[0].x = (short)(sc * x0);
        v[0].y = (short)r0;
        v[0].z = 0;
        v[1].u = (short)x1;
        v[1].v = (short)y1;
        v[1].x = (short)(sc * x1);
        v[1].y = (short)r1;
        v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_STENCIL_TEST);
    if (out == 3 && x0 == 0)
        rect(0, r0, 1, r1, 0xFF000000u);     /* (the line's first half pixel) */
}

/* ---- a screen canvas a texel a target pixel (rr_psp_canvas) ----
   The target's pixel X, Y is the reference's blend of the picture pixels
   under it (rr_mode7.h rr_m7_pixel): across, at the geometry's weight,
   pixels i = tx0 and tx1 (RrPspAcross); down, at its row's, j = ty and
   ty + 1; each pixel the console's rules for it with the canvas's texel
   under the target pixel's centre, which, the canvas the size of the
   target's picture, is texel (X, Y) for all four (rr_cv_q's place for
   each, clamped into its pixel, falls in the same texel). So for each
   group of the target's rows, for each j and i: the screens of the
   picture's rows under them (drawn but for the canvas's layer, F_W where
   it would be on top) made the target's size, a pixel's its picture
   pixel's (i, j); the canvas drawn over them where F_W, its texel a
   pixel; the colour maths and brightness (finish_band's) into G0, G1 (i);
   then the blend across into H (j), H0 and H1 the blend down into the
   frame, as the picture's scaling blends (rr_psp.c). */
#define CV_ROWS 12                           /* a group's target rows, and the picture rows under them, at most */
#define CV_ROW (512 * 4)                     /* a row of the window's buffers: 512 pixels of 32 bits */
#define CV_BUF (CV_ROWS * CV_ROW)
#define V_CM (V_PIC + 0 * CV_BUF)            /* the screens made the target's width, at the picture's rows */
#define V_CS (V_PIC + 1 * CV_BUF)
#define V_WM (V_PIC + 2 * CV_BUF)            /* and its height: the target's rows */
#define V_WS (V_PIC + 3 * CV_BUF)
#define V_G0 (V_PIC + 4 * CV_BUF)            /* the colours: i 0, 1 */
#define V_G1 (V_PIC + 5 * CV_BUF)
#define V_H0 (V_PIC + 6 * CV_BUF)            /* blended across: j 0, 1 */
#define V_H1 (V_PIC + 7 * CV_BUF)
#define V_XP (V_PIC + 8 * CV_BUF)            /* the blend's b - a, a - b */
#define V_XM (V_PIC + 9 * CV_BUF)

static struct {
    const RrPspAcross *a;
    int d[4], n, X0, Y0, Y1, ph;
    unsigned fb;
    int16_t r0[272], r1[272];                /* each target row's picture rows (ty, ty + 1, clamped) */
    uint16_t run[PIC_H];                     /* each picture row's run of rows whose states are the same */
    uint8_t A[272], B[272], two[272];        /* and its blend down's factors, whether it blends */
    int ts;                                  /* (unused) */
    unsigned int *across[2];                 /* the lists kept: the blend across (j) */
    int key[8];
} s_cw;
/* The canvas's texels for the GE (s_cvs, 512 a row): an index in red
   (alpha 0), a full colour (nshvc_ext_canvas_screen32) as 0x80BBGGRR (the
   alpha's bit CV_RGB its mark); a column (cv_w) and a row (cv_h) past them
   clear, where a texel outside the canvas reads. For each of the four
   picture pixels a target pixel blends (i across, j down), the texel under
   the target pixel's centre clamped into the picture pixel (rr_mode7.h
   rr_cv_q), as a column for each target column (s_cvcol[i]) and a row for
   each target row (s_cvrow[j]): the rows of a group copied into V_CA, the
   columns from them into V_CB, the canvas the four's passes read (cv_remap).
   s_d8: each channel of a colour out of the finish with no colour maths at
   brightness b (compose_body.h cp_rgb_finish): its 8 bits at 15, else its
   eighths v - v / 32 to the brightness's, z + z / 32 */
#define CV_RGB 0x80
#define V_CA V_XP                            /* (free while the four are made: cv_across's after) */
#define V_CB V_XM
static int s_cv32;
static uint32_t *s_cvs;
static TV __attribute__((aligned(16))) s_cvcol[2][480 * 2];
static TV __attribute__((aligned(16))) s_cvrow[2][272 * 2];
static uint32_t __attribute__((aligned(64))) s_d8[16][3][256];
static int s_d8_made;

static void make_d8(void)
{
    int b, k, n;
    for (b = 0; b < 16; b++)
        for (n = 0; n < 256; n++) {
            unsigned z = (unsigned)n;
            if (b != 15) {
                z = (unsigned)n - ((unsigned)n >> 5);
                z = ((((z >> 3) * (unsigned)b + 7u) * 4370u) >> 16) << 3;
                z = z + (z >> 5);
            }
            for (k = 0; k < 3; k++)
                s_d8[b][k][n] = z << (8 * k);
        }
    sceKernelDcacheWritebackRange(s_d8, sizeof s_d8);
    s_d8_made = 1;
}

static void wtarget(unsigned off)
{
    s_target_off = off;
    s_target_w = 512;
    s_target_psm = GU_PSM_8888;
    sceGuDrawBufferList(GU_PSM_8888, (void *)off, 512);
}

static void wtex(unsigned off, int h)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 512, h, 512, vram(off));
    sceGuTexFlush();
}

/* a sprite from texel (u, v), w x h, at (x, y): one to one */
static void sprite1(int u, int v, int x, int y, int w, int h)
{
    TV *t = (TV *)sceGuGetMemory(2 * sizeof(TV));
    t[0].u = (short)u;
    t[0].v = (short)v;
    t[0].x = (short)x;
    t[0].y = (short)y;
    t[0].z = 0;
    t[1].u = (short)(u + w);
    t[1].v = (short)(v + h);
    t[1].x = (short)(x + w);
    t[1].y = (short)(y + h);
    t[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, t);
}

/* columns cols[0..k) of rows 0..rs from the texture's own (or, with src,
   from its columns src[col]) */
static void wcols(const int *cols, int k, const int *src, int rs)
{
    TV *v;
    int i;
    if (k <= 0)
        return;
    v = (TV *)sceGuGetMemory(k * 2 * (int)sizeof(TV));
    for (i = 0; i < k; i++) {
        int x = cols ? cols[i] : i, u = src ? src[x] : x;
        v[2 * i].u = (short)u;
        v[2 * i].v = 0;
        v[2 * i].x = (short)x;
        v[2 * i].y = 0;
        v[2 * i].z = 0;
        v[2 * i + 1].u = (short)(u + 1);
        v[2 * i + 1].v = (short)rs;
        v[2 * i + 1].x = (short)(x + 1);
        v[2 * i + 1].y = (short)rs;
        v[2 * i + 1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k * 2, 0, v);
}

static void wrects(const int *cols, int k, int rs, uint32_t c)
{
    CV *v;
    int i;
    if (k <= 0)
        return;
    v = (CV *)sceGuGetMemory(k * 2 * (int)sizeof(CV));
    for (i = 0; i < k; i++) {
        v[2 * i].c = v[2 * i + 1].c = c;
        v[2 * i].x = (short)cols[i];
        v[2 * i].y = 0;
        v[2 * i].z = 0;
        v[2 * i + 1].x = (short)(cols[i] + 1);
        v[2 * i + 1].y = (short)rs;
        v[2 * i + 1].z = 0;
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k * 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

/* the blend across of G0 (a) and G1 (b) into h, CV_ROWS rows: rr_psp.c
   strip_across's, its two columns a pixel each buffer's own */
static void cv_across(unsigned h)
{
    const RrPspAcross *A = s_cw.a;
    int n = s_cw.n, rs = CV_ROWS, i, c;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuPixelMask(0);
    scissor(0, 0, n, rs);
    /* b - a, a - b, a */
    wtarget(V_XP);
    sceGuDisable(GU_BLEND);
    wtex(V_G1, 16);
    sprite1(0, 0, 0, 0, n, rs);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
    wtex(V_G0, 16);
    sprite1(0, 0, 0, 0, n, rs);
    wtarget(V_XM);
    sceGuDisable(GU_BLEND);
    sprite1(0, 0, 0, 0, n, rs);
    sceGuEnable(GU_BLEND);
    wtex(V_G1, 16);
    sprite1(0, 0, 0, 0, n, rs);
    wtarget(h);
    sceGuDisable(GU_BLEND);
    wtex(V_G0, 16);
    sprite1(0, 0, 0, 0, n, rs);
    /* each table's columns: H+(b - a) added, H-(a - b) taken away, a
       channel a lookup */
    sceGuEnable(GU_BLEND);
    for (c = 0; c < 6; c++) {
        sceGuBlendFunc(c < 3 ? GU_ADD : GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
        sceGuPixelMask(~(0xFFu << (8 * (c % 3))));
        sceGuTexMode(GU_PSM_T32, 0, 0, 0);
        sceGuTexImage(0, 512, 16, 512, vram(c < 3 ? V_XP : V_XM));
        sceGuTexFlush();
        sceGuClutMode(GU_PSM_8888, (unsigned)(8 * (c % 3)), 0xFF, 0);
        for (i = 0; i < A->ntab; i++) {
            sceGuClutLoad(32, A->clut[i][c / 3]);
            wcols(A->cols + A->first[i], A->count[i], NULL, rs);
        }
    }
    sceGuPixelMask(0);
    /* the exceptions: in each channel, a's pixels marked, those with b
       marked again, those moved */
    {
        int mark = 0, e;
        sceGuEnable(GU_STENCIL_TEST);
        for (i = 0; i < A->ntab; i++)
            for (e = 0; e < A->nex[i]; e++)
                for (c = 0; c < 3; c++) {
                    const int *x = A->ex[i][e];
                    unsigned m = 0xFFu << (8 * c);
                    if (mark == 0 || mark + 2 > 255) {
                        /* the stencil cleared (a rectangle: no colour) */
                        sceGuDisable(GU_BLEND);
                        sceGuPixelMask(0x00FFFFFFu);
                        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
                        sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
                        rect(0, 0, n, rs, 0);
                        sceGuEnable(GU_TEXTURE_2D);
                        mark = 0;
                    }
                    sceGuDisable(GU_BLEND);
                    sceGuPixelMask(0x00FFFFFFu);
                    sceGuEnable(GU_COLOR_TEST);
                    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
                    wtex(V_G0, 16);
                    sceGuColorFunc(GU_EQUAL, (unsigned)x[0] << (8 * c), m);
                    sceGuStencilFunc(GU_ALWAYS, mark + 1, 0xFF);
                    wcols(A->cols + A->first[i], A->count[i], NULL, rs);
                    /* those of a with b: mark + 1 made mark + 2 */
                    wtex(V_G1, 16);
                    sceGuColorFunc(GU_EQUAL, (unsigned)x[1] << (8 * c), m);
                    sceGuStencilFunc(GU_EQUAL, mark + 1, 0xFF);
                    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
                    wcols(A->cols + A->first[i], A->count[i], NULL, rs);
                    sceGuDisable(GU_COLOR_TEST);
                    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
                    sceGuStencilFunc(GU_EQUAL, mark + 2, 0xFF);
                    sceGuPixelMask(0xFF000000u);
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(x[2] > 0 ? GU_ADD : GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
                    wrects(A->cols + A->first[i], A->count[i], rs, 1u << (8 * c));
                    mark += 2;
                }
        sceGuDisable(GU_STENCIL_TEST);
        sceGuPixelMask(0);
    }
    sceGuDisable(GU_BLEND);
}

/* a list kept, made by fn(arg): made once to measure it, then in its own
   room (its jumps over the vertices its own addresses) */
static unsigned int *keep_list(void (*fn)(int), int arg)
{
    static unsigned int *tmp;
    const int most = 1024 * 1024;
    unsigned int *at;
    int bytes;
    if (!tmp && !(tmp = (unsigned int *)memalign(64, (size_t)most)))
        nshvc_fatal("no memory for the canvas's lists");
    s_prebuild = 1;
    sceGuStart(GU_CALL, tmp);
    fn(arg);
    bytes = (sceGuCheckList() + 16 + 63) & ~63;
    sceGuFinish();
    if (bytes > most - 4096)
        nshvc_fatal("the canvas's lists past their %d bytes", most);
    at = (unsigned int *)memalign(64, (size_t)bytes + 4096);
    if (!at)
        nshvc_fatal("no memory for the canvas's lists");
    sceGuStart(GU_CALL, at);
    fn(arg);
    sceGuFinish();
    s_prebuild = 0;
    sceKernelDcacheWritebackRange(at, (unsigned)bytes + 4096u);
    return at;
}
static void cv_across_list(int j) { cv_across(j ? V_H1 : V_H0); }

/* the canvas's colours for a state: its texel's value (by its layer's
   colour depth) a colour as the GE's, 0 CLEAR */
/* the canvas's palettes made this frame, at most (each picture row's its
   own, and more: the GE run to its end before the slots are used again) */
#define CV_CLUTS 256
static const uint32_t *cv_clut(uint32_t si)
{
    static uint32_t __attribute__((aligned(64))) tab[CV_CLUTS][256];
    static uint32_t key[CV_CLUTS][3], frame[CV_CLUTS];
    static int last;
    U32 mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u, n = (U32)s_cv, bpp = cp_bpp(mode, n);
    U32 pal = (FD_(CP_CVSCREEN) >> 2) & 7u, pw = cp_sw(si, U_(CP_S_PAL));
    U32 direct = bpp == 8u && (cp_b(si, U_(CP_S_TSW), 1u) & 1u);
    int i, k;
    Clut *c;
    const uint32_t *f;
    if (frame[last] == s_frame && key[last][0] == cp_fdb + pw && key[last][1] == mode && key[last][2] == direct)
        return tab[last];
    for (i = 0; i < CV_CLUTS && frame[i] == s_frame; i++)
        if (key[i][0] == cp_fdb + pw && key[i][1] == mode && key[i][2] == direct)
            return tab[last = i];
    if (i == CV_CLUTS) {
        /* (every one this frame's: the list run, the GE done with them) */
        list_run();
        list_begin();
        retarget();
        memset(frame, 0, sizeof frame);
        i = 0;
    }
    c = direct ? clut(pw, 1) : NULL;
    f = direct ? NULL : pal_f(pw);
    for (k = 0; k < 256; k++) {
        U32 pix = (U32)k & ((1u << bpp) - 1u);
        tab[i][k] = !pix ? CLEAR
                    : bpp == 8u ? (direct ? c->direct[pal][pix] : f[pix])
                    : bpp == 4u ? f[pal * 16u + pix]
                                : f[(mode == 0u ? n * 32u : 0u) + pal * 4u + pix];
    }
    sceKernelDcacheWritebackRange(tab[i], sizeof tab[i]);
    key[i][0] = cp_fdb + pw;
    key[i][1] = mode;
    key[i][2] = direct;
    frame[i] = s_frame;
    last = i;
    return tab[i];
}

/* the canvas over the window's screens' rows k0-k1 (target row Y at k0)
   where it would be on top and is opaque: the main screen's maths bit the
   canvas's layer's, the sub screen's backdrop bit cleared */
/* a pass over V_CB's rows k0-k1 into the target's same rows */
static void cv_tex(int psm)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(psm, 0, 0, 0);
    sceGuTexImage(0, 512, 16, 512, vram(V_CB));
    sceGuTexFlush();
}

/* the canvas's full colours over the window's screens' rows k0-k1 where
   it would be on top (F_W; its indices cv_index_rows's, before), and the
   main screen's F_DIR there */
static void cv_insert(uint32_t si, int Y, int k0, int k1)
{
    U32 mode, cgadsub;
    int sc;
    (void)Y;
    cp_load(si);                             /* (cp_b's state) */
    mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
    cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    if (!s_cv32 || (cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u) || !cp_bpp(mode, (U32)s_cv))
        return;
    room();
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    cv_tex(GU_PSM_8888);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_EQUAL, CV_RGB, CV_RGB);
    for (sc = 0; sc < 2; sc++) {
        wtarget(sc ? V_WS : V_WM);
        if (!s_prebuild)
            scissor(0, k0, s_cw.n, k1);
        /* (tested F_W, written through the pixel mask: the one bit) */
        if (sc) {
            sceGuStencilFunc(GU_EQUAL, F_W, F_W);
            sceGuPixelMask((uint32_t)(~F_BACK & 0xFF) << 24);
        } else {
            sceGuStencilFunc(GU_EQUAL, (int)(F_W | ((cgadsub >> s_cv) & 1u ? F_MATH : 0u)), F_W);
            sceGuPixelMask((uint32_t)(~F_MATH & 0xFF) << 24);
        }
        sprite1(0, k0, 0, k0, s_cw.n, k1 - k0);
    }
    /* the main screen's F_DIR: cleared, then set where the texel is a
       colour and the canvas is on top (F_W) */
    wtarget(V_WM);
    if (!s_prebuild)
        scissor(0, k0, s_cw.n, k1);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_DIR & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    rect(0, k0, s_cw.n, k1, 0);
    cv_tex(GU_PSM_8888);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuStencilFunc(GU_EQUAL, F_W | F_DIR, F_W);
    sprite1(0, k0, 0, k0, s_cw.n, k1 - k0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuPixelMask(0);
    sceGuDisable(GU_STENCIL_TEST);
}

/* the canvas's indices over the window's screens' rows k0-k1 of the group
   from target row Ya (picture rows rj, one finish's: mode and maths the same) where it would
   be on top (F_W), through each row's palette (a colour 0's CLEAR
   dropped): a CLUT and a sprite each run of rows of one palette */
static const uint32_t *s_rowclut[PIC_H];    /* each picture row's canvas palette, made in frame s_rowclut_f[r] */
static unsigned s_rowclut_f[PIC_H];

static const uint32_t *row_clut(int r)
{
    if (s_rowclut_f[r] != s_frame) {
        cp_load(row_state(r));
        s_rowclut[r] = cv_clut(row_state(r));
        s_rowclut_f[r] = s_frame;
    }
    return s_rowclut[r];
}

static void cv_index_rows(int Ya, const int16_t *rj, int k0, int k1)
{
    uint32_t si = row_state(rj[Ya + k0 - s_cw.Y0]);
    U32 mode, cgadsub;
    int sc, k, ka;
    cp_load(si);
    mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
    cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    if ((cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u) || !cp_bpp(mode, (U32)s_cv))
        return;
    room();
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    cv_tex(GU_PSM_T32);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuEnable(GU_COLOR_TEST);
    sceGuColorFunc(GU_NOTEQUAL, CLEAR, 0xFFFFFF);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    for (sc = 0; sc < 2; sc++) {
        wtarget(sc ? V_WS : V_WM);
        if (sc) {
            sceGuStencilFunc(GU_EQUAL, F_W, F_W);
            sceGuPixelMask((uint32_t)(~F_BACK & 0xFF) << 24);
        } else {
            sceGuStencilFunc(GU_EQUAL, (int)(F_W | ((cgadsub >> s_cv) & 1u ? F_MATH : 0u)), F_W);
            sceGuPixelMask((uint32_t)(~F_MATH & 0xFF) << 24);
        }
        for (k = k0; k < k1; k = ka) {
            const uint32_t *tab = row_clut(rj[Ya + k - s_cw.Y0]);
            for (ka = k + 1; ka < k1 && row_clut(rj[Ya + ka - s_cw.Y0]) == tab; ka++)
                ;
            room();
            sceGuClutLoad(32, tab);
            scissor(0, k, s_cw.n, ka);
            sprite1(0, k, 0, k, s_cw.n, ka - k);
        }
    }
    sceGuDisable(GU_COLOR_TEST);
    sceGuPixelMask(0);
    sceGuDisable(GU_STENCIL_TEST);
    cp_load(si);
}

/* finish_band's colour maths and output on the window's screens' rows
   k0-k1 (the colour window's bits theirs already), into out */
static void cv_finish(uint32_t si, int Y, int k0, int k1, unsigned out)
{
    U32 cgwsel, cgadsub, inidisp, bright, fixc;
    int n = s_cw.n, k;
    cp_load(si);                             /* (cp_b's state) */
    cgwsel = cp_b(si, U_(CP_S_TSW), 1u);
    cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    inidisp = cp_b(si, U_(CP_S_INIDISP), 0u);
    bright = inidisp & 0x0Fu;
    fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    room();
    wtarget(V_WM);
    if (!s_prebuild)
        scissor(0, k0, n, k1);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    /* clipped: black */
    sceGuPixelMask(0xFF000000u);
    sceGuStencilFunc(GU_EQUAL, F_CLIP, F_CLIP);
    rect(0, k0, n, k1, 0);
    if (cgadsub & 0x3Fu) {
        if (cgadsub & 0x40u) {
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
            sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_HALF & 0xFF) << 24);
            sceGuStencilFunc(GU_EQUAL, F_MATH | F_HALF, F_MATH | F_PREVENT | F_CLIP);
            if (cgwsel & 2u) {
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_EQUAL, 0, F_BACK);
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
                wtex(V_WS, 16);
                sprite1(0, k0, 0, k0, n, k1 - k0);
                sceGuDisable(GU_ALPHA_TEST);
            } else {
                rect(0, k0, n, k1, 0);
            }
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
        }
        sceGuPixelMask(0xFF000000u);
        sceGuStencilFunc(GU_EQUAL, F_MATH, F_MATH | F_PREVENT);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc((cgadsub & 0x80u) ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
        if (cgwsel & 2u) {
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            wtex(V_WS, 16);
            sprite1(0, k0, 0, k0, n, k1 - k0);
        } else {
            rect(0, k0, n, k1, times4(fixc));
        }
        if (cgadsub & 0x40u) {
            sceGuStencilFunc(GU_EQUAL, F_HALF, F_HALF);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0, 0x808080);
            rect(0, k0, n, k1, 0);
        }
        sceGuDisable(GU_BLEND);
    }
    sceGuDisable(GU_STENCIL_TEST);
    sceGuPixelMask(0);
    /* the colours: each channel through its lookup */
    wtarget(out);
    if (!s_prebuild)
        scissor(0, k0, n, k1);
    rect(0, k0, n, k1, 0);
    if (inidisp & 0x80u)
        return;                              /* forced blank: black */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T32, 0, 0, 0);
    sceGuTexImage(0, 512, 16, 512, vram(V_WM));
    sceGuTexFlush();
    for (k = 0; k < 3; k++) {
        sceGuClutMode(GU_PSM_8888, (unsigned)(8 * k), 0xFF, 0);
        sceGuClutLoad(32, s_to8[bright][k]);
        sprite1(0, k0, 0, k0, n, k1 - k0);
    }
    sceGuDisable(GU_BLEND);
    if (s_cv32) {
        /* A full colour on top of the main screen, its pixel not clipped:
           no colour maths reaches it (rr_psp_canvas takes no frame where
           any could), so its colour is its texel's own through s_d8. The
           main screen's F_DIR (not F_CLIP) into out's stencil, out's
           colour there cleared, then each channel's lookup added. */
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_DIR & 0xFF) << 24);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        rect(0, k0, n, k1, 0);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_EQUAL, F_DIR, F_DIR | F_CLIP);
        sceGuStencilFunc(GU_ALWAYS, F_DIR, 0xFF);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        wtex(V_WM, 16);
        sprite1(0, k0, 0, k0, n, k1 - k0);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
        sceGuStencilFunc(GU_EQUAL, F_DIR, F_DIR);
        sceGuPixelMask(0xFF000000u);
        rect(0, k0, n, k1, 0);
        sceGuPixelMask(0);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        cv_tex(GU_PSM_T32);
        for (k = 0; k < 3; k++) {
            sceGuClutMode(GU_PSM_8888, (unsigned)(8 * k), 0xFF, 0);
            sceGuClutLoad(32, s_d8[bright][k]);
            sprite1(0, k0, 0, k0, n, k1 - k0);
        }
        sceGuDisable(GU_BLEND);
        /* (out's F_DIR cleared again) */
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_DIR & 0xFF) << 24);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        rect(0, k0, n, k1, 0);
        sceGuPixelMask(0);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
        sceGuDisable(GU_STENCIL_TEST);
    }
}

/* the colour window's bits on the main screen's rows r0-r1 (finish_band's
   first) */
static void cv_marks(uint32_t si, int r0, int r1, int x0, int x1)
{
    room();
    target(V_MAIN, s_pw);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_CLIP & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_CLIP, 0xFF);
    region_rects(si, r0, r1, x0, x1, clipped);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_PREVENT & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_PREVENT, 0xFF);
    region_rects(si, r0, r1, x0, x1, prevented);
    sceGuPixelMask(0);
    sceGuDisable(GU_STENCIL_TEST);
}

/* a screen's buffer copied (a list kept, or nv sprites at v), colour and
   stencil: the GE writes a buffer's alpha (its stencil) only by the
   stencil's operations, so the colours with the stencil 0, then each bit
   the finish reads set where the texture's alpha has it (the alpha test;
   the bit written alone through the pixel mask) */
static void cv_mag(int sub, const unsigned int *list, const TV *v, int nv)
{
    static const unsigned bits[2][4] = { { F_MATH, F_CLIP, F_PREVENT, F_W }, { F_BACK, F_W, 0, 0 } };
    int b;
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
    for (b = -1; b < 4; b++) {
        unsigned bit = b < 0 ? 0 : bits[sub][b];
        if (b >= 0 && !bit)
            break;
        if (b < 0) {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuPixelMask(0);
            sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        } else {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_EQUAL, (int)bit, (int)bit);
            sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~bit & 0xFFu) << 24);
            sceGuStencilFunc(GU_ALWAYS, (int)bit, 0xFF);
        }
        if (list)
            sceGuCallList(list);
        else
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, nv * 2, 0, v);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuPixelMask(0);
}

/* The window's groups (the same each frame for a geometry): each stripe
   (CV_BAND or BAND rows, the last its own), its target rows whose upper
   picture row is one of the stripe's own, in groups of CV_ROWS rows at
   most, their picture rows too (from clo) */
#define CV_GROUPS 64
typedef struct {
    int s0, Ya, Yb, clo;
    TV *rowv[2];                             /* each j's: each target row from its picture row */
    unsigned int *mag[2][2];                 /* the screens made the target's (i, j): lists kept */
    unsigned int *down;                      /* the blend down into the frame (its buffer set first) */
} CvGroup;
static CvGroup s_cg[CV_GROUPS];
static int s_ncg;
static TV __attribute__((aligned(16))) s_colv[2][480 * 2];   /* each i's: each target column from its picture column */

static void cv_groups(int h)
{
    int s0, s1, sown, Y, Ya, clo;
    s_ncg = 0;
    for (s0 = 0; s0 < h; s0 = sown) {
        s1 = s0 + s_band < h ? s0 + s_band : h;
        sown = s1 < h ? s1 - 1 : s1;
        Y = s_cw.Y0;
        while (Y < s_cw.Y1 && s_cw.r0[Y - s_cw.Y0] < s0)
            Y++;
        while (Y < s_cw.Y1 && s_cw.r0[Y - s_cw.Y0] < sown) {
            Ya = Y;
            clo = s_cw.r0[Y - s_cw.Y0];
            while (Y < s_cw.Y1 && s_cw.r0[Y - s_cw.Y0] < sown && Y - Ya < CV_ROWS && s_cw.r1[Y - s_cw.Y0] - clo < CV_ROWS)
                Y++;
            if (s_ncg == CV_GROUPS)
                nshvc_fatal("the canvas's groups past %d", CV_GROUPS);
            s_cg[s_ncg].s0 = s0;
            s_cg[s_ncg].Ya = Ya;
            s_cg[s_ncg].Yb = Y;
            s_cg[s_ncg].clo = clo;
            s_ncg++;
        }
    }
}

/* group g's screens made the target's for (i, j): a list kept (arg g << 2 | i << 1 | j) */
static void cv_mag_list(int arg)
{
    const CvGroup *g = &s_cg[arg >> 2];
    int i = (arg >> 1) & 1, j = arg & 1, n = s_cw.n, sc;
    int nr = g->Yb - g->Ya, rows = s_cw.r1[g->Yb - 1 - s_cw.Y0] - g->clo + 1;
    for (sc = 0; sc < 2; sc++) {
        /* across: the stripe's rows from clo, each column its picture column
           of i; then down: each target row its picture row of j */
        wtarget(sc ? V_CS : V_CM);
        scissor(0, 0, n, rows);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, s_pw, 32, s_pw, vram((sc ? V_SUB : V_MAIN) + (unsigned)((g->clo - g->s0) * s_pw * 4)));
        sceGuTexFlush();
        cv_mag(sc, NULL, s_colv[i], n);
        wtarget(sc ? V_WS : V_WM);
        scissor(0, 0, n, nr);
        wtex(sc ? V_CS : V_CM, 16);
        cv_mag(sc, NULL, g->rowv[j], nr);
    }
}

/* group g's blend down into the frame (whose buffer is set first): a list kept */
static void cv_down_list(int gi)
{
    const CvGroup *g = &s_cg[gi];
    int k, n = s_cw.n;
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuPixelMask(0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    /* H0's rows (a target row each, in order: one sprite), then H1's
       blended over those that take it */
    sceGuDisable(GU_BLEND);
    wtex(V_H0, 16);
    sprite1(0, 0, s_cw.X0, g->Ya, n, g->Yb - g->Ya);
    for (k = 0; k < g->Yb - g->Ya; k++) {
        int q = g->Ya + k - s_cw.Y0;
        if (!s_cw.two[q])
            continue;
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (unsigned)s_cw.A[q] * 0x010101u, (unsigned)s_cw.B[q] * 0x010101u);
        wtex(V_H1 + (unsigned)k * CV_ROW, 1);
        sprite1(0, 0, s_cw.X0, g->Ya + k, n, 1);
    }
    sceGuDisable(GU_BLEND);
}

/* the groups' lists made for the geometry: their vertices, then the lists */
static void cv_make(void)
{
    int gi, i, j, k, x, n = s_cw.n;
    for (gi = 0; gi < CV_GROUPS; gi++) {
        for (i = 0; i < 2; i++) {
            free(s_cg[gi].rowv[i]);
            s_cg[gi].rowv[i] = NULL;
            for (j = 0; j < 2; j++) {
                free(s_cg[gi].mag[i][j]);
                s_cg[gi].mag[i][j] = NULL;
            }
        }
        free(s_cg[gi].down);
        s_cg[gi].down = NULL;
    }
    cv_groups(s_cw.ph);
    for (i = 0; i < 2; i++) {
        const int *src = i ? s_cw.a->tx1 : s_cw.a->tx0;
        for (x = 0; x < n; x++) {
            TV *v = &s_colv[i][2 * x];
            v[0].u = (short)src[x];
            v[0].v = 0;
            v[0].x = (short)x;
            v[0].y = 0;
            v[0].z = 0;
            v[1].u = (short)(src[x] + 1);
            v[1].v = CV_ROWS;
            v[1].x = (short)(x + 1);
            v[1].y = CV_ROWS;
            v[1].z = 0;
        }
    }
    sceKernelDcacheWritebackRange(s_colv, sizeof s_colv);
    for (gi = 0; gi < s_ncg; gi++) {
        CvGroup *g = &s_cg[gi];
        int nr = g->Yb - g->Ya;
        for (j = 0; j < 2; j++) {
            const int16_t *rj = j ? s_cw.r1 : s_cw.r0;
            TV *v = (TV *)memalign(16, (size_t)nr * 2 * sizeof(TV));
            if (!v)
                nshvc_fatal("no memory for the canvas's lists");
            for (k = 0; k < nr; k++) {
                v[2 * k].u = 0;
                v[2 * k].v = (short)(rj[g->Ya + k - s_cw.Y0] - g->clo);
                v[2 * k].x = 0;
                v[2 * k].y = (short)k;
                v[2 * k].z = 0;
                v[2 * k + 1].u = (short)n;
                v[2 * k + 1].v = (short)(v[2 * k].v + 1);
                v[2 * k + 1].x = (short)n;
                v[2 * k + 1].y = (short)(k + 1);
                v[2 * k + 1].z = 0;
            }
            sceKernelDcacheWritebackRange(v, (unsigned)((size_t)nr * 2 * sizeof(TV)));
            g->rowv[j] = v;
        }
        for (i = 0; i < 2; i++)
            for (j = 0; j < 2; j++)
                g->mag[i][j] = keep_list(cv_mag_list, gi << 2 | i << 1 | j);
        g->down = keep_list(cv_down_list, gi);
    }
}

/* The frame's states' insert and finish (each run of rows of one state's;
   at most CV_STATES of them, else made as they are drawn), lists made
   before the frame's (no list open), in s_cva: a run's class (s_cw.run
   its first row), its lists */
#define CV_STATES 8
static unsigned int __attribute__((aligned(64))) s_cva[16 * 1024];
static int s_cvcls[PIC_H];                   /* each run's (its first row's) class, -1 none */
static unsigned int *s_cvins[CV_STATES], *s_cvfin[CV_STATES][2];
static int s_ncls;

static unsigned int *cv_arena_list(unsigned int **at, uint32_t si, int what)
{
    unsigned int *start = *at;
    int bytes;
    s_prebuild = 1;
    sceGuStart(GU_CALL, start);
    if (what < 0)
        cv_insert(si, s_cw.d[1], 0, CV_ROWS);
    else
        cv_finish(si, s_cw.d[1], 0, CV_ROWS, what ? V_G1 : V_G0);
    bytes = (sceGuCheckList() + 16 + 63) & ~63;
    sceGuFinish();
    s_prebuild = 0;
    *at += bytes / 4;
    return start;
}

static void cv_classes(void)
{
    unsigned int *at = s_cva;
    int r, c;
    s_ncls = 0;
    for (r = 0; r < s_cw.ph; r++)
        s_cvcls[r] = -1;
    for (r = 0; r < s_cw.ph; r++) {
        if (s_cw.run[r] != r)
            continue;
        if (s_ncls == CV_STATES || (char *)(at + 3 * 8192 / 4) > (char *)(s_cva + sizeof s_cva / 4)) {
            /* (too many: none made, each drawn as it comes) */
            for (c = 0; c < s_cw.ph; c++)
                s_cvcls[c] = -1;
            s_ncls = 0;
            return;
        }
        s_cvcls[r] = s_ncls;
        s_cvins[s_ncls] = cv_arena_list(&at, row_state(r), -1);
        s_cvfin[s_ncls][0] = cv_arena_list(&at, row_state(r), 0);
        s_cvfin[s_ncls][1] = cv_arena_list(&at, row_state(r), 1);
        s_ncls++;
    }
    sceKernelDcacheWritebackRange(s_cva, (unsigned)((char *)at - (char *)s_cva));
}

/* nv sprites from the texture into the target, colour and mark: the
   colours with the stencil 0, then CV_RGB where the texel's alpha has it
   (the GE writes a buffer's alpha only by the stencil's operations) */
static void cv_copy(const TV *v, int nv)
{
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuPixelMask(0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * nv, 0, v);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_EQUAL, CV_RGB, CV_RGB);
    sceGuStencilFunc(GU_ALWAYS, CV_RGB, 0xFF);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~CV_RGB & 0xFF) << 24);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * nv, 0, v);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuPixelMask(0);
}

/* V_CB for the (i, j) of group g: its rows' texel rows (j's) into V_CA,
   then each target column's texel column (i's) from them into V_CB */
static void cv_remap(const CvGroup *g, int i, int j)
{
    int nr = g->Yb - g->Ya;
    room();
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    wtarget(V_CA);
    scissor(0, 0, 512, nr);
    sceGuTexImage(0, 512, 512, 512, s_cvs);
    sceGuTexFlush();
    cv_copy(&s_cvrow[j][2 * (g->Ya - s_cw.Y0)], nr);
    wtarget(V_CB);
    scissor(0, 0, s_cw.n, nr);
    sceGuTexImage(0, 512, 16, 512, vram(V_CA));
    sceGuTexFlush();
    cv_copy(s_cvcol[i], s_cw.n);
    sceGuDisable(GU_STENCIL_TEST);
}

/* group g: each of the four (i, j) made, blended across into H0, H1, then
   down into the frame */
static void cv_group(const CvGroup *g)
{
    int nr = g->Yb - g->Ya, i, j, k;
    for (j = 0; j < 2; j++) {
        const int16_t *rj = j ? s_cw.r1 : s_cw.r0;
        for (i = 0; i < 2; i++) {
            room();
            sceGuDisable(GU_BLEND);
            sceGuDisable(GU_COLOR_TEST);
            sceGuDisable(GU_DEPTH_TEST);
            sceGuCallList(g->mag[i][j]);
            cv_remap(g, i, j);
            /* the canvas and the colours, a run of rows of one state at a time
               (a description has a state each line, the same or not) */
            for (k = 0; k < nr;) {
                uint32_t si = row_state(rj[g->Ya + k - s_cw.Y0]);
                int k1 = k + 1, run = s_cw.run[rj[g->Ya + k - s_cw.Y0]];
                while (k1 < nr && s_cw.run[rj[g->Ya + k1 - s_cw.Y0]] == run)
                    k1++;
                int c = s_cvcls[run];
                cv_index_rows(g->Ya, rj, k, k1);
                if (c >= 0) {
                    /* (its lists: the rows scissored) */
                    room();
                    scissor(0, k, s_cw.n, k1);
                    sceGuCallList(s_cvins[c]);
                    sceGuCallList(s_cvfin[c][i]);
                } else {
                    cv_insert(si, g->Ya + k, k, k1);
                    cv_finish(si, g->Ya + k, k, k1, i ? V_G1 : V_G0);
                }
                k = k1;
            }
        }
        room();
        sceGuCallList(s_cw.across[j]);
    }
    room();
    wtarget(s_cw.fb);
    scissor(0, 0, 480, 272);
    sceGuCallList(g->down);
}

/* the stripe from s0's groups */
static void cv_window(int s0, int sown)
{
    int gi;
    (void)sown;
    for (gi = 0; gi < s_ncg; gi++)
        if (s_cg[gi].s0 == s0)
            cv_group(&s_cg[gi]);
}

/* the description at word base drawn into the picture from byte off, its
   rows stride pixels apart */

static int draw_picture(uint32_t base, unsigned off, int stride, int *pw, int *ph)
{
    int w, h, s0, r, sw, scale, sown;
    cp_fdb = base;
    s_pic_off = off;
    s_pic_stride = stride;
    if (!s_to8_made)
        make_to8();
    if (s_cv < 0)
        s_frame++;                           /* (a canvas frame's: rr_psp_canvas's, before its lists) */
    w = (int)FD_(CP_PW);
    h = (int)FD_(CP_PH);
    sw = (int)FD_(CP_SW);
    scale = w / sw;                          /* (2: a frame with a hires line) */
    list_begin();
    sceGuDepthBuffer((void *)V_Z, s_pw);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(0, 65535);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_FLAT);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    for (s0 = 0; s0 < h; s0 = sown) {
        /* (a canvas frame's stripe its BAND - 1 rows and the next one's
           first: its target rows' lower picture rows) */
        int s1 = s0 + s_band < h ? s0 + s_band : h, band_end[BAND], band_ix[BAND], band_fx[BAND], band_wv[BAND];
        sown = s_cv >= 0 && s1 < h ? s1 - 1 : s1;
        s_y0 = s0;
        /* the bands: rows of one part whose states are the same (a
           description has a state for each line, the same or not), or
           differ in their palettes alone (an index band: none in direct
           colour, whose colours are not the palette's) */
        for (r = s0; r < s1;) {
            U32 ri = FD_(CP_ROWS) + 4u * (U32)r, p0 = FD_(ri), np = FD_(ri + 1u);
            I32 dr0 = I_(FD_(ri + 2u));
            int r1 = r + 1, ix = 0, fx = 0, wv = 0;
            if (np == 1u) {
                U32 si = FD_(FD_(CP_PARTS) + 4u * p0 + 1u);
                const uint32_t *a = &FD_(FD_(CP_STATES) + si * U_(CP_ST));
                while (r1 < s1) {
                    U32 rj = FD_(CP_ROWS) + 4u * (U32)r1;
                    U32 sj = FD_(FD_(CP_PARTS) + 4u * FD_(rj) + 1u);
                    const uint32_t *b = &FD_(FD_(CP_STATES) + sj * U_(CP_ST));
                    if (FD_(rj + 1u) != 1u || I_(FD_(rj + 2u)) != dr0 + (r1 - r))
                        break;
                    if (sj != si && memcmp(a, b, CP_ST * sizeof(uint32_t))) {
                        /* the fixed colour's bytes, the palette (not in
                           direct colour), and the windows' edges (WH, the
                           columns, the map, the edges' flags: hide_mask's) */
                        int w;
                        if ((a[CP_S_TSW] | b[CP_S_TSW]) & 0x100u)
                            break;
                        for (w = 0; w < CP_ST; w++)
                            if (a[w] != b[w] && w != CP_S_FIXED && w != CP_S_PAL && w != CP_S_WH && w != CP_S_WCOL &&
                                w != CP_S_WCOL + 1 && w != CP_S_WMAP && w != CP_S_WMAP + 1)
                                break;
                        if (w < CP_ST)
                            break;
                        ix |= a[CP_S_PAL] != b[CP_S_PAL];
                        fx |= ((a[CP_S_FIXED] ^ b[CP_S_FIXED]) & 0x00FFFFFFu) != 0u;
                        wv |= a[CP_S_WH] != b[CP_S_WH] || a[CP_S_WCOL] != b[CP_S_WCOL] || a[CP_S_WCOL + 1] != b[CP_S_WCOL + 1] ||
                              a[CP_S_WMAP] != b[CP_S_WMAP] || a[CP_S_WMAP + 1] != b[CP_S_WMAP + 1] ||
                              ((a[CP_S_FIXED] ^ b[CP_S_FIXED]) & 0xFF000000u) != 0u;
                    }
                    r1++;
                }
            }
            band_end[r - s0] = r1;
            for (; r < r1; r++) {
                band_ix[r - s0] = ix;
                band_fx[r - s0] = fx;
                band_wv[r - s0] = wv;
            }
        }
        /* the sprites, a row (and part) at a time */
        target(V_OBJ, s_pw);
        scissor(0, 0, sw, s_band);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_BLEND);
        sceGuPixelMask(0);
        sceGuClearColor(0);
        sceGuClearStencil(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuEnable(GU_COLOR_TEST);
        sceGuColorFunc(GU_NOTEQUAL, CLEAR, 0xFFFFFF);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        for (r = s0; r < s1;) {
            U32 ri = FD_(CP_ROWS) + 4u * (U32)r, p0 = FD_(ri), np = FD_(ri + 1u), k;
            int gb = r + 1;
            signed char step[128];
            if (np == 1u)
                gb = obj_rows(r, s1, band_ix + (r - s0), step);
            for (k = 0; k < np; k++) {
                U32 part = p0 + k, si = FD_(FD_(CP_PARTS) + 4u * part + 1u);
                int x0 = k ? (int)(FD_(FD_(CP_PARTS) + 4u * part) + FD_(CP_SL)) : 0;     /* (the first from the screen's edge) */
                int x1 = k + 1 < np ? (int)(FD_(FD_(CP_PARTS) + 4u * (part + 1u)) + FD_(CP_SL)) : sw;
                cp_load(si);
                s_index = band_ix[r - s0];
                if (!(cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u) && ((cp_b(si, U_(CP_S_TM), 1u) | cp_b(si, U_(CP_S_TM), 2u)) & 0x10u))
                    draw_obj_rows(si, part, r, gb, x0, x1, gb > r + 1 ? step : NULL);
            }
            r = gb;
        }
        sceGuDisable(GU_COLOR_TEST);
        sceGuDisable(GU_STENCIL_TEST);
        for (r = s0; r < s1;) {
            U32 ri = FD_(CP_ROWS) + 4u * (U32)r, p0 = FD_(ri), np = FD_(ri + 1u), k;
            I32 dr0 = I_(FD_(ri + 2u));
            int r1 = band_end[r - s0], which;
            s_index = band_ix[r - s0];
            s_fx = band_fx[r - s0];
            s_wv = band_wv[r - s0];
            for (k = 0; k < np; k++) {
                U32 part = p0 + k, si = FD_(FD_(CP_PARTS) + 4u * part + 1u);
                int x0 = k ? (int)(FD_(FD_(CP_PARTS) + 4u * part) + FD_(CP_SL)) : 0;     /* (the first from the screen's edge) */
                int x1 = k + 1 < np ? (int)(FD_(FD_(CP_PARTS) + 4u * (part + 1u)) + FD_(CP_SL)) : sw;
                if (x1 <= x0)
                    continue;
                U32 mode;
                int hires;
                cp_load(si);
                mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
                hires = scale < 2 ? 0 : mode == 5u || mode == 6u ? 1 : (cp_b(si, U_(CP_S_NBA), 3u) & 8u) ? 2 : 0;
                if (!(cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u)) {
                    for (which = 0; which < (hires ? 3 : 2); which++)
                        draw_screen(si, which, r, r1, dr0, x0, x1, hires);
                    if (s_index || s_fx)
                        index_band(si, r, r1, x0, x1, hires ? 3 : 2);
                }
                if (hires) {
                    /* the even pixels (the sub screen against the main), then
                       the odd (the main against the sub) */
                    finish_band(si, r, r1, x0, x1, V_SUB2, V_MAIN, 2);
                    finish_band(si, r, r1, x0, x1, V_MAIN, V_SUB, 3);
                } else if (s_cv >= 0) {
                    cv_marks(si, r, r1, x0, x1);
                } else {
                    finish_band(si, r, r1, x0, x1, V_MAIN, V_SUB, scale > 1 ? 1 : 0);
                }
            }
            r = r1;
        }
        if (s_cv >= 0)
            cv_window(s0, sown);
    }
    s_index = s_fx = s_wv = 0;
    list_run();
    *pw = w;
    *ph = h;
    return 1;
}

/* the woven picture's rows of one parity kept from the last woven frame:
   the description they were drawn from (its finished frame's), their
   parity; 0 once the picture is drawn over */
static uint32_t *s_kept;
static size_t s_kept_n, s_kept_cap;
static int s_kept_par = -1;

/* n words the same (eight words at a time: the C library's compares bytes) */
static int same_words(const uint32_t *a, const uint32_t *b, size_t n)
{
    for (; n >= 8; n -= 8, a += 8, b += 8)
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5]) |
            (a[6] ^ b[6]) | (a[7] ^ b[7]))
            return 0;
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

int rr_psp_picture(const uint32_t *desc, size_t words, int *pw, int *ph)
{
    (void)words;
    rr_cp_fd = desc;
    cp_fdb = 0;
    if (!drawable())
        return 0;
    s_kept_par = -1;
    rr_psp_picture_stride = 512;
    return draw_picture(0, 0, 512, pw, ph);
}

int rr_psp_canvas(const uint32_t *desc, size_t words, const int d[4], unsigned fb, const RrPspAcross *a,
                  const struct RrMode7 *m, const struct RrM7Samples *smp)
{
    U32 sc, n, r, ph, sw, cw, ch;
    int w, h, pw, phh, Y, key[8], i;
    float dy;
    (void)words;
    rr_cp_fd = desc;
    cp_fdb = 0;
    sc = FD_(CP_CVSCREEN);
    w = d[2] - d[0];
    h = d[3] - d[1];
    ph = FD_(CP_PH);
    sw = FD_(CP_SW);
    cw = FD_(CP_CVW);
    ch = FD_(CP_CVH);
    /* a screen canvas (at any k, or over the whole screen), its texels and
       the clear ones past them a texture's (512), the picture magnified,
       one part a row, no hires */
    if (!FD_(CP_CVON) || !(sc & 1u) || cw > 511u || ch > 511u || m->cpp != 1 || FD_(CP_PW) != sw || d[0] < 0 ||
        d[1] < 0 || d[2] > 480 || d[3] > 272 || (U32)w < sw || (U32)h < ph || a->n != w || a->X0 != d[0])
        return 0;
    n = FD_(CP_CVLAYER);
    for (r = 0; r < ph; r++) {
        U32 ri = FD_(CP_ROWS) + 4u * r, si, mode, mos;
        if (FD_(ri + 1u) != 1u)
            return 0;
        si = FD_(FD_(CP_PARTS) + 4u * FD_(ri) + 1u);
        cp_load(si);                         /* (cp_b's state: the row's own) */
        mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
        mos = cp_b(si, U_(CP_S_INIDISP), 3u);
        /* (wider than 256: no layer under mosaic, its rows 256 bytes) */
        if (mode == 7u || mode == 5u || mode == 6u || (cp_b(si, U_(CP_S_NBA), 3u) & 0x08u) || (((mos >> n) & 1u) && (mos >> 4)) ||
            (sw > (U32)PIC_W && (mos & 0x0Fu) && (mos >> 4)))
            return 0;
        /* a full colour: none where colour maths could reach one (the
           canvas's own, or the sub screen's with the canvas on it; none
           with maths prevented everywhere, CGWSEL's region 3) */
        if ((sc & 0x20u) && (cp_b(si, U_(CP_S_TSW), 2u) & 0x3Fu) && ((cp_b(si, U_(CP_S_TSW), 1u) >> 4) & 3u) != 3u &&
            (((cp_b(si, U_(CP_S_TSW), 2u) >> n) & 1u) ||
             (((cp_b(si, U_(CP_S_TM), 2u) >> n) & 1u) && (cp_b(si, U_(CP_S_TSW), 1u) & 2u))))
            return 0;
    }
    s_cv = (int)n;
    s_pw = sw > (U32)PIC_W ? 512 : PIC_W;
    s_band = sw > (U32)PIC_W ? CV_BAND : BAND;
    if (!drawable()) {
        s_cv = -1;
        s_pw = PIC_W;
        s_band = BAND;
        return 0;
    }
    /* each target row's picture rows and blend (rr_psp.c strip_down's) */
    s_cw.a = a;
    memcpy(s_cw.d, d, sizeof s_cw.d);
    s_cw.n = w;
    s_cw.X0 = d[0];
    s_cw.Y0 = d[1];
    s_cw.Y1 = d[3];
    s_cw.fb = fb;
    s_cw.ph = (int)ph;
    dy = (float)ph / (float)h;
    for (Y = d[1]; Y < d[3]; Y++) {
        int ty, A, q = Y - d[1], r0, r1;
        float fy = rr_sharp(((float)(Y - d[1]) + 0.5f) * dy, dy, &ty);
        r0 = ty < 0 ? 0 : ty >= (int)ph ? (int)ph - 1 : ty;
        r1 = ty + 1 < 0 ? 0 : ty + 1 >= (int)ph ? (int)ph - 1 : ty + 1;
        A = (int)floorf(256.0f * fy + 0.5f);
        A = A > 255 ? 255 : A;
        s_cw.r0[q] = (int16_t)r0;
        s_cw.r1[q] = (int16_t)r1;
        s_cw.A[q] = (uint8_t)A;
        s_cw.B[q] = (uint8_t)(256 - A > 255 ? 255 : 256 - A);
        s_cw.two[q] = fy > 0 && r1 != r0;
    }
    /* the picture rows' runs of one insert and finish (each row against
       the one before): the same words of the state those read (cv_insert,
       cv_finish: INIDISP's, TSW's, the fixed colour; the palette
       cv_index_rows's and the windows the marks', each row's own) */
    for (r = 0; r < ph; r++) {
        int same = 0;
        if (r) {
            const uint32_t *a = &FD_(FD_(CP_STATES) + row_state((int)r) * U_(CP_ST));
            const uint32_t *b = &FD_(FD_(CP_STATES) + row_state((int)r - 1) * U_(CP_ST));
            same = a == b || (a[CP_S_INIDISP] == b[CP_S_INIDISP] && a[CP_S_TSW] == b[CP_S_TSW] &&
                              ((a[CP_S_FIXED] ^ b[CP_S_FIXED]) & 0x00FFFFFFu) == 0u);
        }
        s_cw.run[r] = (uint16_t)(same ? s_cw.run[r - 1] : r);
    }
    /* the canvas's texels for the GE (s_cvs): indices in red, full
       colours CV_RGB-marked; the column and row past them clear */
    if (!s_cvs && !(s_cvs = (uint32_t *)memalign(64, (size_t)512 * 512 * 4)))
        nshvc_fatal("no memory for the canvas");
    s_cv32 = (sc & 0x20u) != 0u;
    for (Y = 0; Y < (int)ch; Y++) {
        uint32_t *o = s_cvs + (size_t)Y * 512u;
        U32 X;
        if (s_cv32) {
            const uint32_t *t = desc + FD_(CP_CV) + (size_t)Y * cw;
            for (X = 0; X < cw; X++) {
                uint32_t v = t[X];
                o[X] = v >= 0x1000000u ? (uint32_t)CV_RGB << 24 | (v & 0xFFu) << 16 | (v & 0xFF00u) | (v >> 16 & 0xFFu)
                                       : (v & 0xFFu);
            }
        } else {
            const uint8_t *t = (const uint8_t *)(desc + FD_(CP_CV)) + (size_t)Y * cw;
            for (X = 0; X < cw; X++)
                o[X] = t[X];
        }
        o[cw] = 0;
    }
    memset(s_cvs + (size_t)ch * 512u, 0, (size_t)(cw + 1) * 4);
    sceKernelDcacheWritebackRange(s_cvs, (unsigned)((ch + 1) * 512u * 4u));
    if (s_cv32 && !s_d8_made)
        make_d8();
    /* each target column's texel column under its centre clamped into
       picture column i (rr_mode7.h rr_cv_q) */
    for (i = 0; i < 2; i++) {
        int x;
        for (x = 0; x < w; x++) {
            int col = i ? a->tx1[x] : a->tx0[x];
            int32_t u = smp->cols[3 * (d[0] + x) + 2], fu = u - 1024 * (col - m->left) + 512;
            uint32_t ex;
            TV *v = &s_cvcol[i][2 * x];
            fu = fu < 0 ? 0 : fu > 1023 ? 1023 : fu;
            ex = m->cv_fx ? rr_cv_fit((uint32_t)col, (uint32_t)fu, (uint32_t)m->cv_fx, (uint32_t)m->pw)
                          : (uint32_t)col * (uint32_t)m->cv_k + (((uint32_t)fu * (uint32_t)m->cv_k) >> 10);
            if (ex > cw)
                ex = cw;
            v[0].u = (short)ex;
            v[0].v = 0;
            v[0].x = (short)x;
            v[0].y = 0;
            v[0].z = 0;
            v[1].u = (short)(ex + 1);
            v[1].v = CV_ROWS;
            v[1].x = (short)(x + 1);
            v[1].y = CV_ROWS;
            v[1].z = 0;
        }
    }
    sceKernelDcacheWritebackRange(s_cvcol, sizeof s_cvcol);
    /* the lists kept, made again for a new geometry */
    key[0] = (int)FD_(CP_PW);
    key[1] = (int)ph;
    memcpy(key + 2, d, 4 * sizeof(int));
    key[6] = a->ntab;
    key[7] = s_band;
    if (!s_cw.across[0] || memcmp(key, s_cw.key, sizeof key)) {
        for (i = 0; i < 2; i++) {
            free(s_cw.across[i]);
            s_cw.across[i] = keep_list(cv_across_list, i);
        }
        cv_make();
        memcpy(s_cw.key, key, sizeof key);
    }
    /* each target row's texel row under its centre clamped into picture
       row j's, as a sprite at its row of its group */
    {
        int gi, j;
        for (gi = 0; gi < s_ncg; gi++) {
            const CvGroup *g = &s_cg[gi];
            for (Y = g->Ya; Y < g->Yb; Y++) {
                int q = Y - s_cw.Y0;
                for (j = 0; j < 2; j++) {
                    int rr = j ? s_cw.r1[q] : s_cw.r0[q];
                    int32_t fv = smp->rows[4 * Y + 3] - 1024 * rr + 512;
                    uint32_t ey;
                    TV *t = &s_cvrow[j][2 * q];
                    fv = fv < 0 ? 0 : fv > 1023 ? 1023 : fv;
                    ey = m->cv_fy ? rr_cv_fit((uint32_t)rr, (uint32_t)fv, (uint32_t)m->cv_fy, (uint32_t)m->ph)
                                  : (uint32_t)rr * (uint32_t)m->cv_k + (((uint32_t)fv * (uint32_t)m->cv_k) >> 10);
                    if (ey > ch)
                        ey = ch;
                    t[0].u = 0;
                    t[0].v = (short)ey;
                    t[0].x = 0;
                    t[0].y = (short)(Y - g->Ya);
                    t[0].z = 0;
                    t[1].u = (short)(cw + 1);
                    t[1].v = (short)(ey + 1);
                    t[1].x = (short)(cw + 1);
                    t[1].y = (short)(Y - g->Ya + 1);
                    t[1].z = 0;
                }
            }
        }
        sceKernelDcacheWritebackRange(s_cvrow, sizeof s_cvrow);
    }
    if (!s_to8_made)
        make_to8();
    s_frame++;
    cv_classes();
    draw_picture(0, 0, 512, &pw, &phh);
    s_cv = -1;
    s_cv32 = 0;
    s_pw = PIC_W;
    s_band = BAND;
    s_kept_par = -1;                         /* (the picture's VRAM the window's buffers) */
    return 1;
}

int rr_psp_picture_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1)
{
    const uint32_t base[2] = { base0, base1 };
    int f, pw, ph;
    (void)words;
    rr_cp_fd = desc;
    if (ww > 256)
        return 0;
    for (f = 0; f < 2; f++) {
        cp_fdb = base[f];
        if (!drawable() || (int)FD_(CP_PW) != ww || 2 * (int)FD_(CP_PH) != wh)
            return 0;
    }
    /* each parity's rows: the picture's buffer a row of 512 pixels a woven
       pair of rows, the odd 256 pixels on (seen 256 a row). A parity whose
       description is the one the last woven frame's rows of it were drawn
       from (the frame before: then the finished one) is there already. */
    for (f = 0; f < 2; f++) {
        size_t n = base[f] ? words - base[f] : base[!f] ? base[!f] : words;
        if (f == s_kept_par && n == s_kept_n && same_words(desc + base[f], s_kept, n))
            continue;
        draw_picture(base[f], (unsigned)f * 256 * 2, 512, &pw, &ph);
    }
    /* the finished frame's (at word 0) kept, with its parity */
    s_kept_par = -1;
    for (f = 0; f < 2; f++)
        if (!base[f] && base[!f]) {
            size_t n = base[!f];
            if (n > s_kept_cap) {
                free(s_kept);
                s_kept = (uint32_t *)malloc(n * sizeof *s_kept);
                s_kept_cap = s_kept ? n : 0;
            }
            if (n <= s_kept_cap) {
                memcpy(s_kept, desc, n * sizeof *desc);
                s_kept_n = n;
                s_kept_par = f;
            }
        }
    rr_psp_picture_stride = 256;
    return 1;
}

const uint16_t *rr_psp_picture_pixels(void) { return (const uint16_t *)((uintptr_t)vram(V_PIC) | 0x40000000u); }
