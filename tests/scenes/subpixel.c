/* subpixel.c: places with a fraction of a pixel (6.10), checked against window pixels worked out by hand. Headless, the
 * screen 256 x 224 at square pixels in a window four times its size, so a fraction of k * 64 (256ths) is k window pixels
 * and every edge falls between window pixels (no pixel mixes two colours):
 *
 *   RR_SIZE=1024x896 subpixel --headless --updates 9
 *
 * The backdrop is grey 0x212121; tile 0 is every texel 1 (BG colour B, sprite colour A), tile 1 every texel 0.
 *
 *  0-3  fraction f = 0, 64, 128, 192 (k = 0, 1, 2, 3): an 8 x 8 sprite at (100, 50) with x_sub = y_sub = f covers window
 *       columns 400 + k to 431 + k and rows 200 + k to 231 + k (a sprite's row is its y); BG1 (mode 1) with one solid
 *       tile at map cell (4, 4), scroll 0 and scroll_x_sub = scroll_y_sub = f, covers columns 128 - k to 159 - k and rows
 *       124 - k to 155 - k (a scroll moves the layer's pixels left and up; a layer's screen row y shows its row y + 1, as
 *       the console's: tile rows 32-39 on screen rows 31-38)
 *  4-7  the same fractions on Mode 7's scroll, the matrix the identity and the centre 0: the plane's cell (4, 4) covers
 *       columns 128 - k to 159 - k and rows 124 - k to 155 - k (the plane's first row is the console's line 1, too)
 *  8    the per-line fraction: BG1 with solid tiles at cells (4, 4) and (4, 13); NSHVC_LINE_SCROLL_X_SUB + 0 = 128 from
 *       line 100: the upper tile (screen rows 31-38) as at 0 (columns 128-159, rows 124-155), the lower (screen rows
 *       103-110) 2 window pixels left (columns 126-157, rows 412-443)
 *
 * Prints FAIL and exits 1 on any difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SCENARIOS 9
#define SCALE 4
#define W (256 * SCALE)
#define H (224 * SCALE)

static int failed, pics, checked;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static nshvc_tiles tiles, m7_tiles;
static nshvc_map map, m7_map;
static const uint16_t GREY = 0x1084, A = 0x001F, B = 0x03E0, C = 0x7C00;   /* grey 4,4,4; red; green; blue */

static uint32_t rgb_of(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static void init(void)
{
    static uint8_t t[128], t7[2 * 64];
    static nshvc_map_entry e[32 * 32];
    static nshvc_map_entry c7[128 * 128];
    uint16_t c;

    memset(t, 1, 64);                       /* tile 0: every texel 1; tile 1: 0 */
    tiles = nshvc_tiles_create(4, 2, 1);
    nshvc_tiles_write(tiles, 0, t, 2, NSHVC_TILES_PACKED);
    memset(t7, 1, 64);
    m7_tiles = nshvc_tiles_create(8, 2, 1);
    nshvc_tiles_write(m7_tiles, 0, t7, 2, NSHVC_TILES_MODE7);
    for (int i = 0; i < 32 * 32; i++)
        e[i].tile = 1;
    e[4 * 32 + 4].tile = 0;
    e[13 * 32 + 4].tile = 0;
    map = nshvc_map_create(32, 32);
    nshvc_map_write(map, 0, 0, 32, 32, e);
    for (int i = 0; i < 128 * 128; i++)
        c7[i].tile = 1;
    c7[4 * 128 + 4].tile = 0;
    m7_map = nshvc_map_create(128, 128);
    nshvc_map_write(m7_map, 0, 0, 128, 128, c7);
    c = GREY;
    nshvc_palette_write(0, 1, &c);
    c = B;
    nshvc_palette_write(1, 1, &c);          /* BG palette 0, colour 1 */
    c = A;
    nshvc_palette_write(129, 1, &c);        /* sprite palette 0, colour 1 */
}

static void layer(int sub, int line_change)
{
    nshvc_layer l;

    memset(&l, 0, sizeof l);
    l.enabled_main = 1;
    l.tiles = tiles;
    l.map = map;
    l.tile_size = 8;
    l.mosaic = 1;
    l.wrap = 1;
    l.scroll_x_sub = (int16_t)sub;
    l.scroll_y_sub = (int16_t)sub;
    nshvc_layer_set(0, &l);
    if (line_change)
        nshvc_line(100, (nshvc_line_field)(NSHVC_LINE_SCROLL_X_SUB + 0), 128);
}

static void draw(float between)
{
    nshvc_screen scr;
    nshvc_sprite s;
    nshvc_layer off;
    static const int f[4] = { 0, 64, 128, 192 };
    int sc = pics;

    (void)between;
    memset(&scr, 0, sizeof scr);
    scr.width = 256;
    scr.height = 224;
    scr.aspect = 256.0f / 224.0f;           /* square pixels: four window pixels a screen pixel each way */
    scr.brightness = 15;
    nshvc_screen_set(&scr);
    memset(&off, 0, sizeof off);
    for (int i = 0; i < 4; i++)
        nshvc_layer_set((uint8_t)i, &off);
    nshvc_sprites_set(NULL, 0, 0);
    if (sc < 4) {
        nshvc_mode_set(1, 0, 0);
        layer(f[sc], 0);
        memset(&s, 0, sizeof s);
        s.x = 100;
        s.y = 50;
        s.x_sub = s.y_sub = (int16_t)f[sc];
        s.width = s.height = 1;
        s.tiles = tiles;
        s.tile = 0;
        s.priority = 3;
        nshvc_sprites_set(&s, 1, 0);
    } else if (sc < 8) {
        nshvc_mode7 m;

        nshvc_mode_set(7, 0, 0);
        memset(&m, 0, sizeof m);
        m.a = m.d = 256;
        m.outside = 1;
        m.tiles = m7_tiles;
        m.map = m7_map;
        m.scroll_x_sub = m.scroll_y_sub = (int16_t)f[sc - 4];
        nshvc_mode7_set(&m);
        memset(&off, 0, sizeof off);
        off.enabled_main = 1;               /* BG1 on: Mode 7's plane is BG1 */
        off.tiles = tiles;
        off.map = map;
        off.tile_size = 8;
        off.mosaic = 1;
        nshvc_layer_set(0, &off);
    } else if (sc == 8) {
        nshvc_mode_set(1, 0, 0);
        layer(0, 1);
    }
    (void)C;
    pics++;
}

#define AT(x, y) (p[(y) * W + (x)] & 0xFFFFFFu)

/* a box of colour `want` from columns x0 to x1 and rows y0 to y1 (window pixels, inclusive), grey just outside */
static void box(int sc, const char *what, const uint32_t *p, int x0, int x1, int y0, int y1, uint16_t want)
{
    uint32_t in = rgb_of(want), grey = 0x212121u;
    int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;

    CHECK(AT(x0, my) == in && AT(x1, my) == in && AT(mx, y0) == in && AT(mx, y1) == in,
          "scenario %d %s: edges inside %06x %06x %06x %06x, want %06x", sc, what, AT(x0, my), AT(x1, my), AT(mx, y0), AT(mx, y1), in);
    CHECK(AT(x0 - 1, my) == grey && AT(x1 + 1, my) == grey && AT(mx, y0 - 1) == grey && AT(mx, y1 + 1) == grey,
          "scenario %d %s: just outside %06x %06x %06x %06x, want %06x", sc, what, AT(x0 - 1, my), AT(x1 + 1, my), AT(mx, y0 - 1),
          AT(mx, y1 + 1), grey);
}

static void check(int sc, const uint32_t *p, int w, int h)
{
    if (!p || w != W || h != H) {
        CHECK(0, "scenario %d: no picture of %d x %d (%d x %d): run with RR_SIZE=%dx%d", sc, W, H, w, h, W, H);
        return;
    }
    if (sc < 4) {
        int k = sc;

        box(sc, "sprite", p, 400 + k, 431 + k, 200 + k, 231 + k, A);
        box(sc, "layer", p, 128 - k, 159 - k, 124 - k, 155 - k, B);
    } else if (sc < 8) {
        int k = sc - 4;

        box(sc, "mode 7", p, 128 - k, 159 - k, 124 - k, 155 - k, B);
    } else if (sc == 8) {
        box(sc, "upper tile", p, 128, 159, 124, 155, B);
        box(sc, "lower tile", p, 126, 157, 412, 443, B);
    }
}

static void update(const nshvc_input *in)
{
    (void)in;
    if (pics > checked && checked < SCENARIOS) {
        int w = 0, h = 0;
        const uint32_t *p = nshvc_picture_read(&w, &h);

        check(checked++, p, w, h);
    }
}

static void shutdown(void)
{
    update(NULL);
    CHECK(checked == SCENARIOS, "%d of %d scenarios checked: run with --updates %d", checked, SCENARIOS, SCENARIOS);
    printf(failed ? "subpixel: FAILED\n" : "subpixel: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;

    g.id = "subpixel";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
