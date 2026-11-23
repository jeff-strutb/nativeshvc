/* brightness_sub.c: a brightness between the 16 levels (6.13), checked against values worked out from the console's
 * dimming. Headless, the screen 256 x 224 at its own size:
 *
 *   RR_SIZE=256x224 brightness_sub --headless --updates 12
 *
 * BG1 (mode 1) is one solid tile everywhere, colour C = (red 31, green 17, blue 5), a 15-bit colour, so each channel c
 * is dimmed in whole steps: at level b, d(b) = ((c * b + 7) * 4370 >> 16) * 8 in eighths (d(15) = c * 8, undimmed);
 * between b and b + 1 at s 256ths, e = d(b) + (((d(b + 1) - d(b)) * s + 128) >> 8); the window pixel's 8-bit channel is
 * e + (e >> 5). Each scenario sets brightness and brightness_sub on the screen, or (the last two) on a line from 100,
 * and checks a pixel above it and one below. Prints FAIL and exits 1 on any difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define W 256
#define H 224

typedef struct { int b, s, line_b, line_s; } Scenario;
static const Scenario sc[] = {
    { 0, 0, -1, 0 }, { 0, 128, -1, 0 }, { 7, 0, -1, 0 }, { 7, 64, -1, 0 }, { 7, 255, -1, 0 }, { 14, 128, -1, 0 },
    { 14, 255, -1, 0 }, { 15, 200, -1, 0 }, { 3, 1, -1, 0 }, { 11, 192, -1, 0 },
    { 15, 0, 9, 96 }, { 4, 40, 4, 0 },
};
#define SCENARIOS ((int)(sizeof sc / sizeof sc[0]))
static const int C[3] = { 31, 17, 5 };

static int failed, pics, checked;
static nshvc_tiles tiles;
static nshvc_map map;

static void init(void)
{
    static uint8_t t[64];
    static nshvc_map_entry e[32 * 32];
    uint16_t c = (uint16_t)(C[0] | C[1] << 5 | C[2] << 10);

    memset(t, 1, sizeof t);
    tiles = nshvc_tiles_create(4, 1, 1);
    nshvc_tiles_write(tiles, 0, t, 1, NSHVC_TILES_PACKED);
    map = nshvc_map_create(32, 32);
    nshvc_map_write(map, 0, 0, 32, 32, e);
    nshvc_palette_write(1, 1, &c);
}

static void draw(float between)
{
    nshvc_screen scr;
    nshvc_layer l;
    const Scenario *s = &sc[pics < SCENARIOS ? pics : SCENARIOS - 1];

    (void)between;
    memset(&scr, 0, sizeof scr);
    scr.width = W;
    scr.height = H;
    scr.brightness = (uint8_t)s->b;
    scr.brightness_sub = (uint8_t)s->s;
    nshvc_screen_set(&scr);
    nshvc_mode_set(1, 0, 0);
    memset(&l, 0, sizeof l);
    l.enabled_main = 1;
    l.tiles = tiles;
    l.map = map;
    l.tile_size = 8;
    l.mosaic = 1;
    l.wrap = 1;
    nshvc_layer_set(0, &l);
    if (s->line_b >= 0) {
        nshvc_line(100, NSHVC_LINE_BRIGHTNESS, s->line_b);
        nshvc_line(100, NSHVC_LINE_BRIGHTNESS_SUB, s->line_s);
    }
    pics++;
}

static uint32_t expect(int b, int s)
{
    uint32_t out = 0;

    for (int k = 0; k < 3; k++) {
        int c = C[k];
        int lo = b == 15 ? c * 8 : ((c * b + 7) * 4370 >> 16) * 8;
        int hi = b >= 14 ? c * 8 : ((c * (b + 1) + 7) * 4370 >> 16) * 8;
        int e = b == 15 ? lo : lo + (((hi - lo) * s + 128) >> 8);

        out |= (uint32_t)(e + (e >> 5)) << (16 - 8 * k);     /* red the high byte */
    }
    return out;
}

static void check(int n, const uint32_t *p, int w, int h)
{
    const Scenario *s = &sc[n];
    uint32_t above, below, want_above = expect(s->b, s->s), want_below = s->line_b >= 0 ? expect(s->line_b, s->line_s) : want_above;

    if (!p || w != W || h != H) {
        failed = 1;
        printf("FAIL scenario %d: no picture of %d x %d (%d x %d): run with RR_SIZE=%dx%d\n", n, W, H, w, h, W, H);
        return;
    }
    above = p[50 * W + 128] & 0xFFFFFFu;
    below = p[150 * W + 128] & 0xFFFFFFu;
    if (above != want_above || below != want_below) {
        failed = 1;
        printf("FAIL scenario %d (brightness %d + %d/256, line %d + %d/256): %06x %06x, want %06x %06x\n", n, s->b, s->s,
               s->line_b, s->line_s, above, below, want_above, want_below);
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
    if (checked != SCENARIOS) {
        failed = 1;
        printf("FAIL %d of %d scenarios checked: run with --updates %d\n", checked, SCENARIOS, SCENARIOS);
    }
    printf(failed ? "brightness_sub: FAILED\n" : "brightness_sub: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;

    g.id = "brightness-sub";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
