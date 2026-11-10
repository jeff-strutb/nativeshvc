/* picture.c: the game's picture calls. Tiles, maps, images and the palette
 * are textures on the GPU: a write here records only the texels that
 * changed, as a write in the frame being filled. What draw sets, and the
 * changes it makes by line, become the picture's states by row (each row a
 * run of parts, each part a state), which compose.c turns into the
 * renderer's draws. No pixel is made here. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "inspect.h"
#include "picture.h"
#include "rr.h"

#define MAX_SPRITES 65536
#define MAX_SIDE 8192           /* a map's or image's width or height */

eng_tiles eng_tiles_of[ENG_OBJECTS];
eng_map eng_maps_of[ENG_OBJECTS];
eng_image eng_images_of[ENG_OBJECTS];
uint32_t eng_ntiles = 1, eng_nmaps = 1, eng_nimages = 1;   /* 0: none */
eng_list eng_lists_of[ENG_LISTS];
uint32_t eng_nlists = 1;

/* two frames: one being filled, one the render thread may be running */
static rr_frame frames[2];
static int cur;
static uint32_t ntextures = 1;

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
static uint32_t nsprites, sprite_first;
static size_t cap_sprites;
static uint32_t palette[256];       /* kept between pictures: 15-bit BGR, or ENG_RGB | 0xRRGGBB */
static int show_main = 1, show_sub;
static eng_overlay overlays[NSHVC_OVERLAYS];   /* kept between pictures */
static int last_screen_w = 256, last_screen_h = 224;

static void *grow(void *p, size_t *cap, size_t want, size_t size)
{
    size_t c;
    void *n;
    if (want <= *cap)
        return p;
    c = *cap ? *cap : 1024;
    while (c < want)
        c *= 2;
    if (!(n = realloc(p, c * size))) {
        eng_report("nativeshvc", "no memory for %zu items", c);
        exit(70);
    }
    *cap = c;
    return n;
}

/* ---- textures ---- */
uint32_t eng_texture_id(void) { return ntextures++; }

void eng_texture_create(rr_frame *f, uint32_t id, uint32_t w, uint32_t h, int format, int flags)
{
    rr_tex_op *o;
    f->texops = (rr_tex_op *)grow(f->texops, &f->cap_texops, f->ntexops + 1, sizeof *f->texops);
    o = &f->texops[f->ntexops++];
    memset(o, 0, sizeof *o);
    o->id = id;
    o->op = RR_TEX_CREATE;
    o->format = (uint8_t)format;
    o->flags = (uint8_t)flags;
    o->w = w;
    o->h = h;
}

void eng_texture_array(rr_frame *f, uint32_t id, uint32_t w, uint32_t h, uint32_t layers, int flags)
{
    eng_texture_create(f, id, w, h, RR_BGRA8, flags | RR_TEX_ARRAY);
    f->texops[f->ntexops - 1].layers = layers;
}

uint8_t *eng_texture_write(rr_frame *f, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int bytes)
{
    rr_tex_op *o;
    size_t n = (size_t)w * h * (size_t)bytes, at = (f->upload_bytes + 15) & ~(size_t)15;
    f->texops = (rr_tex_op *)grow(f->texops, &f->cap_texops, f->ntexops + 1, sizeof *f->texops);
    f->upload = (uint8_t *)grow(f->upload, &f->upload_cap, at + n, 1);
    o = &f->texops[f->ntexops++];
    memset(o, 0, sizeof *o);
    o->id = id;
    o->op = RR_TEX_WRITE;
    o->x = x;
    o->y = y;
    o->w = w;
    o->h = h;
    o->at = at;
    f->upload_bytes = at + n;
    return f->upload + at;
}

/* ---- objects ---- */
nshvc_tiles nshvc_tiles_create(uint8_t bpp, uint32_t count, uint8_t tpp)
{
    static const char call[] = "nshvc_tiles_create";
    nshvc_tiles r = { 0 };
    eng_tiles *o;
    uint32_t s, most_rows, rows;
    if (!eng_on(call, ENG_GAME))
        return r;
    if ((bpp != 2 && bpp != 4 && bpp != 8) || !count || !tpp || tpp > 16 || eng_ntiles == ENG_OBJECTS) {
        eng_report(call, "bpp %u, count %u, texels_per_pixel %u", bpp, count, tpp);
        return r;
    }
    /* the atlas: 16 tiles a row, more when it would be taller than MAX_SIDE;
       no more tiles than a MAX_SIDE square holds (nothing past that wraps) */
    s = 8u * tpp;
    most_rows = MAX_SIDE / s;
    if ((uint64_t)count > (uint64_t)most_rows * most_rows) {
        eng_report(call, "%u tiles of %u texels a side", count, s);
        return r;
    }
    o = &eng_tiles_of[eng_ntiles];
    o->cols = count < 16 ? count : 16;
    if ((count + o->cols - 1) / o->cols > most_rows)
        o->cols = (count + most_rows - 1) / most_rows;
    rows = (count + o->cols - 1) / o->cols;
    if (o->cols * s > MAX_SIDE) {
        eng_report(call, "%u tiles of %u texels a side", count, s);
        return r;
    }
    r.id = eng_ntiles++;
    o->tex = eng_texture_id();
    o->count = count;
    o->bpp = bpp;
    o->tpp = tpp;
    o->gen = 0;
    o->tile0_gen = 0;
    memset(o->tile0, 0, sizeof o->tile0);   /* a new texture reads 0 on every backend (rr.h) */
    eng_texture_create(&frames[cur], o->tex, o->cols * s, rows * s, RR_R8U, 0);
    return r;
}

void nshvc_tiles_write(nshvc_tiles t, uint32_t first, const void *data, uint32_t count, nshvc_tile_format f)
{
    static const char call[] = "nshvc_tiles_write";
    static uint8_t *tmp;
    static size_t cap;
    eng_tiles *o;
    const uint8_t *in = (const uint8_t *)data;
    uint32_t s, tb, n, a;
    if (!eng_on(call, ENG_GAME))
        return;
    if (!t.id || t.id >= eng_ntiles || !data || (uint64_t)first + count > eng_tiles_of[t.id].count) {
        eng_report(call, "tile set %u, first %u, count %u", t.id, first, count);
        return;
    }
    if (!count)
        return;                         /* no tiles: nothing written */
    o = &eng_tiles_of[t.id];
    if ((f == NSHVC_TILES_PLANAR || f == NSHVC_TILES_MODE7) && o->tpp != 1) {
        eng_report(call, "the SNES's planar and Mode 7 formats have one texel a pixel");
        return;
    }
    s = 8u * o->tpp;
    tb = s * s;
    tmp = (uint8_t *)grow(tmp, &cap, (size_t)count * tb, 1);
    if (f != NSHVC_TILES_PLANAR) {
        memcpy(tmp, in, (size_t)count * tb);
    } else {
        for (n = 0; n < count; n++) {
            uint32_t y, x, p;
            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++) {
                    uint32_t v = 0;
                    for (p = 0; p < o->bpp; p++) {
                        /* planes in pairs: 16 bytes a pair, two bytes a row */
                        uint8_t byte = in[(p / 2) * 16 + y * 2 + (p & 1)];
                        v |= (uint32_t)((byte >> (7 - x)) & 1) << p;
                    }
                    tmp[n * 64 + y * 8 + x] = (uint8_t)v;
                }
            in += o->bpp * 8u;
        }
    }
    if (first == 0) {
        /* tile 0 as Mode 7 reads it outside the plane: the texel at each pixel's centre */
        uint8_t t0[64];
        uint32_t x, y;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                t0[y * 8 + x] = tmp[(size_t)(y * o->tpp + o->tpp / 2) * s + x * o->tpp + o->tpp / 2];
        if (memcmp(t0, o->tile0, sizeof t0)) {
            memcpy(o->tile0, t0, sizeof t0);
            o->tile0_gen++;
        }
    }
    /* a write for each run of tiles along a row of the atlas */
    for (a = first; a < first + count;) {
        uint32_t row = a / o->cols, b = (row + 1) * o->cols, k, y;
        uint8_t *out;
        if (b > first + count)
            b = first + count;
        out = eng_texture_write(&frames[cur], o->tex, (a % o->cols) * s, row * s, (b - a) * s, s, 1);
        for (y = 0; y < s; y++)
            for (k = a; k < b; k++)
                memcpy(out + (size_t)y * (b - a) * s + (size_t)(k - a) * s, tmp + (size_t)(k - first) * tb + (size_t)y * s, s);
        a = b;
    }
    o->gen++;
}

nshvc_map nshvc_map_create(uint32_t w, uint32_t h)
{
    static const char call[] = "nshvc_map_create";
    nshvc_map r = { 0 };
    eng_map *o;
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE || eng_nmaps == ENG_OBJECTS) {
        eng_report(call, "%u x %u", w, h);
        return r;
    }
    r.id = eng_nmaps++;
    o = &eng_maps_of[r.id];
    o->tex = eng_texture_id();
    o->w = w;
    o->h = h;
    o->gen = 0;
    eng_texture_create(&frames[cur], o->tex, w, h, RR_R32U, 0);
    return r;
}

static uint32_t *map_args(const char *call, nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data)
{
    eng_map *o;
    if (!eng_on(call, ENG_GAME))
        return NULL;
    if (!m.id || m.id >= eng_nmaps || !data || !w || !h || (uint64_t)x + w > eng_maps_of[m.id].w || (uint64_t)y + h > eng_maps_of[m.id].h) {
        eng_report(call, "map %u, at %u,%u, %u x %u", m.id, x, y, w, h);
        return NULL;
    }
    o = &eng_maps_of[m.id];
    o->gen++;
    return (uint32_t *)(void *)eng_texture_write(&frames[cur], o->tex, x, y, w, h, 4);
}

void nshvc_map_write(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const nshvc_map_entry *e)
{
    uint32_t *out = map_args("nshvc_map_write", m, x, y, w, h, e), k;
    if (!out)
        return;
    for (k = 0; k < w * h; k++, e++)
        out[k] = (uint32_t)e->tile | (uint32_t)(e->palette & 7) << 16 | (uint32_t)(e->priority != 0) << 19 |
                 (uint32_t)(e->hflip != 0) << 20 | (uint32_t)(e->vflip != 0) << 21;
}

void nshvc_map_write_snes(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *words)
{
    uint32_t *out = map_args("nshvc_map_write_snes", m, x, y, w, h, words), k;
    if (!out)
        return;
    for (k = 0; k < w * h; k++) {
        uint32_t v = words[k];
        out[k] = (v & 0x3FF) | ((v >> 10) & 7) << 16 | ((v >> 13) & 1) << 19 | ((v >> 14) & 1) << 20 | ((v >> 15) & 1) << 21;
    }
}

void nshvc_palette_write(uint32_t first, uint32_t count, const uint16_t *c)
{
    uint32_t i;
    if (!eng_on("nshvc_palette_write", ENG_GAME))
        return;
    if ((uint64_t)first + count > 256 || !c) {
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
    if ((uint64_t)first + count > 256 || !c) {
        eng_report("nshvc_palette_write_rgb", "first %u, count %u", first, count);
        return;
    }
    for (i = 0; i < count; i++)
        palette[first + i] = ENG_RGB | (c[i] & 0xFFFFFFu);
}

nshvc_image nshvc_image_create(uint32_t w, uint32_t h, nshvc_image_format f)
{
    static const char call[] = "nshvc_image_create";
    nshvc_image r = { 0 };
    eng_image *o;
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE || eng_nimages == ENG_OBJECTS || (f != NSHVC_IMAGE_INDEXED && f != NSHVC_IMAGE_RGBA)) {
        eng_report(call, "%u x %u", w, h);
        return r;
    }
    r.id = eng_nimages++;
    o = &eng_images_of[r.id];
    o->tex = eng_texture_id();
    o->w = w;
    o->h = h;
    o->format = (uint32_t)f;
    eng_texture_create(&frames[cur], o->tex, w, h, f == NSHVC_IMAGE_RGBA ? RR_BGRA8 : RR_R8U, 0);
    return r;
}

void nshvc_image_write(nshvc_image i, const void *texels)
{
    static const char call[] = "nshvc_image_write";
    eng_image *o;
    int rgba;
    if (!eng_on(call, ENG_GAME))
        return;
    if (!i.id || i.id >= eng_nimages || !texels) {
        eng_report(call, "image %u", i.id);
        return;
    }
    o = &eng_images_of[i.id];
    rgba = o->format == NSHVC_IMAGE_RGBA;
    /* 0xAARRGGBB words are BGRA bytes */
    memcpy(eng_texture_write(&frames[cur], o->tex, 0, 0, o->w, o->h, rgba ? 4 : 1), texels, (size_t)o->w * o->h * (rgba ? 4 : 1));
}

/* ---- settings for this picture ---- */
#define IN_DRAW(call) eng_on(call, ENG_GAME)

void nshvc_screen_set(const nshvc_screen *s) { if (IN_DRAW("nshvc_screen_set") && s) { screen = *s; screen_set = 1; } }
void nshvc_limits_set(const nshvc_limits *l) { if (IN_DRAW("nshvc_limits_set") && l) limits = *l; }
void nshvc_mode_set(uint8_t m, uint8_t b3, uint8_t h) { if (IN_DRAW("nshvc_mode_set")) { mode = m & 7; bg3 = b3 != 0; mode_hires = h != 0; } }
void nshvc_mode7_set(const nshvc_mode7 *m)
{
    if (!IN_DRAW("nshvc_mode7_set") || !m)
        return;
    if (m->outside > 2) {
        /* 0 repeat, 1 transparent, 2 tile 0: no other. The rest is taken;
           outside stays as it was */
        uint8_t keep = m7.outside;
        eng_report("nshvc_mode7_set", "outside %u: kept at %u", m->outside, keep);
        m7 = *m;
        m7.outside = keep;
        return;
    }
    m7 = *m;
}
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

void nshvc_sprites_show(uint8_t main_screen, uint8_t sub_screen)
{
    if (IN_DRAW("nshvc_sprites_show")) {
        show_main = main_screen != 0;
        show_sub = sub_screen != 0;
    }
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
    sprites = (nshvc_sprite *)grow(sprites, &cap_sprites, count, sizeof *sprites);
    if (count)
        memcpy(sprites, list, count * sizeof *sprites);
    nsprites = count;
    sprite_first = count ? first % count : 0;
}

/* a change from a line on; 0 (and nothing changed) for a value the field
   cannot take: Mode 7's outside is 0 repeat, 1 transparent, 2 tile 0 */
static int add_change(uint32_t line, int32_t column, int field, int32_t value)
{
    if (field == NSHVC_LINE_M7_OUTSIDE && (value < 0 || value > 2))
        return 0;
    changes = (Change *)grow(changes, &cap_changes, nchanges + 1, sizeof *changes);
    changes[nchanges].line = line;
    changes[nchanges].column = column;
    changes[nchanges].order = (uint32_t)nchanges;
    changes[nchanges].field = field;
    changes[nchanges].value = value;
    nchanges++;
    return 1;
}

void nshvc_line(uint32_t line, nshvc_line_field f, int32_t v)
{
    if (IN_DRAW("nshvc_line") && !add_change(line, 0, (int)f, v))
        eng_report("nshvc_line", "line %u, field %d, value %d: not taken", line, (int)f, (int)v);
}
void nshvc_line_from(uint32_t line, int32_t c, nshvc_line_field f, int32_t v)
{
    if (IN_DRAW("nshvc_line_from") && !add_change(line, c, (int)f, v))
        eng_report("nshvc_line_from", "line %u, field %d, value %d: not taken", line, (int)f, (int)v);
}
void nshvc_lines(uint32_t first, uint32_t count, nshvc_line_field f, const int32_t *v)
{
    uint32_t i, bad = 0, at = 0;
    if (!IN_DRAW("nshvc_lines") || !v)
        return;
    for (i = 0; i < count; i++)
        if (!add_change(first + i, 0, (int)f, v[i]) && !bad++)
            at = first + i;
    if (bad)
        eng_report("nshvc_lines", "field %d: %u values not taken, the first at line %u", (int)f, bad, at);   /* one report a call */
}

void nshvc_overlay_set(uint32_t id, nshvc_image i, const float corners[6], float alpha)
{
    static const char call[] = "nshvc_overlay_set";
    if (!eng_on(call, ENG_GAME))
        return;
    if (id >= NSHVC_OVERLAYS || (i.id && (i.id >= eng_nimages || eng_images_of[i.id].format != NSHVC_IMAGE_RGBA)) || (i.id && !corners)) {
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
    if (eng_nlists == ENG_LISTS) {
        eng_report(call, "more than %d lists", ENG_LISTS - 1);
        return r;
    }
    r.id = eng_nlists++;
    return r;
}

static eng_list *list_of(const char *call, nshvc_draw_list d)
{
    if (!eng_on(call, LIST_ROLES))
        return NULL;
    if (!d.id || d.id >= eng_nlists) {
        eng_report(call, "list %u", d.id);
        return NULL;
    }
    return &eng_lists_of[d.id];
}

void nshvc_draw_list_clear(nshvc_draw_list d, uint32_t width, uint32_t height)
{
    eng_list *l = list_of("nshvc_draw_list_clear", d);
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
    eng_list *l = list_of("nshvc_draw_rule", d);
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
    rr_frame *f = &frames[cur];
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!name || nshaders == MAX_SHADERS) {
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
    f->shaders = (rr_shader_load *)grow(f->shaders, &f->cap_shaders, (size_t)f->nshaders + 1, sizeof *f->shaders);
    f->shaders[f->nshaders].id = r.id;
    f->shaders[f->nshaders].code = code;   /* kept: the backend owns it from here */
    f->shaders[f->nshaders].size = size;
    f->nshaders++;
    return r;
}

static void list_add(eng_list *l, const nshvc_vertex *v, uint32_t count, uint32_t lines, const nshvc_material *m)
{
    eng_list_cmd *c;
    size_t k;
    l->v = (nshvc_vertex *)grow(l->v, &l->cap_v, l->nv + count, sizeof *l->v);
    l->cmd = (eng_list_cmd *)grow(l->cmd, &l->cap_c, l->nc + 1, sizeof *l->cmd);
    memcpy(l->v + l->nv, v, count * sizeof *v);
    if (l->rule == NSHVC_FILL_EXACT)
        /* the exact rule: vertices on the list's whole pixels */
        for (k = 0; k < count; k++) {
            nshvc_vertex *o = &l->v[l->nv + k];
            o->x = floorf(o->x + 0.5f);
            o->y = floorf(o->y + 0.5f);
        }
    c = &l->cmd[l->nc++];
    memset(c, 0, sizeof *c);
    c->first = (uint32_t)l->nv;
    c->count = count;
    c->lines = lines;
    if (m) {
        if (m->image.id && m->image.id < eng_nimages)
            c->image = m->image.id;
        else if (m->image.id)
            eng_report("nshvc_draw", "image %u", m->image.id);
        c->shader = m->shader.id;
        memcpy(c->inputs, m->inputs, sizeof c->inputs);
    }
    l->nv += count;
}

void nshvc_draw_tris(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m)
{
    eng_list *l = list_of("nshvc_draw_tris", d);
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
    eng_list *l = list_of("nshvc_draw_lines", d);
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
    eng_list *l = list_of("nshvc_draw_image", d);
    nshvc_material mm;
    nshvc_vertex q[6];
    float w, h;
    int k;
    if (!l)
        return;
    if (!i.id || i.id >= eng_nimages || !to) {
        eng_report("nshvc_draw_image", "image %u", i.id);
        return;
    }
    if (m)
        mm = *m;
    else
        memset(&mm, 0, sizeof mm);
    w = (float)eng_images_of[i.id].w;
    h = (float)eng_images_of[i.id].h;
    mm.image = i;
    memset(q, 0, sizeof q);
    q[0].x = to->x0; q[0].y = to->y0;
    q[1].x = to->x1; q[1].y = to->y0; q[1].u = w;
    q[2].x = to->x0; q[2].y = to->y1; q[2].v = h;
    q[3] = q[2];
    q[4] = q[1];
    q[5].x = to->x1; q[5].y = to->y1; q[5].u = w; q[5].v = h;
    for (k = 0; k < 6; k++) {
        q[k].w = 1.0f;
        q[k].colour = 0xFFFFFFFFu;
    }
    list_add(l, q, 6, 0, &mm);
}

/* ---- the picture's states ---- */
static int change_cmp(const void *a, const void *b)
{
    const Change *x = (const Change *)a, *y = (const Change *)b;
    if (x->line != y->line)
        return x->line < y->line ? -1 : 1;
    if (x->column != y->column)
        return x->column < y->column ? -1 : 1;
    return x->order < y->order ? -1 : 1;
}

static void apply(eng_state *n, uint32_t *pal, int field, int32_t v, uint32_t line, int *pal_changed)
{
    int i = field & 3;
    if (field >= NSHVC_LINE_PALETTE) {
        int e = field - NSHVC_LINE_PALETTE;
        if (e < 256) {
            pal[e] = (uint32_t)v & 0x7FFFu;     /* a 15-bit colour, as nshvc_palette_write takes */
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
    case NSHVC_LINE_SPRITES_MAIN: n->sprites_main = v != 0; break;
    case NSHVC_LINE_SPRITES_SUB: n->sprites_sub = v != 0; break;
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

static uint32_t bgra_of(uint32_t e)
{
    uint32_t r, g, b;
    if (e & ENG_RGB)
        return 0xFF000000u | (e & 0xFFFFFFu);
    r = e & 31;
    g = (e >> 5) & 31;
    b = (e >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static eng_row *rows;
static size_t cap_rows;
static eng_part *parts;
static size_t cap_parts_a;
static eng_state *states;
static size_t cap_states;
static uint32_t *pals;          /* each version's 256 entries, as kept */
static size_t cap_pals;
static uint32_t *bgra;           /* the same, as the palette texture's rows */
static size_t cap_bgra;
static uint32_t palette_tex, palette_rows;

static void build(rr_frame *f)
{
    int w = screen.width > 0 ? screen.width : 256, h = screen.height > 0 ? screen.height : 224;
    int oy = screen.origin_y, r;
    size_t c = 0, nparts = 0, nstates = 0, npal = 0;
    eng_state n;
    uint32_t cur_pal[256];
    eng_picture p;

    memset(&n, 0, sizeof n);
    n.m = mode;
    n.sprites_main = show_main;
    n.sprites_sub = show_sub;
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

    rows = (eng_row *)grow(rows, &cap_rows, (size_t)h, sizeof *rows);
    parts = (eng_part *)grow(parts, &cap_parts_a, (size_t)h * 2 + nchanges * 2 + 1, sizeof *parts);
    states = (eng_state *)grow(states, &cap_states, nchanges + 1, sizeof *states);
    pals = (uint32_t *)grow(pals, &cap_pals, (nchanges + 1) * 256, 4);
    memcpy(pals, cur_pal, sizeof cur_pal);
    npal = 1;
    states[nstates++] = n;

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
                memcpy(pals + npal * 256, cur_pal, sizeof cur_pal);
                n.pal = (int)npal++;
            }
            states[nstates++] = n;
            if (ln == line && col > 0) {
                /* a split inside the row */
                if (!have_part) {
                    parts[nparts].x0 = 0;
                    parts[nparts].state = (uint32_t)(nstates - 2);
                    nparts++;
                    cnt++;
                    have_part = 1;
                }
                parts[nparts].x0 = col + screen.origin_x;
                parts[nparts].state = (uint32_t)(nstates - 1);
                nparts++;
                cnt++;
            }
        }
        if (!have_part) {
            parts[nparts].x0 = 0;
            parts[nparts].state = (uint32_t)(nstates - 1);
            nparts++;
            cnt++;
        }
        rows[r].first = first;
        rows[r].count = cnt;
    }

    /* the palette's versions: a row of the palette texture each */
    if (!palette_tex)
        palette_tex = eng_texture_id();
    if ((uint32_t)npal > palette_rows) {
        palette_rows = palette_rows ? palette_rows : 16;
        while (palette_rows < (uint32_t)npal)
            palette_rows *= 2;
        eng_texture_create(f, palette_tex, 256, palette_rows, RR_BGRA8, 0);
    }
    {
        size_t i;
        bgra = (uint32_t *)grow(bgra, &cap_bgra, npal * 256, 4);
        for (i = 0; i < npal * 256; i++)
            bgra[i] = bgra_of(pals[i]);
        memcpy(eng_texture_write(f, palette_tex, 0, 0, 256, (uint32_t)npal, 4), bgra, npal * 256 * 4);
    }

    memset(&p, 0, sizeof p);
    p.w = w;
    p.h = h;
    p.ox = screen.origin_x;
    p.oy = oy;
    p.lines = screen.overscan ? 239 : 224;
    p.interlace = screen.interlace != 0;
    p.show_l = screen.show_left > 0 ? screen.show_left : 0;
    p.show_r = screen.show_right > 0 && screen.show_right < w ? screen.show_right : w;
    p.show_t = screen.show_top > 0 ? screen.show_top : 0;
    p.show_b = screen.show_bottom > 0 && screen.show_bottom < h ? screen.show_bottom : h;
    p.limits = limits;
    p.rows = rows;
    p.parts = parts;
    p.states = states;
    p.palettes = bgra;
    p.npalettes = (int)npal;
    p.palette_tex = palette_tex;
    p.sprites = sprites;
    p.nsprites = nsprites;
    p.sprite_first = sprite_first;
    p.overlays = overlays;

    f->width = w;
    f->height = h;
    f->aspect = eng_picture_aspect();
    f->data_words = 0;
    f->nuniforms = 0;
    f->npasses = 0;
    f->ncmds = 0;
    eng_compose(f, &p);
    last_screen_w = w;
    last_screen_h = h;
}

/* the shape the screen is shown at: the game's, else the original's pixels,
   7:6 wide, which make 256 x 224 4:3 */
float eng_picture_aspect(void)
{
    const int w = screen.width > 0 ? screen.width : 256, h = screen.height > 0 ? screen.height : 224;
    if (screen.aspect != 0.0f)
        return screen.aspect;
    return (float)w * 7.0f / ((float)h * 6.0f);
}

void eng_picture_size(int *w, int *h)
{
    *w = last_screen_w;
    *h = last_screen_h;
}

void eng_picture_init(void) { memset(palette, 0, sizeof palette); }

/* draw sets the whole picture: nothing carries over but the objects, palette and overlays */
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
    show_main = 1;                      /* until set, main only: nothing carries over */
    show_sub = 0;
    nchanges = 0;
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
    for (k = 1; k < eng_nlists; k++)
        fprintf(f, "  draw list %u: %u x %u, %zu vertices, %zu commands\n", k, eng_lists_of[k].w, eng_lists_of[k].h, eng_lists_of[k].nv,
                eng_lists_of[k].nc);
    eng_inspect_voices(f);
    fflush(f);
}

rr_frame *eng_picture_end(void)
{
    rr_frame *done = &frames[cur];
    FILE *f;
    if ((f = eng_inspect_take()))
        inspect(f);
    build(done);
    cur ^= 1;
    /* the frame to fill next was drawn before the one just finished */
    frames[cur].ntexops = 0;
    frames[cur].upload_bytes = 0;
    frames[cur].nshaders = 0;
    return done;
}
