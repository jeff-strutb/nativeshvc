/*
 * rr_psp.c: the renderer backend on the PSP's GE, drawing into the host's
 * screen buffers (host/psp/host_psp.h).
 *
 * A picture is drawn with sharp bilinear (rr_size.h) in two stages:
 *
 *   across   on the CPU: for each of the picture's rows, each output
 *            column's blend of its two columns, computed as rr_ref_image
 *            computes it (this stage is the reference's own), into a
 *            buffer in the GE's channel order (0xAABBGGRR)
 *   down     on the GE: each output row that buffer's row, then its next
 *            row blended over it by fixed factors A = round(256 t) and
 *            B = 256 - A (at most 255 each)
 *
 * The GE blends as ((2s + 1)(2A + 1) >> 10) + ((2d + 1)(2B + 1) >> 10)
 * (measured: every value of 256 alphas over three channels). With these
 * factors one blend is within 1 of rr_mix's for every pair of channel
 * values at every weight the presentation makes (checked over 456,964 of
 * them), so the picture is within 1 of rr_ref_image, as every backend's
 * is. The GE's own filtering cannot do it: its bilinear weights are
 * sixteenths, truncated, and two blending stages are within 2.
 *
 * A picture the GE drew itself (rr_compose, rr_psp_pic.c) is scaled on the
 * GE throughout, its stage across the reference's too: see scale_ge.
 *
 * A frame presenting Mode 7 or a canvas, and the overlays, are the
 * reference's drawing on the CPU (rr_ref_mode7, rr_ref_overlay), into the
 * frame once the GE has drawn it.
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
    s_fb = s_screen->target();
    list_open();
    sceGuClearColor(abgr(argb | 0xFF000000u));
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

/* ---- a picture, sharp bilinear ---- */
static uint32_t *s_mid;                      /* the stage across: a row of the visible columns per picture row */
static size_t s_mid_rows;
static int *s_tx;                            /* each visible column's picture column, */
static float *s_fx;                          /* and the weight of the next */
static int s_cols_cap;

static void mid_rows(int h)
{
    if ((size_t)h <= s_mid_rows)
        return;
    free(s_mid);
    s_mid = (uint32_t *)memalign(64, (size_t)h * STRIDE * 4);
    if (!s_mid)
        nshvc_fatal("no memory for a picture of %d rows", h);
    s_mid_rows = (size_t)h;
}

typedef struct {
    float u, v, x, y, z;
} Vtx;

/* output row y: the stage across's row r of n columns at x0 */
static void row_sprite(int r, int x0, int n, int y)
{
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    sceGuTexImage(0, STRIDE, 1, STRIDE, s_mid + (size_t)r * STRIDE);
    sceGuTexFlush();
    v[0].u = 0;
    v[0].v = 0;
    v[0].x = (float)x0;
    v[0].y = (float)y;
    v[0].z = 0;
    v[1].u = (float)n;
    v[1].v = 1;
    v[1].x = (float)(x0 + n);
    v[1].y = (float)(y + 1);
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    int X0, X1, Y0, Y1, n, x, y, r;
    float dx, dy;
    if (!s_screen || w <= 0 || h <= 0 || d[2] <= d[0] || d[3] <= d[1])
        return;
    X0 = d[0] < 0 ? 0 : d[0];
    X1 = d[2] > TW ? TW : d[2];
    Y0 = d[1] < 0 ? 0 : d[1];
    Y1 = d[3] > TH ? TH : d[3];
    if (X1 <= X0 || Y1 <= Y0)
        return;
    n = X1 - X0;
    dx = (float)w / (float)(d[2] - d[0]);
    dy = (float)h / (float)(d[3] - d[1]);
    if (n > s_cols_cap) {
        free(s_tx);
        free(s_fx);
        s_tx = (int *)malloc((size_t)n * sizeof *s_tx);
        s_fx = (float *)malloc((size_t)n * sizeof *s_fx);
        if (!s_tx || !s_fx)
            nshvc_fatal("no memory for a picture %d wide", n);
        s_cols_cap = n;
    }
    for (x = 0; x < n; x++)
        s_fx[x] = rr_sharp(((float)(X0 + x - d[0]) + 0.5f) * dx, dx, &s_tx[x]);
    /* across: every picture row the output rows read (rr_texel's clamp) */
    mid_rows(h);
    for (r = 0; r < h; r++) {
        uint32_t *o = s_mid + (size_t)r * STRIDE;
        for (x = 0; x < n; x++) {
            uint32_t a = rr_texel(argb, w, h, s_tx[x], r);
            o[x] = abgr(s_fx[x] > 0 ? rr_mix(a, rr_texel(argb, w, h, s_tx[x] + 1, r), s_fx[x]) : a);
        }
    }
    sceKernelDcacheWritebackRange(s_mid, (unsigned)((size_t)h * STRIDE * 4));
    /* down: on the GE */
    list_open();
    for (y = Y0; y < Y1; y++) {
        int ty, A, B;
        float fy = rr_sharp(((float)(y - d[1]) + 0.5f) * dy, dy, &ty);
        int r0 = ty < 0 ? 0 : ty >= h ? h - 1 : ty, r1 = ty + 1 < 0 ? 0 : ty + 1 >= h ? h - 1 : ty + 1;
        sceGuEnable(GU_TEXTURE_2D);
        sceGuDisable(GU_BLEND);
        row_sprite(r0, X0, n, y);
        if (fy > 0 && r1 != r0) {
            A = (int)floorf(256.0f * fy + 0.5f);
            A = A > 255 ? 255 : A;
            B = 256 - A > 255 ? 255 : 256 - A;
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (unsigned)A * 0x010101u, (unsigned)B * 0x010101u);
            row_sprite(r1, X0, n, y);
        }
    }
    sceGuDisable(GU_BLEND);
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
    int key[6] = { pw, ph, d[0], d[1], d[2], d[3] }, X1, x, i, a, b, k, n, cap = 1;
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
    if (n <= 0 || pw > 512 || ph > 240)
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
    for (x = 0; x < n; x++) {
        int tx, hp[256], hm[256];
        float t = rr_sharp(((float)(s_g->X0 + x - d[0]) + 0.5f) * dx, dx, &tx);
        s_g->tx0[x] = tx < 0 ? 0 : tx >= pw ? pw - 1 : tx;
        s_g->tx1[x] = tx + 1 < 0 ? 0 : tx + 1 >= pw ? pw - 1 : tx + 1;
        s_g->tab[x] = -1;
        if (!(t > 0) || s_g->tx0[x] == s_g->tx1[x])
            continue;
        {
            /* each difference's value the one most of its pairs give; the
               others exceptions */
            static short votes[2][256][256];      /* all 0 between columns: each cleared after */
            int ne = 0, *ex = &s_g->ex[s_g->ntab][0][0];
            for (a = 0; a < 32; a++)
                for (b = 0; b < 32; b++) {
                    int ea = a << 3 | a >> 2, eb = b << 3 | b >> 2;
                    int v = (int)(rr_mix(0xFF000000u | (uint32_t)ea, 0xFF000000u | (uint32_t)eb, t) & 0xFFu);
                    int sg = eb >= ea ? 0 : 1, dd = sg ? ea - eb : eb - ea, hv = sg ? ea - v : v - ea;
                    if (hv < 0) {
                        memset(votes, 0, sizeof votes);
                        return 0;
                    }
                    votes[sg][dd][hv]++;
                }
            for (i = 0; i < 256; i++) {
                int sg, best;
                for (sg = 0; sg < 2; sg++) {
                    int *h = sg ? hm : hp;
                    h[i] = -1;
                    for (best = 0, k = 0; k < 256; k++)
                        if (votes[sg][i][k] > best) {
                            best = votes[sg][i][k];
                            h[i] = k;
                        }
                }
            }
            for (a = 0; a < 32; a++)
                for (b = 0; b < 32; b++) {
                    int ea = a << 3 | a >> 2, eb = b << 3 | b >> 2;
                    int v = (int)(rr_mix(0xFF000000u | (uint32_t)ea, 0xFF000000u | (uint32_t)eb, t) & 0xFFu);
                    int got = eb >= ea ? ea + hp[eb - ea] : ea - hm[ea - eb];
                    if (got == v)
                        continue;
                    if (ne == 48 || (v - got != 1 && v - got != -1)) {
                        memset(votes, 0, sizeof votes);
                        return 0;
                    }
                    ex[3 * ne] = ea;
                    ex[3 * ne + 1] = eb;
                    ex[3 * ne + 2] = v - got;
                    ne++;
                }
            for (a = 0; a < 32; a++)
                for (b = 0; b < 32; b++) {
                    int ea = a << 3 | a >> 2, eb = b << 3 | b >> 2;
                    int v = (int)(rr_mix(0xFF000000u | (uint32_t)ea, 0xFF000000u | (uint32_t)eb, t) & 0xFFu);
                    int sg = eb >= ea ? 0 : 1;
                    votes[sg][sg ? ea - eb : eb - ea][sg ? ea - v : v - ea] = 0;
                }
            s_g->nex[s_g->ntab] = ne;
        }
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
            uint32_t p = (uint32_t)(hp[i] < 0 ? 0 : hp[i]), m = (uint32_t)(hm[i] < 0 ? 0 : hm[i]);
            s_g->clut[k][0][i] = p | p << 8 | p << 16;
            s_g->clut[k][1][i] = m | m << 8 | m << 16;
        }
        for (i = 0; i < s_g->ntab && (memcmp(s_g->clut[i], s_g->clut[k], sizeof s_g->clut[k]) || s_g->nex[i] != s_g->nex[k] ||
                                     memcmp(s_g->ex[i], s_g->ex[k], (size_t)s_g->nex[k] * 3 * sizeof(int)));
             i++)
            ;
        if (i == s_g->ntab)
            s_g->ntab++;
        s_g->tab[x] = i;
        s_g->count[i]++;
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
    sceGuTexImage(0, 512, 256, rr_psp_picture_stride, vram(RR_PSP_V_PICTURE));
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
int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    static uint32_t *pic;
    const uint16_t *v;
    int pw, ph, x, y;
    (void)w;
    (void)h;
    if (!s_screen)
        return 0;
    list_done();
    if (!rr_psp_picture(desc, words, &pw, &ph))
        return 0;
    if (d[2] > d[0] && d[3] > d[1] && geometry(pw, ph, d)) {
        list_open();
        s_cur_off = (unsigned)((char *)s_fb - (char *)sceGeEdramGetAddr());
        s_cur_w = STRIDE;
        scale_ge(pw, ph, d);
        return 1;
    }
    if (!pic && !(pic = (uint32_t *)malloc((size_t)512 * 240 * 4)))     /* (the widest picture's) */
        nshvc_fatal("no memory for the picture");
    v = rr_psp_picture_pixels();
    for (y = 0; y < ph; y++)
        for (x = 0; x < pw; x++)
            pic[y * pw + x] = rr_psp_picture_argb(v[(size_t)y * rr_psp_picture_stride + (size_t)x]);
    rr_image(pic, pw, ph, d);
    return 1;
}

int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4])
{
    (void)desc;
    (void)words;
    (void)ww;
    (void)wh;
    (void)base0;
    (void)base1;
    (void)d;
    return 0;
}

/* Mode 7 and a canvas presented as any frame: the picture at the
   console's resolution (rr_psp_pic.c draws Mode 7's lines), scaled */
int rr_mode7_window(void) { return 0; }

int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s)
{
    (void)desc;
    (void)words;
    (void)m;
    (void)d;
    (void)s;
    return 0;
}

/* ---- on the CPU, over the frame the GE drew ---- */
static uint32_t *s_cpu;                      /* the frame, 0xAARRGGBB, for the reference's drawing */

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    uint32_t *fb;
    int x, y;
    if (!s_screen)
        return;
    if (!s_cpu && !(s_cpu = (uint32_t *)malloc((size_t)TW * TH * 4)))
        nshvc_fatal("no memory for the frame");
    list_done();
    fb = uncached(s_fb);
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            s_cpu[y * TW + x] = abgr(fb[(size_t)y * STRIDE + (size_t)x]);
    rr_ref_mode7(s_cpu, TW, TH, m, d, s);
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            fb[(size_t)y * STRIDE + (size_t)x] = abgr(s_cpu[y * TW + x]);
}

/* rr_ref_overlay on the frame itself: its rows STRIDE apart (the columns
   past TW off the screen), the image's channels in the frame's order (the
   blend treats each alike) */
void rr_overlay(const uint32_t *argb, int w, int h, const float q[6])
{
    static uint32_t *swapped;
    static size_t cap;
    size_t i, n = (size_t)w * (size_t)h;
    if (!s_screen || w <= 0 || h <= 0)
        return;
    if (n > cap) {
        free(swapped);
        swapped = (uint32_t *)malloc(n * 4);
        cap = swapped ? n : 0;
        if (!swapped)
            return;
    }
    for (i = 0; i < n; i++)
        swapped[i] = abgr(argb[i]);
    list_done();
    rr_ref_overlay(uncached(s_fb), STRIDE, TH, swapped, w, h, q);
}

void rr_present(void)
{
    if (!s_screen)
        return;
    list_done();
    s_screen->show();
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
