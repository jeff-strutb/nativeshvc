/* palettes.c: sprite palettes past 7 (6.8), checked against colours worked out by hand. Headless, the picture is
 * 256 x 224, a pixel for a pixel; one scenario a picture:
 *
 *   palettes --headless --updates 8
 *
 * A sprite of palette p shows entry 128 + 16 p + the texel's index (1-15); the tiles are 0 (every texel 1) and 1 (every
 * texel 15). The backdrop (entry 0) is grey 0x212121 so black, which an entry never written is, shows as itself.
 *
 *  0  colours. Sixteen sprites in a row at y 8, each tile 0, palettes 0, 3, 4, 7 (entries below 256, as before), 8, 9 (the
 *     first two of entries 256 on), 100, 1000, 70000 (written in single entries far past each other, so the palette grows
 *     over thousands of pages), 3000 (written with nshvc_palette_write_rgb: 0x123456 exactly), then four never written: 10
 *     (in page 0, whose entry 1 is unwritten), 9999 (in a page the palette holds, no entry of which was written), 400000 (a
 *     page past those it holds) and 0xFFFFFFFF (entry 128 + 16 p past 32 bits): black, each
 *  1  colour maths. A blend of the sprites with the fixed colour (add): palettes 3, 4, 8, 100 and 70000 of one colour at
 *     index 15: 3 shows it as it is (palettes 0-3 take no maths), 4, 8, 100 and 70000 show the same, not that colour
 *  2  line changes. A column of five sprites a palette (8 rows high each, y 4 + 8 k): palette 8, whose entry 257 is changed from line 20
 *     and again from line 36; palette 150000, a page no write has reached, whose entry is changed from line 20 (black above); palette 0, whose entry
 *     129 is changed from line 28 alone (a change of the first 256 beside the pages'). The five rows read: palette 8 A A B B E,
 *     150000 black black C C C, palette 0 as written, as written, as written, D, D
 *  3  palette 8 as written again: a line change leaves the palette as it was
 *  4  palette 100's entry rewritten in draw: its new colour; palette 8 as written
 *  5  nothing written: palette 100 still shows the new colour, 8 and 70000 as written (a page is sent once)
 *  6  palette 100's entry rewritten again: the third colour
 *
 *  7  the palette's last entry (3,145,983: palette 196615's index 15, written in this picture): it shows; palette 196616, whose
 *     entries lie past it, black. Neither is an error
 *
 * Prints FAIL and exits 1 on any difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SCENARIOS 8
#define W 256
#define H 224

static int failed, pics, checked;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static nshvc_tiles tiles;

static uint16_t bgr(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }
static uint32_t rgb_of(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static void put(uint32_t entry, uint16_t c) { nshvc_palette_write(entry, 1, &c); }
static uint32_t entry_of(uint32_t p, uint32_t index) { return 128u + 16u * p + index; }

/* what each scenario's palettes hold: distinct colours, none black */
static uint16_t colour_of(uint32_t p) { return bgr((int)(p * 7 + 3) % 29 + 2, (int)(p * 13 + 5) % 29 + 2, (int)(p * 5 + 11) % 29 + 2); }
static const uint32_t S0[16] = { 0, 3, 4, 7, 8, 9, 100, 1000, 70000, 3000, 10, 9999, 400000, 0xFFFFFFFFu, 1, 2 };
static const uint16_t SAME = 0x294A;        /* index 15 of palettes 3, 4, 8, 100, 70000: r = g = b = 10 */
static const uint16_t FIXED = 0x000A;       /* the fixed colour: red 10 */
static const uint16_t LA = 0x001F, LB = 0x03E0, LE = 0x7C00, LC = 0x03FF, LD = 0x7FE0;
static const uint16_t P100_NEW = 0x7C1F, P100_NEWER = 0x0210;

static void init(void)
{
    static uint8_t t[128];
    uint16_t grey = bgr(4, 4, 4);
    uint32_t i, rgb = 0x123456;
    memset(t, 1, 64);
    memset(t + 64, 15, 64);
    tiles = nshvc_tiles_create(4, 2, 1);
    nshvc_tiles_write(tiles, 0, t, 2, NSHVC_TILES_PACKED);
    nshvc_palette_write(0, 1, &grey);
    for (i = 0; i < 16; i++) {
        uint32_t p = S0[i];
        if (i < 9 && p != 1 && p != 2)
            put(entry_of(p, 1), colour_of(p));
    }
    nshvc_palette_write_rgb(entry_of(3000, 1), 1, &rgb);
    put(entry_of(3, 15), SAME);
    put(entry_of(4, 15), SAME);
    put(entry_of(8, 15), SAME);
    put(entry_of(100, 15), SAME);
    put(entry_of(70000, 15), SAME);
    put(entry_of(8, 1), colour_of(8));
    put(entry_of(100, 1), colour_of(100));
    put(entry_of(0, 1), colour_of(0));
    put(entry_of(0, 15), colour_of(0));
}

static nshvc_sprite sprite(int x, int y, uint32_t p, uint16_t tile)
{
    nshvc_sprite s;
    memset(&s, 0, sizeof s);
    s.x = x;
    s.y = y;
    s.width = s.height = 1;
    s.tiles = tiles;
    s.tile = tile;
    s.palette = p;
    s.priority = 3;
    return s;
}

static void draw(float between)
{
    nshvc_sprite s[24];
    int n = 0, i, k;
    (void)between;
    switch (pics) {
    case 0:
        for (i = 0; i < 16; i++)
            s[n++] = sprite(4 + 16 * i, 8, S0[i], 0);
        break;
    case 1: {
        static const uint32_t ps[5] = { 3, 4, 8, 100, 70000 };
        nshvc_blend b;
        memset(&b, 0, sizeof b);
        b.layers = 0x10;                /* the sprites */
        b.use_fixed = 1;
        b.fixed_bgr15 = FIXED;
        nshvc_blend_set(&b);
        for (i = 0; i < 5; i++)
            s[n++] = sprite(4 + 16 * i, 8, ps[i], 1);
        break;
    }
    case 2:
        for (k = 0; k < 5; k++) {
            s[n++] = sprite(8, 4 + 8 * k, 8, 0);
            s[n++] = sprite(32, 4 + 8 * k, 150000, 0);
            s[n++] = sprite(56, 4 + 8 * k, 0, 0);
        }
        nshvc_line(20, (nshvc_line_field)(NSHVC_LINE_PALETTE + (int)entry_of(8, 1)), LB);
        nshvc_line(20, (nshvc_line_field)(NSHVC_LINE_PALETTE + (int)entry_of(150000, 1)), LC);
        nshvc_line(28, (nshvc_line_field)(NSHVC_LINE_PALETTE + (int)entry_of(0, 1)), LD);
        nshvc_line(36, (nshvc_line_field)(NSHVC_LINE_PALETTE + (int)entry_of(8, 1)), LE);
        break;
    case 3:
    case 4:
    case 5:
    case 6:
        if (pics == 4)
            put(entry_of(100, 1), P100_NEW);
        if (pics == 6)
            put(entry_of(100, 1), P100_NEWER);
        s[n++] = sprite(8, 8, 8, 0);
        s[n++] = sprite(24, 8, 100, 0);
        s[n++] = sprite(40, 8, 70000, 1);
        break;
    case 7:
        put(3145983u, P100_NEW);
        s[n++] = sprite(8, 8, 196615, 1);
        s[n++] = sprite(24, 8, 196616, 1);
        break;
    default:
        break;
    }
    nshvc_sprites_set(s, (uint32_t)n, 0);
    pics++;
}

#define AT(x, y) (p[(y) * W + (x)] & 0xFFFFFFu)

static void check(int sc, const uint32_t *p, int w, int h)
{
    int i, k;
    if (!p || w != W || h != H) {
        CHECK(0, "scenario %d: no picture of %d x %d (%d x %d)", sc, W, H, w, h);
        return;
    }
    if (sc == 0) {
        for (i = 0; i < 16; i++) {
            uint32_t p0 = S0[i], want, got = AT(8 + 16 * i, 12);
            if (i == 9)
                want = 0x123456;
            else if (i >= 10 && i <= 13)
                want = 0;
            else if (p0 == 1 || p0 == 2)
                want = 0;
            else
                want = rgb_of(colour_of(p0));
            if (i >= 14)
                want = 0;
            CHECK(got == want, "colours: sprite %d (palette %u) shows %06x, wants %06x", i, p0, got, want);
        }
        CHECK(AT(2, 2) == 0x212121u, "backdrop %06x", AT(2, 2));
        CHECK(AT(2, 12) == 0x212121u, "left of the sprites %06x", AT(2, 12));
    } else if (sc == 1) {
        uint32_t plain = rgb_of(SAME), a = AT(8, 12), b = AT(24, 12), c = AT(40, 12), d = AT(56, 12), e = AT(72, 12);
        CHECK(a == plain, "palette 3 takes no colour maths: %06x, wants %06x", a, plain);
        CHECK(b != plain, "palette 4 takes colour maths: %06x", b);
        CHECK(c == b && d == b && e == b, "palettes 8, 100, 70000 are maths'd as 4: %06x %06x %06x against %06x", c, d, e, b);
        CHECK(((b >> 16) & 255) > ((plain >> 16) & 255), "maths added red: %06x", b);
    } else if (sc == 2) {
        const uint16_t col8[5] = { colour_of(8), colour_of(8), LB, LB, LE }, c150[5] = { 0xFFFF, 0xFFFF, LC, LC, LC };
        const uint16_t c0[5] = { colour_of(0), colour_of(0), colour_of(0), LD, LD };
        for (k = 0; k < 5; k++) {
            int y = 4 + 8 * k + 4;
            uint32_t w8 = rgb_of(col8[k]), w150 = c150[k] == 0xFFFF ? 0u : rgb_of(c150[k]), w0 = rgb_of(c0[k]);
            CHECK(AT(12, y) == w8, "line changes: palette 8, sprite %d shows %06x, wants %06x", k, AT(12, y), w8);
            CHECK(AT(36, y) == w150, "line changes: palette 150000, sprite %d shows %06x, wants %06x", k, AT(36, y), w150);
            CHECK(AT(60, y) == w0, "line changes: palette 0, sprite %d shows %06x, wants %06x", k, AT(60, y), w0);
        }
    } else if (sc == 7) {
        CHECK(AT(12, 12) == rgb_of(P100_NEW), "the last entry shows %06x", AT(12, 12));
        CHECK(AT(28, 12) == 0, "palette 196616 shows %06x", AT(28, 12));
    } else if (sc >= 3) {
        uint16_t c100 = sc == 3 ? colour_of(100) : sc == 6 ? P100_NEWER : P100_NEW;
        CHECK(AT(12, 12) == rgb_of(colour_of(8)), "scenario %d: palette 8 shows %06x", sc, AT(12, 12));
        CHECK(AT(28, 12) == rgb_of(c100), "scenario %d: palette 100 shows %06x, wants %06x", sc, AT(28, 12), rgb_of(c100));
        CHECK(AT(44, 12) == rgb_of(SAME), "scenario %d: palette 70000 shows %06x", sc, AT(44, 12));
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
    printf(failed ? "palettes: FAILED\n" : "palettes: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "palettes";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
