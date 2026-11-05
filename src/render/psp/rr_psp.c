/*
 * rr_psp.c: the renderer backend on the PSP's GE, drawing into the host's
 * screen buffers (host/psp/host_psp.h).
 *
 * A picture the GE drew (rr_compose, rr_psp_pic.c) is drawn with sharp
 * bilinear (rr_size.h) on the GE, in two stages (scale_ge):
 *
 *   across   each output column's blend of its two columns, by lookups
 *   down     each output row that stage's row, then its next row blended
 *            over it by fixed factors A = round(256 t) and B = 256 - A (at
 *            most 255 each)
 *
 * The GE blends as ((2s + 1)(2A + 1) >> 10) + ((2d + 1)(2B + 1) >> 10)
 * (measured: every value of 256 alphas over three channels). With these
 * factors one blend is within 1 of rr_mix's for every pair of channel
 * values at every weight the presentation makes (checked over 456,964 of
 * them), so the picture is within 1 of rr_ref_image, as every backend's
 * is. The GE's own filtering cannot do it: its bilinear weights are
 * sixteenths, truncated, and two blending stages are within 2.
 *
 * A frame presenting a canvas is drawn at the target's resolution
 * (rr_psp_canvas), its Mode 7 lines at the console's scaled as any picture;
 * overlays are the GE's. A picture made on the CPU is no path here (rr_image,
 * rr_mode7: fatal).
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "nshvc/nshvc.h"
#include "rr.h"
#include "rr_size.h"
#include "rr_mode7.h"
#include "host.h"
#include "host_psp.h"
#include "rr_psp_pic.h"

#define TW 480
#define TH 272
#define STRIDE HOST_PSP_STRIDE

static unsigned int __attribute__((aligned(64))) s_list[128 * 1024];
static const HostPspScreen *s_screen;
static void *s_fb;                           /* the frame's buffer (the GE's address) */
static int s_listing;                        /* a display list is open */
static int s_on;                             /* the frame's overlays so far (rr_overlay) */

static uint32_t *uncached(void *p) { return (uint32_t *)((uintptr_t)p | 0x40000000u); }
static void *vram(unsigned off) { return (char *)sceGeEdramGetAddr() + off; }
static uint32_t abgr(uint32_t c) { return (c & 0xFF00FF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16; }

int rr_open(void)
{
    if (s_screen)
        return 1;
    if (!host_window_handle())
        host_window_open(256, 224, "");
    s_screen = (const HostPspScreen *)host_window_handle();
    if (!s_screen)
        return 0;
    sceGuInit();
    /* the size the GU's clears cover (its display: the host's own) */
    sceGuDispBuffer(TW, TH, (void *)0, STRIDE);
    return 1;
}

void rr_close(void)
{
    if (s_screen)
        sceGuTerm();
    s_screen = NULL;
}

const char *rr_name(void) { return "psp"; }

void rr_target(int *w, int *h)
{
    *w = TW;
    *h = TH;
}

static void list_open(void)
{
    if (s_listing)
        return;
    s_screen->ge_back();
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)((char *)s_fb - (char *)sceGeEdramGetAddr()), STRIDE);
    sceGuOffset(2048 - TW / 2, 2048 - TH / 2);
    sceGuViewport(2048, 2048, TW, TH);
    sceGuScissor(0, 0, TW, TH);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuShadeModel(GU_FLAT);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    s_listing = 1;
}

/* the GE's work so far done, for the CPU to draw on the frame */
static void list_done(void)
{
    if (!s_listing)
        return;
    sceGuFinish();
    sceGuSync(0, 0);
    s_listing = 0;
}

void rr_begin(uint32_t argb)
{
    if (!s_screen)
        return;
    list_done();
    s_on = 0;                                /* (the overlays' textures free: the GE done) */
    s_fb = s_screen->target();
    list_open();
    sceGuClearColor(abgr(argb | 0xFF000000u));
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

/* ---- a picture, sharp bilinear ---- */
typedef struct {
    float u, v, x, y, z;
} Vtx;

typedef struct {
    uint32_t c;
    short x, y, z;
} CVtx;

/* an app's image: the console draws none (its pictures are the GE's, drawn
   from their descriptions), and the PSP scales one only as the GE draws
   the picture */
void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    (void)argb;
    (void)d;
    nshvc_fatal("rr_image(%dx%d): the PSP draws no picture made on the CPU", w, h);
}

/* ---- a picture on the GE, scaled there ---- */
/* The stage across: the reference's blend of a and b at weight t (rr_mix)
 * is, for the console's colours (each channel 5 bits made 8: a picture's),
 * a + H+(b - a) where b >= a and a - H-(a - b) where a > b, each H a
 * function of the difference, but for the pairs where (b - a) t lands
 * within a float's step of a half and a's size decides the rounding
 * (40 of a 4:3 window's): those a table's exceptions, each 1 either way.
 * Every pair of the 32 values is taken at each weight a geometry makes.
 * On the GE then, in strips of rows: the differences (subtractions by
 * factors 255, clamped at 0: exact) through the tables (palette lookups, a
 * channel at a time) added to a copy of a, and subtracted (factors 255
 * again: exact), a draw for each table's columns; each exception's pixels
 * found by the colour test on a then b (a stencil mark each) and moved its
 * 1; then the stage down as rr_image's. */
/* a strip's buffers (in VRAM past the picture: free once it is drawn),
   each its rows (the strip's and the next one's first) of the visible
   columns, s_g->stride apart: the stage across's, b - a, a - b */
#define V_I RR_PSP_V_SCRATCH
#define V_TP (V_I + s_g->stride * (s_g->rows + 1) * 4)
#define V_TM (V_TP + s_g->stride * (s_g->rows + 1) * 4)

typedef struct {
    short u, v, x, y, z;
} TV;

typedef struct {
    int key[6];                              /* pw, ph, d */
    unsigned used;                           /* when it was last asked for */
    int stride, rows, tex_w, tex_h;          /* the strip buffers' row in pixels, a strip's rows, their
                                                size as a texture (powers of 2) */
    int ok, X0, n, ntab;
    int *tx0, *tx1, *tab;                    /* each visible column's pair, and its table (-1: a alone) */
    uint32_t (*clut)[2][256];                /* each table: H+, then H-, each in all three channels'
                                                bytes (the pixel mask keeps the one looked up) */
    int (*ex)[48][3];                        /* each table's exceptions: a, b, the move (1 or -1) */
    int *nex;
    int *cols, *first, *count;               /* each table's columns: cols[first .. first + count) */
    unsigned int *calls[32];                 /* each strip's stage across, a list the frame calls */
    unsigned int *down[32];                  /* and its stage down, into the frame (into() before it) */
} Geometry;

static Geometry s_gs[2], *s_g;               /* the two geometries asked for last; the one in use */
static unsigned s_asked;

static void make_calls(int ph, const int d[4]);

/* the geometry pw x ph over d: its columns and tables; 0 when its stage
   across cannot be the GE's (above) */
static int geometry(int pw, int ph, const int d[4])
{
    int key[6] = { pw, ph, d[0], d[1], d[2], d[3] }, X1, x, i, k, n, cap = 1;
    float dx;
    s_asked++;
    for (i = 0; i < 2; i++)
        if (s_gs[i].tx0 && !memcmp(key, s_gs[i].key, sizeof key)) {
            s_g = &s_gs[i];
            s_g->used = s_asked;
            return s_g->ok;
        }
    s_g = &s_gs[s_gs[0].used <= s_gs[1].used ? 0 : 1];
    free(s_g->tx0);
    free(s_g->tx1);
    free(s_g->tab);
    free(s_g->clut);
    free(s_g->cols);
    free(s_g->first);
    free(s_g->count);
    free(s_g->ex);
    free(s_g->nex);
    for (i = 0; i < 32; i++) {
        free(s_g->calls[i]);
        free(s_g->down[i]);
    }
    memset(s_g, 0, sizeof *s_g);
    memcpy(s_g->key, key, sizeof key);
    s_g->used = s_asked;
    s_g->X0 = d[0] < 0 ? 0 : d[0];
    X1 = d[2] > TW ? TW : d[2];
    n = s_g->n = X1 - s_g->X0;
    if (n <= 0 || pw > 512 || ph > 480)          /* (a woven picture 480 rows) */
        return 0;
    s_g->stride = (n + 63) & ~63;
    s_g->rows = (RR_PSP_V_END - RR_PSP_V_SCRATCH) / (3 * s_g->stride * 4) - 1;
    s_g->rows = s_g->rows > 32 ? 32 : s_g->rows;
    for (s_g->tex_h = 1; s_g->tex_h < s_g->rows + 1; s_g->tex_h <<= 1)
        ;
    for (s_g->tex_w = 1; s_g->tex_w < s_g->stride; s_g->tex_w <<= 1)
        ;
    s_g->tx0 = (int *)malloc((size_t)n * sizeof(int));
    s_g->tx1 = (int *)malloc((size_t)n * sizeof(int));
    s_g->tab = (int *)malloc((size_t)n * sizeof(int));
    s_g->clut = (uint32_t(*)[2][256])memalign(64, sizeof *s_g->clut);   /* a table: grown as they are found */
    s_g->cols = (int *)malloc((size_t)n * sizeof(int));
    s_g->first = (int *)calloc((size_t)n, sizeof(int));
    s_g->count = (int *)calloc((size_t)n, sizeof(int));
    s_g->ex = (int(*)[48][3])malloc((size_t)n * sizeof *s_g->ex);
    s_g->nex = (int *)calloc((size_t)n, sizeof(int));
    if (!s_g->tx0 || !s_g->tx1 || !s_g->tab || !s_g->clut || !s_g->cols || !s_g->first || !s_g->count || !s_g->ex || !s_g->nex)
        nshvc_fatal("no memory for a picture %d wide", n);
    dx = (float)pw / (float)(d[2] - d[0]);
    {
    /* each column's table by its weight W (rr_mix in whole 256ths): rr_mix
       is a + ((b - a) W + 128 >> 8) a channel, so H+(d) = (d W + 128) >> 8
       and H-(d) = -((-d W + 128) >> 8), exactly, no exceptions; one table a
       W met (a W of 0, a alone) */
    int wtab[257], W;
    for (W = 0; W < 257; W++)
        wtab[W] = -1;
    for (x = 0; x < n; x++) {
        int tx;
        float t = rr_sharp(((float)(s_g->X0 + x - d[0]) + 0.5f) * dx, dx, &tx);
        s_g->tx0[x] = tx < 0 ? 0 : tx >= pw ? pw - 1 : tx;
        s_g->tx1[x] = tx + 1 < 0 ? 0 : tx + 1 >= pw ? pw - 1 : tx + 1;
        s_g->tab[x] = -1;
        if (!(t > 0) || s_g->tx0[x] == s_g->tx1[x])
            continue;
        W = rr_mix_w(t);
        if (W <= 0)
            continue;
        if (W > 256)                             /* (t within 1: rr_sharp's) */
            return 0;
        if (wtab[W] < 0) {
            k = s_g->ntab;
            if (k + 1 > cap) {
                /* room for more tables: twice as many */
                uint32_t (*c)[2][256] = (uint32_t(*)[2][256])memalign(64, (size_t)(2 * cap) * sizeof *c);
                if (!c)
                    nshvc_fatal("no memory for a picture %d wide", n);
                memcpy(c, s_g->clut, (size_t)k * sizeof *c);
                free(s_g->clut);
                s_g->clut = c;
                cap *= 2;
            }
            for (i = 0; i < 256; i++) {
                uint32_t p = (uint32_t)((i * W + 128) >> 8), m = (uint32_t)(-((-i * W + 128) >> 8));
                s_g->clut[k][0][i] = p | p << 8 | p << 16;
                s_g->clut[k][1][i] = m | m << 8 | m << 16;
            }
            s_g->nex[k] = 0;
            wtab[W] = s_g->ntab++;
        }
        s_g->tab[x] = wtab[W];
        s_g->count[wtab[W]]++;
    }
    }
    /* each table's columns together */
    for (i = 1; i < s_g->ntab; i++)
        s_g->first[i] = s_g->first[i - 1] + s_g->count[i - 1];
    {
        int *fill = (int *)malloc((size_t)(s_g->ntab ? s_g->ntab : 1) * sizeof(int));
        if (!fill)
            nshvc_fatal("no memory for a picture %d wide", n);
        memcpy(fill, s_g->first, (size_t)s_g->ntab * sizeof(int));
        for (x = 0; x < n; x++)
            if (s_g->tab[x] >= 0)
                s_g->cols[fill[s_g->tab[x]]++] = x;
        free(fill);
    }
    sceKernelDcacheWritebackRange(s_g->clut, (unsigned)((size_t)s_g->ntab * sizeof *s_g->clut));
    make_calls(ph, d);
    s_g->ok = 1;
    return 1;
}

/* the display list begun again before it could fill, drawing into the
   buffer at `off` (sceGuStart sets the GU's own again) */
static unsigned s_cur_off;
static int s_cur_w;

static void into(unsigned off, int w)
{
    s_cur_off = off;
    s_cur_w = w;
    sceGuDrawBufferList(GU_PSM_8888, (void *)off, w);
}

static int s_building;                       /* a strip's call list being made: the room it is made in */
static int s_built;                          /* (the bytes of call lists made before it) */

static void room(void)
{
    if (s_building) {
        if (s_built + sceGuCheckList() > s_building - 16 * 1024)
            nshvc_fatal("the picture's scaling: its lists past their %d bytes", s_building);
        return;
    }
    if (sceGuCheckList() > (int)sizeof s_list - 64 * 1024) {
        sceGuFinish();
        sceGuSync(0, 0);
        sceGuStart(GU_DIRECT, s_list);
        into(s_cur_off, s_cur_w);
    }
}

/* columns cols[0..k) of the strip (rows 0..rs) from the picture's columns
   src[col] (rows s0..), or from a strip buffer's own (src NULL) */
static void column_sprites(const int *cols, int k, const int *src, int s0, int rs)
{
    TV *v;
    int i;
    if (k <= 0)
        return;
    room();
    v = (TV *)sceGuGetMemory(k * 2 * (int)sizeof(TV));
    for (i = 0; i < k; i++) {
        int x = cols ? cols[i] : i, u = src ? src[x] : x;
        v[2 * i].u = (short)u;
        v[2 * i].v = (short)(src ? s0 : 0);
        v[2 * i].x = (short)x;
        v[2 * i].y = 0;
        v[2 * i].z = 0;
        v[2 * i + 1].u = (short)(u + 1);
        v[2 * i + 1].v = (short)((src ? s0 : 0) + rs);
        v[2 * i + 1].x = (short)(x + 1);
        v[2 * i + 1].y = (short)rs;
        v[2 * i + 1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k * 2, 0, v);
}

/* solid columns cols[0..k) of the strip, rows 0..rs, in colour c */
static void column_rects(const int *cols, int k, int rs, uint32_t c)
{
    struct {
        uint32_t c;
        short x, y, z;
    } *v;
    int i;
    if (k <= 0)
        return;
    room();
    v = sceGuGetMemory(k * 2 * (int)sizeof *v);
    for (i = 0; i < k; i++) {
        v[2 * i].c = c;
        v[2 * i].x = (short)cols[i];
        v[2 * i].y = 0;
        v[2 * i].z = 0;
        v[2 * i + 1].c = c;
        v[2 * i + 1].x = (short)(cols[i] + 1);
        v[2 * i + 1].y = (short)rs;
        v[2 * i + 1].z = 0;
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k * 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

static void picture_texture(void)
{
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, 512, 512, rr_psp_picture_stride, vram(RR_PSP_V_PICTURE));
    sceGuTexFlush();
}

/* the stage across of the strip from picture row s0 (rs rows): what a call
   list of the geometry's holds, the same every frame */
static void strip_across(int s0, int rs)
{
    int n = s_g->n, i, c;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuPixelMask(0);
    {
        sceGuScissor(0, 0, n, rs);
        /* b - a, a - b, a */
        into(V_TP, s_g->stride);
        picture_texture();
        sceGuDisable(GU_BLEND);
        column_sprites(NULL, n, s_g->tx1, s0, rs);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
        column_sprites(NULL, n, s_g->tx0, s0, rs);
        into(V_TM, s_g->stride);
        sceGuDisable(GU_BLEND);
        column_sprites(NULL, n, s_g->tx0, s0, rs);
        sceGuEnable(GU_BLEND);
        column_sprites(NULL, n, s_g->tx1, s0, rs);
        into(V_I, s_g->stride);
        sceGuDisable(GU_BLEND);
        column_sprites(NULL, n, s_g->tx0, s0, rs);
        /* each table's columns: H+(b - a) added, H-(a - b) taken away, a
           channel a lookup */
        sceGuEnable(GU_BLEND);
        for (c = 0; c < 6; c++) {
            sceGuBlendFunc(c < 3 ? GU_ADD : GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
            sceGuPixelMask(~(0xFFu << (8 * (c % 3))));
            sceGuTexMode(GU_PSM_T32, 0, 0, 0);
            sceGuTexImage(0, s_g->tex_w, s_g->tex_h, s_g->stride, vram(c < 3 ? V_TP : V_TM));
            sceGuTexFlush();
            sceGuClutMode(GU_PSM_8888, (unsigned)(8 * (c % 3)), 0xFF, 0);
            for (i = 0; i < s_g->ntab; i++) {
                room();
                sceGuClutLoad(32, s_g->clut[i][c / 3]);
                column_sprites(s_g->cols + s_g->first[i], s_g->count[i], NULL, 0, rs);
            }
        }
        sceGuPixelMask(0);
        /* the exceptions: in each channel, a's pixels marked, those with b
           marked again, those moved */
        {
            int mark = 0, e;
            sceGuEnable(GU_STENCIL_TEST);
            sceGuClearStencil(0);
            sceGuClear(GU_STENCIL_BUFFER_BIT);
            for (i = 0; i < s_g->ntab; i++)
                for (e = 0; e < s_g->nex[i]; e++)
                    for (c = 0; c < 3; c++) {
                        const int *x = s_g->ex[i][e];
                        unsigned m = 0xFFu << (8 * c);
                        if (mark + 2 > 255) {
                            sceGuClear(GU_STENCIL_BUFFER_BIT);
                            mark = 0;
                        }
                        room();
                        sceGuDisable(GU_BLEND);
                        sceGuPixelMask(0x00FFFFFFu);
                        sceGuEnable(GU_COLOR_TEST);
                        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
                        picture_texture();
                        sceGuColorFunc(GU_EQUAL, (unsigned)x[0] << (8 * c), m);
                        sceGuStencilFunc(GU_ALWAYS, mark + 1, 0xFF);
                        column_sprites(s_g->cols + s_g->first[i], s_g->count[i], s_g->tx0, s0, rs);
                        /* those of a with b: mark + 1 made mark + 2 */
                        sceGuColorFunc(GU_EQUAL, (unsigned)x[1] << (8 * c), m);
                        sceGuStencilFunc(GU_EQUAL, mark + 1, 0xFF);
                        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
                        column_sprites(s_g->cols + s_g->first[i], s_g->count[i], s_g->tx1, s0, rs);
                        sceGuDisable(GU_COLOR_TEST);
                        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
                        sceGuStencilFunc(GU_EQUAL, mark + 2, 0xFF);
                        sceGuPixelMask(0xFF000000u);
                        sceGuEnable(GU_BLEND);
                        sceGuBlendFunc(x[2] > 0 ? GU_ADD : GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
                        column_rects(s_g->cols + s_g->first[i], s_g->count[i], rs, 1u << (8 * c));
                        mark += 2;
                    }
            sceGuDisable(GU_STENCIL_TEST);
            sceGuPixelMask(0);
        }
    }
    sceGuDisable(GU_BLEND);
}

/* strip s0's (rs rows) call list into buf (cap bytes); its bytes */
static void strip_down(int s0, int ph, const int d[4]);

static int build_call(unsigned int *buf, int cap, int s0, int rs, int ph, const int *down)
{
    int used;
    s_built = 0;
    s_building = cap;
    sceGuStart(GU_CALL, buf);
    if (down)
        strip_down(s0, ph, down);
    else
        strip_across(s0, rs);
    used = sceGuCheckList();
    sceGuFinish();
    s_building = 0;
    return (used + 16 + 63) & ~63;           /* (its return, rounded to a cache line) */
}

/* each strip's stages across and down made call lists: the geometry's,
   once (the stage down is the same each frame but for the frame it draws
   into, which the frame sets before it calls the list). A list
   holds its own addresses (the GU's jumps over the vertices), so it is made
   where it stays: once in room for the most it could take (each draw's
   vertices, 20 bytes a column, and 64 bytes for each setting of state and
   each draw: the GU's commands are 4 to 16), measured, then again in just
   its own room. */
static void make_calls(int ph, const int d[4])
{
    int i, k = 0, s0, most = 0, bytes, j;
    unsigned int *tmp;
    most += 5 * (s_g->n * 20 + 64) + 16 * 64;                    /* the copies, the state between */
    most += 6 * (8 * 64);                                         /* the lookups' state, */
    for (i = 0; i < s_g->ntab; i++)                               /* each table's palette and columns */
        most += 6 * (2 * 64 + s_g->count[i] * 20);
    for (i = 0; i < s_g->ntab; i++)                               /* the exceptions: their three draws, state */
        most += s_g->nex[i] * 3 * (16 * 64 + s_g->count[i] * (20 + 20 + 24));
    most += 4096 + 16 * 1024;                                     /* (and the most one step adds: room() looks before it) */
    if (most < TH * 2 * 512 + 4096 + 16 * 1024)                   /* the stage down: each frame row's two draws */
        most = TH * 2 * 512 + 4096 + 16 * 1024;
    tmp = (unsigned int *)memalign(64, (size_t)most);
    if (!tmp)
        nshvc_fatal("no memory for the picture's scaling (%d bytes)", most);
    for (s0 = 0; s0 < ph; s0 += s_g->rows, k++) {
        int rs = ph - s0 < s_g->rows + 1 ? ph - s0 : s_g->rows + 1;
        for (j = 0; j < 2; j++) {
            unsigned int **at = j ? &s_g->down[k] : &s_g->calls[k];
            const int *down = j ? d : NULL;
            bytes = build_call(tmp, most, s0, rs, ph, down);
            *at = (unsigned int *)memalign(64, (size_t)bytes);
            if (!*at)
                nshvc_fatal("no memory for the picture's scaling (%d bytes)", bytes);
            if (build_call(*at, bytes + 16 * 1024 > most ? most : bytes + 16 * 1024, s0, rs, ph, down) > bytes)
                nshvc_fatal("the picture's scaling: a list larger the second time");
            sceKernelDcacheWritebackRange(*at, (unsigned)bytes);
        }
    }
    free(tmp);
}

/* a strip's stage down: the frame's rows whose upper row is in the strip
   (the frame drawn into set before) */
static void strip_down(int s0, int ph, const int d[4])
{
    int n = s_g->n, X0 = s_g->X0, Y0 = d[1] < 0 ? 0 : d[1], Y1 = d[3] > TH ? TH : d[3], y, i;
    float dy = (float)ph / (float)(d[3] - d[1]);
    {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuEnable(GU_SCISSOR_TEST);        /* (the GU sends a scissor only with its test on) */
        sceGuScissor(0, 0, TW, TH);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        for (y = Y0; y < Y1; y++) {
            int ty, A, B, r0, r1;
            float fy = rr_sharp(((float)(y - d[1]) + 0.5f) * dy, dy, &ty);
            r0 = ty < 0 ? 0 : ty >= ph ? ph - 1 : ty;
            r1 = ty + 1 < 0 ? 0 : ty + 1 >= ph ? ph - 1 : ty + 1;
            if (r0 < s0 || r0 >= s0 + s_g->rows)
                continue;
            room();
            for (i = 0; i < 2; i++) {
                Vtx *v;
                if (i && !(fy > 0 && r1 != r0))
                    break;
                if (!i) {
                    sceGuDisable(GU_BLEND);
                } else {
                    A = (int)floorf(256.0f * fy + 0.5f);
                    A = A > 255 ? 255 : A;
                    B = 256 - A > 255 ? 255 : 256 - A;
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (unsigned)A * 0x010101u, (unsigned)B * 0x010101u);
                }
                sceGuTexImage(0, s_g->tex_w, 1, s_g->stride, (char *)vram(V_I) + (size_t)((i ? r1 : r0) - s0) * s_g->stride * 4);
                sceGuTexFlush();
                v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
                v[0].u = 0;
                v[0].v = 0;
                v[0].x = (float)X0;
                v[0].y = (float)y;
                v[0].z = 0;
                v[1].u = (float)n;
                v[1].v = 1;
                v[1].x = (float)(X0 + n);
                v[1].y = (float)(y + 1);
                v[1].z = 0;
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
            }
        }
    }
}

static void scale_ge(int pw, int ph, const int d[4])
{
    unsigned fb = (unsigned)((char *)s_fb - (char *)sceGeEdramGetAddr());
    int s0, k = 0;
    (void)pw;
    (void)d;
    for (s0 = 0; s0 < ph; s0 += s_g->rows, k++) {
        room();
        sceGuCallList(s_g->calls[k]);
        into(fb, STRIDE);
        sceGuCallList(s_g->down[k]);
    }
    sceGuDisable(GU_BLEND);
}

/* the picture on the GE (rr_psp_pic.c), scaled on the GE where the
   geometry allows (scale_ge), else read back and presented as rr_image
   presents one */
static int present_picture(int pw, int ph, const int d[4]);

int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    int pw, ph;
    (void)w;
    (void)h;
    if (!s_screen)
        return 0;
    list_done();
    if (!rr_psp_picture(desc, words, &pw, &ph))
        return 0;
    return present_picture(pw, ph, d);
}

/* the picture drawn (rr_psp_pic.c), pw x ph: scaled on the GE where the
   geometry allows (scale_ge), else read back and presented as rr_image
   presents one */
static int present_picture(int pw, int ph, const int d[4])
{
    if (!(d[2] > d[0] && d[3] > d[1]) || !geometry(pw, ph, d))
        nshvc_fatal("a picture of %dx%d over (%d, %d)-(%d, %d): not on the PSP's screen", pw, ph, d[0], d[1], d[2], d[3]);
    list_open();
    s_cur_off = (unsigned)((char *)s_fb - (char *)sceGeEdramGetAddr());
    s_cur_w = STRIDE;
    scale_ge(pw, ph, d);
    return 1;
}

int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4])
{
    if (!s_screen)
        return 0;
    list_done();
    if (!rr_psp_picture_woven(desc, words, ww, wh, base0, base1))
        return 0;
    return present_picture(ww, wh, d);
}

/* Mode 7 presented as any frame: the picture at the console's resolution
   (rr_psp_pic.c draws Mode 7's lines), scaled; a canvas (with no Mode 7
   line) at the screen's resolution (rr_mode7) */
/* a canvas frame at the target's resolution, its Mode 7 lines at the
   console's (rr_mode7_plain); a frame of Mode 7 alone as any frame */
int rr_mode7_window(int what) { return (what & 6) || !(what & 1); }
int rr_mode7_plain(void) { return 1; }
/* (rr_psp_canvas reads a screen canvas's texels from the record) */
int rr_compose_canvas(void) { return 0; }

/* the PSP's screen: its picture's buffer (rr_psp_pic.c), 512 x 240 (a hires
   or interlaced frame 256 across) */
void rr_screen_max(int *w, int *h)
{
    *w = 512;
    *h = 240;
}

/* a canvas's texels: a texture's, 512 x 512 (the GE's largest) */
void rr_canvas_max(int *w, int *h)
{
    *w = 512;
    *h = 512;
}

int rr_psp_across(int pw, int ph, const int d[4], RrPspAcross *a)
{
    if (!(d[2] > d[0] && d[3] > d[1]) || !geometry(pw, ph, d))
        return 0;
    a->n = s_g->n;
    a->X0 = s_g->X0;
    a->ntab = s_g->ntab;
    a->tx0 = s_g->tx0;
    a->tx1 = s_g->tx1;
    a->cols = s_g->cols;
    a->first = s_g->first;
    a->count = s_g->count;
    a->nex = s_g->nex;
    a->clut = (const uint32_t(*)[2][256])s_g->clut;
    a->ex = (const int(*)[48][3])s_g->ex;
    return 1;
}

/* a canvas frame: on the GE where it is drawn there (rr_psp_canvas), else
   the reference's drawing (rr_mode7) */
int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s)
{
    RrPspAcross a;
    if (!s_screen)
        return 0;
    list_done();
    if (!rr_psp_across(m->pw, m->ph, d, &a))
        return 0;
    return rr_psp_canvas(desc, words, d, (unsigned)((char *)s_fb - (char *)sceGeEdramGetAddr()), &a, m, s);
}

/* a frame's records presented at the target's resolution: the PSP's are
   its compositor's (rr_compose_mode7) */
void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    (void)m;
    (void)d;
    (void)s;
    nshvc_fatal("rr_mode7: the PSP presents a frame's records from its compositor alone");
}

/* An overlay on the GE (rr_ref_overlay's): the image in a texture of
   powers of 2, its last column and row repeated past it (the bilinear
   filter clamped at its edge as rr_texel clamps), its parallelogram two
   triangles, filtered bilinear and blended by its alpha, the constant alpha
   the vertices' (the texture modulated by them, white). Each overlay of a
   frame its own texture until the GE has drawn it. */
#define OVERLAYS 16
static uint32_t *s_obuf[OVERLAYS];
static size_t s_ocap[OVERLAYS];

typedef struct {
    float u, v;
    uint32_t c;
    float x, y, z;
} OV;

void rr_overlay(const uint32_t *argb, int w, int h, const float q[6], float alpha)
{
    const float uv[4] = { 0, 0, (float)w, (float)h };
    rr_overlay_quads(argb, w, h, 1, uv, q, alpha);
}

static void over_draw(int W, int H, int tbw, const void *t, int nq, const float *uv, const float *q, float alpha);

/* VRAM the game drew, as a texture the GE reads (rr.h) */
int rr_ge_picture(const char *what, const void *vram, int stride, int w, int h, int psm)
{
    uintptr_t base = (uintptr_t)sceGeEdramGetAddr(), a = (uintptr_t)vram;
    size_t buf = HOST_PSP_VRAM_USED / 3, off = a - base, bpp = psm == GU_PSM_8888 ? 4u : 2u, row = (size_t)stride * bpp;
    if (psm != GU_PSM_8888 && psm != GU_PSM_4444)
        nshvc_fatal("%s: psm %d: GU_PSM_8888, or a character canvas's GU_PSM_4444", what, psm);
    if (w < 1 || h < 1 || w > 512 || h > 512 || stride < w || (row & 15) || (a & 15) || a < base ||
        off + row * (size_t)h > HOST_PSP_VRAM_USED || off / buf != (off + row * (size_t)h - 1) / buf)
        nshvc_fatal("%s: %d x %d at %p, %d pixels a row: 16 bytes aligned, each row too, at most 512 x 512, "
                    "in one of the screen's buffers (nshvc_ext_ge_vram)", what, w, h, vram, stride);
    return 1;
}

/* an overlay's image in the GE's VRAM: read as it stands, the column and row
   past it the game's (nshvc_ext_overlay_gpu) */
void rr_overlay_gpu(uint64_t image, uint64_t view, int w, int h, const float q[6], float alpha, uint64_t event,
                    uint64_t value)
{
    const float uv[4] = { 0, 0, (float)w, (float)h };
    int W, H;
    (void)event;
    (void)value;
    if (!s_screen || w <= 0 || h <= 0)
        return;
    for (W = 4; W < w + 1; W <<= 1)
        ;
    for (H = 1; H < h + 1; H <<= 1)
        ;
    over_draw(W, H, (int)view, (const void *)(uintptr_t)image, 1, uv, q, alpha);
}

void rr_overlay_quads(const uint32_t *argb, int w, int h, int nq, const float *uv, const float *q, float alpha)
{
    int W, H, x, y;
    uint32_t *t;
    if (!s_screen || w <= 0 || h <= 0 || nq <= 0)
        return;
    if (w > 511 || h > 511)
        nshvc_fatal("an overlay of %dx%d (511 the PSP's most)", w, h);
    if (s_on == OVERLAYS) {
        /* (every texture this frame's: the GE done with them) */
        list_done();
        s_on = 0;
    }
    /* (a texture's rows at least 16 bytes apart: 4 texels) */
    for (W = 4; W < w + 1; W <<= 1)
        ;
    for (H = 1; H < h + 1; H <<= 1)
        ;
    if (s_ocap[s_on] < (size_t)W * (size_t)H) {
        free(s_obuf[s_on]);
        s_obuf[s_on] = (uint32_t *)memalign(64, (size_t)W * (size_t)H * 4);
        s_ocap[s_on] = s_obuf[s_on] ? (size_t)W * (size_t)H : 0;
        if (!s_obuf[s_on])
            nshvc_fatal("no memory for an overlay of %dx%d", w, h);
    }
    t = s_obuf[s_on++];
    for (y = 0; y <= h; y++) {
        const uint32_t *src = argb + (size_t)(y < h ? y : h - 1) * (size_t)w;
        uint32_t *o = t + (size_t)y * (size_t)W;
        for (x = 0; x < w; x++)
            o[x] = abgr(src[x]);
        o[w] = o[w - 1];
    }
    sceKernelDcacheWritebackRange(t, (unsigned)((size_t)W * (size_t)(h + 1) * 4));
    over_draw(W, H, W, t, nq, uv, q, alpha);
}

/* nq parallelograms from texture t (W x H, tbw texels a row), filtered,
   blended by its alpha times the constant's */
static void over_draw(int W, int H, int tbw, const void *t, int nq, const float *uv, const float *q, float alpha)
{
    int i, j;
    uint32_t vc = 0x00FFFFFFu | (uint32_t)(alpha * 255.0f + 0.5f) << 24;
    OV *v;
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4];
    list_open();
    v = (OV *)sceGuGetMemory(nq * 6 * (int)sizeof(OV));
    for (j = 0; j < nq; j++) {
        const float *qq = q + 6 * j, *u = uv + 4 * j;
        c[0][0] = qq[0], c[0][1] = qq[1], c[0][2] = u[0], c[0][3] = u[1];
        c[1][0] = qq[2], c[1][1] = qq[3], c[1][2] = u[2], c[1][3] = u[1];
        c[2][0] = qq[4], c[2][1] = qq[5], c[2][2] = u[0], c[2][3] = u[3];
        c[3][0] = qq[2] + qq[4] - qq[0], c[3][1] = qq[3] + qq[5] - qq[1], c[3][2] = u[2], c[3][3] = u[3];
        /* (the GE takes a pixel at 7/16 of it, its centre less one of its
           sixteenths; the reference at its centre: the parallelogram moved a
           sixteenth up and left) */
        for (i = 0; i < 6; i++) {
            v[6 * j + i].c = vc;
            v[6 * j + i].x = c[ix[i]][0] - 1.0f / 16.0f;
            v[6 * j + i].y = c[ix[i]][1] - 1.0f / 16.0f;
            v[6 * j + i].z = 0;
            v[6 * j + i].u = c[ix[i]][2];
            v[6 * j + i].v = c[ix[i]][3];
        }
    }
    sceGuScissor(0, 0, TW, TH);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, W, H, tbw, t);
    sceGuTexFlush();
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, nq * 6, 0, v);
    sceGuDisable(GU_BLEND);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
}

void rr_present(void)
{
    if (!s_screen)
        return;
    list_done();
    s_screen->show();
}

/* The pause's backdrop on the GE: the frame shown halved n times, each by
   the filter over four texels (a level a buffer past the screen's, its
   rows a power of 2 apart), magnified back over the screen by the filter
   (rr_backdrop_draw). A box of 2^n pixels and then a triangle of 2^n either
   side: standard deviation 2^n / 2, so 2^n is about the box blurs' (rr.h:
   4.47 cells of radius / 4) twice over. The tint laid over the last level
   by the blend. The buffers free while paused (no picture is drawn). */
static unsigned s_bd_off;
static int s_bd_w, s_bd_h, s_bd_tw, s_bd_on, s_bd_psm;
static float s_bd_u, s_bd_v;                 /* the screen's size on the last level */

static void bd_sprite(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    v[0].u = u0;
    v[0].v = v0;
    v[0].x = x0 - 1.0f / 16.0f;              /* (the GE's sample at 7/16 of a pixel: rr_overlay's) */
    v[0].y = y0 - 1.0f / 16.0f;
    v[0].z = 0;
    v[1].u = u1;
    v[1].v = v1;
    v[1].x = x1 - 1.0f / 16.0f;
    v[1].y = y1 - 1.0f / 16.0f;
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static int pow2_at(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

int rr_backdrop_make(float radius_px, uint32_t tint)
{
    const void *src;
    unsigned off = RR_PSP_V_PICTURE;
    int w = TW, h = TH, tbw = STRIDE, n = 0, k;
    float want = 2.24f * radius_px;
    if (!s_screen)
        return 0;
    while ((float)(3 << n) < 2.0f * want && n < 6)   /* (the nearest power of 2: past 2^n x 1.5) */
        n++;
    list_done();
    list_open();
    src = (const char *)s_screen->shown();
    /* (the GE's drawing region the buffers' 512, not the display's 480:
       REGION1, REGION2) */
    sceGuSendCommandi(21, 0);
    sceGuSendCommandi(22, (511 << 10) | 511);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* (no blur: the frame itself, 16 bits a pixel, the VRAM past the
       screens' its size at 16; one to one, no filter) */
    sceGuTexFilter(n ? GU_LINEAR : GU_NEAREST, n ? GU_LINEAR : GU_NEAREST);
    s_bd_psm = n ? GU_PSM_8888 : GU_PSM_5650;
    for (k = 0; k < (n ? n : 1); k++) {
        int w2 = n ? (w + 1) / 2 : w, h2 = n ? (h + 1) / 2 : h, t2 = pow2_at(w2 + 1);
        int uw = n ? 2 * w2 : w2, vh = n ? 2 * h2 : h2;   /* (a level's own edge copy reads to w, h: past them when odd) */
        sceGuDrawBufferList(s_bd_psm, (void *)off, t2);
        sceGuScissor(0, 0, w2 + 1, h2 + 1);
        sceGuTexImage(0, pow2_at(w + 1), pow2_at(h + 1), tbw, src);
        sceGuTexFlush();
        if (!n) {
            bd_sprite(1.0f / 16.0f, 1.0f / 16.0f, (float)w2 + 1.0f / 16.0f, (float)h2 + 1.0f / 16.0f, 0, 0, (float)uw, (float)vh);
            s_bd_off = off;
            s_bd_w = w2;
            s_bd_h = h2;
            s_bd_tw = t2;
            break;
        }
        bd_sprite(0, 0, (float)w2, (float)h2, 0, 0, (float)uw, (float)vh);
        /* (the last column and row again past them, for the next halving's
           and the magnifying filter's edge: a texel of the source a pixel
           across them) */
        bd_sprite((float)w2, 0, (float)(w2 + 1), (float)h2, (float)(w - 1), 0, (float)w, (float)vh);
        bd_sprite(0, (float)h2, (float)w2, (float)(h2 + 1), 0, (float)(h - 1), (float)uw, (float)h);
        bd_sprite((float)w2, (float)h2, (float)(w2 + 1), (float)(h2 + 1), (float)(w - 1), (float)(h - 1), (float)w, (float)h);
        src = (const char *)sceGeEdramGetAddr() + off;
        s_bd_off = off;
        s_bd_w = w2;
        s_bd_h = h2;
        s_bd_tw = t2;
        off += (unsigned)t2 * (unsigned)(h2 + 1) * (n ? 4u : 2u);
        off = (off + 63u) & ~63u;
        w = w2;
        h = h2;
        tbw = t2;
    }
    /* the tint over it */
    if (tint >> 24) {
        CVtx *v = (CVtx *)sceGuGetMemory(2 * sizeof(CVtx));
        uint32_t c = abgr(tint);
        v[0].c = v[1].c = c;
        v[0].x = v[0].y = v[0].z = 0;
        v[1].x = (short)(s_bd_w + 1);
        v[1].y = (short)(s_bd_h + 1);
        v[1].z = 0;
        sceGuDisable(GU_TEXTURE_2D);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_TEXTURE_2D);
    }
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    list_done();
    s_bd_u = (float)TW / (float)(1 << n);
    s_bd_v = (float)TH / (float)(1 << n);
    s_bd_on = 1;
    return 1;
}

void rr_backdrop_draw(const float q[6])
{
    if (!s_bd_on)
        return;
    list_open();
    sceGuDrawBuffer(GU_PSM_8888, (void *)((char *)s_fb - (char *)sceGeEdramGetAddr()), STRIDE);
    sceGuScissor(0, 0, TW, TH);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(s_bd_psm, 0, 0, 0);
    sceGuTexImage(0, pow2_at(s_bd_tw), pow2_at(s_bd_h + 1), s_bd_tw, (const char *)sceGeEdramGetAddr() + s_bd_off);
    sceGuTexFlush();
    if (s_bd_psm == GU_PSM_8888) {
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        bd_sprite(q[0], q[1], q[2], q[5], 0, 0, s_bd_u, s_bd_v);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    } else {
        /* (no blur: the frame, one to one, no filter) */
        bd_sprite(q[0] + 1.0f / 16.0f, q[1] + 1.0f / 16.0f, q[2] + 1.0f / 16.0f, q[5] + 1.0f / 16.0f, 0, 0, s_bd_u, s_bd_v);
    }
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
}

/* the picture drawn on the GE apart from the frame (into its buffer,
   rr_psp_pic.c), read back: 5551, each channel widened as the GE widens it */
int rr_picture(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[3], uint32_t *argb)
{
    const uint16_t *v;
    int pw = w, ph = h, x, y;
    if (!s_screen || w <= 0 || h <= 0)
        return 0;
    list_done();
    if (wp[0] ? !rr_psp_picture_woven(desc, words, (int)wp[0], h, wp[1], wp[2]) : !rr_psp_picture(desc, words, &pw, &ph))
        return 0;
    if (pw != w || ph != h)
        nshvc_fatal("rr_picture: the picture drawn %dx%d, asked %dx%d", pw, ph, w, h);
    v = rr_psp_picture_pixels();
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            argb[(size_t)y * (size_t)w + (size_t)x] = rr_psp_picture_argb(v[(size_t)y * (size_t)rr_psp_picture_stride + (size_t)x]);
    return 1;
}

/* the PSP's target is its screen's */
void rr_size(int w, int h)
{
    if (w != TW || h != TH)
        nshvc_fatal("rr_size(%d, %d): the PSP's target is its screen, %d x %d", w, h, TW, TH);
}

int rr_read(uint32_t *argb)
{
    const uint32_t *fb;
    int x, y;
    if (!s_screen)
        return 0;
    list_done();
    fb = uncached(s_screen->shown());
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            argb[y * TW + x] = abgr(fb[(size_t)y * STRIDE + (size_t)x]) | 0xFF000000u;
    return 1;
}
