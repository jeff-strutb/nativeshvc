/* cost.c: what the engine's drawing costs, case by case, with purpose-made
 * content and no game. One case a run, one fixed picture: the objects are made
 * in init, draw sets the same picture every update, and update does nothing
 * but take the time.
 *
 *   cost --case NAME --headless --updates N
 *
 * Cases, on the standard 256 x 224 screen (the content is seeded: the same
 * every run):
 *   backdrop            nothing but the backdrop
 *   tiles-KIND-N        N = 1 to 4 tile layers (mode 1, or mode 0 for 4), each
 *                       map entry a tile of KIND: blank (every texel 0), solid
 *                       (every texel not 0) or mixed (half its texels 0, in a
 *                       checker); each layer at its own scroll, so tiles do not
 *                       line up
 *   sprites             128 sprites of 16 x 16 over the screen, no tile layers,
 *                       their tiles mixed
 *   sprites-solid       the same sprites, their tiles solid
 *   maths               tiles-solid-2, one layer on the sub screen and add-half
 *                       colour maths on the other
 *   mesh-K-M            K instances of a mesh of M flat-coloured triangles
 *                       (nshvc_draw_mesh), spread over the screen at depths of 20
 *                       to 120, in one draw list; the instances are copied to the
 *                       call each picture, the mesh written once. mesh-1000-1000,
 *                       mesh-1000-100 and so on, K up to 100000, M up to 10000
 * Prints one line at the end: the case, the updates run, the wall time from
 * the first update to the last, and the pictures a second over those (the
 * updates after the first, one picture each).
 *
 * What it measures: the time the game thread and the engine take to make each
 * picture's frame, and whatever the run waits for. On this renderer a headless
 * run draws nothing unless the picture is read: as written the figure is the
 * engine's cost without the GPU's drawing. With RR_DRAW=1 set (RR_SIZE=WxH
 * for the size) every picture is drawn, into a texture nothing reads, and the
 * figure includes the GPU's drawing. A run with a
 * window measures the display's pacing, not the cost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nshvc/nshvc.h"

enum { C_BACKDROP, C_TILES, C_SPRITES, C_MATHS, C_MESH };
enum { K_BLANK, K_SOLID, K_MIXED };

static const char *case_name = "";
static int which, kind, layers;
static nshvc_tiles tiles[4], sprite_tiles;
static nshvc_map maps[4];
static struct timespec first, last;
static int mesh_k, mesh_m;
static nshvc_mesh mesh;
static nshvc_draw_list mesh_list;
static nshvc_instance *mesh_inst;

static uint32_t seed = 12345;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* a tile set's planar bytes: every tile of `k`, `bpp` planes, `count` tiles */
static void tile_bytes(uint8_t *out, int k, int bpp, int count)
{
    int t, i;
    for (t = 0; t < count; t++)
        for (i = 0; i < bpp * 8; i++) {
            int row = (i % 16) / 2;     /* a byte of each plane, two a row: 4-bit tiles' second pair repeats the rows */
            out[t * bpp * 8 + i] = k == K_BLANK ? 0x00 : k == K_SOLID ? 0xFF : (row & 1 ? 0xAA : 0x55);
        }
}

static int bpp_of(int mode, int layer) { return mode == 0 ? 2 : layer < 2 ? 4 : 2; }

static void init(void)
{
    uint16_t pal[256];
    int i;
    for (i = 0; i < 256; i++)
        pal[i] = (uint16_t)(rnd() & 0x7FFF);
    nshvc_palette_write(0, 256, pal);
    if (which == C_TILES || which == C_MATHS) {
        int mode = layers == 4 ? 0 : 1, n, x, y;
        for (n = 0; n < layers; n++) {
            int bpp = bpp_of(mode, n);
            uint8_t *d = (uint8_t *)malloc((size_t)bpp * 8 * 16);
            uint16_t words[32 * 32];
            tiles[n] = nshvc_tiles_create((uint8_t)bpp, 16, 1);
            tile_bytes(d, kind, bpp, 16);
            nshvc_tiles_write(tiles[n], 0, d, 16, NSHVC_TILES_PLANAR);
            free(d);
            maps[n] = nshvc_map_create(32, 32);
            for (y = 0; y < 32; y++)
                for (x = 0; x < 32; x++)
                    words[y * 32 + x] = (uint16_t)(((x * 7 + y * 3 + n) % 16) | ((x + y + n) % 8) << 10);
            nshvc_map_write_snes(maps[n], 0, 0, 32, 32, words);
        }
    } else if (which == C_MESH) {
        nshvc_mesh_vertex *v = (nshvc_mesh_vertex *)calloc((size_t)mesh_m * 3, sizeof *v);
        int t, c;
        mesh = nshvc_mesh_create((uint32_t)mesh_m * 3);
        mesh_list = nshvc_draw_list_create();
        for (t = 0; t < mesh_m; t++) {
            /* a small triangle somewhere within a unit sphere, a colour a face */
            float cx = (float)(rnd() % 2000) / 1000.0f - 1.0f, cy = (float)(rnd() % 2000) / 1000.0f - 1.0f, cz = (float)(rnd() % 2000) / 1000.0f - 1.0f;
            uint32_t col = 0xFF000000u | (rnd() & 0xFFFFFFu);
            for (c = 0; c < 3; c++) {
                nshvc_mesh_vertex *p = &v[t * 3 + c];
                p->x = cx + (float)(rnd() % 400) / 1000.0f - 0.2f;
                p->y = cy + (float)(rnd() % 400) / 1000.0f - 0.2f;
                p->z = cz + (float)(rnd() % 400) / 1000.0f - 0.2f;
                p->colour = col;
            }
        }
        nshvc_mesh_write(mesh, v, (uint32_t)mesh_m * 3);
        free(v);
        mesh_inst = (nshvc_instance *)calloc((size_t)mesh_k, sizeof *mesh_inst);
        for (t = 0; t < mesh_k; t++) {
            float z = 20.0f + (float)(rnd() % 100000) / 1000.0f;
            mesh_inst[t].z = z;
            mesh_inst[t].x = ((float)(rnd() % 20000) / 10000.0f - 1.0f) * z * 0.6f;
            mesh_inst[t].y = ((float)(rnd() % 20000) / 10000.0f - 1.0f) * z * 0.45f;
            mesh_inst[t].size = 1.0f;
        }
    } else if (which == C_SPRITES) {
        uint8_t *d = (uint8_t *)malloc(32 * 256);
        sprite_tiles = nshvc_tiles_create(4, 256, 1);
        tile_bytes(d, kind, 4, 256);
        nshvc_tiles_write(sprite_tiles, 0, d, 256, NSHVC_TILES_PLANAR);
        free(d);
    }
}

static void update(const nshvc_input *in)
{
    (void)in;
    if (!nshvc_updates())
        clock_gettime(CLOCK_MONOTONIC, &first);
    clock_gettime(CLOCK_MONOTONIC, &last);
}

static void draw(float between)
{
    (void)between;
    if (which == C_TILES || which == C_MATHS) {
        int n;
        nshvc_mode_set(layers == 4 ? 0 : 1, 0, 0);
        for (n = 0; n < layers; n++) {
            nshvc_layer a;
            memset(&a, 0, sizeof a);
            a.enabled_main = which == C_MATHS && n == 1 ? 0 : 1;
            a.enabled_sub = which == C_MATHS && n == 1 ? 1 : 0;
            a.tiles = tiles[n];
            a.map = maps[n];
            a.tile_size = 8;
            a.scroll_x = n * 37 + 5;    /* not a multiple of a tile, and not the same for two layers */
            a.scroll_y = n * 53 + 11;
            a.mosaic = 1;
            a.wrap = 1;
            nshvc_layer_set((uint8_t)n, &a);
        }
        if (which == C_MATHS) {
            nshvc_blend b;
            memset(&b, 0, sizeof b);
            b.half = 1;
            b.layers = 0x3F;
            nshvc_blend_set(&b);
        }
    } else {
        nshvc_mode_set(1, 0, 0);
    }
    if (which == C_MESH) {
        nshvc_view view;
        nshvc_layer a;
        memset(&view, 0, sizeof view);
        view.rot[0] = view.rot[4] = view.rot[8] = 1.0f;
        view.f = 200.0f; view.cx = 128.0f; view.cy = 112.0f;
        view.near = 1.0f;
        view.depth_a = 1.0f; view.depth_b = 1.0f; view.depth_max = 0.99f;
        nshvc_draw_list_clear(mesh_list, 256, 224);
        nshvc_draw_mesh(mesh_list, mesh, &view, mesh_inst, (uint32_t)mesh_k, NULL);
        memset(&a, 0, sizeof a);
        a.enabled_main = 1;
        a.draw_list = mesh_list;
        nshvc_layer_set(0, &a);
    }
    if (which == C_SPRITES) {
        static nshvc_sprite spr[128];
        int i;
        for (i = 0; i < 128; i++) {
            nshvc_sprite *s = &spr[i];
            memset(s, 0, sizeof *s);
            s->x = (i * 37) % 240;
            s->y = (i * 53) % 208;
            s->width = s->height = 2;
            s->tiles = sprite_tiles;
            s->tile = (uint16_t)((i % 15) + 16 * ((i / 15) % 14));    /* a 16 x 16 sprite reads the tile after and the row below */
            s->palette = (uint8_t)(i % 8);
            s->priority = (uint8_t)(i % 4);
        }
        nshvc_sprites_set(spr, 128, 0);
    }
}

static void finish(void)
{
    uint64_t n = nshvc_updates();
    double secs = (double)(last.tv_sec - first.tv_sec) + (double)(last.tv_nsec - first.tv_nsec) / 1e9;
    printf("cost: %s: %llu updates, %.4f s from the first to the last, %.1f pictures a second\n", case_name, (unsigned long long)n, secs,
           secs > 0.0 && n > 1 ? (double)(n - 1) / secs : 0.0);
    fflush(stdout);
}

static int parse(const char *s)
{
    if (!strcmp(s, "backdrop")) { which = C_BACKDROP; return 1; }
    if (!strcmp(s, "sprites")) { which = C_SPRITES; kind = K_MIXED; return 1; }
    if (!strcmp(s, "sprites-solid")) { which = C_SPRITES; kind = K_SOLID; return 1; }
    if (!strcmp(s, "maths")) { which = C_MATHS; kind = K_SOLID; layers = 2; return 1; }
    if (!strncmp(s, "mesh-", 5)) {
        int k = 0, m = 0;
        if (sscanf(s + 5, "%d-%d", &k, &m) != 2 || k < 1 || k > 100000 || m < 1 || m > 10000)
            return 0;
        which = C_MESH;
        mesh_k = k;
        mesh_m = m;
        return 1;
    }
    if (!strncmp(s, "tiles-", 6)) {
        const char *k = s + 6;
        which = C_TILES;
        if (!strncmp(k, "blank-", 6)) { kind = K_BLANK; k += 6; }
        else if (!strncmp(k, "solid-", 6)) { kind = K_SOLID; k += 6; }
        else if (!strncmp(k, "mixed-", 6)) { kind = K_MIXED; k += 6; }
        else return 0;
        layers = atoi(k);
        return layers >= 1 && layers <= 4 && !k[1];
    }
    return 0;
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--case"))
            case_name = argv[i + 1];
    if (!parse(case_name)) {
        fprintf(stderr, "usage: cost --case backdrop|sprites|sprites-solid|maths|mesh-K-M|tiles-{blank,solid,mixed}-{1,2,3,4} [--headless --updates N]\n");
        return 2;
    }
    g.title = "Cost";
    g.id = "cost";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = finish;
    return nshvc_run(&g, argc, argv);
}
