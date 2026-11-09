/* picture.c: the game's picture calls. Objects (tiles, maps, images) live in
 * one store on the GPU; a write here logs only what changed, and a picture's
 * settings become the description the GPU draws from. No pixel is made here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "inspect.h"
#include "packet.h"
#include "picture_defs.h"
#include "rr.h"

#define MAX_OBJECTS 4096
#define MAX_SPRITES 65536

typedef struct { uint32_t base, count, bpp, tpp; } Tiles;
typedef struct { uint32_t base, w, h; } Map;
typedef struct { uint32_t base, w, h, format; } Image;

static Tiles tiles[MAX_OBJECTS];
static Map maps[MAX_OBJECTS];
static Image images[MAX_OBJECTS];
static uint32_t ntiles = 1, nmaps = 1, nimages = 1;   /* 0: none */
static size_t store_used = 1;       /* word 0 is never an object */

/* two packets: one being filled, one the render thread may be reading */
static eng_packet packets[2];
static int cur;

/* ---- this picture's settings ---- */
typedef struct { uint32_t line; int32_t column; uint32_t order; int field; int32_t value; } Change;
static Change *changes;
static size_t nchanges, cap_changes;

static nshvc_screen screen;
static int screen_set;
static nshvc_limits limits;
static uint8_t mode, bg3, mode_hires;
static nshvc_layer layers[4];
static nshvc_mode7 m7;
static nshvc_windows windows;
static nshvc_blend blend;
static nshvc_sprite *sprites;
static uint32_t nsprites, cap_sprites, sprite_first;
static uint32_t palette[256];       /* kept between pictures */

/* a draw list: filled by the game or by a job given it */
typedef struct {
    uint32_t w, h, rule;
    nshvc_vertex *v;
    size_t nv, cap_v;
    uint32_t *cmd;
    size_t nc, cap_c;
} List;
#define MAX_LISTS 256
static List lists[MAX_LISTS];
static uint32_t nlists = 1;

typedef struct { uint32_t image; float corners[6]; float alpha; } Overlay;
#define TOUCH_ZONES 12
static Overlay overlays[NSHVC_OVERLAYS + TOUCH_ZONES];   /* kept between pictures; the last the on-screen controls */
static int last_screen_w = 256, last_screen_h = 224;
static uint32_t touch_image;

static void reserve(uint32_t **p, size_t *cap, size_t want)
{
    if (want <= *cap)
        return;
    size_t c = *cap ? *cap : 4096;
    while (c < want)
        c *= 2;
    uint32_t *n = (uint32_t *)realloc(*p, c * 4);
    if (!n) {
        eng_report("nativeshvc", "no memory for %zu words", c);
        exit(70);
    }
    *p = n;
    *cap = c;
}

/* a run of words for the store, to be filled by the caller */
static uint32_t *log_write(size_t at, size_t words)
{
    eng_packet *p = &packets[cur];
    uint32_t *o;
    reserve(&p->ops, &p->ops_cap, p->ops_words + 2 + words);
    o = p->ops + p->ops_words;
    o[0] = (uint32_t)at;
    o[1] = (uint32_t)words;
    p->ops_words += 2 + words;
    return o + 2;
}

static uint32_t store_alloc(size_t words)
{
    size_t at = store_used;
    store_used += words;
    return (uint32_t)at;
}

/* ---- objects ---- */
nshvc_tiles nshvc_tiles_create(uint8_t bpp, uint32_t count, uint8_t tpp)
{
    static const char call[] = "nshvc_tiles_create";
    nshvc_tiles r = { 0 };
    if (!eng_on(call, ENG_GAME))
        return r;
    if ((bpp != 2 && bpp != 4 && bpp != 8) || !count || !tpp || ntiles == MAX_OBJECTS) {
        eng_report(call, "bpp %u, count %u, texels_per_pixel %u", bpp, count, tpp);
        return r;
    }
    r.id = ntiles++;
    tiles[r.id].base = store_alloc((size_t)count * 16u * tpp * tpp);
    tiles[r.id].count = count;
    tiles[r.id].bpp = bpp;
    tiles[r.id].tpp = tpp;
    return r;
}

void nshvc_tiles_write(nshvc_tiles t, uint32_t first, const void *data, uint32_t count, nshvc_tile_format f)
{
    static const char call[] = "nshvc_tiles_write";
    Tiles *o;
    const uint8_t *in = (const uint8_t *)data;
    uint32_t *out, n, tw;
    if (!eng_on(call, ENG_GAME))
        return;
    if (!t.id || t.id >= ntiles || !data || first + count > tiles[t.id].count) {
        eng_report(call, "tile set %u, first %u, count %u", t.id, first, count);
        return;
    }
    o = &tiles[t.id];
    tw = 16u * o->tpp * o->tpp;
    out = log_write(o->base + (size_t)first * tw, (size_t)count * tw);
    if (f != NSHVC_TILES_PLANAR) {
        memcpy(out, in, (size_t)count * tw * 4);
        return;
    }
    if (o->tpp != 1) {
        eng_report(call, "the SNES's planar format has one texel a pixel");
        memset(out, 0, (size_t)count * tw * 4);
        return;
    }
    for (n = 0; n < count; n++) {
        uint32_t y, x, p, bytes = o->bpp * 8u;
        uint8_t b[64];
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                uint32_t v = 0;
                for (p = 0; p < o->bpp; p++) {
                    /* planes in pairs: 16 bytes a pair, two bytes a row */
                    uint8_t byte = in[(p / 2) * 16 + y * 2 + (p & 1)];
                    v |= (uint32_t)((byte >> (7 - x)) & 1) << p;
                }
                b[y * 8 + x] = (uint8_t)v;
            }
        memcpy(out + n * tw, b, 64);
        in += bytes;
    }
}

nshvc_map nshvc_map_create(uint32_t w, uint32_t h)
{
    static const char call[] = "nshvc_map_create";
    nshvc_map r = { 0 };
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!w || !h || w > 0xFFFF || h > 0xFFFF || nmaps == MAX_OBJECTS) {
        eng_report(call, "%u x %u", w, h);
        return r;
    }
    r.id = nmaps++;
    maps[r.id].base = store_alloc((size_t)w * h);
    maps[r.id].w = w;
    maps[r.id].h = h;
    return r;
}

static int map_args(const char *call, nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data)
{
    if (!eng_on(call, ENG_GAME))
        return 0;
    if (!m.id || m.id >= nmaps || !data || x + w > maps[m.id].w || y + h > maps[m.id].h) {
        eng_report(call, "map %u, at %u,%u, %u x %u", m.id, x, y, w, h);
        return 0;
    }
    return 1;
}

void nshvc_map_write(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const nshvc_map_entry *e)
{
    Map *o;
    uint32_t r, c;
    if (!map_args("nshvc_map_write", m, x, y, w, h, e))
        return;
    o = &maps[m.id];
    for (r = 0; r < h; r++) {
        uint32_t *out = log_write(o->base + (size_t)(y + r) * o->w + x, w);
        for (c = 0; c < w; c++, e++)
            out[c] = (uint32_t)e->tile | (uint32_t)(e->palette & 7) << 16 | (uint32_t)(e->priority != 0) << 19 |
                     (uint32_t)(e->hflip != 0) << 20 | (uint32_t)(e->vflip != 0) << 21;
    }
}

void nshvc_map_write_snes(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *words)
{
    Map *o;
    uint32_t r, c;
    if (!map_args("nshvc_map_write_snes", m, x, y, w, h, words))
        return;
    o = &maps[m.id];
    for (r = 0; r < h; r++) {
        uint32_t *out = log_write(o->base + (size_t)(y + r) * o->w + x, w);
        for (c = 0; c < w; c++, words++) {
            uint32_t v = *words;
            out[c] = (v & 0x3FF) | ((v >> 10) & 7) << 16 | ((v >> 13) & 1) << 19 | ((v >> 14) & 1) << 20 | ((v >> 15) & 1) << 21;
        }
    }
}

void nshvc_palette_write(uint32_t first, uint32_t count, const uint16_t *c)
{
    uint32_t i;
    if (!eng_on("nshvc_palette_write", ENG_GAME))
        return;
    if (first + count > 256 || !c) {
        eng_report("nshvc_palette_write", "first %u, count %u", first, count);
        return;
    }
    for (i = 0; i < count; i++)
        palette[first + i] = c[i] & 0x7FFFu;
}

void nshvc_palette_write_rgb(uint32_t first, uint32_t count, const uint32_t *c)
{
    uint32_t i;
    if (!eng_on("nshvc_palette_write_rgb", ENG_GAME))
        return;
    if (first + count > 256 || !c) {
        eng_report("nshvc_palette_write_rgb", "first %u, count %u", first, count);
        return;
    }
    for (i = 0; i < count; i++)
        palette[first + i] = PB_RGB | (c[i] & 0xFFFFFFu);
}

nshvc_image nshvc_image_create(uint32_t w, uint32_t h, nshvc_image_format f)
{
    static const char call[] = "nshvc_image_create";
    nshvc_image r = { 0 };
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!w || !h || w > 0xFFFF || h > 0xFFFF || nimages == MAX_OBJECTS) {
        eng_report(call, "%u x %u", w, h);
        return r;
    }
    r.id = nimages++;
    images[r.id].base = store_alloc((size_t)w * h);
    images[r.id].w = w;
    images[r.id].h = h;
    images[r.id].format = (uint32_t)f;
    return r;
}

void nshvc_image_write(nshvc_image i, const void *texels)
{
    static const char call[] = "nshvc_image_write";
    Image *o;
    size_t n, k;
    uint32_t *out;
    if (!eng_on(call, ENG_GAME))
        return;
    if (!i.id || i.id >= nimages || !texels) {
        eng_report(call, "image %u", i.id);
        return;
    }
    o = &images[i.id];
    n = (size_t)o->w * o->h;
    out = log_write(o->base, n);
    if (o->format == NSHVC_IMAGE_RGBA) {
        memcpy(out, texels, n * 4);
    } else {
        const uint8_t *b = (const uint8_t *)texels;
        for (k = 0; k < n; k++)
            out[k] = b[k];
    }
}

/* ---- settings for this picture ---- */
#define IN_DRAW(call) eng_on(call, ENG_GAME)

void nshvc_screen_set(const nshvc_screen *s) { if (IN_DRAW("nshvc_screen_set") && s) { screen = *s; screen_set = 1; } }
void nshvc_limits_set(const nshvc_limits *l) { if (IN_DRAW("nshvc_limits_set") && l) limits = *l; }
void nshvc_mode_set(uint8_t m, uint8_t b3, uint8_t h) { if (IN_DRAW("nshvc_mode_set")) { mode = m & 7; bg3 = b3 != 0; mode_hires = h != 0; } }
void nshvc_mode7_set(const nshvc_mode7 *m) { if (IN_DRAW("nshvc_mode7_set") && m) m7 = *m; }
void nshvc_windows_set(const nshvc_windows *w) { if (IN_DRAW("nshvc_windows_set") && w) windows = *w; }
void nshvc_blend_set(const nshvc_blend *b) { if (IN_DRAW("nshvc_blend_set") && b) blend = *b; }

void nshvc_layer_set(uint8_t n, const nshvc_layer *l)
{
    if (!IN_DRAW("nshvc_layer_set") || !l)
        return;
    if (n > 3) {
        eng_report("nshvc_layer_set", "layer %u", n);
        return;
    }
    layers[n] = *l;
}

void nshvc_sprites_set(const nshvc_sprite *list, uint32_t count, uint32_t first)
{
    static const char call[] = "nshvc_sprites_set";
    if (!IN_DRAW(call))
        return;
    if ((count && !list) || count > MAX_SPRITES) {
        eng_report(call, "%u sprites", count);
        return;
    }
    if (count > cap_sprites) {
        nshvc_sprite *n = (nshvc_sprite *)realloc(sprites, count * sizeof *n);
        if (!n)
            exit(70);
        sprites = n;
        cap_sprites = count;
    }
    if (count)
        memcpy(sprites, list, count * sizeof *sprites);
    nsprites = count;
    sprite_first = count ? first % count : 0;
}

static void add_change(uint32_t line, int32_t column, int field, int32_t value)
{
    if (nchanges == cap_changes) {
        size_t c = cap_changes ? cap_changes * 2 : 1024;
        Change *n = (Change *)realloc(changes, c * sizeof *n);
        if (!n)
            exit(70);
        changes = n;
        cap_changes = c;
    }
    changes[nchanges].line = line;
    changes[nchanges].column = column;
    changes[nchanges].order = (uint32_t)nchanges;
    changes[nchanges].field = field;
    changes[nchanges].value = value;
    nchanges++;
}

void nshvc_line(uint32_t line, nshvc_line_field f, int32_t v) { if (IN_DRAW("nshvc_line")) add_change(line, 0, (int)f, v); }
void nshvc_line_from(uint32_t line, int32_t c, nshvc_line_field f, int32_t v) { if (IN_DRAW("nshvc_line_from")) add_change(line, c, (int)f, v); }
void nshvc_lines(uint32_t first, uint32_t count, nshvc_line_field f, const int32_t *v)
{
    uint32_t i;
    if (!IN_DRAW("nshvc_lines") || !v)
        return;
    for (i = 0; i < count; i++)
        add_change(first + i, 0, (int)f, v[i]);
}

void nshvc_overlay_set(uint32_t id, nshvc_image i, const float corners[6], float alpha)
{
    static const char call[] = "nshvc_overlay_set";
    if (!eng_on(call, ENG_GAME))
        return;
    if (id >= NSHVC_OVERLAYS || (i.id && (i.id >= nimages || images[i.id].format != NSHVC_IMAGE_RGBA)) || (i.id && !corners)) {
        eng_report(call, "overlay %u, image %u", id, i.id);
        return;
    }
    overlays[id].image = i.id;
    if (i.id)
        memcpy(overlays[id].corners, corners, sizeof overlays[id].corners);
    overlays[id].alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
}

/* ---- draw lists ---- */
#define LIST_ROLES (ENG_GAME | ENG_JOB)

nshvc_draw_list nshvc_draw_list_create(void)
{
    static const char call[] = "nshvc_draw_list_create";
    nshvc_draw_list r = { 0 };
    if (!eng_on(call, ENG_GAME))
        return r;
    if (nlists == MAX_LISTS) {
        eng_report(call, "more than %d lists", MAX_LISTS - 1);
        return r;
    }
    r.id = nlists++;
    return r;
}

static List *list_of(const char *call, nshvc_draw_list d)
{
    if (!eng_on(call, LIST_ROLES))
        return NULL;
    if (!d.id || d.id >= nlists) {
        eng_report(call, "list %u", d.id);
        return NULL;
    }
    return &lists[d.id];
}

void nshvc_draw_list_clear(nshvc_draw_list d, uint32_t width, uint32_t height)
{
    List *l = list_of("nshvc_draw_list_clear", d);
    if (!l)
        return;
    if (!width || !height || width > 16384 || height > 16384) {
        eng_report("nshvc_draw_list_clear", "%u x %u", width, height);
        return;
    }
    l->w = width;
    l->h = height;
    l->nv = l->nc = 0;
    l->rule = NSHVC_FILL_SMOOTH;
}

void nshvc_draw_rule(nshvc_draw_list d, nshvc_fill_rule r)
{
    List *l = list_of("nshvc_draw_rule", d);
    if (!l)
        return;
    l->rule = (uint32_t)r;
}

#define MAX_SHADERS 256
static uint32_t nshaders = 1;

nshvc_shader nshvc_shader_load(const char *name)
{
    static const char call[] = "nshvc_shader_load";
    nshvc_shader r = { 0 };
    char path[256];
    nshvc_asset a;
    uint8_t *code = NULL;
    size_t size = 0, cap = 0;
    eng_packet *p = &packets[cur];
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!name || nshaders == MAX_SHADERS || p->nshaders == ENG_SHADER_LOADS) {
        eng_report(call, "shader %s", name ? name : "(none)");
        return r;
    }
    snprintf(path, sizeof path, "shaders/%s.%s", name, rr_shader_ext());
    a = nshvc_asset_open(path);
    if (!a.id) {
        eng_report(call, "no %s in the game's package", path);
        return r;
    }
    for (;;) {
        uint32_t n;
        if (size == cap) {
            uint8_t *g;
            cap = cap ? cap * 2 : 65536;
            if (!(g = (uint8_t *)realloc(code, cap)))
                exit(70);
            code = g;
        }
        n = nshvc_asset_read(a, code + size, (uint32_t)(cap - size));
        if (!n)
            break;
        size += n;
    }
    nshvc_asset_close(a);
    r.id = nshaders++;
    p->shaders[p->nshaders].id = r.id;
    p->shaders[p->nshaders].code = code;   /* kept: the backend owns it from here */
    p->shaders[p->nshaders].size = size;
    p->nshaders++;
    return r;
}

static void list_add(List *l, const nshvc_vertex *v, uint32_t count, uint32_t prim, const nshvc_material *m)
{
    uint32_t base = 0, w = 0, h = 0;
    if (m && m->image.id && m->image.id < nimages) {
        base = images[m->image.id].base;
        w = images[m->image.id].w;
        h = images[m->image.id].h;
    }
    if (l->nv + count > l->cap_v) {
        size_t c = l->cap_v ? l->cap_v : 1024;
        nshvc_vertex *n;
        while (c < l->nv + count)
            c *= 2;
        n = (nshvc_vertex *)realloc(l->v, c * sizeof *n);
        if (!n)
            exit(70);
        l->v = n;
        l->cap_v = c;
    }
    reserve(&l->cmd, &l->cap_c, (l->nc + 1) * ENG_CMD_WORDS);
    memcpy(l->v + l->nv, v, count * sizeof *v);
    if (l->rule == NSHVC_FILL_EXACT) {
        /* the exact rule: whole-pixel vertices, a small offset so that no
           edge meets a pixel's centre, so every GPU fills the same spans */
        size_t k;
        for (k = 0; k < count; k++) {
            nshvc_vertex *o = &l->v[l->nv + k];
            o->x = floorf(o->x + 0.5f) + 1.0f / 64.0f;
            o->y = floorf(o->y + 0.5f) + 1.0f / 128.0f;
        }
    }
    {
        uint32_t *c = l->cmd + l->nc * ENG_CMD_WORDS;
        c[0] = (uint32_t)l->nv;
        c[1] = count;
        c[2] = prim;
        c[3] = base;
        c[4] = w;
        c[5] = h;
        c[6] = m ? m->shader.id : 0;
        if (m)
            memcpy(c + 7, m->inputs, 32);
        else
            memset(c + 7, 0, 32);
    }
    l->nv += count;
    l->nc++;
}

void nshvc_draw_tris(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m)
{
    List *l = list_of("nshvc_draw_tris", d);
    if (!l || !count)
        return;
    if (!v || count % 3) {
        eng_report("nshvc_draw_tris", "%u vertices", count);
        return;
    }
    list_add(l, v, count, 0, m);
}

void nshvc_draw_lines(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m)
{
    List *l = list_of("nshvc_draw_lines", d);
    if (!l || !count)
        return;
    if (!v || count % 2) {
        eng_report("nshvc_draw_lines", "%u vertices", count);
        return;
    }
    list_add(l, v, count, 1, m);
}

void nshvc_draw_image(nshvc_draw_list d, nshvc_image i, const nshvc_rect *to, const nshvc_material *m)
{
    List *l = list_of("nshvc_draw_image", d);
    nshvc_material mm;
    nshvc_vertex q[6];
    float w, h;
    if (!l)
        return;
    if (!i.id || i.id >= nimages || !to) {
        eng_report("nshvc_draw_image", "image %u", i.id);
        return;
    }
    if (m)
        mm = *m;
    else
        memset(&mm, 0, sizeof mm);
    w = (float)images[i.id].w;
    h = (float)images[i.id].h;
    mm.image = i;
    memset(q, 0, sizeof q);
    q[0].x = to->x0; q[0].y = to->y0;
    q[1].x = to->x1; q[1].y = to->y0; q[1].u = w;
    q[2].x = to->x0; q[2].y = to->y1; q[2].v = h;
    q[3] = q[2];
    q[4] = q[1];
    q[5].x = to->x1; q[5].y = to->y1; q[5].u = w; q[5].v = h;
    list_add(l, q, 6, 0, &mm);
}

/* ---- the description ---- */
static int change_cmp(const void *a, const void *b)
{
    const Change *x = (const Change *)a, *y = (const Change *)b;
    if (x->line != y->line)
        return x->line < y->line ? -1 : 1;
    if (x->column != y->column)
        return x->column < y->column ? -1 : 1;
    return x->order < y->order ? -1 : 1;
}

static uint32_t pack2(int32_t lo, int32_t hi) { return ((uint32_t)lo & 0xFFFFu) | (uint32_t)hi << 16; }

static const uint8_t bpp_of[8][4] = {
    { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 }, { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 8, 0, 0, 0 }
};

static void layer_words(uint32_t *l, int n, const nshvc_layer *a, int m)
{
    uint32_t kind = 0, f;
    uint32_t bpp = bpp_of[m & 7][n];
    uint32_t mosaic = a->mosaic ? a->mosaic : 1;
    memset(l, 0, PB_L * 4);
    if (a->image.id && a->image.id < nimages) {
        kind = 2;
        l[PB_L_T] = images[a->image.id].base;
        l[PB_L_M] = pack2((int32_t)images[a->image.id].w, (int32_t)images[a->image.id].h);
    } else if (a->draw_list.id && a->draw_list.id < nlists && lists[a->draw_list.id].w) {
        kind = 3;
        l[PB_L_M] = pack2((int32_t)lists[a->draw_list.id].w, (int32_t)lists[a->draw_list.id].h);
    } else if (a->tiles.id && a->tiles.id < ntiles && a->map.id && a->map.id < nmaps && bpp) {
        kind = 1;
        l[PB_L_T] = tiles[a->tiles.id].base;
        l[PB_L_A] = tiles[a->tiles.id].tpp;
        l[PB_L_M] = maps[a->map.id].base;
        l[PB_L_S] = pack2((int32_t)maps[a->map.id].w, (int32_t)maps[a->map.id].h);
        if (a->offset_per_tile.id && a->offset_per_tile.id < nmaps)
            l[PB_L_O] = maps[a->offset_per_tile.id].base;
    }
    /* a layer that isn't drawn can still hold the map and scroll that
       offset-per-tile reads */
    if (!kind && a->map.id && a->map.id < nmaps)
        l[PB_L_S] = pack2((int32_t)maps[a->map.id].w, (int32_t)maps[a->map.id].h);
    f = (uint32_t)(a->enabled_main != 0) | (uint32_t)(a->enabled_sub != 0) << 1 | (uint32_t)(a->tile_size == 16) << 2 |
        (mosaic - 1) << 4 | (uint32_t)(a->wrap != 0) << 9 | (uint32_t)(a->direct_colour != 0) << 10 | kind << 11 | bpp << 13;
    l[PB_L_F] = f;
    l[PB_L_X] = (uint32_t)a->scroll_x;
    l[PB_L_Y] = (uint32_t)a->scroll_y;
}

/* the state as it stands */
typedef struct {
    int m, b3, hires, bright, blank;
    nshvc_layer l[4];
    nshvc_mode7 m7;
    nshvc_windows w;
    nshvc_blend b;
    int anchor[4];
} Now;

static void state_words(uint32_t *s, const Now *n, uint32_t pal_at)
{
    int i;
    uint32_t mm = 0, ms = 0;
    memset(s, 0, PB_ST * 4);
    s[PB_S_MODE] = (uint32_t)n->m | (uint32_t)n->b3 << 3 | (uint32_t)n->hires << 4 | (uint32_t)n->bright << 8 | (uint32_t)n->blank << 12;
    s[PB_S_BLEND] = (uint32_t)n->b.fixed_bgr15 | (uint32_t)(n->b.subtract != 0) << 16 | (uint32_t)(n->b.half != 0) << 17 |
                    (uint32_t)(n->b.use_fixed != 0) << 18 | (uint32_t)(n->b.layers & 63) << 20 |
                    (uint32_t)(n->b.clip & 3) << 26 | (uint32_t)(n->b.prevent & 3) << 28;
    s[PB_S_W] = (uint32_t)n->w.w[0].left;
    s[PB_S_W + 1] = (uint32_t)n->w.w[0].right;
    s[PB_S_W + 2] = (uint32_t)n->w.w[1].left;
    s[PB_S_W + 3] = (uint32_t)n->w.w[1].right;
    for (i = 0; i < 4; i++)
        s[PB_S_USE] |= (uint32_t)n->w.layer_use[i] << (8 * i);
    for (i = 0; i < 5; i++) {
        mm |= (uint32_t)(n->w.mask_main[i] != 0) << i;
        ms |= (uint32_t)(n->w.mask_sub[i] != 0) << i;
    }
    s[PB_S_USE2] = (uint32_t)n->w.layer_use[4] | (uint32_t)n->w.layer_use[5] << 8 | mm << 16 | ms << 24;
    s[PB_S_M7] = pack2(n->m7.a, n->m7.b);
    s[PB_S_M7 + 1] = pack2(n->m7.c, n->m7.d);
    s[PB_S_M7 + 2] = pack2(n->m7.centre_x, n->m7.centre_y);
    s[PB_S_M7 + 3] = pack2(n->m7.scroll_x, n->m7.scroll_y);
    s[PB_S_M7F] = (uint32_t)(n->m7.outside & 3) | (uint32_t)(n->m7.flip_x != 0) << 2 | (uint32_t)(n->m7.flip_y != 0) << 3 |
                  (uint32_t)(n->m7.extbg != 0) << 4;
    if (n->m7.tiles.id && n->m7.tiles.id < ntiles)
        s[PB_S_M7T] = tiles[n->m7.tiles.id].base;
    if (n->m7.map.id && n->m7.map.id < nmaps)
        s[PB_S_M7M] = maps[n->m7.map.id].base;
    s[PB_S_PAL] = pal_at;
    for (i = 0; i < 4; i++) {
        layer_words(s + PB_S_L + i * PB_L, i, &n->l[i], n->m);
        s[PB_S_L + i * PB_L + PB_L_A] |= (uint32_t)n->anchor[i] << 16;
    }
}

static void apply(Now *n, uint32_t *pal, int field, int32_t v, uint32_t line, int *pal_changed)
{
    int i = field & 3;
    if (field >= NSHVC_LINE_PALETTE) {
        int e = field - NSHVC_LINE_PALETTE;
        if (e < 256) {
            pal[e] = (uint32_t)v;
            *pal_changed = 1;
        }
    } else if (field < NSHVC_LINE_SCROLL_Y) {
        n->l[i].scroll_x = v;
    } else if (field < NSHVC_LINE_MAIN) {
        n->l[i].scroll_y = v;
    } else if (field < NSHVC_LINE_SUB) {
        n->l[i].enabled_main = (uint8_t)v;
    } else if (field < NSHVC_LINE_WIN1_LEFT) {
        n->l[i].enabled_sub = (uint8_t)v;
    } else switch (field) {
    case NSHVC_LINE_WIN1_LEFT: n->w.w[0].left = v; break;
    case NSHVC_LINE_WIN1_RIGHT: n->w.w[0].right = v; break;
    case NSHVC_LINE_WIN2_LEFT: n->w.w[1].left = v; break;
    case NSHVC_LINE_WIN2_RIGHT: n->w.w[1].right = v; break;
    case NSHVC_LINE_FIXED: n->b.fixed_bgr15 = (uint16_t)v; break;
    case NSHVC_LINE_BRIGHTNESS: n->bright = v & 15; break;
    case NSHVC_LINE_MODE: n->m = v & 7; break;
    case NSHVC_LINE_M7_A: n->m7.a = (int16_t)v; break;
    case NSHVC_LINE_M7_B: n->m7.b = (int16_t)v; break;
    case NSHVC_LINE_M7_C: n->m7.c = (int16_t)v; break;
    case NSHVC_LINE_M7_D: n->m7.d = (int16_t)v; break;
    case NSHVC_LINE_M7_CENTRE_X: n->m7.centre_x = (int16_t)v; break;
    case NSHVC_LINE_M7_CENTRE_Y: n->m7.centre_y = (int16_t)v; break;
    case NSHVC_LINE_M7_SCROLL_X: n->m7.scroll_x = (int16_t)v; break;
    case NSHVC_LINE_M7_SCROLL_Y: n->m7.scroll_y = (int16_t)v; break;
    case NSHVC_LINE_M7_OUTSIDE: n->m7.outside = (uint8_t)v; break;
    case NSHVC_LINE_M7_FLIP_X: n->m7.flip_x = (uint8_t)v; break;
    case NSHVC_LINE_M7_FLIP_Y: n->m7.flip_y = (uint8_t)v; break;
    case NSHVC_LINE_M7_EXTBG: n->m7.extbg = (uint8_t)v; break;
    case NSHVC_LINE_BLANK: n->blank = v != 0; break;
    case NSHVC_LINE_BG3_PRIORITY: n->b3 = v != 0; break;
    case NSHVC_LINE_HIRES: n->hires = v != 0; break;
    case NSHVC_LINE_SUBTRACT: n->b.subtract = (uint8_t)v; break;
    case NSHVC_LINE_HALF: n->b.half = (uint8_t)v; break;
    case NSHVC_LINE_USE_FIXED: n->b.use_fixed = (uint8_t)v; break;
    case NSHVC_LINE_BLEND_LAYERS: n->b.layers = (uint8_t)v; break;
    case NSHVC_LINE_CLIP: n->b.clip = (uint8_t)v; break;
    case NSHVC_LINE_PREVENT: n->b.prevent = (uint8_t)v; break;
    default:
        if (field >= NSHVC_LINE_MOSAIC && field < NSHVC_LINE_MOSAIC + 4) {
            n->l[field - NSHVC_LINE_MOSAIC].mosaic = (uint8_t)v;
            n->anchor[field - NSHVC_LINE_MOSAIC] = (int)line;
        } else if (field >= NSHVC_LINE_TILE_SIZE && field < NSHVC_LINE_TILE_SIZE + 4) {
            n->l[field - NSHVC_LINE_TILE_SIZE].tile_size = (uint8_t)v;
        } else if (field >= NSHVC_LINE_WIN_USE && field < NSHVC_LINE_WIN_USE + 6) {
            n->w.layer_use[field - NSHVC_LINE_WIN_USE] = (uint8_t)v;
        } else if (field >= NSHVC_LINE_MASK_MAIN && field < NSHVC_LINE_MASK_MAIN + 5) {
            n->w.mask_main[field - NSHVC_LINE_MASK_MAIN] = (uint8_t)v;
        } else if (field >= NSHVC_LINE_MASK_SUB && field < NSHVC_LINE_MASK_SUB + 5) {
            n->w.mask_sub[field - NSHVC_LINE_MASK_SUB] = (uint8_t)v;
        } else {
            eng_report("nshvc_line", "field %d on line %u", field, line);
        }
        break;
    }
}

/* a sprite's rows on the screen: with the wrap, kept to the original's 256 */
static int sprite_rows(const nshvc_sprite *s)
{
    int n = s->height * 8;
    return limits.sprite_wrap && n > 256 ? 256 : n;
}

static int sprite_row(const nshvc_sprite *s, int k, int oy, int h)
{
    int r = s->y + k;
    if (limits.sprite_wrap)
        r = oy + ((r - oy) & 255);
    return r < 0 || r >= h ? -1 : r;
}

static uint32_t sprite_words(uint32_t *o, const nshvc_sprite *s)
{
    uint32_t base = s->tiles.id && s->tiles.id < ntiles ? tiles[s->tiles.id].base : 0;
    o[PB_P_X] = (uint32_t)s->x;
    o[PB_P_Y] = (uint32_t)s->y;
    o[PB_P_S] = pack2(s->width, s->height);
    o[PB_P_T] = base;
    o[PB_P_N] = (uint32_t)s->tile | (uint32_t)(s->palette & 7) << 16 | (uint32_t)(s->priority & 3) << 19 |
                (uint32_t)(s->hflip != 0) << 21 | (uint32_t)(s->vflip != 0) << 22;
    return base;
}

static void build(eng_packet *p)
{
    int w = screen.width > 0 ? screen.width : 256, h = screen.height > 0 ? screen.height : 224;
    int oy = screen.origin_y, ox = screen.origin_x, r, i, hires_any = 0, lines = screen.overscan ? 239 : 224;
    size_t at, rows_at, parts_at, states_at, sprites_at, entries_at, nparts = 0, cap_parts;
    uint32_t *d, pal_at;
    uint32_t cur_pal[256];
    Now n;
    size_t c = 0;
    static uint32_t *row_count;
    static size_t row_cap;
    size_t states = 0;
    uint32_t total_entries = 0, si;

    memset(&n, 0, sizeof n);
    n.m = mode;
    n.b3 = bg3;
    n.hires = mode_hires;
    n.bright = screen_set ? screen.brightness : 15;
    n.blank = screen.blank;
    memcpy(n.l, layers, sizeof n.l);
    n.m7 = m7;
    n.w = windows;
    n.b = blend;
    memcpy(cur_pal, palette, sizeof cur_pal);
    if (nchanges)
        qsort(changes, nchanges, sizeof *changes, change_cmp);

    /* layout: an upper bound on parts is a part for each change plus each row */
    cap_parts = (size_t)h + nchanges;
    rows_at = PB_HEADER;
    parts_at = rows_at + (size_t)h * 4;
    states_at = parts_at + cap_parts * 2;
    reserve(&p->desc, &p->desc_cap, states_at + PB_ST + 256);
    d = p->desc;
    /* the first state and palette */
    at = states_at + (cap_parts + 1) * PB_ST;
    reserve(&p->desc, &p->desc_cap, at + 256 * (cap_parts + 1));
    d = p->desc;
    pal_at = (uint32_t)at;
    memcpy(d + at, cur_pal, sizeof cur_pal);
    at += 256;
    state_words(d + states_at, &n, pal_at);
    states = 1;
    hires_any = n.hires || n.m == 5 || n.m == 6;

    for (r = 0; r < h; r++) {
        uint32_t line = (uint32_t)(r - oy);
        uint32_t first = (uint32_t)nparts, cnt = 0;
        int have_part = 0;
        /* changes before or at this row's start, then within it */
        while (c < nchanges && (r < oy ? 0 : changes[c].line <= line)) {
            uint32_t ln = changes[c].line;
            int32_t col = ln == line ? changes[c].column : 0;
            int pal_changed = 0;
            /* those at this column together */
            while (c < nchanges && changes[c].line == ln && (ln == line ? changes[c].column : 0) == col) {
                apply(&n, cur_pal, changes[c].field, changes[c].value, ln, &pal_changed);
                c++;
            }
            if (pal_changed) {
                pal_at = (uint32_t)at;
                memcpy(d + at, cur_pal, sizeof cur_pal);
                at += 256;
            }
            if (states < cap_parts + 1) {
                state_words(d + states_at + states * PB_ST, &n, pal_at);
                states++;
            }
            hires_any |= n.hires || n.m == 5 || n.m == 6;
            if (ln == line && col > 0) {
                /* a split inside the row */
                if (!have_part) {
                    d[parts_at + nparts * 2] = 0;
                    d[parts_at + nparts * 2 + 1] = (uint32_t)(states - 2);
                    nparts++;
                    cnt++;
                    have_part = 1;
                }
                d[parts_at + nparts * 2] = (uint32_t)(col + ox);
                d[parts_at + nparts * 2 + 1] = (uint32_t)(states - 1);
                nparts++;
                cnt++;
            }
        }
        if (!have_part) {
            d[parts_at + nparts * 2] = 0;
            d[parts_at + nparts * 2 + 1] = (uint32_t)(states - 1);
            nparts++;
            cnt++;
        }
        d[rows_at + (size_t)r * 4] = first;
        d[rows_at + (size_t)r * 4 + 1] = cnt;
    }

    /* the sprites */
    sprites_at = at;
    reserve(&p->desc, &p->desc_cap, at + (size_t)nsprites * PB_SP);
    d = p->desc;
    for (si = 0; si < nsprites; si++)
        sprite_words(d + sprites_at + (size_t)si * PB_SP, &sprites[si]);
    at += (size_t)nsprites * PB_SP;

    /* each sprite's rows, in priority order */
    if ((size_t)h + 1 > row_cap) {
        free(row_count);
        row_count = (uint32_t *)malloc(((size_t)h + 1) * 4);
        row_cap = row_count ? (size_t)h + 1 : 0;
        if (!row_count)
            exit(70);
    }
    memset(row_count, 0, ((size_t)h + 1) * 4);
    for (si = 0; si < nsprites; si++) {
        const nshvc_sprite *s = &sprites[(sprite_first + si) % nsprites];
        int k, n = sprite_rows(s);
        for (k = 0; k < n; k++)
            if ((r = sprite_row(s, k, oy, h)) >= 0)
                row_count[r]++;
    }
    for (r = 0; r < h; r++) {
        d[rows_at + (size_t)r * 4 + 2] = total_entries;
        total_entries += row_count[r];
    }
    entries_at = at;
    reserve(&p->desc, &p->desc_cap, at + total_entries);
    d = p->desc;
    at += total_entries;
    memset(row_count, 0, (size_t)h * 4);
    for (si = 0; si < nsprites; si++) {
        uint32_t idx = (sprite_first + si) % nsprites;
        const nshvc_sprite *s = &sprites[idx];
        int k, n = sprite_rows(s);
        for (k = 0; k < n; k++)
            if ((r = sprite_row(s, k, oy, h)) >= 0)
                d[entries_at + d[rows_at + (size_t)r * 4 + 2] + row_count[r]++] = idx;
    }
    /* the limits: past the first sprites_per_line the rest are dropped, then
       tiles are spent from the last on the line backward */
    if (limits.sprites_per_line || limits.sprite_tiles_per_line) {
        for (r = oy; r < oy + lines && r < h; r++) {
            uint32_t base, in_range = 0, out = 0, k;
            int tiles_left;
            if (r < 0)
                continue;
            base = (uint32_t)entries_at + d[rows_at + (size_t)r * 4 + 2];
            for (k = 0; k < row_count[r]; k++) {
                uint32_t e = d[base + k];
                const nshvc_sprite *s = &sprites[e & 0xFFFFFFu];
                int in_cols = s->x < ox + 256 && s->x + s->width * 8 > ox;
                if (in_cols && limits.sprites_per_line && in_range >= limits.sprites_per_line)
                    continue;
                in_range += in_cols != 0;
                d[base + out++] = e;
            }
            row_count[r] = out;
            tiles_left = limits.sprite_tiles_per_line ? limits.sprite_tiles_per_line : 0x7FFFFFFF;
            for (k = row_count[r]; k-- > 0;) {
                uint32_t e = d[base + k] & 0xFFFFFFu;
                const nshvc_sprite *s = &sprites[e];
                int sx = s->x - ox, vis = s->width;
                if (sx < 0)
                    vis -= (-sx + 7) / 8;
                if (sx + s->width * 8 > 256)
                    vis -= (sx + s->width * 8 - 256) / 8;
                if (vis < 0)
                    vis = 0;
                if (vis > tiles_left)
                    vis = tiles_left;
                tiles_left -= vis;
                if (limits.sprite_tiles_per_line)
                    d[base + k] = e | (uint32_t)(vis + 1) << 24;
            }
        }
    }
    for (r = 0; r < h; r++)
        d[rows_at + (size_t)r * 4 + 3] = row_count[r];

    /* the overlays: each window pixel's texel from the inverse of its corners */
    {
        uint32_t ov_at = (uint32_t)at, count = 0;
        reserve(&p->desc, &p->desc_cap, at + (NSHVC_OVERLAYS + TOUCH_ZONES) * PB_OV);
        d = p->desc;
        for (si = 0; si < NSHVC_OVERLAYS + TOUCH_ZONES; si++) {
            const Overlay *o = &overlays[si];
            if (o->image && o->alpha > 0) {
                const float *q = o->corners;
                const Image *im = &images[o->image];
                float ax = q[2] - q[0], ay = q[3] - q[1], bx = q[4] - q[0], by = q[5] - q[1], det = ax * by - ay * bx;
                float m[6];
                uint32_t *e = d + at;
                if (det == 0)
                    continue;
                m[0] = by / det * (float)im->w;
                m[1] = -bx / det * (float)im->w;
                m[2] = -(m[0] * q[0] + m[1] * q[1]);
                m[3] = -ay / det * (float)im->h;
                m[4] = ax / det * (float)im->h;
                m[5] = -(m[3] * q[0] + m[4] * q[1]);
                e[PB_V_T] = im->base;
                e[PB_V_S] = pack2((int32_t)im->w, (int32_t)im->h);
                memcpy(e + PB_V_M, m, sizeof m);
                memcpy(e + PB_V_A, &o->alpha, 4);
                at += PB_OV;
                count++;
            }
        }
        d[PB_OVERLAYS] = ov_at;
        d[PB_OVCOUNT] = count;
    }
    d[PB_W] = (uint32_t)w;
    d[PB_H] = (uint32_t)h;
    d[PB_OX] = (uint32_t)ox;
    d[PB_OY] = (uint32_t)oy;
    d[PB_ROWS] = (uint32_t)rows_at;
    d[PB_PARTS] = (uint32_t)parts_at;
    d[PB_STATES] = (uint32_t)states_at;
    d[PB_SPRITES] = (uint32_t)sprites_at;
    d[PB_ENTRIES] = (uint32_t)entries_at;
    d[PB_CLIP] = 0;
    for (i = 0; i < 5; i++)
        d[PB_CLIP] |= (uint32_t)(limits.layer_clip[i] != 0) << i;
    d[PB_CLIP] |= (uint32_t)(limits.sprite_wrap != 0) << 8;
    if (screen.interlace) {
        static uint32_t field;
        field ^= 1;
        d[PB_FIELD] = field + 1;
    } else {
        d[PB_FIELD] = 0;
    }
    d[PB_LINES] = (uint32_t)lines;
    d[PB_PW] = (uint32_t)(hires_any ? 2 * w : w);
    /* the draw lists the layers name */
    p->list_words = 0;
    for (i = 0; i < 4; i++) {
        uint32_t id = layers[i].draw_list.id;
        eng_list *e = &p->lists[i];
        memset(e, 0, sizeof *e);
        if (id && id < nlists && lists[id].w) {
            const List *l = &lists[id];
            size_t k, need = l->nv * ENG_DRAW_WORDS + l->nc * ENG_CMD_WORDS;
            reserve(&p->list_data, &p->list_cap, p->list_words + need);
            e->w = l->w;
            e->h = l->h;
            e->verts = p->list_words;
            e->nverts = l->nv;
            for (k = 0; k < l->nv; k++) {
                uint32_t *o = p->list_data + p->list_words + k * ENG_DRAW_WORDS;
                memcpy(o, &l->v[k].x, 16);
                o[4] = l->v[k].colour;
            }
            p->list_words += l->nv * ENG_DRAW_WORDS;
            e->cmds = p->list_words;
            e->ncmds = l->nc;
            memcpy(p->list_data + p->list_words, l->cmd, l->nc * ENG_CMD_WORDS * 4);
            p->list_words += l->nc * ENG_CMD_WORDS;
        }
    }
    last_screen_w = w;
    last_screen_h = h;
    p->desc_words = at;
    p->store_words = store_used;
    p->width = hires_any ? 2 * w : w;
    p->height = screen.interlace ? 2 * h : h;
    p->keep = screen.interlace != 0;
}

void eng_picture_size(int *w, int *h)
{
    *w = last_screen_w;
    *h = last_screen_h;
}

void eng_picture_init(void) { memset(palette, 0, sizeof palette); }

/* draw sets the whole picture: nothing carries over but the objects and palette */
void eng_picture_begin(void)
{
    memset(&screen, 0, sizeof screen);
    screen_set = 0;
    memset(&limits, 0, sizeof limits);
    mode = bg3 = mode_hires = 0;
    memset(layers, 0, sizeof layers);
    memset(&m7, 0, sizeof m7);
    memset(&windows, 0, sizeof windows);
    memset(&blend, 0, sizeof blend);
    nsprites = 0;
    nchanges = 0;
}

/* on a touch screen, the on-screen controls: a soft disc for each */
static void touch_overlays(void)
{
    int z, n = eng_input_zones(), w = 0, h = 0;
    enum { D = 64 };
    if (!eng_input_touch_host() || n > TOUCH_ZONES)
        return;
    host_window_pixels(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    if (!touch_image) {
        static uint32_t disc[D * D];
        nshvc_image im = nshvc_image_create(D, D, NSHVC_IMAGE_RGBA);
        int x, y;
        for (y = 0; y < D; y++)
            for (x = 0; x < D; x++) {
                int dx = 2 * x + 1 - D, dy = 2 * y + 1 - D, r2 = dx * dx + dy * dy;
                int edge = D * D, inner = (D - 8) * (D - 8);
                disc[y * D + x] = r2 > edge ? 0u : r2 > inner ? 0xB0FFFFFFu : 0x40FFFFFFu;
            }
        nshvc_image_write(im, disc);
        touch_image = im.id;
    }
    for (z = 0; z < n; z++) {
        float cx, cy, r;
        Overlay *o = &overlays[NSHVC_OVERLAYS + z];
        eng_input_zone(z, &cx, &cy, &r);
        cx *= (float)w;
        cy *= (float)h;
        r *= (float)h;
        o->image = touch_image;
        o->corners[0] = cx - r;
        o->corners[1] = cy - r;
        o->corners[2] = cx + r;
        o->corners[3] = cy - r;
        o->corners[4] = cx - r;
        o->corners[5] = cy + r;
        o->alpha = 1;
    }
}

static void inspect(FILE *f)
{
    int i;
    uint32_t k;
    fprintf(f, "picture\n  screen %d x %d origin %d,%d brightness %u blank %u\n  mode %u bg3 %u hires %u\n", screen.width, screen.height,
            screen.origin_x, screen.origin_y, screen_set ? screen.brightness : 15, screen.blank, mode, bg3, mode_hires);
    for (i = 0; i < 4; i++) {
        const nshvc_layer *l = &layers[i];
        fprintf(f, "  layer %d main %u sub %u tiles %u map %u tile_size %u scroll %d,%d mosaic %u wrap %u image %u list %u\n", i, l->enabled_main,
                l->enabled_sub, l->tiles.id, l->map.id, l->tile_size, l->scroll_x, l->scroll_y, l->mosaic, l->wrap, l->image.id, l->draw_list.id);
    }
    fprintf(f, "  mode7 %d %d %d %d centre %d,%d scroll %d,%d outside %u\n", m7.a, m7.b, m7.c, m7.d, m7.centre_x, m7.centre_y, m7.scroll_x,
            m7.scroll_y, m7.outside);
    fprintf(f, "  windows %d-%d %d-%d\n  blend subtract %u half %u fixed %04x layers %02x\n", windows.w[0].left, windows.w[0].right, windows.w[1].left,
            windows.w[1].right, blend.subtract, blend.half, blend.fixed_bgr15, blend.layers);
    fprintf(f, "  palette");
    for (k = 0; k < 256; k++)
        fprintf(f, "%s%06x", k % 16 ? " " : "\n    ", palette[k]);
    fprintf(f, "\n  sprites %u first %u\n", nsprites, sprite_first);
    for (k = 0; k < nsprites; k++)
        fprintf(f, "    %u at %d,%d size %ux%u tile %u palette %u priority %u flips %u%u\n", k, sprites[k].x, sprites[k].y, sprites[k].width,
                sprites[k].height, sprites[k].tile, sprites[k].palette, sprites[k].priority, sprites[k].hflip, sprites[k].vflip);
    fprintf(f, "  line changes %zu\n", nchanges);
    for (k = 0; k < nchanges; k++)
        fprintf(f, "    line %u column %d field %d value %d\n", changes[k].line, changes[k].column, changes[k].field, changes[k].value);
    for (k = 1; k < nlists; k++)
        fprintf(f, "  draw list %u: %u x %u, %zu vertices, %zu commands\n", k, lists[k].w, lists[k].h, lists[k].nv, lists[k].nc);
    eng_inspect_voices(f);
    fflush(f);
}

eng_packet *eng_picture_end(void)
{
    eng_packet *done = &packets[cur];
    FILE *f;
    touch_overlays();
    if ((f = eng_inspect_take()))
        inspect(f);
    build(done);
    cur ^= 1;
    packets[cur].ops_words = 0;
    packets[cur].nshaders = 0;
    return done;
}
