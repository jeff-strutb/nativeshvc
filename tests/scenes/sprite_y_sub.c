/* sprite_y_sub.c: pieces of one actor stacked in y, drawn at fractions of a row (6.10) across the strips a per-line value cuts
 * the picture into, show no line between them. Headless, the screen 256 x 224 with one window pixel to a screen pixel:
 *
 *   RR_SIZE=256x224 sprite_y_sub --headless --updates 6
 *
 * The backdrop is green 0x03E0; the sprite colour is red 0x001F. Six 8 x 8 pieces (rows 30 to 77) stand one above the
 * next at x 100, each with y_sub = f. The brightness steps from 15 down by one every 8 rows from row 33 (NSHVC_LINE_BRIGHTNESS,
 * which the sprites' strips follow: a strip a step), so a strip's edge falls inside the pieces. Every window pixel from
 * row 32 to row 75 in column 104 must be the sprite's red at that row's brightness, none of the backdrop's green: a
 * strip that left the fraction's width bare along its first row showed green there (the sprite moved after it was kept
 * to the strip's rectangle). Scenarios 0 to 5: f = 0, 64, 99, 128, 200, 255.
 *
 * Prints FAIL and exits 1 on any difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define W 256
#define H 224
#define SCENARIOS 6
#define PIECES 6

static nshvc_tiles tiles;
static int failed, pics, checked;
static const int FRAC[SCENARIOS] = { 0, 64, 99, 128, 200, 255 };

#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void init(void)
{
    static uint8_t t[64];
    uint16_t c;

    memset(t, 1, sizeof t);
    tiles = nshvc_tiles_create(4, 1, 1);
    nshvc_tiles_write(tiles, 0, t, 1, NSHVC_TILES_PACKED);
    c = 0x03E0;
    nshvc_palette_write(0, 1, &c);
    c = 0x001F;
    nshvc_palette_write(129, 1, &c);        /* sprite palette 0, colour 1 */
}

static void draw(float between)
{
    nshvc_screen scr;
    nshvc_sprite s[PIECES];
    nshvc_layer off;
    int32_t bright[H];

    (void)between;
    memset(&scr, 0, sizeof scr);
    scr.width = W;
    scr.height = H;
    scr.aspect = (float)W / (float)H;
    scr.brightness = 15;
    nshvc_screen_set(&scr);
    memset(&off, 0, sizeof off);
    for (int i = 0; i < 4; i++)
        nshvc_layer_set((uint8_t)i, &off);
    nshvc_mode_set(1, 0, 0);
    for (int r = 0; r < H; r++)
        bright[r] = r < 33 ? 15 : 15 - (r - 33) / 8;        /* 15 above row 33, then a step down every 8 rows */
    nshvc_lines(0, H, NSHVC_LINE_BRIGHTNESS, bright);
    memset(s, 0, sizeof s);
    for (int i = 0; i < PIECES; i++) {
        s[i].x = 100;
        s[i].y = 30 + 8 * i;
        s[i].y_sub = (int16_t)FRAC[pics];
        s[i].width = s[i].height = 1;
        s[i].tiles = tiles;
        s[i].priority = 3;
    }
    nshvc_sprites_set(s, PIECES, 0);
    pics++;
}

#define AT(x, y) (p[(y) * W + (x)] & 0xFFFFFFu)

static void check(int sc, const uint32_t *p, int w, int h)
{
    if (!p || w != W || h != H) {
        CHECK(0, "scenario %d: no picture of %d x %d (%d x %d): run with RR_SIZE=%dx%d", sc, W, H, w, h, W, H);
        return;
    }
    for (int y = 32; y <= 75; y++) {
        uint32_t c = AT(104, y);

        CHECK((c & 0x00FF00u) == 0 && (c & 0xFF0000u) != 0, "scenario %d (y_sub %d) row %d: %06x, want the sprite's red, no green", sc, FRAC[sc], y, c);
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
    printf(failed ? "sprite_y_sub: FAILED\n" : "sprite_y_sub: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;

    g.id = "sprite-y-sub";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
