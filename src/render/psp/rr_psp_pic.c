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
#include <stdlib.h>
#include <string.h>

#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "nshvc/nshvc.h"
#include "rr_compose.h"
#include "rr_psp_pic.h"
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
#define V_SUB (V_MAIN + PIC_W * BAND * 4)
#define V_OBJ (V_SUB + PIC_W * BAND * 4)
#define V_Z (V_OBJ + PIC_W * BAND * 4)
#define V_SUB2 (V_Z + PIC_W * BAND * 4)          /* a hires line's sub screen as its even pixels' own */

/* the main screen's stencil */
#define F_MATH 0x01
#define F_CLIP 0x02
#define F_PREVENT 0x04
#define F_HALF 0x08
#define F_BACK 0x80                          /* the sub screen's: its backdrop */

#define CLEAR 0x00FFFFFFu                    /* a tile's colour 0 */

static unsigned int __attribute__((aligned(64))) s_list[256 * 1024];
#define LIST_ROOM (64 * 1024)                /* bytes kept free: far more than any one step adds (a bucket's
                                                tiles: 256 sprites, 5 KB) */

static unsigned s_target_off;                /* the buffer drawn into (target()) */
static int s_target_w, s_target_psm;

/* the buffer drawn into: a stripe's (32 bits a pixel), or the picture (16) */
static void target(unsigned off, int w)
{
    s_target_off = off;
    s_target_w = w;
    s_target_psm = off == V_PIC ? GU_PSM_5551 : GU_PSM_8888;
    sceGuDrawBufferList(s_target_psm, (void *)off, w);
}

/* the display list finished and begun again before it could fill: the
   GE's state carries over (its registers are its own) but the buffer drawn
   into, which sceGuStart sets again to the GU's own (rr_psp's screen) */
static void room(void)
{
    if (sceGuCheckList() > (int)sizeof s_list - LIST_ROOM) {
        sceGuFinish();
        sceGuSync(0, 0);
        sceGuStart(GU_DIRECT, s_list);
        target(s_target_off, s_target_w);
    }
}

static void *vram(unsigned off) { return (char *)sceGeEdramGetAddr() + off; }

/* ---- tiles ---- */
typedef struct {
    int used;
    uint32_t key;                            /* the copy's first word and page */
    uint32_t frame;                          /* the frame it was last brought up to date in */
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
           decoded again in each kind already made */
        const uint16_t *src = words + base;
        int any = 0;
        if (memcmp(src, a->shadow, sizeof a->shadow) != 0)
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
    int have_direct, have_ext;
} Clut;

#define CLUTS 16
static Clut *s_clut[CLUTS];

static uint32_t times4(unsigned c) { return 0xFF000000u | (c >> 10 & 31u) << 18 | (c >> 5 & 31u) << 10 | (c & 31u) << 2; }

static Clut *clut(uint32_t key, int direct)
{
    Clut *c = NULL;
    int i, k;
    for (i = 0; i < CLUTS; i++)
        if (s_clut[i] && s_clut[i]->frame == s_frame && s_clut[i]->key == key) {
            c = s_clut[i];
            break;
        }
    if (!c) {
        for (i = 0; i < CLUTS && s_clut[i] && s_clut[i]->frame == s_frame; i++)
            ;
        if (i == CLUTS)
            nshvc_fatal("more than %d palettes in a frame", CLUTS);
        if (!s_clut[i] && !(s_clut[i] = (Clut *)memalign(64, sizeof(Clut))))
            nshvc_fatal("no memory for the palettes");
        c = s_clut[i];
        c->key = key;
        c->frame = s_frame;
        c->have_direct = c->have_ext = 0;
        for (k = 0; k < 256; k++) {
            uint32_t v = times4((rr_cp_fd[key + ((unsigned)k >> 1)] >> (16u * ((unsigned)k & 1u))) & 0xFFFFu);
            c->c[0][k] = (k & 3) ? v : CLEAR;
            c->c[1][k] = (k & 15) ? v : CLEAR;
            c->c[2][k] = k ? v : CLEAR;
        }
        sceKernelDcacheWritebackRange(c, sizeof c->c);
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
    if (direct > 0 && !c->have_direct) {
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

/* a stripe buffer (main, sub or sprites) drawn over itself's rows y0-y1,
   columns x0-x1 as a texture */
static void stripe_tex(unsigned off, int x0, int y0, int x1, int y1)
{
    TV *v = (TV *)sceGuGetMemory(2 * sizeof(TV));
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, PIC_W, BAND, PIC_W, vram(off));
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

/* the spans of columns x0-x1 (the picture's) where `keep(x)` holds */
typedef int (*SpanFn)(uint32_t si, int n, int x);
static int spans(uint32_t si, int n, int x0, int x1, SpanFn keep, int out[][2])
{
    int x, k = 0, on = 0;
    for (x = x0; x < x1; x++) {
        int v = keep(si, n, x);
        if (v && !on) {
            out[k][0] = x;
            on = 1;
        } else if (!v && on) {
            out[k++][1] = x;
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

/* the tiles of background n over rows r0-r1 (the band's, dr0 the first's
   hardware row) and columns x0-x1, at their depths, by the texture they
   come from and their palette */
#define CELLS 256                            /* a band's tiles in one bucket at most: 33 x 6 */
static TV s_cell[2][16][8][CELLS * 2];       /* by priority, texture image (and 2 bits' palette), palette */
static int s_cells[2][16][8];

/* (phase: a hires layer's (modes 5, 6) texels the screen takes, 0 the even
   (the sub screen), 1 the odd (the main); -1 a layer of a screen's pixels) */
static void draw_bg(uint32_t si, int n, int r0, int r1, I32 dr0, int x0, int x1, int phase)
{
    const uint16_t *words = (const uint16_t *)(rr_cp_fd + FD_(CP_VRAM));
    U32 ld = cp_st[U_(CP_S_LAYER) + (U32)n], bpp = (ld >> 1) & 15u, xs = (ld >> 5) & 7u, ys = (ld >> 8) & 7u;
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u, page = cp_b(si, U_(CP_S_PAGE), (U32)n);
    U32 hofs = (((n >> 1) == 0 ? cp_st[CP_S_HOFS] : cp_st[CP_S_HOFS + 1]) >> (16u * ((U32)n & 1u))) & 0xFFFFu;
    U32 vofs = (((n >> 1) == 0 ? cp_st[CP_S_VOFS] : cp_st[CP_S_VOFS + 1]) >> (16u * ((U32)n & 1u))) & 0xFFFFu;
    U32 nbaw = cp_b(si, U_(CP_S_NBA), n < 2 ? 0u : 1u), nba = ((nbaw >> (((U32)n & 1u) * 4u)) & 0x0Fu) << 12;
    int kind = bpp == 2u ? 0 : bpp == 4u ? 1 : 2, sl = (int)FD_(CP_SL), x, y, p, im, pl, direct, cx;
    int tk = phase >= 0 ? kind + 3 : kind;   /* the texture's kind: hires, the split images */
    Atlas *a = atlas(words, cp_sw(si, U_(CP_S_VBASE)) + (page << 15), tk);
    Clut *c;
    if (phase >= 0)
        xs = 4u;                             /* (hires: 16 texels a tile across) */
    direct = bpp == 8u && (cp_b(si, U_(CP_S_TSW), 1u) & 1u);
    c = clut(cp_sw(si, U_(CP_S_PAL)), direct);
    memset(s_cells, 0, sizeof s_cells);
    /* the cell at band column x, row y, w across: the map's tile at (xx,
       yy), its texels from nu across in the image (fu flipped) */
#define CELL(xx, yy, w, nu, fu)                                                                                    \
    do {                                                                                                           \
        U32 e = cp_map_entry(si, (U32)n, (xx) >> xs, (yy) >> ys);                                                  \
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
    /* the cells made, drawn: a bucket a texture image and palette */
#define FLUSH()                                                                                                    \
    do {                                                                                                           \
        sceGuEnable(GU_TEXTURE_2D);                                                                                \
        sceGuTexMode(kind == 2 ? GU_PSM_T8 : GU_PSM_T4, 0, 0, 0);                                                  \
        for (im = 0; im < 16; im++)                                                                                \
            for (pl = 0; pl < 8; pl++)                                                                             \
                for (p = 0; p < 2; p++) {                                                                          \
                    int cnt = s_cells[p][im][pl], timg = kind == 0 ? im / 4 : im;                                  \
                    TV *v;                                                                                         \
                    if (!cnt)                                                                                      \
                        continue;                                                                                  \
                    room();                                                                                        \
                    sceGuTexImage(0, 128, 512, 128, tile_image(a, tk, timg << 10));                                \
                    sceGuTexFlush();                                                                               \
                    if (kind == 0) {                                                                               \
                        /* the window of 4 colours the palette starts at */                                        \
                        int b = (im & 3) * 8 + pl;                                                                 \
                        sceGuClutMode(GU_PSM_8888, 0, 0x0F, 0);                                                    \
                        sceGuClutLoad(2, &c->c[0][b * 4]);                                                         \
                    } else if (kind == 1) {                                                                        \
                        sceGuClutMode(GU_PSM_8888, 0, 0x0F, (unsigned)pl);                                         \
                        sceGuClutLoad(32, c->c[1]);                                                                \
                    } else {                                                                                       \
                        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);                                                    \
                        sceGuClutLoad(32, direct ? c->direct[pl] : c->c[2]);                                       \
                    }                                                                                              \
                    v = (TV *)sceGuGetMemory(cnt * 2 * (int)sizeof(TV));                                           \
                    memcpy(v, s_cell[p][im][pl], (size_t)cnt * 2 * sizeof(TV));                                    \
                    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, cnt * 2, 0, v);   \
                }                                                                                                  \
    } while (0)
    if ((mode == 2u || mode == 4u) && n < 2) {
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
    const uint16_t *v = (const uint16_t *)(rr_cp_fd + FD_(CP_VRAM)) + cp_sw(si, U_(CP_S_VBASE)) +
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
    Clut *c = clut(cp_sw(si, U_(CP_S_PAL)), n ? -1 : direct);
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
static void draw_obj_row(uint32_t si, uint32_t part, int r, int x0, int x1)
{
    const uint16_t *words = (const uint16_t *)(rr_cp_fd + FD_(CP_VRAM));
    U32 at = FD_(FD_(CP_PARTS) + 4u * part + 2u), n = FD_(FD_(CP_PARTS) + 4u * part + 3u), i;
    U32 obsel = cp_b(si, U_(CP_S_INIDISP), 1u), page = cp_b(si, U_(CP_S_PAGE4), 0u);
    U32 base = (obsel & 7u) << 13, gap = (((obsel >> 3) & 3u) + 1u) << 12;
    int sl = (int)FD_(CP_SL);
    Atlas *a;
    Clut *c;
    if (!n)
        return;
    a = atlas(words, cp_sw(si, U_(CP_S_VBASE)) + (page << 15), 1);
    c = clut(cp_sw(si, U_(CP_S_PAL)), 0);
    scissor(x0, r - s_y0, x1, r - s_y0 + 1);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuClutMode(GU_PSM_8888, 0, 0x0F, 8);
    sceGuClutLoad(32, c->c[1]);
    for (i = n; i-- > 0;) {
        U32 e = FD_(CP_SPR) + (at + i) * U_(CP_SE), f = FD_(e + 1u), name = FD_(e + 2u), col;
        I32 sxp = I_(FD_(e)), keep = I_((f >> 18) & 63u) - 1, first = sxp > -8 ? 0 : (-sxp - 8) / 8 + 1;
        U32 row = f & 0xFFu, cols = (f >> 8) & 15u, hflip = (f >> 12) & 1u, pal = (f >> 13) & 7u, prio = (f >> 16) & 3u;
        U32 cc;
        TV tv[16][2];
        int img[16], nt = 0, k, j;
        for (cc = 0; cc < cols; cc++) {
            U32 t, ca;
            int T;
            if (FD_(CP_LIMITS) != 0u && !(keep >= 0 && I_(cc) - first < keep))
                continue;
            if (sl + sxp + I_(cc) * 8 + 8 <= 0)
                continue;
            col = hflip ? cols - 1u - cc : cc;
            t = (((name & 0xF0u) + ((row >> 3) << 4)) & 0xF0u) | ((name + col) & 0x0Fu);
            ca = (base + t * 16u + ((name & 0x100u) != 0u ? gap : 0u)) & 0x7FFFu;
            T = (int)(ca / 16u);
            img[nt] = T >> 10;
            put(tv[nt++], sl + sxp + I_(cc) * 8, r - s_y0, 8, 1, tile_u(T), tile_v(T) + (int)(row & 7u), (int)hflip, 0, 0);
        }
        if (!nt)
            continue;
        room();
        sceGuStencilFunc(GU_ALWAYS, (int)(1u | prio << 1 | (pal >= 4u ? 8u : 0u)), 0xFF);
        sceGuClutMode(GU_PSM_8888, 0, 0x0F, 8u + pal);
        /* a draw for each texture image the tiles are in (they overlap
           nothing of each other: their columns are their own) */
        for (k = 0; k < 2; k++) {
            TV *v;
            int cnt = 0;
            for (j = 0; j < nt; j++)
                cnt += img[j] == k;
            if (!cnt)
                continue;
            sceGuTexImage(0, 128, 512, 128, tile_image(a, 1, k << 10));
            sceGuTexFlush();
            v = (TV *)sceGuGetMemory(cnt * 2 * (int)sizeof(TV));
            for (j = 0, cnt = 0; j < nt; j++)
                if (img[j] == k) {
                    v[2 * cnt] = tv[j][0];
                    v[2 * cnt + 1] = tv[j][1];
                    cnt++;
                }
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, cnt * 2, 0, v);
        }
    }
}

/* whether the frame is one drawn here: its features, and its VRAM pages
   each an atlas */
static int drawable(void)
{
    U32 r, ph = FD_(CP_PH), keys[ATLASES];
    int nkeys = 0;
    if ((FD_(CP_PW) != FD_(CP_SW) && FD_(CP_PW) != 2u * FD_(CP_SW)) || FD_(CP_SW) > PIC_W || ph > PIC_H ||
        FD_(CP_CVON) || FD_(CP_OUT))
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
            if (((mode == 5u || mode == 6u) && (cp_b(si, U_(CP_S_NBA), 3u) & 0x01u)) || cp_b(si, U_(CP_S_FIXED), 3u) ||
                cp_b(si, U_(CP_S_TSW), 3u))
                return 0;
            for (n = 0; n < 5u; n++) {
                U32 key = cp_sw(si, U_(CP_S_VBASE)) + ((n < 4u ? cp_b(si, U_(CP_S_PAGE), n) : cp_b(si, U_(CP_S_PAGE4), 0u)) << 15);
                int j;
                /* (Mode 7's plane read from the VRAM's words: m7_fill) */
                if (!((tmts >> n) & 1u) || (n < 4u && (mode == 7u || !cp_bpp(mode, n))))
                    continue;
                /* a plain layer, or offset-per-tile's (modes 2, 4, 6) or hires
                   (5, 6) without mosaic */
                if (n < 4u && !(cp_st[U_(CP_S_LAYER) + n] & 1u) &&
                    !((((mode == 2u || mode == 4u) && n < 2u) || mode == 5u || mode == 6u) &&
                      !(((cp_b(si, U_(CP_S_INIDISP), 3u) >> n) & 1u) && (cp_b(si, U_(CP_S_INIDISP), 3u) >> 4))))
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
    target(which == 0 ? V_MAIN : which == 1 ? V_SUB : V_SUB2, PIC_W);
    scissor(x0, r0 - s_y0, x1, r1 - s_y0);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuPixelMask(0);
    sceGuClearColor(times4(which == 1 ? fixc : cp_pal(si, 0u)));
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
            sceGuStencilFunc(GU_ALWAYS, own ? (int)((cgadsub >> n) & 1u) : 0, 0xFF);
            ns = spans(si, (int)n, x0, x1, shows, sp);
            for (k = 0; k < ns; k++) {
                scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
                draw_m7(si, (int)n, r0, r1, sp[k][0], sp[k][1]);
            }
        }
    }
    for (n = 0; n < 4u && mode != 7u; n++) {
        if (!cp_bpp(mode, n) || !((on >> n) & 1u))
            continue;
        sceGuStencilFunc(GU_ALWAYS, own ? (int)((cgadsub >> n) & 1u) : 0, 0xFF);
        ns = spans(si, (int)n, x0, x1, shows, sp);
        for (k = 0; k < ns; k++) {
            scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
            draw_bg(si, (int)n, r0, r1, dr0, sp[k][0], sp[k][1], hires == 1 ? (which ? 0 : 1) : -1);
        }
    }
    if ((on >> 4) & 1u) {
        /* the sprite buffer at each priority's depth, its maths' bit its stencil's */
        sceGuDisable(GU_COLOR_TEST);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        ns = spans(si, 4, x0, x1, shows, sp);
        for (p = 0; p < 4; p++)
            for (m = 0; m < 2; m++) {
                U32 z = cp_zobj(mode, (U32)p);
                sceGuAlphaFunc(GU_EQUAL, 1 | p << 1 | m << 3, 0x0F);
                sceGuStencilFunc(GU_ALWAYS, own ? (int)(m && ((cgadsub >> 4) & 1u)) : 0, 0xFF);
                for (k = 0; k < ns; k++) {
                    TV *v;
                    room();
                    v = (TV *)sceGuGetMemory(2 * sizeof(TV));
                    scissor(sp[k][0], r0 - s_y0, sp[k][1], r1 - s_y0);
                    sceGuEnable(GU_TEXTURE_2D);
                    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
                    sceGuTexImage(0, PIC_W, BAND, PIC_W, vram(V_OBJ));
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
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_COLOR_TEST);
}

/* The band's colour maths on a screen's pixels (own) against the other
   screen's (other), then its rows of the picture (out): 0 a pixel each; 1
   a line 512 across that is not hires, each pixel twice; 2 a hires line's
   even pixels (own the sub screen, other the main), the rows cleared and
   their odd pixels marked in the picture's stencil; 3 its odd pixels (own
   the main), its first black (cp_pixel) */
static void finish_band(uint32_t si, int r0, int r1, int x0, int x1, unsigned own, unsigned other, int out)
{
    U32 cgwsel = cp_b(si, U_(CP_S_TSW), 1u), cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    U32 inidisp = cp_b(si, U_(CP_S_INIDISP), 0u), bright = inidisp & 0x0Fu;
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    int sp[130][2], ns, k, y0 = r0 - s_y0, y1 = r1 - s_y0;
    int sc = out ? 2 : 1;
    room();
    target(own, PIC_W);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    /* clip and prevent: their bits set over the colour window's spans */
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_CLIP & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_CLIP, 0xFF);
    ns = spans(si, 0, x0, x1, clipped, sp);
    for (k = 0; k < ns; k++) {
        scissor(sp[k][0], y0, sp[k][1], y1);
        rect(sp[k][0], y0, sp[k][1], y1, 0);
    }
    sceGuPixelMask(0x00FFFFFFu | (uint32_t)(~F_PREVENT & 0xFF) << 24);
    sceGuStencilFunc(GU_ALWAYS, F_PREVENT, 0xFF);
    ns = spans(si, 0, x0, x1, prevented, sp);
    for (k = 0; k < ns; k++) {
        scissor(sp[k][0], y0, sp[k][1], y1);
        rect(sp[k][0], y0, sp[k][1], y1, 0);
    }
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
            rect(x0, y0, x1, y1, times4(fixc));
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
    target(V_PIC, rr_psp_picture_stride);
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
    sceGuTexImage(0, PIC_W, BAND, PIC_W, vram(own));
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

int rr_psp_picture(const uint32_t *desc, size_t words, int *pw, int *ph)
{
    int w, h, s0, r, sw, scale;
    (void)words;
    rr_cp_fd = desc;
    cp_fdb = 0;
    if (!drawable())
        return 0;
    if (!s_to8_made)
        make_to8();
    s_frame++;
    w = (int)FD_(CP_PW);
    h = (int)FD_(CP_PH);
    sw = (int)FD_(CP_SW);
    scale = w / sw;                          /* (2: a frame with a hires line) */
    sceGuStart(GU_DIRECT, s_list);
    sceGuDepthBuffer((void *)V_Z, PIC_W);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(0, 65535);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_FLAT);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    for (s0 = 0; s0 < h; s0 += BAND) {
        int s1 = s0 + BAND < h ? s0 + BAND : h;
        s_y0 = s0;
        /* the sprites, a row (and part) at a time */
        target(V_OBJ, PIC_W);
        scissor(0, 0, sw, BAND);
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
        for (r = s0; r < s1; r++) {
            U32 ri = FD_(CP_ROWS) + 4u * (U32)r, p0 = FD_(ri), np = FD_(ri + 1u), k;
            for (k = 0; k < np; k++) {
                U32 part = p0 + k, si = FD_(FD_(CP_PARTS) + 4u * part + 1u);
                int x0 = (int)(FD_(FD_(CP_PARTS) + 4u * part) + FD_(CP_SL));
                int x1 = k + 1 < np ? (int)(FD_(FD_(CP_PARTS) + 4u * (part + 1u)) + FD_(CP_SL)) : sw;
                cp_load(si);
                if (!(cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u) && ((cp_b(si, U_(CP_S_TM), 1u) | cp_b(si, U_(CP_S_TM), 2u)) & 0x10u))
                    draw_obj_row(si, part, r, x0, x1);
            }
        }
        sceGuDisable(GU_COLOR_TEST);
        sceGuDisable(GU_STENCIL_TEST);
        /* the bands: rows of one part whose states are the same (a
           description has a state for each line, the same or not) */
        for (r = s0; r < s1;) {
            U32 ri = FD_(CP_ROWS) + 4u * (U32)r, p0 = FD_(ri), np = FD_(ri + 1u), k;
            I32 dr0 = I_(FD_(ri + 2u));
            int r1 = r + 1, which;
            if (np == 1u) {
                U32 si = FD_(FD_(CP_PARTS) + 4u * p0 + 1u);
                while (r1 < s1) {
                    U32 rj = FD_(CP_ROWS) + 4u * (U32)r1;
                    U32 sj = FD_(FD_(CP_PARTS) + 4u * FD_(rj) + 1u);
                    if (FD_(rj + 1u) != 1u || I_(FD_(rj + 2u)) != dr0 + (r1 - r) ||
                        (sj != si && memcmp(&FD_(FD_(CP_STATES) + sj * U_(CP_ST)), &FD_(FD_(CP_STATES) + si * U_(CP_ST)),
                                            CP_ST * sizeof(uint32_t))))
                        break;
                    r1++;
                }
            }
            for (k = 0; k < np; k++) {
                U32 part = p0 + k, si = FD_(FD_(CP_PARTS) + 4u * part + 1u);
                int x0 = (int)(FD_(FD_(CP_PARTS) + 4u * part) + FD_(CP_SL));
                int x1 = k + 1 < np ? (int)(FD_(FD_(CP_PARTS) + 4u * (part + 1u)) + FD_(CP_SL)) : sw;
                if (x1 <= x0)
                    continue;
                U32 mode;
                int hires;
                cp_load(si);
                mode = cp_b(si, U_(CP_S_INIDISP), 2u) & 7u;
                hires = scale < 2 ? 0 : mode == 5u || mode == 6u ? 1 : (cp_b(si, U_(CP_S_NBA), 3u) & 8u) ? 2 : 0;
                if (!(cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u))
                    for (which = 0; which < (hires ? 3 : 2); which++)
                        draw_screen(si, which, r, r1, dr0, x0, x1, hires);
                if (hires) {
                    /* the even pixels (the sub screen against the main), then
                       the odd (the main against the sub) */
                    finish_band(si, r, r1, x0, x1, V_SUB2, V_MAIN, 2);
                    finish_band(si, r, r1, x0, x1, V_MAIN, V_SUB, 3);
                } else {
                    finish_band(si, r, r1, x0, x1, V_MAIN, V_SUB, scale > 1 ? 1 : 0);
                }
            }
            r = r1;
        }
    }
    sceGuFinish();
    sceGuSync(0, 0);
    *pw = w;
    *ph = h;
    return 1;
}

const uint16_t *rr_psp_picture_pixels(void) { return (const uint16_t *)((uintptr_t)vram(V_PIC) | 0x40000000u); }
