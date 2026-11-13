/* compose.c: the engine's standard picture as native draws on the renderer
 * (rr.h), at the window's resolution. Each capability a game calls becomes
 * geometry and GPU state:
 *
 *   tile layers   an instance for each 8 x 8 texels of a tile the screen
 *                 shows, placed by scroll, its map entry read by the vertex
 *                 stage, which also reads the tile's class (kept when the game
 *                 writes the tile: blank, solid or mixed) and keeps only the
 *                 instances a draw is for: one draw for the solid tiles
 *                 (tiles_solid, whose fragment stage rejects nothing, so the
 *                 GPU skips what they hide), one for the mixed (tiles), none
 *                 for the blank; offset-per-tile moves each column's
 *   mosaic        an instance for each block, of the one texel it shows (blocks)
 *   Mode 7        a quad textured through the matrix (mode7), from a plane drawn
 *                 into a texture from its map and tiles when they change
 *                 (plane); where it is smaller than its texels, the plane in
 *                 colour with its smaller levels, filtered (tint)
 *   sprites       an instance for each, in pieces where the limits or the wrap
 *                 cut it (sprites); the first sprite in priority order that
 *                 shows a pixel keeps it: a stencil bit
 *   images        a quad (image); draw lists: their own triangles, and lines
 *                 as quads a list pixel wide (list, lines)
 *   priorities    depth and the depth test, the screen's draws front to back
 *   windows       stencil bits, set from rectangles made from the window edges
 *   by line       strips: the rows (and columns) where a capability's values
 *                 are the same are drawn together, with those values; a draw
 *                 list's or the sprites' strips of one state are drawn once,
 *                 kept to them by a stencil bit
 *   colour maths  done where a layer's colour is written (maths.glsl): on its
 *                 own pixel and what the other screen shows there (the sub
 *                 screen, or on a hires row's even half-columns the main
 *                 alone), each drawn into a target of its own before the main
 *                 screen when the maths need it; a draw has one set of the
 *                 settings, the colour window read from a bit table
 *   the picture   one pass into the window: the main screen, the black outside
 *                 the part shown, the overlays; a draw list with fewer samples
 *                 than that pass has is drawn first into a side target of its
 *                 own at its count and placed where its layer is
 *   hires         the sub screen in the main's even half-columns: a stencil
 *                 bit; the first half-column black, drawn nearest
 *   interlace     the one picture that keeps state: the screen is drawn into
 *                 the frame, this field's half-rows of it into the picture
 *                 (kept from the picture before), the picture into the window
 *
 * Pixels texels cover are drawn whole; only the pixel an edge falls in is
 * mixed (common.glsl, and the samples at geometry's edges). Nothing of a layer
 * is worked out per pixel but the texels the pixel shows. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "picture.h"
#include "shaders/programs.h"

static rr_frame *F;
static const eng_picture *P;
static uint32_t rows_at;        /* each row's palette versions in the frame's words (common.glsl row_palette) */
static uint32_t win_at;         /* the colour window's bit table in the frame's words (maths.glsl); its first word is stored in the word before the rows table */

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
    (*u)->i[7][3] = (int32_t)rows_at;
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

/* the gathered rectangles as one draw of program (P_FILL, P_FIELD), or none */
static rr_cmd *rects_draw(int program, rr_uniform **u)
{
    uint32_t at;
    rr_cmd *c;
    if (!nrects)
        return NULL;
    at = words(nrects * 4);
    memcpy(&F->data[at], rects, nrects * 16);
    c = draw(program, u);
    (*u)->i[0][0] = (int32_t)at;
    c->instances = (uint32_t)nrects;
    nrects = 0;
    return c;
}

static rr_cmd *fill_rects(rr_uniform **u) { return rects_draw(P_FILL, u); }

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
/* the main screen alone is read on a hires row for the even half-columns' partners and flags, so also with the fixed colour */
static int need_aux(const eng_state *s) { return !s->blank && maths_of(s) && hires_of(s); }

static void colour_of(uint32_t bgra, float *c)
{
    c[0] = (float)((bgra >> 16) & 255) / 255.0f;
    c[1] = (float)((bgra >> 8) & 255) / 255.0f;
    c[2] = (float)(bgra & 255) / 255.0f;
}

static uint32_t pal0(const eng_state *s) { return P->palettes[(size_t)s->pal * 256]; }

/* ---- views: a screen drawn into a target ---- */
enum {
    V_MAIN,     /* the main screen, into the frame (odd half-columns on a hires row) */
    V_SUBM,     /* the sub screen, into the frame's even half-columns on a hires row */
    V_SUB,      /* the sub screen, into extra target 0, for the colour maths */
    V_AUX       /* the main screen, into extra target 1: on a hires row an even half-column's partner and flag (alpha: whether its layer takes part in the maths) */
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

/* the alpha a layer (0-3 BG, 4 sprites, 5 backdrop) writes: in the frame and
   in the aux target, 1 if it takes part in the colour maths (the aux's is read
   for a hires row's even half-columns); in the sub screen's, 1, that a layer
   is there */
static float flag_of(int v, const eng_state *s, int bit)
{
    if (v == V_SUB)
        return 1.0f;
    return (s->b.layers >> bit) & 1 ? 1.0f : 0.0f;
}

/* ---- the colour maths and the palette, as a draw's settings ---- */

/* whether the colour window's regions (the clip, the prevent) are read for state s */
static int uses_cwin(const eng_state *s)
{
    int clip = s->b.clip & 3, prevent = s->b.prevent & 3;
    if (s->blank)
        return 0;
    return clip == 1 || clip == 2 || (maths_of(s) && (prevent == 1 || prevent == 2));
}

/* a draw's colour settings on view v for state s, as the bits 0-14 maths.glsl
   reads (0 for a draw that is no main screen's) */
static int32_t colour_word(int v, const eng_state *s)
{
    const nshvc_blend *b = &s->b;
    int maths = maths_of(s);
    if (!in_frame(v) || s->blank)
        return 0;
    return 1 | (maths && b->subtract) << 1 | (maths && b->half) << 2 | (maths && b->use_fixed) << 3 | maths << 4 | (b->clip & 3) << 5 |
           (maths ? (b->prevent & 3) : 0) << 7 | (15 - (s->bright & 15)) << 9 | (v == V_SUBM) << 13 | uses_cwin(s) << 14 | (hires_of(s) != 0) << 15;
}

/* the fixed colour a draw's maths reads, 15-bit BGR */
static int32_t colour_fixed(int v, const eng_state *s)
{
    return in_frame(v) && !s->blank && maths_of(s) ? (int32_t)(s->b.fixed_bgr15 & 0x7FFF) : 0;
}

/* whether two states have the same colour settings on view v */
static int colour_same(int v, const eng_state *a, const eng_state *b)
{
    return colour_word(v, a) == colour_word(v, b) && colour_fixed(v, a) == colour_fixed(v, b);
}

/* a draw's colour maths: its settings, and the other screen's texture where
   it reads one (the sub screen, or on a hires row's even half-columns the aux) */
static void colour_set(rr_uniform *u, rr_cmd *d, int v, const eng_state *s)
{
    u->f[1][2] = (float)colour_word(v, s);     /* small whole numbers, exact as floats */
    u->f[1][3] = (float)colour_fixed(v, s);
    if (in_frame(v) && !s->blank && maths_of(s)) {
        if (v == V_SUBM)
            d->tex[10] = RR_TEX_EXTRA1;     /* the previous dot's main pixel and flag */
        if (!s->b.use_fixed)
            d->tex[9] = RR_TEX_EXTRA0;      /* the sub screen: the partner, or the previous dot's transparency */
    }
}

/* the palette version every pixel of the rectangle shows, as its rows have
   it (common.glsl row_palette: a row's first part from its left edge, its
   last to its right); -1 where more than one */
static int pal_over(int x0, int y0, int x1, int y1)
{
    int r, pal = -1;
    if (y0 < 0) y0 = 0;
    if (y1 > P->h) y1 = P->h;
    for (r = y0; r < y1; r++) {
        const eng_row *row = &P->rows[r];
        uint32_t i;
        for (i = 0; i < row->count; i++) {
            const eng_part *pt = &P->parts[row->first + i];
            int p;
            if ((i + 1 < row->count && P->parts[row->first + i + 1].x0 <= x0) || (i > 0 && pt->x0 >= x1))
                continue;
            p = P->states[pt->state].pal;
            if (pal >= 0 && p != pal)
                return -1;
            pal = p;
        }
    }
    return pal;
}

/* the palette version of a draw over the rectangle, where it is one (u.i[7].x, one more than the version; 0: by row) */
static void pal_set(rr_uniform *u, int x0, int y0, int x1, int y1)
{
    int p = pal_over(x0, y0, x1, y1);
    u->i[7][0] = p >= 0 ? p + 1 : 0;
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

/* ---- groups: the strips of one state, drawn once ---- */
typedef struct { const eng_state *s; int x0, y0, x1, y1, done; } Strip;
static Strip *gs;
static size_t ngs, cap_gs;

static void gather(const eng_state *st, int x0, int y0, int x1, int y1)
{
    gs = (Strip *)grow(gs, &cap_gs, ngs + 1, sizeof *gs);
    gs[ngs].s = st;
    gs[ngs].x0 = x0;
    gs[ngs].y0 = y0;
    gs[ngs].x1 = x1;
    gs[ngs].y1 = y1;
    gs[ngs].done = 0;
    ngs++;
}

/* the gathered strips, in sets that eq holds between: each set drawn once by
   fn, kept to the set's strips by the stencil's SB_GROUP bit, set over them
   before and cleared after; box their bounds */
typedef void (*group_fn)(const Ctx *c, const eng_state *s, const float *box);
static void groups(const Ctx *c, eq_fn eq, group_fn fn)
{
    size_t i, j;
    for (i = 0; i < ngs; i++) {
        float box[4];
        size_t n;
        uint32_t at;
        rr_uniform *u;
        rr_cmd *d;
        if (gs[i].done)
            continue;
        box[0] = (float)gs[i].x0;
        box[1] = (float)gs[i].y0;
        box[2] = (float)gs[i].x1;
        box[3] = (float)gs[i].y1;
        nrects = 0;
        for (j = i; j < ngs; j++) {
            if (gs[j].done || !eq(c, gs[i].s, gs[j].s))
                continue;
            gs[j].done = 1;
            rect_add((float)gs[j].x0, (float)gs[j].y0, (float)gs[j].x1, (float)gs[j].y1);
            if ((float)gs[j].x0 < box[0]) box[0] = (float)gs[j].x0;
            if ((float)gs[j].y0 < box[1]) box[1] = (float)gs[j].y0;
            if ((float)gs[j].x1 > box[2]) box[2] = (float)gs[j].x1;
            if ((float)gs[j].y1 > box[3]) box[3] = (float)gs[j].y1;
        }
        n = nrects;
        d = fill_rects(&u);
        at = (uint32_t)u->i[0][0];
        d->blend = RR_BLEND_NOCOLOUR;
        d->stencil = RR_STENCIL_SET;
        d->write_mask = SB_GROUP;
        d->ref = 0xFF;
        fn(c, gs[i].s, box);
        d = draw(P_FILL, &u);
        u->i[0][0] = (int32_t)at;
        d->instances = (uint32_t)n;
        d->blend = RR_BLEND_NOCOLOUR;
        d->stencil = RR_STENCIL_SET;
        d->write_mask = SB_GROUP;
        d->ref = 0;
    }
    ngs = 0;
}

/* a draw kept to its group's strips */
static void in_group(rr_cmd *d)
{
    d->stencil = d->stencil == RR_STENCIL_OFF ? RR_STENCIL_EQUAL : d->stencil;
    d->read_mask |= SB_GROUP;
    d->ref |= SB_GROUP;
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
    /* the main screen's backdrop is palette colour 0 (the aux target holds the main screen: the partner a hires row's
       even half-column takes is its colour, backdrop included); the sub screen's is the fixed colour */
    if (in_frame(v) || v == V_AUX)
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
    return hires_of(a) == hires_of(b) && backdrop_colour(v, a) == backdrop_colour(v, b) && flag_of(v, a, 5) == flag_of(v, b, 5) &&
           colour_same(v, a, b);
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
    u->f[3][3] = v == V_SUB ? 0.0f : flag_of(v, s, 5);     /* the sub screen's backdrop is no layer */
    u->f[4][0] = depth_of(0);
    colour_set(u, d, v, s);
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
        masked(a, n, v) != masked(b, n, v) || !colour_same(v, a, b))
        return 0;
    /* the palette's version splits nothing: it is read by row (row_palette) where a draw has more than one */
    if (k == K_LIST)
        return p->draw_list.id == q->draw_list.id;
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
    u->i[3][0] = bpp;
    u->i[3][1] = s->m == 0 ? n * 32 : 0;
    u->i[3][2] = bpp == 8 && a->direct_colour;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[3] = (int32_t)t->tex;
    d->tex[4] = (int32_t)m->tex;
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
        d->tex[5] = (int32_t)o->tex;
    }
    u->f[3][1] = depth_of(zbg(s, n, 0));
    u->f[3][2] = depth_of(zbg(s, n, 1));
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* each tile set's classes in this frame's words, once (as a list's vertices are): two bits a tile, 16 a word */
static uint32_t cls_at[ENG_OBJECTS];
static uint8_t cls_put[ENG_OBJECTS];

static uint32_t class_words(uint32_t set)
{
    const eng_tiles *t = &eng_tiles_of[set];
    size_t n = (t->count + 15) / 16;
    if (!cls_put[set]) {
        cls_at[set] = words(n);
        memcpy(&F->data[cls_at[set]], t->cls, n * 4);
        cls_put[set] = 1;
    }
    return cls_at[set];
}

/* a tile layer over a rectangle: its solid tiles by tiles_solid, then its mixed tiles by tiles (a blank
   tile draws nothing), each a draw of the same rectangle, uniforms, depth, stencil, colour settings and
   palette; under mosaic, one draw of blocks */
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
        int pass;
        for (pass = 0; pass < 2; pass++) {
            int sxs, sys, offx, offy, c0, c1, rows;
            d = draw(pass ? P_TILES : P_TILES_SOLID, &u);
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
            u->i[6][0] = pass ? ENG_TILE_MIXED : ENG_TILE_SOLID;
            u->i[6][1] = (int32_t)class_words(s->l[n].tiles.id);
            u->i[6][2] = (int32_t)eng_tiles_of[s->l[n].tiles.id].count;
            d->instances = (uint32_t)((c1 - c0 + 1) * rows);
            rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
            u->f[3][0] = flag_of(v, s, n);
            colour_set(u, d, v, s);
            pal_set(u, x0, y0, x1, y1);
            d->depth = RR_DEPTH_LEQUAL;
            stencil_of(d, v, s, n);
        }
        return;
    }
    rect_of(u, (float)x0, (float)y0, (float)x1, (float)y1);
    u->f[3][0] = flag_of(v, s, n);
    colour_set(u, d, v, s);
    pal_set(u, x0, y0, x1, y1);
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
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[rgba ? 1 : 3] = (int32_t)im->tex;
    colour_set(u, d, v, s);
    pal_set(u, x0, y0, x1, y1);
    d->depth = RR_DEPTH_LEQUAL;
    stencil_of(d, v, s, n);
}

/* ---- lists with fewer samples than the screens: a side target each ---- */
static uint8_t list_side[ENG_LISTS];    /* by list: 1 + its side target, or 0 (drawn in the screens' passes) */
static uint8_t side_samples[RR_SIDES], side_own[RR_SIDES];     /* side_own: 0 none, 1 a depth of its own with NEAR_0, 2 with NEAR_1 */
static uint32_t side_list[RR_SIDES];
static int nsides;

/* the samples a list is drawn with: as many as asked that the device has, no more than the screens' */
static int list_samples(const eng_list *l)
{
    int n = rr_samples_at_most((int)l->samples);
    return n < rr_samples() ? n : rr_samples();
}

static int lists_in(const eng_state *s, int n) { return kind_of(s, n) == K_LIST && !s->blank && (s->l[n].enabled_main || s->l[n].enabled_sub); }

/* the lists this picture draws with fewer samples than the screens' own, or with a depth of their own, each given a side
   target (more than RR_SIDES: the rest said so, and drawn in the screens' passes, with the picture's depth) */
static void side_lists(void)
{
    int r, said = 0;
    memset(list_side, 0, sizeof list_side);
    nsides = 0;
    for (r = 0; r < P->h; r++) {
        uint32_t i;
        for (i = 0; i < P->rows[r].count; i++) {
            const eng_state *s = &P->states[P->parts[P->rows[r].first + i].state];
            int n;
            for (n = 0; n < 4; n++) {
                uint32_t id;
                int c;
                if (!lists_in(s, n))
                    continue;
                id = s->l[n].draw_list.id;
                c = list_samples(&eng_lists_of[id]);
                int own = eng_lists_of[id].own != 0;
                if (own && !rr_depth_own()) {
                    static int said_depth;
                    if (!said_depth++)
                        eng_report("nativeshvc", "no 32-bit float depth here: a list's own depth is not made, its shared depth is used");
                    own = 0;
                }
                if (list_side[id] || (c >= rr_samples() && !own))
                    continue;
                if (nsides == RR_SIDES) {
                    if (!said++)
                        eng_report("nativeshvc", "more than %d draw lists with fewer samples than the picture's in one picture: the rest are drawn with its", RR_SIDES);
                    continue;
                }
                list_side[id] = (uint8_t)(1 + nsides);
                side_samples[nsides] = (uint8_t)c;
                side_own[nsides] = (uint8_t)(own ? 1 + (eng_lists_of[id].order == NSHVC_DEPTH_NEAR_1) : 0);
                side_list[nsides++] = id;
            }
        }
    }
}

/* each list's vertices in this frame's words, once: seven words a vertex */
static uint32_t list_at[ENG_LISTS], list_mw_at[ENG_LISTS];   /* and its meshes' view and instance words */
static uint8_t list_put[ENG_LISTS];

/* a list's commands over a box, for state s's layer n on view v in a screen (flagged, its stencil and group, its
   colour maths); or, with `side`, into its side target: raw colour, alpha 1, the depths of its own */
static void list_draws(uint32_t id, const float *box, int v, const eng_state *s, int n, int side)
{
    uint32_t k;
    const eng_list *l = &eng_lists_of[id];
    float z = side ? 1.0f : depth_of(zbg(s, n, 0));
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
        if (l->nmw) {
            list_mw_at[id] = words(l->nmw);
            memcpy(&F->data[list_mw_at[id]], l->mw, l->nmw * sizeof *l->mw);
        }
        list_put[id] = 1;
    }
    for (k = 0; k < l->nc; k++) {
        const eng_list_cmd *cm = &l->cmd[k];
        rr_uniform *u;
        rr_cmd *d;
        if (cm->mesh && !eng_meshes_of[cm->mesh].count)
            continue;                   /* a mesh with no triangles written: nothing */
        d = draw(cm->mesh ? P_LIST_MESH : cm->lines ? P_LINES : P_LIST, &u);
        rect_of(u, box[0], box[1], box[2], box[3]);
        u->f[3][0] = side ? 1.0f : flag_of(v, s, n);
        memcpy(u->f[4], cm->inputs, 32);
        u->f[6][0] = (float)P->w / (float)l->w;
        u->f[6][1] = (float)P->h / (float)l->h;
        /* the list's depths within its priority's: in front of what is behind it, behind what is in front */
        if (side && side_own[list_side[id] - 1]) {
            /* a depth of its own: the list's z as it is, over the whole range */
            u->f[6][2] = 0.0f;
            u->f[6][3] = 1.0f;
        } else if (l->order == NSHVC_DEPTH_NEAR_1) {
            /* the picture's slice is nearer-is-less: the list's order reversed into it, z = 1 nearest at its near end */
            u->f[6][2] = z;
            u->f[6][3] = z - 0.999f / 16.0f;
        } else {
            u->f[6][2] = z - 0.999f / 16.0f;
            u->f[6][3] = z;
        }
        u->i[0][0] = (int32_t)(list_at[id] + cm->first * 7);
        if (cm->image) {
            const eng_image *im = &eng_images_of[cm->image];
            u->i[0][1] = (int32_t)im->w;
            u->i[0][2] = (int32_t)im->h;
            u->i[0][3] = 1;
            if (im->format == NSHVC_IMAGE_RGBA) {
                d->tex[0] = (int32_t)im->tex;
            } else {
                u->i[1][0] = 1;
                d->tex[1] = (int32_t)P->palette_tex;
                d->tex[3] = (int32_t)im->tex;
            }
        }
        d->shader = (uint16_t)cm->shader;
        if (!side)
            colour_set(u, d, v, s);
        pal_set(u, (int)box[0], (int)box[1], (int)box[2], (int)box[3]);
        if (cm->mesh) {
            /* one instanced draw: the mesh's vertices once for each instance, projected by list_mesh.vert from the view's and
               instances' words and the mesh's texture (ti1) */
            const eng_mesh *ms = &eng_meshes_of[cm->mesh];
            u->i[2][0] = (int32_t)(list_mw_at[id] + cm->moff);
            d->tex[4] = (int32_t)ms->tex;
            d->vertices = ms->count;
            d->instances = cm->ninst;
        } else {
            d->vertices = cm->lines ? cm->count / 2 * 6 : cm->count;
        }
        d->depth = cm->keep ? RR_DEPTH_KEEP : RR_DEPTH_LEQUAL;
        d->cull = cm->cull;
        d->front_ccw = cm->front_ccw;
        d->bias = cm->bias;
        d->slope = cm->slope;
        if (!side) {
            stencil_of(d, v, s, n);
            in_group(d);
        }
    }
}

/* a list's commands, once for its group of strips: in the screen's pass, or the side target's list placed over the box */
static void emit_list(const Ctx *c, const eng_state *s, const float *box)
{
    int v = c->view, n = c->n;
    uint32_t id = s->l[n].draw_list.id;
    if (list_side[id]) {
        rr_uniform *u;
        rr_cmd *d = draw(P_PLACED, &u);
        rect_of(u, box[0], box[1], box[2], box[3]);
        u->f[3][0] = flag_of(v, s, n);
        u->f[3][1] = depth_of(zbg(s, n, 0));
        d->tex[0] = RR_TEX_SIDE0 - (list_side[id] - 1);
        colour_set(u, d, v, s);
        d->depth = RR_DEPTH_LEQUAL;
        stencil_of(d, v, s, n);
        in_group(d);
        return;
    }
    list_draws(id, box, v, s, n, 0);
}

/* the side targets' passes: each list once, over the picture, at its own samples */
static void side_passes(void)
{
    int k;
    for (k = 0; k < nsides; k++) {
        float box[4];
        box[0] = box[1] = 0.0f;
        box[2] = (float)P->w;
        box[3] = (float)P->h;
        pass_begin(RR_TARGET_SIDE, 0);
        F->passes[F->npasses - 1].layer = (uint32_t)k;
        F->passes[F->npasses - 1].samples = side_samples[k];
        F->passes[F->npasses - 1].depth_own = side_own[k] != 0;
        F->passes[F->npasses - 1].depth_greater = side_own[k] == 2;
        F->passes[F->npasses - 1].depth_clear = side_own[k] == 2 ? 0.0f : 1.0f;     /* the farthest */
        list_draws(side_list[k], box, V_MAIN, NULL, 0, 1);
        pass_end();
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
    default: gather(s, x0, y0, x1, y1); break;
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
           masked(a, 1, v) == masked(b, 1, v) && hires_of(a) == hires_of(b) && colour_same(v, a, b);
}

/* the plane's texel at screen point (x, y) for state s, as the console's sum
   from row y0 on: t[0] x + t[1] y + t[2], t[3] x + t[4] y + t[5] */
static void m7_coeffs(const eng_state *s, int y0, double *t)
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
    t[0] = a * fx / 256.0;
    t[1] = b * fy / 256.0;
    t[2] = (ox + a * ux0 + b * ey0) / 256.0;
    t[3] = c * fx / 256.0;
    t[4] = d * fy / 256.0;
    t[5] = (oy + c * ux0 + d * ey0) / 256.0;
}

/* the plane's texel at screen point (x, y), as the console's sum from row y0
   on: f[4], f[5], in texels; the console's own pixel (x, y) shows the texel
   at the point (x, y), its top left corner. The console rounds b and d
   times each line by itself: what the shaders add to this for each row
   (common.glsl m7_line_round) in i[4] and i[5].y */
static void m7_affine(rr_uniform *u, const eng_state *s, int y0)
{
    double t[6];
    int k;
    m7_coeffs(s, y0, t);
    for (k = 0; k < 3; k++) {
        u->f[4][k] = (float)t[k];
        u->f[5][k] = (float)t[3 + k];
    }
    u->i[4][0] = s->m7.b;
    u->i[4][1] = s->m7.d;
    /* the console's line of picture row r, as m7_coeffs counts it */
    u->i[4][2] = s->m7.flip_y ? 254 + P->oy : 1 - P->oy;
    u->i[4][3] = s->m7.flip_y ? -1 : 1;
    u->i[5][1] = y0;
}

/* the state at row r, column x */
static const eng_state *state_at(int r, int x)
{
    const eng_row *row = &P->rows[r];
    uint32_t i, k = row->first;
    for (i = 1; i < row->count; i++)
        if (P->parts[row->first + i].x0 <= x)
            k = row->first + i;
    return &P->states[P->parts[k].state];
}

/* The true texel step from row to row at the strip's edges, where the next
   row is drawn by another matrix (the perspective of a matrix by line): from
   the row above (ra) to the first (y0) in f[6], from the last to the row
   below (rb) in f[7], each the step's start and its change by column, from
   the rows' centres by their own matrices (1e30 where that row is no Mode 7
   or off the picture). Within the strip the step is the strip's own. The
   shader's row step is the shorter, by length, of the two sides'. */
static void m7_steps(rr_uniform *u, const eng_state *s, int x0, int y0, int y1)
{
    double t[6], n[6];
    int e, k;
    m7_coeffs(s, y0, t);
    u->i[7][2] = y0;
    u->i[6][3] = y1;
    for (e = 0; e < 2; e++) {
        int r = e ? y1 : y0 - 1, mine = e ? y1 - 1 : y0;
        const eng_state *o = r >= 0 && r < P->h ? state_at(r, x0) : NULL;
        float *f = u->f[6 + e];
        if (!o || o->m != 7 || o->blank || !m7_valid(o)) {
            /* no row to step to: 1e30 says so, and the shader takes the other side's step */
            f[0] = f[1] = 1e30f;
            f[2] = f[3] = 0.0f;
            continue;
        }
        m7_coeffs(o, r, n);
        for (k = 0; k < 2; k++) {
            const double *lo = e ? t : n, *hi = e ? n : t;
            double ylo = (e ? mine : r) + 0.5, yhi = (e ? r : mine) + 0.5;
            /* this row's texel minus the row above's: (hi - lo) */
            f[k] = (float)(hi[k * 3 + 1] * yhi + hi[k * 3 + 2] - lo[k * 3 + 1] * ylo - lo[k * 3 + 2]);
            f[2 + k] = (float)(hi[k * 3] - lo[k * 3]);
        }
    }
}

static void m7_fields(rr_uniform *u, rr_cmd *d, const eng_state *s, int kind)
{
    const eng_tiles *t = &eng_tiles_of[s->m7.tiles.id];
    u->i[2][3] = s->pal;
    u->i[3][2] = s->l[0].direct_colour != 0;
    u->i[6][0] = kind;
    u->i[6][1] = s->m7.outside;         /* 0, 1 or 2: the engine takes no other */
    u->i[6][2] = (int32_t)t->tpp;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[3] = (int32_t)plane_tex;
    d->tex[4] = (int32_t)t->tex;
    d->depth = RR_DEPTH_LEQUAL;
}

static int m7_filtered(const eng_state *s, int kind, rr_cmd *d, rr_uniform *u);

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
            colour_set(u, d, v, s);
            pal_set(u, a, b, e, f);
            stencil_of(d, v, s, n);
            continue;
        }
        /* BG1 once; BG2's texels of each priority, each at its depth */
        for (k = n ? 2 : 1; k <= (n ? 3 : 1); k++) {
            rr_uniform *u;
            rr_cmd *d = draw(P_MODE7, &u);
            m7_affine(u, s, y0);
            m7_steps(u, s, x0, y0, y1);
            m7_fields(u, d, s, k);
            /* where it is drawn smaller than its texels: the plane in colour, filtered */
            u->i[7][0] = m7_filtered(s, k, d, u);
            rect_of(u, (float)a, (float)b, (float)e, (float)f);
            u->f[3][0] = flag_of(v, s, n);
            u->f[3][1] = depth_of(n ? 1 : 3);
            u->f[3][2] = depth_of(n ? 5 : 3);
            colour_set(u, d, v, s);     /* its palette is the state's own (i[2].w), i[7].x is the filter's */
            stencil_of(d, v, s, n);
        }
    }
}

/* the field of this picture, 1 even or 2 odd: it alternates with each picture, as the console's does (an interlaced
   picture draws this field's half-rows; OBJ interlace takes each sprite's rows by it, interlaced or not) */
static int field;

/* ---- sprites ---- */
typedef struct { int32_t x0, y0, x1, y1, lx, ly; uint32_t size, n; } Piece;
typedef struct { uint32_t first, count, tiles, solid; } Run;
static Piece *pieces;
static size_t npieces, cap_pieces;
static Run *runs;
static size_t nruns, cap_runs;
static uint32_t pieces_at;

/* whether every tile a sprite can show is solid (no texel 0), by the classes the tile set keeps: the
   tiles the sprite's width and height reach from its first, as the fragment stage steps them */
static uint32_t sprite_solid(const nshvc_sprite *s)
{
    const eng_tiles *t = &eng_tiles_of[s->tiles.id];
    int row, col;
    for (row = 0; row < s->height; row++)
        for (col = 0; col < s->width; col++) {
            uint32_t n = ((uint32_t)s->tile & 0xFF00u) | ((((uint32_t)s->tile & 0xF0u) + (uint32_t)row * 16u) & 0xF0u) | ((((uint32_t)s->tile & 0xFFu) + (uint32_t)col) & 0x0Fu);
            if (n >= t->count || ((t->cls[n >> 4] >> ((n & 15) * 2)) & 3u) != ENG_TILE_SOLID)
                return 0;
        }
    return 1;
}

static void piece_add(const nshvc_sprite *s, int r0, int k0, int k1, int cols)
{
    Piece *p;
    if (cols <= 0 || k1 <= k0)
        return;
    /* a sprite twice as high as wide, flipped vertically, as the console does: each square half flips in place,
       so a piece never spans the two (the vertex stage maps by the half the piece starts in) */
    if (s->vflip && s->height == 2 * s->width && k0 < s->width * (P->obj_interlace ? 4 : 8) && k1 > s->width * (P->obj_interlace ? 4 : 8)) {
        int half = s->width * (P->obj_interlace ? 4 : 8);       /* the screen row where texel row width * 8 is */
        piece_add(s, r0, k0, half, cols);
        piece_add(s, r0 + (half - k0), half, k1, cols);
        return;
    }
    pieces = (Piece *)grow(pieces, &cap_pieces, npieces + 1, sizeof *pieces);
    p = &pieces[npieces++];
    p->x0 = s->x;
    p->y0 = r0;
    p->x1 = s->x + cols * 8;
    p->y1 = r0 + (k1 - k0);
    p->lx = 0;
    p->ly = P->obj_interlace ? (P->interlace ? 2 * k0 : 2 * k0 + (field == 2)) : k0;   /* OBJ interlace: the texel row each screen row shows */
    p->size = (uint32_t)s->width | (uint32_t)s->height << 16;
    p->n = (uint32_t)s->tile | (uint32_t)(s->palette & 7) << 16 | (uint32_t)(s->priority & 3) << 19 | (uint32_t)(s->hflip != 0) << 21 |
           (uint32_t)(s->vflip != 0) << 22;
}

static int sprite_rows(const nshvc_sprite *s)
{
    int n = s->height * (P->obj_interlace ? 4 : 8);      /* OBJ interlace: half as high on screen */
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
                vis -= first;           /* the columns wholly off the left; one partly on counts, as on the right */
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
        uint32_t solid;
        if (s->tiles.id && s->tiles.id < eng_ntiles && s->width && s->height) {
            for (k = 0; k <= rows; k++) {
                int rr = k < rows ? sprite_row(s, k) : -1, c = k < rows ? cols_of[at + (size_t)k] : 0;
                /* without screen interlace, OBJ interlace puts a different texel row on each screen row: a piece a row */
                if (k0 >= 0 && rr >= 0 && rr == last + 1 && c == cols && !(P->obj_interlace && !P->interlace)) {
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
        /* a run is consecutive pieces of one tile set and one class: sprites keep their own order, the first to show a pixel keeping it */
        solid = sprite_solid(s);
        if (nruns && runs[nruns - 1].tiles == s->tiles.id && runs[nruns - 1].solid == solid) {
            runs[nruns - 1].count += (uint32_t)(npieces - before);
        } else {
            runs = (Run *)grow(runs, &cap_runs, nruns + 1, sizeof *runs);
            runs[nruns].first = (uint32_t)before;
            runs[nruns].count = (uint32_t)(npieces - before);
            runs[nruns].tiles = s->tiles.id;
            runs[nruns].solid = solid;
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
    return a->m == b->m && flag_of(v, a, 4) == flag_of(v, b, 4) && masked(a, 4, v) == masked(b, 4, v) &&
           hires_of(a) == hires_of(b) && colour_same(v, a, b);
}

static void emit_spr(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    if (spr_on(c->view, s) && clip_layer(4, &x0, &y0, &x1, &y1))
        gather(s, x0, y0, x1, y1);
}

/* log2 of n when it is a power of two, else -1 */
static int32_t log2_of(int32_t n)
{
    int32_t l = 0;
    if (n <= 0 || (n & (n - 1)))
        return -1;
    while ((1 << l) < n)
        l++;
    return l;
}

/* the sprites, once for each group of strips with their settings */
static void spr_group(const Ctx *c, const eng_state *s, const float *box)
{
    int v = c->view, p, p2;
    size_t k;
    for (k = 0; k < nruns; k++) {
        const eng_tiles *t = &eng_tiles_of[runs[k].tiles];
        rr_uniform *u;
        rr_cmd *d;
        if (!runs[k].count)
            continue;
        /* the program says whether the tiles are all solid and whether the sheet's sizes are powers of two */
        p2 = t->cols && log2_of(8 * (int32_t)t->tpp) >= 0 && log2_of((int32_t)t->cols) >= 0;
        d = draw(runs[k].solid ? (p2 ? P_SPRITES_SOLID_P2 : P_SPRITES_SOLID) : (p2 ? P_SPRITES_P2 : P_SPRITES), &u);
        rect_of(u, box[0], box[1], box[2], box[3]);
        u->f[3][0] = flag_of(v, s, 4);
        u->f[3][1] = v == V_SUB ? 1.0f : 0.0f;
        u->f[3][2] = P->obj_interlace && P->interlace ? 2.0f : 1.0f;    /* texel rows to a screen row */
        for (p = 0; p < 4; p++)
            u->f[4][p] = depth_of(zobj(s->m, p));
        u->i[0][0] = (int32_t)(pieces_at + runs[k].first * 8);
        u->i[0][1] = (int32_t)t->cols;
        u->i[0][2] = (int32_t)t->tpp;
        if (t->cols) {
            int32_t side = 8 * (int32_t)t->tpp, ls = log2_of(side), lc = log2_of((int32_t)t->cols);
            u->i[0][3] = (int32_t)((t->count + t->cols - 1) / t->cols) * side;    /* the atlas's height, as it was made */
            u->i[1][0] = ls;
            u->i[1][1] = lc;
        }
        d->tex[0] = (int32_t)P->palette_tex;
        d->tex[3] = (int32_t)t->tex;
        d->instances = runs[k].count;
        colour_set(u, d, v, s);
        pal_set(u, (int)box[0], (int)box[1], (int)box[2], (int)box[3]);
        d->depth = RR_DEPTH_LEQUAL;
        stencil_of(d, v, s, 4);
        /* the first sprite to show a pixel keeps it, behind a layer or not */
        d->stencil = RR_STENCIL_EQUAL_INC;
        d->read_mask |= SB_SPRITE;
        d->write_mask = SB_SPRITE;
        in_group(d);
    }
}

/* ---- a screen ---- */

/* A screen's draws front to back, so that what is hidden is not shaded: the
   sprites, Mode 7, the layers, the backdrop. No result depends on their
   order: no two layers take the same depth (zbg, zobj and depth_of give each
   priority of each layer and the sprites their own, and a draw list's depths
   lie between its layer's and the next one's), a layer's own draws are in
   disjoint rectangles or keep their order, and the sprites' stencil bit is
   set by the sprites alone, in their own order, wherever one is drawn. */
static void screen_draws(int v)
{
    Ctx c;
    c.view = v;
    c.n = 0;
    strips(&c, eq_spr, emit_spr);
    groups(&c, eq_spr, spr_group);
    strips(&c, eq_m7, emit_m7);
    for (c.n = 0; c.n < 4; c.n++) {
        strips(&c, eq_bg, emit_bg);
        groups(&c, eq_bg, emit_list);
    }
    c.n = 0;
    strips(&c, eq_backdrop, emit_backdrop);
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
static int (*const masks_of[5])(const eng_state *) = { masks0, masks1, masks2, masks3, masks4 };

/* ---- the colour window: a bit a picture pixel, set inside it ---- */

/* the first word of the table, rows of (width + 31) / 32 words, the bits set
   where a row's column is inside the colour window of its state; none unless
   a state reads it */
static void window_table(void)
{
    uint32_t per = ((uint32_t)P->w + 31) / 32, *bits;
    int r;
    win_at = 0;
    F->data[rows_at - 1] = 0;
    if (!any_state(uses_cwin))
        return;
    win_at = words((size_t)P->h * per);
    F->data[rows_at - 1] = win_at;
    bits = &F->data[win_at];
    memset(bits, 0, (size_t)P->h * per * 4);
    for (r = 0; r < P->h; r++) {
        const eng_row *row = &P->rows[r];
        uint32_t i;
        for (i = 0; i < row->count; i++) {
            const eng_part *pt = &P->parts[row->first + i];
            const eng_state *s = &P->states[pt->state];
            int x0 = pt->x0, x1 = i + 1 < row->count ? P->parts[row->first + i + 1].x0 : P->w, b[6], nb = 0, use = s->w.layer_use[5], j, x;
            if (x0 < 0) x0 = 0;
            if (x1 > P->w) x1 = P->w;
            if (x1 <= x0 || !uses_cwin(s))
                continue;
            b[nb++] = x0;
            if (use & (NSHVC_WIN1 | NSHVC_WIN2)) {
                if (s->w.w[0].left > x0 && s->w.w[0].left < x1) b[nb++] = s->w.w[0].left;
                if (s->w.w[0].right + 1 > x0 && s->w.w[0].right + 1 < x1) b[nb++] = s->w.w[0].right + 1;
                if (s->w.w[1].left > x0 && s->w.w[1].left < x1) b[nb++] = s->w.w[1].left;
                if (s->w.w[1].right + 1 > x0 && s->w.w[1].right + 1 < x1) b[nb++] = s->w.w[1].right + 1;
                qsort(b, (size_t)nb, sizeof *b, cmp_int);
            }
            b[nb] = x1;
            for (j = 0; j < nb; j++) {
                if (b[j + 1] <= b[j] || !inside(&s->w, use, b[j]))
                    continue;
                /* the run's bits by whole words, masks at its two ends */
                x = b[j];
                for (;;) {
                    uint32_t lo = (uint32_t)(x & 31), end = (uint32_t)(b[j + 1] - (x & ~31)), hi = end < 32 ? end : 32;
                    uint32_t m = (hi == 32 ? ~0u : (1u << hi) - 1u) & (~0u << lo);
                    bits[(size_t)r * per + (size_t)(x >> 5)] |= m;
                    x = (x & ~31) + 32;
                    if (x >= b[j + 1])
                        break;
                }
            }
        }
    }
}

/* ---- passes ---- */

/* black where nothing of the picture is shown: outside the part shown */
static void show_rects(void)
{
    rr_uniform *u;
    rr_cmd *d;
    if (P->show_t > 0)
        rect_add(0.0f, 0.0f, (float)P->w, (float)P->show_t);
    if (P->show_b < P->h)
        rect_add(0.0f, (float)P->show_b, (float)P->w, (float)P->h);
    if (P->show_l > 0)
        rect_add(0.0f, (float)P->show_t, (float)P->show_l, (float)P->show_b);
    if (P->show_r < P->w)
        rect_add((float)P->show_r, (float)P->show_t, (float)P->w, (float)P->show_b);
    if ((d = fill_rects(&u)))
        u->f[3][3] = 1.0f;
}

/* the first half-column of a hires row is black, whatever is drawn there: the
   nearest depth, drawn first, hides everything after it */
static void emit_first(const Ctx *c, const eng_state *s, int x0, int y0, int x1, int y1)
{
    (void)c;
    (void)x1;
    if (hires_of(s) && !s->blank && x0 == 0)
        rect_add(0.0f, (float)y0, 0.5f, (float)y1);
}

/* a pass for a target's screens: the windows' bits, then the screen: the main
   (V_MAIN and V_SUBM) or one view of the other two */
static void target_pass(int target, int v)
{
    int L;
    pass_begin(target, 0);
    F->passes[F->npasses - 1].clear[3] = in_frame(v) ? 1.0f : 0.0f;     /* opaque black; no layer, no backdrop */
    for (L = 0; L < 5; L++)
        if (any_state(masks_of[L]))
            window_bits(L, SB_LAYER0 << L);
    if (in_frame(v)) {
        Ctx c = { V_MAIN, 0 };
        rr_uniform *u;
        rr_cmd *d;
        if (any_state(hires_of)) {
            strips(&c, eq_hires, emit_stripes);
            if ((d = fill_rects(&u))) {
                d->blend = RR_BLEND_NOCOLOUR;
                d->stencil = RR_STENCIL_SET;
                d->write_mask = SB_STRIPE;
                d->ref = 0xFF;
            }
            strips(&c, eq_hires, emit_first);
            if ((d = fill_rects(&u))) {
                u->f[3][3] = 1.0f;
                u->f[4][0] = 0.0f;
                d->depth = RR_DEPTH_LEQUAL;
            }
        }
        screen_draws(V_MAIN);
        screen_draws(V_SUBM);
    } else {
        screen_draws(v);
    }
}

/* the overlays over the window */
static void overlay_draws(void)
{
    int k;
    for (k = 0; k < NSHVC_OVERLAYS; k++) {
        const eng_overlay *o = &P->overlays[k];
        const eng_image *im;
        rr_uniform *u;
        rr_cmd *d;
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
}

/* ---- Mode 7's plane, and in colour ---- */
static int uses_m7(const eng_state *s) { return s->m == 7 && m7_valid(s) && !s->blank; }
static uint32_t plane_version;

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
    plane_version++;
    pass_begin(RR_TARGET_TEXTURE, plane_tex);
    d = draw(P_PLANE, &u);
    u->f[1][0] = 1024.0f;
    u->f[1][1] = 1024.0f;
    u->i[0][0] = (int32_t)m->w;
    u->i[0][1] = (int32_t)m->h;
    u->i[0][2] = (int32_t)t->cols;
    u->i[0][3] = (int32_t)t->tpp;
    d->tex[3] = (int32_t)m->tex;
    d->tex[4] = (int32_t)t->tex;
    d->space = RR_SPACE_TARGET;
    d->instances = 128 * 128;
    pass_end();
}

/* Where Mode 7's plane is drawn smaller than its texels, it is read along
   each pixel's footprint by the fragment stage's own arithmetic (the level
   of the footprint's size, each texel of it by its share of the footprint's
   area; mode7.frag): the plane in colour, full size with every smaller
   level (each the box mean of the one below). For each of BG1 (kind 1) and BG2's two priorities (2, 3) the base is
   the plane, and its tile 0 for the outside, in the palette most of the rows
   drawing it have. A row whose palette gives a plane value another colour
   reads the plane in its own palette instead. The four such palettes whose
   rows are nearest (the fewest texels a window pixel, of those drawn smaller
   than their texels) have it full size, so they are exact. Past those, each
   has the plane in its palette at a quarter's side (256 x 256), and the row
   reads the base plus that minus the base at the same size: exact wherever a
   pixel spans four texels or more, and nearer only the difference's finest
   detail is lost. Tile 0 (8 x 8) is kept whole in every palette. Each plane
   is kept by what it shows, made again only when the plane, its tiles or
   that palette change.

   Outside the plane: where it repeats, it is read repeating; where it is
   see-through, it is read as transparent outside its texture; where tile 0
   repeats outside it, tile 0 is read repeating everywhere, plus the plane
   less tile 0 repeated across the plane, both transparent outside their
   textures: so a footprint across the plane's edge mixes the plane and
   what is outside it by how much of the footprint each has, at any
   distance. Tile 0 repeated across the plane is kept full size for the
   base and for each full-size plane drawn so (in a pool of up to 7 layers),
   and past those at a quarter's side beside the palette's plane.

   Memory: the plane (1024 x 1024, a byte a texel: 1 MB); the three bases and
   the four full-size planes (1024 x 1024 BGRA with smaller levels, about
   5.6 MB each: 16.8 MB and 22.4 MB); the quarter-size planes in arrays of at
   most 64 layers (about 349 KB each, the first six of each array the
   bases' at that size), 22.4 MB for the first 57 such palettes in a picture
   (one array keeps a layer spare), an array more past those; with tile 0
   outside, a palette takes two layers. Tile 0's are 8 x 8. Where tile 0 is outside: tile 0 repeated
   across the plane, about 5.6 MB for each base and full-size plane drawn
   so, at most 7 of them. */
#define M7_FULL 4           /* full-size planes for palettes other than the bases' */
#define M7_SMALL 256        /* the side of the planes past those */
#define M7_LAYERS 64        /* an array's layers at most: the bases at that size, then palettes */
#define M7_KEEP 6           /* an array's layers for the bases: their planes (kind - 1), tile 0 repeated (kind + 2) */
#define M7_QS (3 + M7_FULL)   /* the most planes of tile 0 repeated full size: the bases and full-size planes */

/* what a plane in colour shows: kind, direct colour, the plane's version,
   its tiles and their writes; then each plane value's colour */
typedef struct { uint32_t key[5]; int32_t colours[256]; } Look;
typedef struct { Look look; uint32_t plane, tile0; uint8_t made, ok, claimed; int q; } Full;   /* q: its tile 0 repeated in qpool's layer, or -1 */
/* hash: each layer's look's (look_hash), compared before the look itself */
typedef struct { uint32_t plane, tile0; int layers; Look look[M7_LAYERS]; uint32_t hash[M7_LAYERS]; uint8_t ok[M7_LAYERS], claimed[M7_LAYERS]; } Small;
/* a plane wanted past the bases': q its tile 0 repeated, an array's layer, or -1 (none, or the same as the base's) */
typedef struct { const eng_state *s; int kind, tile0_out, qneed, q; double rank; int32_t place; uint32_t hash; Look look; } Want;   /* hash: look's */
/* what Mode 7 shows for a kind on some rows: one look (palette version,
   direct colour, tile set) and the place it is read from, found from a state
   by its palette version through a chain (next) */
typedef struct { const eng_state *s; Look look; uint32_t hash; int next, rows, tile0_out; double rank; int32_t want, place, q; } Var;
static Full bases[3], fulls[M7_FULL];
static Small **smalls;
static size_t nsmalls, cap_smalls;
static int base_on[3];              /* by kind: whether its base was drawn this frame */
static Var *vars[3];                /* by kind, this frame's; place: 0 the base, 1 + n full plane n, else M7_SMALLS + array * M7_LAYERS + layer */
static size_t nvars[3], cap_vars[3];
static int32_t *var_head[3];        /* by kind and palette version: its first look, or -1 */
static size_t cap_head[3];
static uint32_t qpool;              /* the full-size planes of tile 0 repeated: an array */
static int q_layers;
static Look q_look[M7_QS];
static uint8_t q_ok[M7_QS], q_claimed[M7_QS];
#define M7_SMALLS (1 + M7_FULL)

/* plane value t's colour in kind's palette, as 0xRRGGBB, or -1 where see-through */
static int32_t m7_colour(const eng_state *s, int kind, int t)
{
    const uint32_t *pal = P->palettes + (size_t)s->pal * 256;
    if (kind == 1) {
        if (!t)
            return -1;
        if (s->l[0].direct_colour) {
            uint32_t c = (uint32_t)t, v = ((c & 7u) << 2) | ((c & 0x38u) >> 1) << 5 | ((c & 0xC0u) >> 3) << 10, r = v & 31, g = (v >> 5) & 31,
                     b = (v >> 10) & 31;
            return (int32_t)((r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2));
        }
        return (int32_t)(pal[t] & 0xFFFFFFu);
    }
    if (!(t & 0x7F) || (t >> 7) != kind - 2)
        return -1;
    return (int32_t)(pal[t & 0x7F] & 0xFFFFFFu);
}

static void look_of(Look *l, const eng_state *s, int kind)
{
    int t;
    l->key[0] = (uint32_t)kind;
    l->key[1] = (uint32_t)(kind == 1 && s->l[0].direct_colour);
    l->key[2] = plane_version;
    l->key[3] = s->m7.tiles.id;
    l->key[4] = eng_tiles_of[s->m7.tiles.id].gen;
    for (t = 0; t < 256; t++)
        l->colours[t] = m7_colour(s, kind, t);
}

static int look_eq(const Look *a, const Look *b) { return !memcmp(a, b, sizeof *a); }

/* what tile 0 repeated across the plane shows in s's palette for kind: tile
   0's own texels' colours (the tile set's copy), nothing of the plane */
static void tile0_look(Look *q, const eng_state *s, int kind)
{
    const eng_tiles *t = &eng_tiles_of[s->m7.tiles.id];
    int i;
    memset(q, 0, sizeof *q);
    q->key[0] = (uint32_t)kind | 0x100u;
    q->key[1] = (uint32_t)(kind == 1 && s->l[0].direct_colour);
    q->key[3] = s->m7.tiles.id;
    q->key[4] = t->tile0_gen;
    for (i = 0; i < 64; i++)
        q->colours[i] = m7_colour(s, kind, t->tile0[i]);
}

static uint32_t look_hash(const Look *l)
{
    const uint8_t *b = (const uint8_t *)l;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof *l; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* the plane (source 0), or its tile 0 repeated (1: 8 x 8 is tile 0 itself), in
   s's palette for kind, into tex (its layer `layer` if an array, else -1),
   size texels a side, each the mean of the plane's texels it covers */
static void tint_pass(uint32_t tex, int size, int source, int layer, const eng_state *s, int kind)
{
    const eng_tiles *t = &eng_tiles_of[s->m7.tiles.id];
    rr_uniform *u;
    rr_cmd *d;
    pass_begin(RR_TARGET_TEXTURE, tex);
    if (layer >= 0)
        F->passes[F->npasses - 1].layer = (uint32_t)layer;
    F->passes[F->npasses - 1].mips = 1;
    d = draw(P_TINT, &u);
    u->f[1][0] = (float)size;
    u->f[1][1] = (float)size;
    u->i[0][0] = source;
    u->i[0][1] = (int32_t)t->tpp;
    u->i[0][2] = size == 8 ? 1 : 1024 / size;   /* each texel the mean of these a side of the plane's */
    u->i[2][3] = s->pal;
    u->i[3][2] = kind == 1 && s->l[0].direct_colour;
    u->i[6][0] = kind;
    d->tex[0] = (int32_t)P->palette_tex;
    d->tex[3] = (int32_t)plane_tex;
    d->tex[4] = (int32_t)t->tex;
    d->space = RR_SPACE_TARGET;
    pass_end();
}

/* a full-size plane and its tile 0 made to show l, s's palette for kind */
static void full_make(Full *f, const eng_state *s, int kind, const Look *l)
{
    if (!f->made) {
        f->plane = eng_texture_id();
        f->tile0 = eng_texture_id();
        eng_texture_create(F, f->plane, 1024, 1024, RR_BGRA8, RR_TEX_TARGET | RR_TEX_MIPS);
        eng_texture_create(F, f->tile0, 8, 8, RR_BGRA8, RR_TEX_TARGET | RR_TEX_MIPS);
        f->made = 1;
    }
    f->look = *l;
    f->ok = 1;
    tint_pass(f->plane, 1024, 0, -1, s, kind);
    tint_pass(f->tile0, 8, 1, -1, s, kind);
}

/* array a (re)made with `layers` layers: what its layers showed is gone */
static void small_size(Small *a, int layers)
{
    if (!a->plane) {
        a->plane = eng_texture_id();
        a->tile0 = eng_texture_id();
    }
    eng_texture_array(F, a->plane, M7_SMALL, M7_SMALL, (uint32_t)layers, RR_TEX_TARGET | RR_TEX_MIPS);
    eng_texture_array(F, a->tile0, 8, 8, (uint32_t)layers, RR_TEX_TARGET | RR_TEX_MIPS);
    a->layers = layers;
    memset(a->ok, 0, sizeof a->ok);
}

static int m7_drawn(const eng_state *s, int kind)
{
    if (!uses_m7(s) || s->l[0].mosaic > 1)
        return 0;
    return kind == 1 ? s->l[0].enabled_main || s->l[0].enabled_sub : s->m7.extbg && (s->l[1].enabled_main || s->l[1].enabled_sub);
}

/* the texel step from row r to row r + 1 at column x, each by its own
   matrix; where either is no Mode 7, s's own step within a row */
static double m7_row_step(const eng_state *s, int r, double x)
{
    double t[6], n[6], dx, dy;
    const eng_state *a = r >= 0 && r < P->h ? state_at(r, (int)x) : NULL, *b = r + 1 >= 0 && r + 1 < P->h ? state_at(r + 1, (int)x) : NULL;
    if (a && b && a->m == 7 && !a->blank && m7_valid(a) && b->m == 7 && !b->blank && m7_valid(b)) {
        m7_coeffs(a, r, t);
        m7_coeffs(b, r + 1, n);
        dx = n[0] * x + n[1] * (r + 1.5) + n[2] - (t[0] * x + t[1] * (r + 0.5) + t[2]);
        dy = n[3] * x + n[4] * (r + 1.5) + n[5] - (t[3] * x + t[4] * (r + 0.5) + t[5]);
    } else {
        m7_coeffs(s, r, t);
        dx = t[1];
        dy = t[4];
    }
    return sqrt(dx * dx + dy * dy);
}

/* the most texels a window pixel can span at row r, columns x0-x1, for
   state s: across, by its matrix; down, by the step from the row above and
   to the row below (the shader's footprint, the shorter of those, is no more than the larger), at both
   ends of the run, where the most is (each step's length is convex along a
   row); sx, sy window pixels a picture pixel. More than 1: drawn smaller
   than its texels somewhere there */
static double m7_reach(const eng_state *s, int r, int x0, int x1, double sx, double sy)
{
    double t[6], most, e;
    int k;
    m7_coeffs(s, r, t);
    most = sqrt(t[0] * t[0] + t[3] * t[3]) / sx;
    for (k = 0; k < 2; k++) {
        double x = k ? x1 - 1 : x0;
        if ((e = m7_row_step(s, r, x) / sy) > most)
            most = e;
        if ((e = m7_row_step(s, r - 1, x) / sy) > most)
            most = e;
    }
    return most;
}

static int cmp_want(const void *a, const void *b)
{
    const Want *p = (const Want *)a, *q = (const Want *)b;
    if (p->rank != q->rank)
        return p->rank < q->rank ? -1 : 1;
    return p->kind != q->kind ? p->kind - q->kind : p->s->pal - q->s->pal;
}

/* the first M7_FULL wants full size: those already showing what they want kept, the rest made */
static void full_places(Want *wants, size_t nw)
{
    size_t k, c;
    for (c = 0; c < M7_FULL; c++)
        fulls[c].claimed = 0;
    for (k = 0; k < nw && k < M7_FULL; k++) {
        wants[k].place = -1;
        for (c = 0; c < M7_FULL; c++)
            if (!fulls[c].claimed && fulls[c].ok && look_eq(&fulls[c].look, &wants[k].look)) {
                fulls[c].claimed = 1;
                wants[k].place = (int32_t)(1 + c);
                break;
            }
    }
    for (k = 0; k < nw && k < M7_FULL; k++) {
        if (wants[k].place >= 0)
            continue;
        for (c = 0; fulls[c].claimed; c++)
            ;
        fulls[c].claimed = 1;
        full_make(&fulls[c], wants[k].s, wants[k].kind, &wants[k].look);
        wants[k].place = (int32_t)(1 + c);
    }
}

/* a layer of array a no one has this frame, past the bases'; -1 none */
static int small_free(const Small *a)
{
    int l;
    for (l = M7_KEEP; l < a->layers; l++)
        if (!a->claimed[l])
            return l;
    return -1;
}

/* a layer of array a showing look (hash its look_hash), past the bases': one
   no one has this frame, or (any) one already had, which can be shared; -1 none */
static int small_find(const Small *a, const Look *look, uint32_t hash, int any)
{
    int l;
    for (l = M7_KEEP; l < a->layers; l++)
        if ((any || !a->claimed[l]) && a->ok[l] && a->hash[l] == hash && look_eq(&a->look[l], look))
            return l;
    return -1;
}

/* layer l of array a set to show look, drawn from s for kind (source 0 the plane, 1 tile 0 repeated) */
static void small_make(Small *a, int l, const Look *look, uint32_t hash, int source, const eng_state *s, int kind)
{
    a->claimed[l] = 1;
    a->look[l] = *look;
    a->hash[l] = hash;
    a->ok[l] = 1;
    tint_pass(a->plane, M7_SMALL, source, l, s, kind);
    if (!source)
        tint_pass(a->tile0, 8, 1, l, s, kind);
}

/* the wants past the full-size ones at a quarter's side: each its plane in a
   layer, and, where tile 0 is outside and its colours are not the base's,
   its tile 0 repeated in another of the same array, shared by the wants
   there with the same colouring. Every layer already showing what a want
   needs is kept before any is made, so none is made over one still wanted */
static void small_places(Want *wants, size_t nw, const eng_state *const *base_of)
{
    size_t k, c, need = 0, room;
    int l;
    Look q, bq;
    uint32_t qh;
    for (k = 0; k < nw; k++) {
        wants[k].qneed = 0;
        if (wants[k].tile0_out) {
            tile0_look(&q, wants[k].s, wants[k].kind);
            tile0_look(&bq, base_of[wants[k].kind - 1], wants[k].kind);
            wants[k].qneed = !look_eq(&q, &bq);
        }
        need += 1 + (size_t)wants[k].qneed;
    }
    /* room for them, and a layer more an array: a want's layers are in one
       array, so with that much free some array always has room for the next */
    for (;;) {
        room = 0;
        for (c = 0; c < nsmalls; c++)
            room += (size_t)(smalls[c]->layers - M7_KEEP);
        if (room >= need + nsmalls)
            break;
        if (nsmalls && smalls[nsmalls - 1]->layers < M7_LAYERS) {
            small_size(smalls[nsmalls - 1], smalls[nsmalls - 1]->layers * 2);
            continue;
        }
        smalls = (Small **)grow((void *)smalls, &cap_smalls, nsmalls + 1, sizeof *smalls);
        if (!(smalls[nsmalls] = (Small *)calloc(1, sizeof **smalls))) {
            eng_report("nativeshvc", "no memory for Mode 7's planes");
            exit(70);
        }
        small_size(smalls[nsmalls++], 16);
    }
    for (c = 0; c < nsmalls; c++)
        memset(smalls[c]->claimed, 0, sizeof smalls[c]->claimed);
    /* kept: every layer showing a want's plane, and its tile 0 repeated beside it */
    for (k = 0; k < nw; k++) {
        wants[k].place = -1;
        wants[k].q = -1;
        for (c = 0; c < nsmalls; c++) {
            Small *a = smalls[c];
            if ((l = small_find(a, &wants[k].look, wants[k].hash, 0)) < 0)
                continue;
            a->claimed[l] = 1;
            wants[k].place = (int32_t)(M7_SMALLS + c * M7_LAYERS + (size_t)l);
            if (wants[k].qneed) {
                tile0_look(&q, wants[k].s, wants[k].kind);
                if ((wants[k].q = small_find(a, &q, look_hash(&q), 1)) >= 0)
                    a->claimed[wants[k].q] = 1;
            }
            break;
        }
    }
    /* a kept plane whose tile 0 repeated is not beside it: one made there by
       another want shared, else made there in a layer no one keeps, or, with
       none, both made elsewhere */
    for (k = 0; k < nw; k++) {
        Small *a;
        if (wants[k].place < 0 || !wants[k].qneed || wants[k].q >= 0)
            continue;
        c = (size_t)(wants[k].place - M7_SMALLS) / M7_LAYERS;
        a = smalls[c];
        tile0_look(&q, wants[k].s, wants[k].kind);
        qh = look_hash(&q);
        if ((l = small_find(a, &q, qh, 1)) < 0 && (l = small_free(a)) >= 0)
            small_make(a, l, &q, qh, 1, wants[k].s, wants[k].kind);
        if (l < 0) {
            a->claimed[(wants[k].place - M7_SMALLS) % M7_LAYERS] = 0;
            wants[k].place = -1;
            continue;
        }
        a->claimed[l] = 1;
        wants[k].q = l;
    }
    /* made: the rest, in an array with room for all its layers */
    for (k = 0; k < nw; k++) {
        Small *a = NULL;
        if (wants[k].place >= 0)
            continue;
        for (c = 0; c < nsmalls; c++) {
            int n = 0;
            for (l = M7_KEEP; l < smalls[c]->layers; l++)
                n += !smalls[c]->claimed[l];
            if (n >= 1 + wants[k].qneed) {
                a = smalls[c];
                break;
            }
        }
        l = small_free(a);
        small_make(a, l, &wants[k].look, wants[k].hash, 0, wants[k].s, wants[k].kind);
        wants[k].place = (int32_t)(M7_SMALLS + c * M7_LAYERS + (size_t)l);
        wants[k].q = -1;
        if (wants[k].qneed) {
            tile0_look(&q, wants[k].s, wants[k].kind);
            qh = look_hash(&q);
            if ((l = small_find(a, &q, qh, 1)) < 0) {
                l = small_free(a);
                small_make(a, l, &q, qh, 1, wants[k].s, wants[k].kind);
            }
            a->claimed[l] = 1;
            wants[k].q = l;
        }
    }
    /* each array a palette of a kind is in: that kind's base at its size in
       layer kind - 1, and its tile 0 repeated in kind + 2 where a palette's is */
    for (c = 0; c < nsmalls; c++) {
        Small *a = smalls[c];
        int kinds = 0, kind;
        for (l = M7_KEEP; l < a->layers; l++)
            if (a->claimed[l])
                kinds |= 1 << (a->look[l].key[0] & 0xFFu) << (a->look[l].key[0] & 0x100u ? 4 : 0);
        for (kind = 1; kind <= 3; kind++) {
            if (!(kinds & (1 << kind | 1 << kind << 4)))
                continue;
            look_of(&q, base_of[kind - 1], kind);
            if ((kinds & 1 << kind) && !(a->ok[kind - 1] && look_eq(&a->look[kind - 1], &q))) {
                a->look[kind - 1] = q;
                a->ok[kind - 1] = 1;
                tint_pass(a->plane, M7_SMALL, 0, kind - 1, base_of[kind - 1], kind);
            }
            tile0_look(&q, base_of[kind - 1], kind);
            if ((kinds & 1 << kind << 4) && !(a->ok[kind + 2] && look_eq(&a->look[kind + 2], &q))) {
                a->look[kind + 2] = q;
                a->ok[kind + 2] = 1;
                tint_pass(a->plane, M7_SMALL, 1, kind + 2, base_of[kind - 1], kind);
            }
        }
    }
}

/* tile 0 repeated across the plane, full size, for each base and full-size
   plane drawn with tile 0 outside: one layer of the pool for each distinct
   tile 0 colouring among them, those already showing it kept */
static void q_places(const Want *wants, size_t nw, const int *tile0_out, const eng_state *const *base_of)
{
    Full *f[M7_QS];
    const eng_state *s[M7_QS];
    int kinds[M7_QS], n = 0, i, l, pass;
    size_t k;
    Look q;
    for (i = 0; i < 3; i++)
        if (base_on[i] && tile0_out[i]) {
            f[n] = &bases[i];
            s[n] = base_of[i];
            kinds[n++] = i + 1;
        }
    for (k = 0; k < nw && k < M7_FULL; k++)
        if (wants[k].tile0_out) {
            f[n] = &fulls[wants[k].place - 1];
            s[n] = wants[k].s;
            kinds[n++] = wants[k].kind;
        }
    if (!n)
        return;
    if (q_layers < n) {
        /* as many layers as could be wanted, no more (each about 5.6 MB): the
           pool made again, every layer to be drawn again */
        if (!qpool)
            qpool = eng_texture_id();
        q_layers = n;
        eng_texture_array(F, qpool, 1024, 1024, (uint32_t)q_layers, RR_TEX_TARGET | RR_TEX_MIPS);
        memset(q_ok, 0, sizeof q_ok);
    }
    memset(q_claimed, 0, sizeof q_claimed);
    /* first those a layer already shows (shared where the colourings are the same), then the rest made */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < n; i++) {
            if (f[i]->q >= 0)
                continue;
            tile0_look(&q, s[i], kinds[i]);
            for (l = 0; l < q_layers; l++)
                if (q_ok[l] && look_eq(&q_look[l], &q))
                    break;
            if (l < q_layers) {
                q_claimed[l] = 1;
                f[i]->q = l;
                continue;
            }
            if (!pass)
                continue;
            for (l = 0; q_claimed[l]; l++)
                ;
            q_claimed[l] = 1;
            q_look[l] = q;
            q_ok[l] = 1;
            tint_pass(qpool, 1024, 1, l, s[i], kinds[i]);
            f[i]->q = l;
        }
}

/* the look kind shows for state s: -1 none this frame */
static int var_of(const eng_state *s, int kind)
{
    int i;
    uint32_t d = (uint32_t)(kind == 1 && s->l[0].direct_colour);
    if (!var_head[kind - 1] || s->pal < 0 || s->pal >= P->npalettes)
        return -1;
    for (i = var_head[kind - 1][s->pal]; i >= 0; i = vars[kind - 1][i].next)
        if (vars[kind - 1][i].look.key[1] == d && vars[kind - 1][i].look.key[3] == s->m7.tiles.id)
            return i;
    return -1;
}

static void tint_passes(void)
{
    static Want *wants;
    static int32_t *order;
    static size_t cap_wants, cap_order;
    size_t nv = (size_t)P->npalettes, nw = 0, k, c;
    int r, kind, pw = 0, ph = 0;
    double sx, sy;
    const eng_state *base_of[3] = {NULL, NULL, NULL};
    int tile0_out[3] = {0, 0, 0};       /* by kind: whether any row has tile 0 outside the plane */
    base_on[0] = base_on[1] = base_on[2] = 0;
    nvars[0] = nvars[1] = nvars[2] = 0;
    if (!nv)
        return;
    /* window pixels a picture pixel, across and down */
    host_window_pixels(&pw, &ph);
    if (pw <= 0 || ph <= 0) {
        pw = F->width;
        ph = F->height;
    }
    if (F->aspect > 0.0f) {
        double h = (double)ph < pw / (double)F->aspect ? (double)ph : pw / (double)F->aspect;
        sx = h * F->aspect / P->w;
        sy = h / P->h;
    } else {
        sx = (double)pw / P->w;
        sy = (double)ph / P->h;
    }
    if (!(sx > 0.0) || !(sy > 0.0))
        sx = sy = 1.0;
    for (c = 0; c < 3; c++)
        bases[c].q = -1;
    for (c = 0; c < M7_FULL; c++)
        fulls[c].q = -1;
    for (kind = 1; kind <= 3; kind++) {
        Var *vs;
        int32_t *head;
        size_t i, j, best_i = 0;
        int best = -1;
        Full *b = &bases[kind - 1];
        var_head[kind - 1] = (int32_t *)grow(var_head[kind - 1], &cap_head[kind - 1], nv, sizeof **var_head);
        head = var_head[kind - 1];
        for (i = 0; i < nv; i++)
            head[i] = -1;
        /* each look drawing this kind: its rows, and its rank: those drawn
           smaller than their texels first, the nearest of them (the fewest
           texels a window pixel) first; then the rest, the nearest to that first */
        for (r = 0; r < P->h; r++) {
            const eng_row *row = &P->rows[r];
            uint32_t pi;
            for (pi = 0; pi < row->count; pi++) {
                const eng_state *s = &P->states[P->parts[row->first + pi].state];
                int x0 = P->parts[row->first + pi].x0, x1 = pi + 1 < row->count ? P->parts[row->first + pi + 1].x0 : P->w, at;
                double rank;
                if (!m7_drawn(s, kind))
                    continue;
                rank = m7_reach(s, r, x0, x1, sx, sy);
                rank = rank > 1.0 ? rank : 2e9 - rank;
                if ((at = var_of(s, kind)) < 0) {
                    Var *nvr;
                    vars[kind - 1] = (Var *)grow(vars[kind - 1], &cap_vars[kind - 1], nvars[kind - 1] + 1, sizeof **vars);
                    at = (int)nvars[kind - 1]++;
                    nvr = &vars[kind - 1][at];
                    nvr->s = s;
                    look_of(&nvr->look, s, kind);
                    nvr->hash = look_hash(&nvr->look);
                    nvr->rows = 0;
                    nvr->tile0_out = 0;
                    nvr->rank = rank;
                    nvr->want = -1;
                    nvr->place = 0;
                    nvr->q = -1;
                    nvr->next = head[s->pal];
                    head[s->pal] = at;
                }
                vs = &vars[kind - 1][at];
                if (rank < vs->rank)
                    vs->rank = rank;
                if (rank >= 1e9)
                    continue;           /* nowhere smaller than its texels: no planes for it */
                vs->rows++;
                if (s->m7.outside == 2) {
                    vs->tile0_out = 1;
                    tile0_out[kind - 1] = 1;
                }
            }
        }
        vs = vars[kind - 1];
        /* the base: the look with the most rows drawn smaller than their texels
           (rows: those only); looks with none have no planes, as nothing filters them */
        for (i = 0; i < nvars[kind - 1]; i++) {
            int total = 0;
            if (!vs[i].rows)
                continue;
            for (j = 0; j < nvars[kind - 1]; j++)
                if (vs[j].hash == vs[i].hash && look_eq(&vs[j].look, &vs[i].look))
                    total += vs[j].rows;
            if (total > best) {
                best = total;
                best_i = i;
            }
        }
        if (best < 0)
            continue;
        base_on[kind - 1] = 1;
        base_of[kind - 1] = vs[best_i].s;
        if (!b->ok || !look_eq(&b->look, &vs[best_i].look))
            full_make(b, vs[best_i].s, kind, &vs[best_i].look);
        /* the other looks, the same ones sharing a want, each ranked by its nearest rows */
        for (i = 0; i < nvars[kind - 1]; i++) {
            if (!vs[i].rows || (vs[i].hash == vs[best_i].hash && look_eq(&vs[i].look, &vs[best_i].look)))
                continue;
            for (j = 0; j < i; j++)
                if (vs[j].want >= 0 && vs[j].hash == vs[i].hash && look_eq(&vs[j].look, &vs[i].look))
                    break;
            if (j < i) {
                Want *o = &wants[vs[j].want];
                vs[i].want = vs[j].want;
                if (vs[i].rank < o->rank)
                    o->rank = vs[i].rank;
                o->tile0_out |= vs[i].tile0_out;
                continue;
            }
            wants = (Want *)grow(wants, &cap_wants, nw + 1, sizeof *wants);
            wants[nw].s = vs[i].s;
            wants[nw].kind = kind;
            wants[nw].rank = vs[i].rank;
            wants[nw].tile0_out = vs[i].tile0_out;
            wants[nw].qneed = 0;
            wants[nw].q = -1;
            wants[nw].look = vs[i].look;
            wants[nw].hash = vs[i].hash;
            vs[i].want = (int32_t)nw;
            nw++;
        }
    }
    if (nw) {
        /* nearest first; each look kept pointing at its want */
        order = (int32_t *)grow(order, &cap_order, nw, sizeof *order);
        for (k = 0; k < nw; k++)
            wants[k].place = (int32_t)k;
        qsort(wants, nw, sizeof *wants, cmp_want);
        for (k = 0; k < nw; k++)
            order[wants[k].place] = (int32_t)k;
        full_places(wants, nw);
        if (nw > M7_FULL)
            small_places(wants + M7_FULL, nw - M7_FULL, base_of);
        for (kind = 1; kind <= 3; kind++)
            for (k = 0; k < nvars[kind - 1]; k++) {
                Var *v = &vars[kind - 1][k];
                if (v->want < 0)
                    continue;
                v->want = order[v->want];
                v->place = wants[v->want].place;
                v->q = wants[v->want].q;
            }
    }
    q_places(wants, nw, tile0_out, base_of);
}

/* a Mode 7 draw of kind where it is drawn smaller than its texels: the
   textures it filters with (mode7.vert) and its uniforms i[7].y, i[5].x and
   i[5].z; 0 when there is nothing to filter with */
static int m7_filtered(const eng_state *s, int kind, rr_cmd *d, rr_uniform *u)
{
    int32_t p;
    int outside = s->m7.outside, at;
    const Full *f;
    const Small *a = NULL;
    if (!base_on[kind - 1] || (at = var_of(s, kind)) < 0 || !vars[kind - 1][at].rows)
        return 0;                       /* nowhere smaller than its texels: drawn sharp */
    p = vars[kind - 1][at].place;
    f = p >= 1 && p < M7_SMALLS ? &fulls[p - 1] : &bases[kind - 1];
    if (outside == 2 && f->q < 0)
        return 0;
    if (p >= M7_SMALLS)
        a = smalls[(p - M7_SMALLS) / M7_LAYERS];
    u->i[7][1] = a ? (p - M7_SMALLS) % M7_LAYERS : -1;
    u->i[5][0] = a ? vars[kind - 1][at].q : -1;
    u->i[5][2] = f->q;
    if (outside == 0) {
        d->tex[1] = (int32_t)f->plane;
        if (a)
            d->tex[6] = (int32_t)a->plane;
        return 1;
    }
    d->tex[2] = (int32_t)f->plane;
    if (a)
        d->tex[7] = (int32_t)a->plane;
    if (outside == 2) {
        d->tex[1] = (int32_t)f->tile0;
        d->tex[8] = (int32_t)qpool;
        if (a)
            d->tex[6] = (int32_t)a->tile0;
    }
    return 1;
}

void eng_compose(rr_frame *f, const eng_picture *p)
{
    rr_uniform *u;
    rr_cmd *d;
    F = f;
    P = p;
    field = field == 1 ? 2 : 1;
    rows_at = 0;
    {
        /* each row's palette versions, by the column each starts at: what the
           layers, sprites and lists read their colours through */
        int r;
        uint32_t at;
        words(1);                       /* the colour window's table, if any (window_table), just before the rows */
        at = words((size_t)p->h * 2);
        rows_at = at;
        for (r = 0; r < p->h; r++) {
            const eng_row *row = &p->rows[r];
            uint32_t i, first = (uint32_t)F->data_words, n = 0;
            int last = -1;
            for (i = 0; i < row->count; i++) {
                const eng_part *pt = &p->parts[row->first + i];
                int pal = p->states[pt->state].pal;
                uint32_t e;
                if (pal == last)
                    continue;
                e = words(2);
                F->data[e] = (uint32_t)(n ? pt->x0 : 0);
                F->data[e + 1] = (uint32_t)pal;
                last = pal;
                n++;
            }
            F->data[at + (uint32_t)r * 2] = first;
            F->data[at + (uint32_t)r * 2 + 1] = n;
        }
    }
    window_table();
    memset(list_put, 0, sizeof list_put);
    memset(cls_put, 0, sizeof cls_put);
    sprite_pieces();
    plane_pass();
    tint_passes();
    side_lists();
    side_passes();
    if (any_state(need_sub)) {
        target_pass(RR_TARGET_EXTRA0, V_SUB);
        pass_end();
    }
    if (any_state(need_aux)) {
        target_pass(RR_TARGET_EXTRA1, V_AUX);
        pass_end();
    }
    if (!p->interlace) {
        /* the window itself: the main screen, the black outside the part shown, the overlays */
        target_pass(RR_TARGET_WINDOW, V_MAIN);
        show_rects();
        overlay_draws();
        pass_end();
        return;
    }
    /* an interlaced picture keeps the other field's half-rows from the picture
       before: the main screen into the frame, this field's half-rows of it into the picture, the picture into the window */
    target_pass(RR_TARGET_FRAME, V_MAIN);
    pass_end();
    pass_begin(RR_TARGET_PICTURE, 0);
    F->passes[F->npasses - 1].load = 1;
    F->passes[F->npasses - 1].clear[3] = 1.0f;     /* opaque black where nothing is drawn */
    {
        int r;
        for (r = 0; r < p->h; r++) {
            float y0 = (float)r + (field == 2 ? 0.5f : 0.0f);
            rect_add(0.0f, y0, (float)p->w, y0 + 0.5f);
        }
        d = rects_draw(P_FIELD, &u);
        d->tex[0] = RR_TEX_FRAME;
    }
    show_rects();
    pass_end();
    pass_begin(RR_TARGET_WINDOW, 0);
    F->passes[F->npasses - 1].clear[3] = 1.0f;
    d = draw(RR_SHOW, &u);
    d->space = RR_SPACE_TARGET;
    d->tex[0] = RR_TEX_PICTURE;
    overlay_draws();
    pass_end();
}
