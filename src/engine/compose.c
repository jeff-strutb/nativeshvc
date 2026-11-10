/* compose.c: the engine's standard picture as native draws on the renderer
 * (rr.h), at the window's resolution. Each capability a game calls becomes
 * geometry and GPU state:
 *
 *   tile layers   an instance for each 8 x 8 texels of a tile the screen
 *                 shows, placed by scroll, its map entry read by the vertex
 *                 stage (program tiles); offset-per-tile moves each column's
 *   mosaic        an instance for each block, of the one texel it shows (blocks)
 *   Mode 7        a quad textured through the matrix (mode7), from a plane drawn
 *                 into a texture from its map and tiles when they change (plane)
 *   sprites       an instance for each, in pieces where the limits or the wrap
 *                 cut it (sprites); the first sprite in priority order that
 *                 shows a pixel keeps it: a stencil bit
 *   images        a quad (image); draw lists: their own triangles and lines (list)
 *   priorities    depth and the depth test
 *   windows       stencil bits, set from rectangles made from the window edges
 *   by line       strips: the rows (and columns) where a capability's values
 *                 are the same are drawn together, with those values
 *   colour maths  the sub screen drawn into its own target, then laid onto the
 *                 main screen by blend state; the main screen's alpha says
 *                 which pixels take part
 *   hires         the sub screen in the main's even half-columns: a stencil bit
 *   brightness    blend state
 *
 * Pixels texels cover are drawn whole; only the pixel an edge falls in is
 * mixed (common.glsl, and the frame's samples at geometry's edges). Nothing is
 * worked out per pixel but the texels a pixel shows. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "picture.h"
#include "shaders/programs.h"

static rr_frame *F;
static const eng_picture *P;

static void *grow(void *p, size_t *cap, size_t want, size_t size)
{
    size_t c;
    void *n;
    if (want <= *cap)
        return p;
    c = *cap ? *cap : 256;
    while (c < want)
        c *= 2;
    if (!(n = realloc(p, c * size))) {
        eng_report("nativeshvc", "no memory for %zu items", c);
        exit(70);
    }
    *cap = c;
    return n;
}

/* ---- the frame ---- */
static uint32_t words(size_t n)
{
    size_t at = F->data_words;
    F->data = (uint32_t *)grow(F->data, &F->data_cap, at + n, 4);
    F->data_words = at + n;
    return (uint32_t)at;
}

static void wordf(uint32_t at, float v) { memcpy(&F->data[at], &v, 4); }

/* a draw, its uniforms returned in *u (valid until the next draw) */
static rr_cmd *draw(int program, rr_uniform **u)
{
    rr_cmd *c;
    F->cmds = (rr_cmd *)grow(F->cmds, &F->cap_cmds, F->ncmds + 1, sizeof *F->cmds);
    F->uniforms = (rr_uniform *)grow(F->uniforms, &F->cap_uniforms, F->nuniforms + 1, sizeof *F->uniforms);
    c = &F->cmds[F->ncmds++];
    memset(c, 0, sizeof *c);
    c->program = (uint8_t)program;
    c->blend = RR_BLEND_REPLACE;
    c->space = RR_SPACE_VIEW;
    c->uniform = (uint32_t)F->nuniforms;
    c->vertices = 6;
    c->instances = 1;
    *u = &F->uniforms[F->nuniforms++];
    memset(*u, 0, sizeof **u);
    (*u)->f[1][0] = (float)P->w;
    (*u)->f[1][1] = (float)P->h;
    return c;
}

static void rect_of(rr_uniform *u, float x0, float y0, float x1, float y1)
{
    u->f[2][0] = x0;
    u->f[2][1] = y0;
    u->f[2][2] = x1;
    u->f[2][3] = y1;
}

static void pass_begin(int target, uint32_t texture)
{
    rr_pass *p;
    F->passes = (rr_pass *)grow(F->passes, &F->cap_passes, F->npasses + 1, sizeof *F->passes);
    p = &F->passes[F->npasses++];
    memset(p, 0, sizeof *p);
    p->target = (uint8_t)target;
    p->texture = texture;
    p->first = F->ncmds;
}

static void pass_end(void) { F->passes[F->npasses - 1].count = F->ncmds - F->passes[F->npasses - 1].first; }

/* rectangles for one fill draw, gathered */
static float *rects;
static size_t nrects, cap_rects;

static void rect_add(float x0, float y0, float x1, float y1)
{
    rects = (float *)grow(rects, &cap_rects, (nrects + 1) * 4, 4);
    rects[nrects * 4] = x0;
    rects[nrects * 4 + 1] = y0;
    rects[nrects * 4 + 2] = x1;
    rects[nrects * 4 + 3] = y1;
    nrects++;
}

/* the gathered rectangles as one fill draw, or none */
static rr_cmd *fill_rects(rr_uniform **u)
{
    uint32_t at;
    rr_cmd *c;
    if (!nrects)
        return NULL;
    at = words(nrects * 4);
    memcpy(&F->data[at], rects, nrects * 16);
    c = draw(P_FILL, u);
    (*u)->i[0][0] = (int32_t)at;
    c->instances = (uint32_t)nrects;
    nrects = 0;
    return c;
}

static rr_cmd *fill(float x0, float y0, float x1, float y1, rr_uniform **u)
{
    rect_add(x0, y0, x1, y1);
    return fill_rects(u);
}

/* ---- the console's rules, as values ---- */
static const uint8_t bpp_of[8][4] = {
    { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 }, { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 8, 0, 0, 0 }
};

/* priority: higher in front */
static int zbg(const eng_state *s, int n, int prio)
{
    int mode = s->m;
    if (mode == 0)
        return n == 0 ? (prio ? 11 : 8) : n == 1 ? (prio ? 10 : 7) : n == 2 ? (prio ? 5 : 2) : (prio ? 4 : 1);
    if (mode == 1) {
        if (n == 0)
            return prio ? 9 : 6;
        if (n == 1)
            return prio ? 8 : 5;
        return prio ? (s->b3 ? 11 : 3) : 1;
    }
    if (mode == 7)
        return n == 0 ? 3 : (prio ? 5 : 1);
    return n == 0 ? (prio ? 7 : 3) : (prio ? 5 : 1);
}

static int zobj(int mode, int prio)
{
    if (mode == 0)
        return 3 + 3 * prio;
    if (mode == 1)
        return prio == 0 ? 2 : prio == 1 ? 4 : prio == 2 ? 7 : 10;
    if (mode == 7)
        return prio == 0 ? 2 : prio == 1 ? 4 : prio == 2 ? 6 : 7;
    return 2 + 2 * prio;
}

/* priority z as depth: the backdrop (0) farthest, at 1 */
static float depth_of(int z) { return (float)(16 - z) / 16.0f; }

static int hires_of(const eng_state *s) { return s->hires || s->m == 5 || s->m == 6; }
static int maths_of(const eng_state *s) { return (s->b.layers & 63) != 0; }
static int need_sub(const eng_state *s) { return !s->blank && maths_of(s) && !s->b.use_fixed; }
static int need_aux(const eng_state *s) { return need_sub(s) && hires_of(s); }

static void colour_of(uint32_t bgra, float *c)
{
    c[0] = (float)((bgra >> 16) & 255) / 255.0f;
    c[1] = (float)((bgra >> 8) & 255) / 255.0f;
    c[2] = (float)(bgra & 255) / 255.0f;
}

static void bgr15_of(uint32_t v, float *c)
{
    uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
    colour_of((r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2), c);
}

static uint32_t pal0(const eng_state *s) { return P->palettes[(size_t)s->pal * 256]; }

/* ---- views: a screen drawn into a target ---- */
enum {
    V_MAIN,     /* the main screen, into the frame (odd half-columns on a hires row) */
    V_SUBM,     /* the sub screen, into the frame's even half-columns on a hires row */
    V_SUB,      /* the sub screen, into extra target 0, for the colour maths */
    V_AUX       /* the main screen, into extra target 1, for a hires row's even half-columns' colour maths */
};

typedef struct { int view, n; } Ctx;

static int sub_screen(int v) { return v == V_SUBM || v == V_SUB; }
static int in_frame(int v) { return v == V_MAIN || v == V_SUBM; }

static int view_on(int v, const eng_state *s)
{
    if (s->blank)
        return 0;
    switch (v) {
    case V_MAIN: return 1;
    case V_SUBM: return hires_of(s);
    case V_SUB: return need_sub(s);
    default: return need_aux(s);
    }
}

static int enabled(const nshvc_layer *l, int v) { return sub_screen(v) ? l->enabled_sub != 0 : l->enabled_main != 0; }

/* the alpha a layer (0-3 BG, 4 sprites, 5 backdrop) writes: in the frame,
   whether it takes part in the colour maths, halved when the maths halve;
   elsewhere 1, that a layer is there */
static float flag_of(int v, const eng_state *s, int bit)
{
    if (!in_frame(v))
        return 1.0f;
    return (s->b.layers >> bit) & 1 ? (s->b.half ? 0.5f : 1.0f) : 0.0f;
}

/* whether layer L (0-4) is hidden inside its window on this view's screen */
static int masked(const eng_state *s, int L, int v)
{
    int on = sub_screen(v) ? s->w.mask_sub[L] : s->w.mask_main[L];
    return on && (s->w.layer_use[L] & (NSHVC_WIN1 | NSHVC_WIN2));
}

/* the stencil test for a draw of layer L (-1 none) on a view */
static void stencil_of(rr_cmd *c, int v, const eng_state *s, int L)
{
    uint8_t rm = 0, ref = 0;
    if (in_frame(v) && hires_of(s)) {
        rm |= SB_STRIPE;
        if (v == V_SUBM)
            ref |= SB_STRIPE;
    }
    if (L >= 0 && masked(s, L, v))
        rm |= (uint8_t)(SB_LAYER0 << L);
    c->stencil = rm ? RR_STENCIL_EQUAL : RR_STENCIL_OFF;
    c->read_mask = rm;
    c->ref = ref;
}

/* a layer kept to the original's 256 x lines: the rectangle cut; 0 when empty */
static int clip_layer(int L, int *x0, int *y0, int *x1, int *y1)
{
    if (!P->limits.layer_clip[L])
        return *x0 < *x1 && *y0 < *y1;
    if (*x0 < P->ox) *x0 = P->ox;
    if (*x1 > P->ox + 256) *x1 = P->ox + 256;
    if (*y0 < P->oy) *y0 = P->oy;
    if (*y1 > P->oy + P->lines) *y1 = P->oy + P->lines;
    return *x0 < *x1 && *y0 < *y1;
}

/* ---- strips ---- */
typedef int (*eq_fn)(const Ctx *c, const eng_state *a, const eng_state *b);
typedef void (*emit_fn)(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1);
typedef struct { int x0, x1, y0; const eng_state *s; } Seg;
static Seg *open_s, *cur_s;
static size_t cap_open, cap_cur;

/* the picture's rows in rectangles where eq holds for neighbouring parts and
   rows, emit called for each with its state */
static void strips(const Ctx *c, eq_fn eq, emit_fn emit)
{
    size_t nopen = 0, ncur, k;
    int r;
    for (r = 0; r <= P->h; r++) {
        int same;
        ncur = 0;
        if (r < P->h) {
            const eng_row *row = &P->rows[r];
            uint32_t i;
            for (i = 0; i < row->count; i++) {
                const eng_part *pt = &P->parts[row->first + i];
                const eng_state *s = &P->states[pt->state];
                int x0 = pt->x0, x1 = i + 1 < row->count ? P->parts[row->first + i + 1].x0 : P->w;
                if (x0 < 0) x0 = 0;
                if (x1 > P->w) x1 = P->w;
                if (x1 <= x0)
                    continue;
                if (ncur && cur_s[ncur - 1].x1 == x0 && eq(c, cur_s[ncur - 1].s, s)) {
                    cur_s[ncur - 1].x1 = x1;
                    continue;
                }
                cur_s = (Seg *)grow(cur_s, &cap_cur, ncur + 1, sizeof *cur_s);
                cur_s[ncur].x0 = x0;
                cur_s[ncur].x1 = x1;
                cur_s[ncur].s = s;
                ncur++;
            }
        }
        same = r < P->h && ncur == nopen;
        for (k = 0; same && k < ncur; k++)
            same = cur_s[k].x0 == open_s[k].x0 && cur_s[k].x1 == open_s[k].x1 && eq(c, open_s[k].s, cur_s[k].s);
        if (same)
            continue;
        for (k = 0; k < nopen; k++)
            emit(c, open_s[k].s, open_s[k].x0, open_s[k].y0, open_s[k].x1, r);
        open_s = (Seg *)grow(open_s, &cap_open, ncur + 1, sizeof *open_s);
        for (k = 0; k < ncur; k++) {
            open_s[k] = cur_s[k];
            open_s[k].y0 = r;
        }
        nopen = ncur;
    }
}

/* ---- windows: stencil bits ---- */

/* whether column x is inside the windows `use` names */
static int inside(const nshvc_windows *w, int use, int x)
{
    int e1 = use & NSHVC_WIN1, e2 = use & NSHVC_WIN2, logic = (use >> 4) & 3, w1, w2;
    if (!e1 && !e2)
        return 0;
    w1 = x >= w->w[0].left && x <= w->w[0].right;
    w2 = x >= w->w[1].left && x <= w->w[1].right;
    if (use & NSHVC_WIN1_INVERT)
        w1 ^= 1;
    if (use & NSHVC_WIN2_INVERT)
        w2 ^= 1;
    if (e1 && !e2)
        return w1;
    if (e2 && !e1)
        return w2;
    if (logic == 0)
        return w1 | w2;
    if (logic == 1)
        return w1 & w2;
    if (logic == 2)
        return w1 ^ w2;
    return !(w1 ^ w2);
}

typedef struct { int x0, x1, y0; } Span;
static Span *wopen, *wcur;
static size_t cap_wopen, cap_wcur;

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* the columns inside window use `use` (layer_use[L]) on each row, as rectangles, gathered */
static void window_rects(int L)
{
    size_t nopen = 0, ncur, k;
    int r;
    for (r = 0; r <= P->h; r++) {
        int same;
        ncur = 0;
        if (r < P->h) {
            const eng_row *row = &P->rows[r];
            uint32_t i;
            for (i = 0; i < row->count; i++) {
                const eng_part *pt = &P->parts[row->first + i];
                const eng_state *s = &P->states[pt->state];
                int x0 = pt->x0, x1 = i + 1 < row->count ? P->parts[row->first + i + 1].x0 : P->w, b[6], nb = 0, j;
                if (x0 < 0) x0 = 0;
                if (x1 > P->w) x1 = P->w;
                if (x1 <= x0 || !(s->w.layer_use[L] & (NSHVC_WIN1 | NSHVC_WIN2)))
                    continue;
                /* where the windows' edges fall in this part, then each run between */
                b[nb++] = x0;
                if (s->w.w[0].left > x0 && s->w.w[0].left < x1) b[nb++] = s->w.w[0].left;
                if (s->w.w[0].right + 1 > x0 && s->w.w[0].right + 1 < x1) b[nb++] = s->w.w[0].right + 1;
                if (s->w.w[1].left > x0 && s->w.w[1].left < x1) b[nb++] = s->w.w[1].left;
                if (s->w.w[1].right + 1 > x0 && s->w.w[1].right + 1 < x1) b[nb++] = s->w.w[1].right + 1;
                qsort(b, (size_t)nb, sizeof *b, cmp_int);
                b[nb] = x1;
                for (j = 0; j < nb; j++) {
                    if (b[j + 1] <= b[j] || !inside(&s->w, s->w.layer_use[L], b[j]))
                        continue;
                    if (ncur && wcur[ncur - 1].x1 == b[j]) {
                        wcur[ncur - 1].x1 = b[j + 1];
                        continue;
                    }
                    wcur = (Span *)grow(wcur, &cap_wcur, ncur + 1, sizeof *wcur);
                    wcur[ncur].x0 = b[j];
                    wcur[ncur].x1 = b[j + 1];
                    ncur++;
                }
            }
        }
        same = r < P->h && ncur == nopen;
        for (k = 0; same && k < ncur; k++)
            same = wcur[k].x0 == wopen[k].x0 && wcur[k].x1 == wopen[k].x1;
        if (same)
            continue;
        for (k = 0; k < nopen; k++)
            rect_add((float)wopen[k].x0, (float)wopen[k].y0, (float)wopen[k].x1, (float)r);
        wopen = (Span *)grow(wopen, &cap_wopen, ncur + 1, sizeof *wopen);
        for (k = 0; k < ncur; k++) {
            wopen[k] = wcur[k];
            wopen[k].y0 = r;
        }
        nopen = ncur;
    }
}

/* stencil bit `bit` set inside the windows of L (0-4 layers, 5 colour) */
static void window_bits(int L, int bit)
{
    rr_uniform *u;
    rr_cmd *c;
    window_rects(L);
    if (!(c = fill_rects(&u)))
        return;
    c->blend = RR_BLEND_NOCOLOUR;
    c->stencil = RR_STENCIL_SET;
    c->write_mask = (uint8_t)bit;
    c->ref = 0xFF;
}

/* ---- the hires rows' even half-columns: a stencil bit ---- */
static int eq_hires(const Ctx *c, const eng_state *a, const eng_state *b) { (void)c; return hires_of(a) == hires_of(b) && a->blank == b->blank; }

static void emit_stripes(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    int x;
    (void)c;
    if (!hires_of(s) || s->blank)
        return;
    for (x = x0; x < x1; x++)
        rect_add((float)x, (float)y0, (float)x + 0.5f, (float)y1);
}

/* ---- the backdrop ---- */
static uint32_t backdrop_colour(int v, const eng_state *s)
{
    if (in_frame(v))
        return pal0(s);
    {
        uint32_t f = s->b.fixed_bgr15, r = f & 31, g = (f >> 5) & 31, b = (f >> 10) & 31;
        return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
}

static int eq_backdrop(const Ctx *c, const eng_state *a, const eng_state *b)
{
    int v = c->view, on = view_on(v, a);
    if (on != view_on(v, b))
        return 0;
    if (!on)
        return 1;
    return hires_of(a) == hires_of(b) && backdrop_colour(v, a) == backdrop_colour(v, b) && flag_of(v, a, 5) == flag_of(v, b, 5);
}

static void emit_backdrop(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    rr_uniform *u;
    rr_cmd *d;
    int v = c->view;
    if (!view_on(v, s))
        return;
    d = fill((float)x0, (float)y0, (float)x1, (float)y1, &u);
    colour_of(backdrop_colour(v, s), u->f[3]);
    u->f[3][3] = in_frame(v) ? flag_of(v, s, 5) : 0.0f;
    u->f[4][0] = depth_of(0);
    d->depth = RR_DEPTH_LEQUAL;
    stencil_of(d, v, s, -1);
}

/* ---- tile layers, images and draw lists ---- */
enum { K_NONE, K_TILES, K_IMAGE, K_LIST };

static int kind_of(const eng_state *s, int n)
{
    const nshvc_layer *a = &s->l[n];
    if (a->image.id && a->image.id < eng_nimages)
        return K_IMAGE;
    if (a->draw_list.id && a->draw_list.id < eng_nlists && eng_lists_of[a->draw_list.id].w)
        return K_LIST;
    if (a->tiles.id && a->tiles.id < eng_ntiles && a->map.id && a->map.id < eng_nmaps && bpp_of[s->m][n])
        return K_TILES;
    return K_NONE;
}

static int bg_on(int v, const eng_state *s, int n) { return view_on(v, s) && s->m != 7 && enabled(&s->l[n], v) && kind_of(s, n) != K_NONE; }

/* offset-per-tile: on for BG1 and BG2 with its map and BG3's */
static int opt_on(const eng_state *s, int n)
{
    const nshvc_layer *a = &s->l[n];
    return n < 2 && a->offset_per_tile.id && a->offset_per_tile.id < eng_nmaps && s->l[2].map.id && s->l[2].map.id < eng_nmaps;
}

static int eq_bg(const Ctx *c, const eng_state *a, const eng_state *b)
{
    int v = c->view, n = c->n, on = bg_on(v, a, n), k;
    const nshvc_layer *p = &a->l[n], *q = &b->l[n];
    if (on != bg_on(v, b, n))
        return 0;
    if (!on)
        return 1;
    k = kind_of(a, n);
    if (k != kind_of(b, n) || a->m != b->m || a->b3 != b->b3 || hires_of(a) != hires_of(b) || flag_of(v, a, n) != flag_of(v, b, n) ||
        masked(a, n, v) != masked(b, n, v))
        return 0;
    if (k == K_LIST)
        return p->draw_list.id == q->draw_list.id;
    if (a->pal != b->pal)
        return 0;
    if (k == K_IMAGE)
        return p->image.id == q->image.id;
    if (p->tiles.id != q->tiles.id || p->map.id != q->map.id || (p->tile_size == 16) != (q->tile_size == 16) || p->scroll_x != q->scroll_x ||
        p->scroll_y != q->scroll_y || p->wrap != q->wrap || p->direct_colour != q->direct_colour ||
        (p->mosaic > 1 ? p->mosaic : 1) != (q->mosaic > 1 ? q->mosaic : 1) || (p->mosaic > 1 && a->anchor[n] != b->anchor[n]))
        return 0;
    if (opt_on(a, n) != opt_on(b, n))
        return 0;
    if (opt_on(a, n))
        return p->offset_per_tile.id == q->offset_per_tile.id && a->l[2].map.id == b->l[2].map.id && a->l[2].scroll_x == b->l[2].scroll_x &&
               a->l[2].scroll_y == b->l[2].scroll_y;
    return 1;
}

/* the fields tiles and blocks share: how screen pixels become layer pixels, the map, the tiles, offset-per-tile */
static void tile_fields(rr_uniform *u, rr_cmd *d, const eng_state *s, int n)
{
    const nshvc_layer *a = &s->l[n];
    const eng_tiles *t = &eng_tiles_of[a->tiles.id];
    const eng_map *m = &eng_maps_of[a->map.id];
    int lhires = s->m == 5 || s->m == 6, sxs = lhires ? 2 : 1, sys = lhires && P->interlace ? 2 : 1, big = a->tile_size == 16;
    int bpp = bpp_of[s->m][n];
    u->f[4][0] = (float)sxs;
    u->f[4][1] = (float)sys;
    u->f[4][2] = (float)(sxs * (a->scroll_x - P->ox));
    u->f[4][3] = (float)(sys * (1 - P->oy) + a->scroll_y);
    u->i[1][0] = (int32_t)m->w;
    u->i[1][1] = (int32_t)m->h;
    u->i[1][2] = lhires || big ? 16 : 8;
    u->i[1][3] = big ? 16 : 8;
    u->i[2][0] = (int32_t)t->tpp;
    u->i[2][1] = (int32_t)t->cols;
    u->i[2][2] = a->wrap != 0;
    u->i[2][3] = s->pal;
    u->i[3][0] = bpp;
    u->i[3][1] = s->m == 0 ? n * 32 : 0;
    u->i[3][2] = bpp == 8 && a->direct_colour;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[2] = (int32_t)t->tex;
    d->tex[3] = (int32_t)m->tex;
    if (opt_on(s, n)) {
        const eng_map *o = &eng_maps_of[a->offset_per_tile.id], *m3 = &eng_maps_of[s->l[2].map.id];
        u->i[3][3] = s->m == 4 ? 2 : 1;
        u->i[4][0] = (int32_t)(m3->w < o->w ? m3->w : o->w);
        u->i[4][1] = (int32_t)(m3->h < o->h ? m3->h : o->h);
        u->i[4][2] = s->l[2].scroll_x;
        u->i[4][3] = s->l[2].scroll_y;
        u->i[5][0] = 19 + n;
        u->i[5][1] = a->scroll_x;
        u->i[5][2] = a->scroll_y;
        u->i[5][3] = lhires;
        d->tex[4] = (int32_t)o->tex;
    }
    u->f[3][1] = depth_of(zbg(s, n, 0));
    u->f[3][2] = depth_of(zbg(s, n, 1));
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void emit_tiles(int v, const eng_state *s, int n, int x0, int y0, int x1, int y1)
{
    const nshvc_layer *a = &s->l[n];
    rr_uniform *u;
    rr_cmd *d;
    int mosaic = a->mosaic > 1 ? a->mosaic : 1;
    if (mosaic > 1) {
        int gx = P->ox, gy = P->oy + s->anchor[n];
        int bx0 = floordiv(x0 - gx, mosaic), bx1 = floordiv(x1 - 1 - gx, mosaic);
        int by0 = floordiv(y0 - gy, mosaic), by1 = floordiv(y1 - 1 - gy, mosaic);
        d = draw(P_BLOCKS, &u);
        tile_fields(u, d, s, n);
        u->f[6][0] = (float)mosaic;
        u->f[6][1] = (float)gx;
        u->f[6][2] = (float)gy;
        u->i[0][0] = bx0;
        u->i[0][1] = by0;
        u->i[0][2] = bx1 - bx0 + 1;
        u->i[0][3] = by1 - by0 + 1;
        d->instances = (uint32_t)((bx1 - bx0 + 1) * (by1 - by0 + 1));
    } else {
        int sxs, sys, offx, offy, c0, c1, rows;
        d = draw(P_TILES, &u);
        tile_fields(u, d, s, n);
        sxs = (int)u->f[4][0];
        sys = (int)u->f[4][1];
        offx = (int)u->f[4][2];
        offy = (int)u->f[4][3];
        c0 = floordiv(sxs * x0 + offx, 8);
        c1 = floordiv(sxs * x1 + offx - 1, 8);
        rows = opt_on(s, n) ? (sys * (y1 - y0) + 7) / 8 + 1 : floordiv(sys * y1 + offy - 1, 8) - floordiv(sys * y0 + offy, 8) + 1;
        u->i[0][0] = c0;
        u->i[0][2] = c1 - c0 + 1;
        u->i[0][3] = rows;
        d->instances = (uint32_t)((c1 - c0 + 1) * rows);
    }
    rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
    u->f[3][0] = flag_of(v, s, n);
    d->depth = RR_DEPTH_LEQUAL;
    stencil_of(d, v, s, n);
}

static void emit_image(int v, const eng_state *s, int n, int x0, int y0, int x1, int y1)
{
    const eng_image *im = &eng_images_of[s->l[n].image.id];
    rr_uniform *u;
    rr_cmd *d = draw(P_IMAGE, &u);
    int rgba = im->format == NSHVC_IMAGE_RGBA;
    rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
    u->f[3][0] = flag_of(v, s, n);
    u->f[3][1] = depth_of(zbg(s, n, 0));
    u->f[4][0] = (float)im->w / (float)P->w;
    u->f[4][1] = (float)im->h / (float)P->h;
    u->i[0][0] = (int32_t)im->w;
    u->i[0][1] = (int32_t)im->h;
    u->i[0][2] = rgba;
    u->i[0][3] = s->pal;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[rgba ? 1 : 2] = (int32_t)im->tex;
    d->depth = RR_DEPTH_LEQUAL;
    stencil_of(d, v, s, n);
}

/* each list's vertices in this frame's words, once: seven words a vertex */
static uint32_t list_at[ENG_LISTS];
static uint8_t list_put[ENG_LISTS];

static void emit_list(int v, const eng_state *s, int n, int x0, int y0, int x1, int y1)
{
    uint32_t id = s->l[n].draw_list.id, k;
    const eng_list *l = &eng_lists_of[id];
    float z = depth_of(zbg(s, n, 0));
    if (!list_put[id]) {
        size_t i;
        list_at[id] = words(l->nv * 7);
        for (i = 0; i < l->nv; i++) {
            uint32_t at = list_at[id] + (uint32_t)i * 7;
            const nshvc_vertex *p = &l->v[i];
            wordf(at, p->x);
            wordf(at + 1, p->y);
            wordf(at + 2, p->z);
            wordf(at + 3, p->w);
            wordf(at + 4, p->u);
            wordf(at + 5, p->v);
            F->data[at + 6] = p->colour;
        }
        list_put[id] = 1;
    }
    for (k = 0; k < l->nc; k++) {
        const eng_list_cmd *cm = &l->cmd[k];
        rr_uniform *u;
        rr_cmd *d = draw(P_LIST, &u);
        rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
        u->f[3][0] = flag_of(v, s, n);
        memcpy(u->f[4], cm->inputs, 32);
        u->f[6][0] = (float)P->w / (float)l->w;
        u->f[6][1] = (float)P->h / (float)l->h;
        /* the list's depths within its priority's: in front of what is behind it, behind what is in front */
        u->f[6][2] = z - 0.999f / 16.0f;
        u->f[6][3] = z;
        u->i[0][0] = (int32_t)(list_at[id] + cm->first * 7);
        if (cm->image) {
            u->i[0][1] = (int32_t)eng_images_of[cm->image].w;
            u->i[0][2] = (int32_t)eng_images_of[cm->image].h;
            u->i[0][3] = 1;
            d->tex[0] = (int32_t)eng_images_of[cm->image].tex;
        }
        d->shader = (uint16_t)cm->shader;
        d->topology = cm->lines ? RR_LINES : RR_TRIANGLES;
        d->vertices = cm->count;
        d->scissor = 1;
        d->depth = RR_DEPTH_LEQUAL;
        stencil_of(d, v, s, n);
    }
}

static void emit_bg(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    int v = c->view, n = c->n;
    if (!bg_on(v, s, n) || !clip_layer(n, &x0, &y0, &x1, &y1))
        return;
    switch (kind_of(s, n)) {
    case K_TILES: emit_tiles(v, s, n, x0, y0, x1, y1); break;
    case K_IMAGE: emit_image(v, s, n, x0, y0, x1, y1); break;
    default: emit_list(v, s, n, x0, y0, x1, y1); break;
    }
}

/* ---- Mode 7 ---- */
static uint32_t plane_tex;
static uint32_t plane_key[4];   /* tiles, their writes, map, its writes */

static int m7_valid(const eng_state *s)
{
    return s->m7.tiles.id && s->m7.tiles.id < eng_ntiles && s->m7.map.id && s->m7.map.id < eng_nmaps;
}

static int m7_bg1(int v, const eng_state *s) { return enabled(&s->l[0], v); }
static int m7_bg2(int v, const eng_state *s) { return s->m7.extbg && enabled(&s->l[1], v); }
static int m7_on(int v, const eng_state *s) { return view_on(v, s) && s->m == 7 && m7_valid(s) && (m7_bg1(v, s) || m7_bg2(v, s)); }

static int eq_m7(const Ctx *c, const eng_state *a, const eng_state *b)
{
    int v = c->view, on = m7_on(v, a);
    const nshvc_mode7 *p = &a->m7, *q = &b->m7;
    if (on != m7_on(v, b))
        return 0;
    if (!on)
        return 1;
    return p->a == q->a && p->b == q->b && p->c == q->c && p->d == q->d && p->centre_x == q->centre_x && p->centre_y == q->centre_y &&
           p->scroll_x == q->scroll_x && p->scroll_y == q->scroll_y && p->outside == q->outside && p->flip_x == q->flip_x &&
           p->flip_y == q->flip_y && p->extbg == q->extbg && p->tiles.id == q->tiles.id && p->map.id == q->map.id &&
           m7_bg1(v, a) == m7_bg1(v, b) && m7_bg2(v, a) == m7_bg2(v, b) &&
           (a->l[0].mosaic > 1 ? a->l[0].mosaic : 1) == (b->l[0].mosaic > 1 ? b->l[0].mosaic : 1) &&
           (a->l[0].mosaic <= 1 || a->anchor[0] == b->anchor[0]) && a->l[0].direct_colour == b->l[0].direct_colour && a->pal == b->pal &&
           flag_of(v, a, 0) == flag_of(v, b, 0) && flag_of(v, a, 1) == flag_of(v, b, 1) && masked(a, 0, v) == masked(b, 0, v) &&
           masked(a, 1, v) == masked(b, 1, v) && hires_of(a) == hires_of(b);
}

/* the plane's texel at screen point (x, y), as the console's sum from row y0
   on: f[4], f[5], in texels; the console's own pixel (x, y) shows the texel
   at the point (x, y), its top left corner */
static void m7_affine(rr_uniform *u, const eng_state *s, int y0)
{
    const nshvc_mode7 *m = &s->m7;
    int a = m->a, b = m->b, c = m->c, d = m->d, cx = m->centre_x, cy = m->centre_y;
    int x0 = m->scroll_x - cx, yy0 = m->scroll_y - cy, lineno = y0 - P->oy + 1;
    double fx = m->flip_x ? -1 : 1, fy = m->flip_y ? -1 : 1, ux0, ey0, ox, oy;
    x0 = (x0 & 0x2000) ? (x0 | ~0x3FF) : (x0 & 0x3FF);
    yy0 = (yy0 & 0x2000) ? (yy0 | ~0x3FF) : (yy0 & 0x3FF);
    if (m->flip_y)
        lineno = 255 - lineno;
    /* the first row's origin is the console's sum, each product kept to 1/4 texel */
    ox = (double)(((a * x0) & ~63) + ((b * lineno) & ~63) + ((b * yy0) & ~63) + cx * 256);
    oy = (double)(((c * x0) & ~63) + ((d * lineno) & ~63) + ((d * yy0) & ~63) + cy * 256);
    /* a screen point's place: its column from the original's left edge, down
       from the first row's top, so that a pixel covers the texels the console
       shows there; flipped, the same pixels run the other way */
    ux0 = m->flip_x ? 256.0 + P->ox : -(double)P->ox;
    ey0 = m->flip_y ? 1.0 + y0 : -(double)y0;
    u->f[4][0] = (float)(a * fx / 256.0);
    u->f[4][1] = (float)(b * fy / 256.0);
    u->f[4][2] = (float)((ox + a * ux0 + b * ey0) / 256.0);
    u->f[5][0] = (float)(c * fx / 256.0);
    u->f[5][1] = (float)(d * fy / 256.0);
    u->f[5][2] = (float)((oy + c * ux0 + d * ey0) / 256.0);
}

static void m7_fields(rr_uniform *u, rr_cmd *d, const eng_state *s, int kind)
{
    const eng_tiles *t = &eng_tiles_of[s->m7.tiles.id];
    u->i[2][3] = s->pal;
    u->i[3][2] = s->l[0].direct_colour != 0;
    u->i[6][0] = kind;
    u->i[6][1] = s->m7.outside & 3;
    u->i[6][2] = (int32_t)t->tpp;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[2] = (int32_t)plane_tex;
    d->tex[3] = (int32_t)t->tex;
    d->depth = RR_DEPTH_LEQUAL;
}

static void emit_m7(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    int v = c->view, n, mosaic = s->l[0].mosaic > 1 ? s->l[0].mosaic : 1;
    if (!m7_on(v, s))
        return;
    for (n = 0; n < 2; n++) {
        int a = x0, b = y0, e = x1, f = y1, k;
        if (!(n ? m7_bg2(v, s) : m7_bg1(v, s)) || !clip_layer(n, &a, &b, &e, &f))
            continue;
        if (mosaic > 1) {
            int gx = P->ox, gy = P->oy + s->anchor[0];
            int bx0 = floordiv(a - gx, mosaic), bx1 = floordiv(e - 1 - gx, mosaic), by0 = floordiv(b - gy, mosaic), by1 = floordiv(f - 1 - gy, mosaic);
            rr_uniform *u;
            rr_cmd *d = draw(P_BLOCKS, &u);
            m7_affine(u, s, y0);
            m7_fields(u, d, s, n ? 2 : 1);
            rect_of(u, (float)a, (float)b, (float)e, (float)f);
            u->f[3][0] = flag_of(v, s, n);
            u->f[3][1] = depth_of(n ? 1 : 3);
            u->f[3][2] = depth_of(n ? 5 : 3);
            u->f[6][0] = (float)mosaic;
            u->f[6][1] = (float)gx;
            u->f[6][2] = (float)gy;
            u->i[0][0] = bx0;
            u->i[0][1] = by0;
            u->i[0][2] = bx1 - bx0 + 1;
            u->i[0][3] = by1 - by0 + 1;
            d->instances = (uint32_t)((bx1 - bx0 + 1) * (by1 - by0 + 1));
            stencil_of(d, v, s, n);
            continue;
        }
        /* BG1 once; BG2's texels of each priority, each at its depth */
        for (k = n ? 2 : 1; k <= (n ? 3 : 1); k++) {
            rr_uniform *u;
            rr_cmd *d = draw(P_MODE7, &u);
            m7_affine(u, s, y0);
            m7_fields(u, d, s, k);
            rect_of(u, (float)a, (float)b, (float)e, (float)f);
            u->f[3][0] = flag_of(v, s, n);
            u->f[3][1] = depth_of(n ? 1 : 3);
            u->f[3][2] = depth_of(n ? 5 : 3);
            stencil_of(d, v, s, n);
        }
    }
}

/* ---- sprites ---- */
typedef struct { int32_t x0, y0, x1, y1, lx, ly; uint32_t size, n; } Piece;
typedef struct { uint32_t first, count, tiles; } Run;
static Piece *pieces;
static size_t npieces, cap_pieces;
static Run *runs;
static size_t nruns, cap_runs;
static uint32_t pieces_at;

static void piece_add(const nshvc_sprite *s, int r0, int k0, int k1, int cols)
{
    Piece *p;
    if (cols <= 0 || k1 <= k0)
        return;
    pieces = (Piece *)grow(pieces, &cap_pieces, npieces + 1, sizeof *pieces);
    p = &pieces[npieces++];
    p->x0 = s->x;
    p->y0 = r0;
    p->x1 = s->x + cols * 8;
    p->y1 = r0 + (k1 - k0);
    p->lx = 0;
    p->ly = k0;
    p->size = (uint32_t)s->width | (uint32_t)s->height << 16;
    p->n = (uint32_t)s->tile | (uint32_t)(s->palette & 7) << 16 | (uint32_t)(s->priority & 3) << 19 | (uint32_t)(s->hflip != 0) << 21 |
           (uint32_t)(s->vflip != 0) << 22;
}

static int sprite_rows(const nshvc_sprite *s)
{
    int n = s->height * 8;
    return P->limits.sprite_wrap && n > 256 ? 256 : n;
}

static int sprite_row(const nshvc_sprite *s, int k)
{
    int r = s->y + k;
    if (P->limits.sprite_wrap)
        r = P->oy + ((r - P->oy) & 255);
    return r < 0 || r >= P->h ? -1 : r;
}

/* the sprites in priority order as pieces: where a sprite's rows run on
   together with the same tiles kept, under the limits (the SNES's: past the
   first sprites_per_line on a line the rest are dropped, then tiles are spent
   from the last sprite on the line backward) */
static void sprite_pieces(void)
{
    static int *cols_of, *who, *row_start, *row_n, *entries, *kept;
    static size_t cap_cols, cap_who, cap_rs, cap_rn, cap_e, cap_kept;
    const nshvc_limits *lim = &P->limits;
    uint32_t i, n = P->nsprites;
    size_t total = 0, at;
    int r;
    npieces = nruns = 0;
    if (!n)
        return;
    for (i = 0; i < n; i++)
        total += (size_t)sprite_rows(&P->sprites[i]);
    cols_of = (int *)grow(cols_of, &cap_cols, total + 1, sizeof *cols_of);
    who = (int *)grow(who, &cap_who, total + 1, sizeof *who);
    /* each sprite's rows, in priority order: the columns of tiles each keeps, and whose they are */
    at = 0;
    for (i = 0; i < n; i++) {
        const nshvc_sprite *s = &P->sprites[(P->sprite_first + i) % n];
        int k, rows = sprite_rows(s);
        for (k = 0; k < rows; k++) {
            cols_of[at + (size_t)k] = s->width;
            who[at + (size_t)k] = (int)((P->sprite_first + i) % n);
        }
        at += (size_t)rows;
    }
    if (lim->sprites_per_line || lim->sprite_tiles_per_line) {
        size_t ne = 0;
        row_start = (int *)grow(row_start, &cap_rs, (size_t)P->h + 1, sizeof *row_start);
        row_n = (int *)grow(row_n, &cap_rn, (size_t)P->h + 1, sizeof *row_n);
        memset(row_n, 0, ((size_t)P->h + 1) * sizeof *row_n);
        at = 0;
        for (i = 0; i < n; i++) {
            const nshvc_sprite *s = &P->sprites[(P->sprite_first + i) % n];
            int k, rows = sprite_rows(s);
            for (k = 0; k < rows; k++)
                if ((r = sprite_row(s, k)) >= 0)
                    row_n[r]++;
        }
        for (r = 0; r < P->h; r++) {
            row_start[r] = (int)ne;
            ne += (size_t)row_n[r];
        }
        entries = (int *)grow(entries, &cap_e, ne + 1, sizeof *entries);
        kept = (int *)grow(kept, &cap_kept, n + 1, sizeof *kept);
        memset(row_n, 0, ((size_t)P->h + 1) * sizeof *row_n);
        for (i = 0; i < n; i++) {
            const nshvc_sprite *s = &P->sprites[(P->sprite_first + i) % n];
            int k, rows = sprite_rows(s);
            for (k = 0; k < rows; k++)
                if ((r = sprite_row(s, k)) >= 0)
                    entries[row_start[r] + row_n[r]++] = (int)(at + (size_t)k);
            at += (size_t)rows;
        }
        for (r = P->oy < 0 ? 0 : P->oy; r < P->oy + P->lines && r < P->h; r++) {
            int k, nk = 0, in_range = 0, tiles_left = lim->sprite_tiles_per_line ? lim->sprite_tiles_per_line : 0x7FFFFFFF;
            for (k = 0; k < row_n[r]; k++) {
                int e = entries[row_start[r] + k];
                const nshvc_sprite *s = &P->sprites[who[e]];
                int in_cols = s->x < P->ox + 256 && s->x + s->width * 8 > P->ox;
                if (in_cols && lim->sprites_per_line && in_range >= lim->sprites_per_line) {
                    cols_of[e] = 0;
                    continue;
                }
                in_range += in_cols != 0;
                kept[nk++] = e;
            }
            if (!lim->sprite_tiles_per_line)
                continue;
            for (k = nk; k-- > 0;) {
                int e = kept[k];
                const nshvc_sprite *s = &P->sprites[who[e]];
                int sx = s->x - P->ox, vis = s->width, first = sx > -8 ? 0 : (-sx - 8) / 8 + 1;
                if (sx < 0)
                    vis -= (-sx + 7) / 8;
                if (sx + s->width * 8 > 256)
                    vis -= (sx + s->width * 8 - 256) / 8;
                if (vis < 0)
                    vis = 0;
                if (vis > tiles_left)
                    vis = tiles_left;
                tiles_left -= vis;
                cols_of[e] = first + vis < s->width ? first + vis : s->width;
            }
        }
    }
    /* the pieces: rows running on, on the screen, keeping the same columns;
       runs of pieces that share a tile set */
    at = 0;
    for (i = 0; i < n; i++) {
        const nshvc_sprite *s = &P->sprites[(P->sprite_first + i) % n];
        int k, rows = sprite_rows(s), k0 = -1, r0 = 0, last = -2, cols = 0;
        size_t before = npieces;
        if (s->tiles.id && s->tiles.id < eng_ntiles && s->width && s->height) {
            for (k = 0; k <= rows; k++) {
                int rr = k < rows ? sprite_row(s, k) : -1, c = k < rows ? cols_of[at + (size_t)k] : 0;
                if (k0 >= 0 && rr >= 0 && rr == last + 1 && c == cols) {
                    last = rr;
                    continue;
                }
                if (k0 >= 0)
                    piece_add(s, r0, k0, k, cols);
                k0 = rr >= 0 ? k : -1;
                r0 = rr;
                last = rr;
                cols = c;
            }
        }
        at += (size_t)rows;
        if (npieces == before)
            continue;
        if (nruns && runs[nruns - 1].tiles == s->tiles.id) {
            runs[nruns - 1].count += (uint32_t)(npieces - before);
        } else {
            runs = (Run *)grow(runs, &cap_runs, nruns + 1, sizeof *runs);
            runs[nruns].first = (uint32_t)before;
            runs[nruns].count = (uint32_t)(npieces - before);
            runs[nruns].tiles = s->tiles.id;
            nruns++;
        }
    }
    if (npieces) {
        pieces_at = words(npieces * 8);
        memcpy(&F->data[pieces_at], pieces, npieces * sizeof *pieces);
    }
}

static int spr_on(int v, const eng_state *s) { return view_on(v, s) && npieces && (sub_screen(v) ? s->sprites_sub : s->sprites_main); }

static int eq_spr(const Ctx *c, const eng_state *a, const eng_state *b)
{
    int v = c->view, on = spr_on(v, a);
    if (on != spr_on(v, b))
        return 0;
    if (!on)
        return 1;
    return a->m == b->m && a->pal == b->pal && flag_of(v, a, 4) == flag_of(v, b, 4) && masked(a, 4, v) == masked(b, 4, v) &&
           hires_of(a) == hires_of(b);
}

static void emit_spr(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    int v = c->view, p;
    size_t k;
    if (!spr_on(v, s) || !clip_layer(4, &x0, &y0, &x1, &y1))
        return;
    for (k = 0; k < nruns; k++) {
        const eng_tiles *t = &eng_tiles_of[runs[k].tiles];
        rr_uniform *u;
        rr_cmd *d;
        if (!runs[k].count)
            continue;
        d = draw(P_SPRITES, &u);
        rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
        u->f[3][0] = flag_of(v, s, 4);
        u->f[3][1] = in_frame(v) ? 0.0f : 1.0f;
        for (p = 0; p < 4; p++)
            u->f[4][p] = depth_of(zobj(s->m, p));
        u->i[0][0] = (int32_t)(pieces_at + runs[k].first * 8);
        u->i[0][1] = (int32_t)t->cols;
        u->i[0][2] = (int32_t)t->tpp;
        u->i[0][3] = s->pal;
        d->tex[0] = (int32_t)P->palette_tex;
        d->tex[2] = (int32_t)t->tex;
        d->instances = runs[k].count;
        d->depth = RR_DEPTH_LEQUAL;
        stencil_of(d, v, s, 4);
        /* the first sprite to show a pixel keeps it, behind a layer or not */
        d->stencil = RR_STENCIL_EQUAL_INC;
        d->read_mask |= SB_SPRITE;
        d->write_mask = SB_SPRITE;
    }
}

/* ---- a screen ---- */
static void screen_draws(int v)
{
    Ctx c;
    c.view = v;
    c.n = 0;
    strips(&c, eq_backdrop, emit_backdrop);
    for (c.n = 0; c.n < 4; c.n++)
        strips(&c, eq_bg, emit_bg);
    c.n = 0;
    strips(&c, eq_m7, emit_m7);
    strips(&c, eq_spr, emit_spr);
}

/* whether any row needs a test of `pred` */
static int any_state(int (*pred)(const eng_state *))
{
    int r;
    for (r = 0; r < P->h; r++) {
        uint32_t i;
        for (i = 0; i < P->rows[r].count; i++)
            if (pred(&P->states[P->parts[P->rows[r].first + i].state]))
                return 1;
    }
    return 0;
}

static int masks_layer(const eng_state *s, int L)
{
    return (s->w.mask_main[L] || s->w.mask_sub[L]) && (s->w.layer_use[L] & (NSHVC_WIN1 | NSHVC_WIN2));
}
static int masks0(const eng_state *s) { return masks_layer(s, 0); }
static int masks1(const eng_state *s) { return masks_layer(s, 1); }
static int masks2(const eng_state *s) { return masks_layer(s, 2); }
static int masks3(const eng_state *s) { return masks_layer(s, 3); }
static int masks4(const eng_state *s) { return masks_layer(s, 4); }
static int colour_window(const eng_state *s)
{
    return !s->blank && (s->b.clip == 1 || s->b.clip == 2 || ((s->b.prevent == 1 || s->b.prevent == 2) && maths_of(s))) &&
           (s->w.layer_use[5] & (NSHVC_WIN1 | NSHVC_WIN2));
}
static int (*const masks_of[5])(const eng_state *) = { masks0, masks1, masks2, masks3, masks4 };

/* a target's pass: the windows' bits, then its screen */
static void target_pass(int target, int v)
{
    int L;
    pass_begin(target, 0);
    for (L = 0; L < 5; L++)
        if (any_state(masks_of[L]))
            window_bits(L, SB_LAYER0 << L);
    if (in_frame(v)) {
        Ctx c = { V_MAIN, 0 };
        rr_uniform *u;
        rr_cmd *d;
        if (any_state(colour_window))
            window_bits(5, SB_COLOUR);
        if (any_state(hires_of)) {
            strips(&c, eq_hires, emit_stripes);
            if ((d = fill_rects(&u))) {
                d->blend = RR_BLEND_NOCOLOUR;
                d->stencil = RR_STENCIL_SET;
                d->write_mask = SB_STRIPE;
                d->ref = 0xFF;
            }
        }
        screen_draws(V_MAIN);
        screen_draws(V_SUBM);
    } else {
        screen_draws(v);
    }
}

/* ---- the colour maths ---- */

/* the stencil test that keeps a draw to a colour window region: mode 0
   never, 1 outside, 2 inside, 3 always; `not` its complement. 0: nowhere */
static int region(int mode, int not, uint8_t *rm, uint8_t *ref)
{
    mode &= 3;
    if (not)
        mode = 3 - mode;
    if (mode == 0)
        return 0;
    if (mode == 1) {
        *rm |= SB_COLOUR;
    } else if (mode == 2) {
        *rm |= SB_COLOUR;
        *ref |= SB_COLOUR;
    }
    return 1;
}

static int maths_on(const eng_state *s) { return !s->blank && (maths_of(s) || (s->b.clip & 3)); }

static int eq_maths(const Ctx *c, const eng_state *a, const eng_state *b)
{
    const nshvc_blend *p = &a->b, *q = &b->b;
    int on = maths_on(a);
    (void)c;
    if (on != maths_on(b))
        return 0;
    if (!on)
        return 1;
    return p->subtract == q->subtract && p->half == q->half && p->use_fixed == q->use_fixed && p->layers == q->layers && p->clip == q->clip &&
           p->prevent == q->prevent && p->fixed_bgr15 == q->fixed_bgr15 && hires_of(a) == hires_of(b);
}

/* one step over the rectangle, kept to its half-columns and region */
static rr_cmd *maths_step(int program, int blend, uint8_t rm, uint8_t ref, int x0, int y0, int x1, int y1, rr_uniform **u)
{
    rr_cmd *d;
    if (program == P_FILL) {
        d = fill((float)x0, (float)y0, (float)x1, (float)y1, u);
    } else {
        d = draw(program, u);
        rect_of(*u, (float)x0, (float)y0, (float)x1, (float)y1);
    }
    d->blend = (uint8_t)blend;
    d->stencil = rm ? RR_STENCIL_EQUAL : RR_STENCIL_OFF;
    d->read_mask = rm;
    d->ref = ref;
    return d;
}

static void emit_maths(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    const nshvc_blend *b = &s->b;
    int half, hires = hires_of(s), maths = maths_of(s), sub = !b->use_fixed;
    int add = b->subtract ? RR_BLEND_RSUB_BY_A : RR_BLEND_ADD_BY_A, mix = b->subtract ? RR_BLEND_RMIX_BY_A : RR_BLEND_MIX_BY_A;
    float fixed[3];
    (void)c;
    if (!maths_on(s))
        return;
    bgr15_of(b->fixed_bgr15, fixed);
    for (half = 0; half < (hires ? 2 : 1); half++) {
        /* odd half-columns (or a whole row) take the sub screen; a hires row's even ones the main */
        uint8_t srm = hires ? SB_STRIPE : 0, sref = half ? SB_STRIPE : 0, rm, ref;
        int other = half ? RR_TEX_EXTRA1 : RR_TEX_EXTRA0, k;
        rr_uniform *u;
        rr_cmd *d;
        /* where maths are prevented, nothing takes part */
        rm = srm, ref = sref;
        if (maths && region(b->prevent, 0, &rm, &ref)) {
            d = maths_step(P_FILL, RR_BLEND_A_ZERO, rm, ref, x0, y0, x1, y1, &u);
            (void)d;
        }
        /* where clipped, the main screen is black and the maths don't halve */
        rm = srm, ref = sref;
        if (region(b->clip, 0, &rm, &ref)) {
            d = maths_step(P_FILL, RR_BLEND_RGB_ZERO_A_ADD, rm, ref, x0, y0, x1, y1, &u);
            u->f[3][3] = 1.0f;
        }
        if (!maths)
            continue;
        /* the other screen's colour, or the fixed colour, laid on by blend
           state: added or taken away, halved or not */
        for (k = 0; k < 5; k++) {
            int blend = add, discard = 0, notclip = 0, clip = 0;
            if (!b->half) {
                if (k)
                    break;
            } else if (k == 0) {
                /* the other screen's backdrop: the fixed colour, not halved */
                if (!sub)
                    continue;
                blend = RR_BLEND_A_ADD;
                discard = 1;
            } else if (k == 1) {
                blend = mix;
                discard = sub ? 2 : 0;
                notclip = 1;
            } else if (k == 2) {
                clip = 1;
            } else if (k == 3) {
                if (!sub)
                    continue;
                discard = 1;
                notclip = 1;
            } else {
                break;
            }
            rm = srm, ref = sref;
            if ((clip && !region(b->clip, 0, &rm, &ref)) || (notclip && !region(b->clip, 1, &rm, &ref)))
                continue;
            d = maths_step(P_COMBINE, blend, rm, ref, x0, y0, x1, y1, &u);
            u->f[3][0] = fixed[0];
            u->f[3][1] = fixed[1];
            u->f[3][2] = fixed[2];
            u->i[0][0] = !sub;
            u->i[0][1] = discard;
            if (sub)
                d->tex[0] = other;
        }
    }
}

/* ---- the finish: brightness, black ---- */
static int eq_bright(const Ctx *c, const eng_state *a, const eng_state *b)
{
    (void)c;
    return a->blank == b->blank && (a->blank || a->bright == b->bright) && hires_of(a) == hires_of(b);
}

static void emit_bright(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    rr_uniform *u;
    rr_cmd *d;
    (void)c;
    if (s->blank) {
        d = fill((float)x0, (float)y0, (float)x1, (float)y1, &u);
        (void)d;
        return;
    }
    /* a hires row's first half-column is black */
    if (hires_of(s) && x0 == 0)
        fill(0.0f, (float)y0, 0.5f, (float)y1, &u);
    if (s->bright < 15) {
        d = fill((float)x0, (float)y0, (float)x1, (float)y1, &u);
        d->blend = RR_BLEND_RGB_SCALE;
        d->constant = (float)s->bright / 15.0f;
    }
}

/* ---- Mode 7's plane ---- */
static int uses_m7(const eng_state *s) { return s->m == 7 && m7_valid(s) && !s->blank; }

static void plane_pass(void)
{
    const eng_state *s = NULL;
    const eng_tiles *t;
    const eng_map *m;
    int r;
    rr_uniform *u;
    rr_cmd *d;
    for (r = 0; r < P->h && !s; r++) {
        uint32_t i;
        for (i = 0; i < P->rows[r].count && !s; i++)
            if (uses_m7(&P->states[P->parts[P->rows[r].first + i].state]))
                s = &P->states[P->parts[P->rows[r].first + i].state];
    }
    if (!s)
        return;
    t = &eng_tiles_of[s->m7.tiles.id];
    m = &eng_maps_of[s->m7.map.id];
    if (!plane_tex) {
        plane_tex = eng_texture_id();
        eng_texture_create(F, plane_tex, 1024, 1024, RR_R8U, RR_TEX_TARGET);
        plane_key[0] = 0;
    }
    if (plane_key[0] == s->m7.tiles.id && plane_key[1] == t->gen && plane_key[2] == s->m7.map.id && plane_key[3] == m->gen)
        return;
    plane_key[0] = s->m7.tiles.id;
    plane_key[1] = t->gen;
    plane_key[2] = s->m7.map.id;
    plane_key[3] = m->gen;
    pass_begin(RR_TARGET_TEXTURE, plane_tex);
    d = draw(P_PLANE, &u);
    u->f[1][0] = 1024.0f;
    u->f[1][1] = 1024.0f;
    u->i[0][0] = (int32_t)m->w;
    u->i[0][1] = (int32_t)m->h;
    u->i[0][2] = (int32_t)t->cols;
    u->i[0][3] = (int32_t)t->tpp;
    d->tex[2] = (int32_t)m->tex;
    d->tex[3] = (int32_t)t->tex;
    d->space = RR_SPACE_TARGET;
    d->instances = 128 * 128;
    pass_end();
}

void eng_compose(rr_frame *f, const eng_picture *p)
{
    Ctx c = { V_MAIN, 0 };
    rr_uniform *u;
    rr_cmd *d;
    int k;
    F = f;
    P = p;
    memset(list_put, 0, sizeof list_put);
    sprite_pieces();
    plane_pass();
    if (any_state(need_sub)) {
        target_pass(RR_TARGET_EXTRA0, V_SUB);
        pass_end();
    }
    if (any_state(need_aux)) {
        target_pass(RR_TARGET_EXTRA1, V_AUX);
        pass_end();
    }
    target_pass(RR_TARGET_FRAME, V_MAIN);
    strips(&c, eq_maths, emit_maths);
    strips(&c, eq_bright, emit_bright);
    /* outside the part shown: black */
    if (p->show_t > 0)
        rect_add(0.0f, 0.0f, (float)p->w, (float)p->show_t);
    if (p->show_b < p->h)
        rect_add(0.0f, (float)p->show_b, (float)p->w, (float)p->h);
    if (p->show_l > 0)
        rect_add(0.0f, (float)p->show_t, (float)p->show_l, (float)p->show_b);
    if (p->show_r < p->w)
        rect_add((float)p->show_r, (float)p->show_t, (float)p->w, (float)p->show_b);
    fill_rects(&u);
    pass_end();

    /* the overlays, over the window */
    pass_begin(RR_TARGET_OUTPUT, 0);
    for (k = 0; k < NSHVC_OVERLAYS; k++) {
        const eng_overlay *o = &p->overlays[k];
        const eng_image *im;
        if (!o->image || o->alpha <= 0)
            continue;
        im = &eng_images_of[o->image];
        d = draw(P_OVERLAY, &u);
        u->f[3][0] = o->alpha;
        memcpy(u->f[4], o->corners, 16);
        u->f[5][0] = o->corners[4];
        u->f[5][1] = o->corners[5];
        u->i[0][0] = (int32_t)im->w;
        u->i[0][1] = (int32_t)im->h;
        d->tex[0] = (int32_t)im->tex;
        d->blend = RR_BLEND_OVER;
        d->space = RR_SPACE_TARGET;
    }
    pass_end();
}
