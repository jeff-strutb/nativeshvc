/*
 * vram_direct.c: VRAM pages 1-3 written outside the console's bus and time
 * (nshvc_ext_vram_copy, nshvc_ext_vram_direct, contract 2.3), checked on a
 * cartridge of a few lines of C: BG1 in Mode 0 shows tile 0 everywhere, its
 * colour the picture's.
 *
 *   vram_direct           runs the checks; exit 0 when every one holds
 *   vram_direct --copy0   a copy to page 0: must stop the run (exit 70)
 *   vram_direct --direct0 page 0 written directly: must stop the run (exit 70)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "snes.h"

extern unsigned long ppu_version_overflow;

static int fails;
static const char *mode = "";

#define CHAR 0x1000             /* BG1's characters, in words: tile 0's 8 rows */
#define RED 0xFFFF0000u         /* colour 1, as the picture has it (0xFFRRGGBB) */
#define BLUE 0xFF0000FFu

static void check(int ok, const char *what, long long got, long long want)
{
    printf("%s %s (%lld, want %lld)\n", ok ? "ok  " : "FAIL", what, got, want);
    fails += !ok;
}

/* tile 0's 8 rows in colour c (1 or 2): plane 0 for 1, plane 1 for 2 */
static uint16_t row_of(int c) { return c == 1 ? 0x00FF : 0xFF00; }

static void tile_by_port(int c)
{
    int r;
    wr8(0x002115, 0x80);
    wr16(0x002116, CHAR);
    for (r = 0; r < 8; r++)
        wr16(0x002118, row_of(c));
}

static int at_line(void *ctx) { return snes_vcounter() == *(unsigned *)ctx; }

static void wait_line(unsigned l)
{
    nshvc_wait_until(at_line, &l);
}

/* the finished frame's row y, as one colour (0 when it is not one) */
static uint32_t row_colour(int y)
{
    int w, h, x;
    const uint32_t *p = nshvc_picture(&w, &h);
    for (x = 1; x < w; x++)
        if (p[y * w + x] != p[y * w])
            return 0;
    return p[y * w];
}

static int first_row(uint32_t c)
{
    int w, h, y;
    nshvc_picture(&w, &h);
    for (y = 0; y < h; y++)
        if (row_colour(y) == c)
            return y;
    return -1;
}

static int tile_is(int page, int c)
{
    const uint16_t *v = nshvc_vram() + ((unsigned)page << 15);
    int r;
    for (r = 0; r < 8; r++)
        if (v[CHAR + r] != row_of(c))
            return 0;
    return 1;
}

static void abi_reset(void)
{
    static uint8_t src[2000];
    uint64_t t;
    unsigned long over;
    int i;
    cpu.e = 0;
    wr8(0x004200, 0x00);
    nshvc_ext_vram(256);
    if (!strcmp(mode, "--copy0"))
        nshvc_ext_vram_copy(0, 1);
    if (!strcmp(mode, "--direct0"))
        nshvc_ext_vram_direct(0);

    /* the scene, in forced blank: BG1, Mode 0, map at 0 (tile 0), tile 0
       red; colour 1 red, 2 blue */
    nshvc_wait_vblank();
    wr8(0x002100, 0x80);
    wr8(0x002105, 0x00);
    wr8(0x002107, 0x00);
    wr8(0x00210B, 0x01);
    wr8(0x00212C, 0x01);
    wr8(0x002121, 0x01);
    wr8(0x002122, 0x1F);
    wr8(0x002122, 0x00);
    wr8(0x002122, 0x00);
    wr8(0x002122, 0x7C);
    wr8(0x002115, 0x80);
    wr16(0x002116, 0x0000);
    for (i = 0; i < 0x400; i++)
        wr16(0x002118, 0x0000);
    tile_by_port(1);
    wr8(0x002100, 0x0F);

    /* the copy: page 0 into page 1, all of it */
    nshvc_ext_vram_copy(1, 0);
    check(!memcmp(nshvc_vram() + 0x8000, nshvc_vram(), 0x10000), "the copy: page 1 is page 0's 64 KB",
          memcmp(nshvc_vram() + 0x8000, nshvc_vram(), 0x10000) != 0, 0);

    /* while lines are drawn: a write through the port to page 0 is lost,
       as on the console; with page 1 direct, it lands there at once */
    wait_line(100);
    tile_by_port(2);
    check(tile_is(0, 1), "while lines are drawn, the port's writes to page 0 are lost", tile_is(0, 1), 1);
    nshvc_ext_vram_direct(1);
    tile_by_port(2);
    nshvc_ext_vram_direct(-1);
    check(tile_is(1, 2) && tile_is(0, 1), "direct to page 1: the writes land there, page 0 unchanged",
          tile_is(1, 2), 1);

    /* a DMA of only the data port to the direct page: no time (the S-CPU's
       costs off, so its own writes take none either) */
    for (i = 0; i < 2000; i += 2) {
        src[i] = (uint8_t)i;
        src[i + 1] = (uint8_t)(i >> 8 | 0x80);
    }
    memcpy(nshvc_wram() + 0x2000, src, sizeof src);
    wait_line(120);
    nshvc_set_cpu_costs(0);
    nshvc_ext_vram_direct(2);
    t = snes_clock;
    wr8(0x002115, 0x80);
    wr16(0x002116, 0x4000);
    wr8(0x004300, 0x01);
    wr8(0x004301, 0x18);
    wr16(0x004302, 0x2000);
    wr8(0x004304, 0x7E);
    wr16(0x004305, 2000);
    wr8(0x00420B, 0x01);
    check(snes_clock == t, "a DMA of 2,000 bytes to the direct page takes no time", (long long)(snes_clock - t), 0);
    {
        const uint16_t *v = nshvc_vram() + (2u << 15);
        int bad = 0;
        for (i = 0; i < 1000; i++)
            bad += v[0x4000 + i] != (uint16_t)(src[2 * i] | src[2 * i + 1] << 8);
        check(!bad, "its 1,000 words land in page 2, in order", bad, 0);
    }
    /* with a channel that writes elsewhere (CGRAM) too: that one takes the
       unit's time, the direct one none */
    wr16(0x004302, 0x2000);
    wr16(0x004305, 2000);
    wr8(0x004310, 0x00);
    wr8(0x004311, 0x22);
    wr16(0x004312, 0x2000);
    wr8(0x004314, 0x7E);
    wr16(0x004315, 4);
    wr8(0x002121, 0x10);
    t = snes_clock;
    wr8(0x00420B, 0x03);
    check(snes_clock - t >= 8 + 8 + 4 * 8 && snes_clock - t <= 8 + 8 + 4 * 8 + 16 + 40,
          "with a CGRAM channel too: the unit's time for its 4 bytes only", (long long)(snes_clock - t), 48);
    nshvc_ext_vram_direct(-1);
    nshvc_set_cpu_costs(1);

    /* the picture: page 1 changed while no layer read it, so the frame
       stays red, and none of its copies were spent on page 1 */
    over = ppu_version_overflow;
    nshvc_wait_vblank();
    check(first_row(RED) == 0 && first_row(BLUE) < 0, "page 1 changed unshown: the frame is page 0's, red",
          first_row(BLUE), -1);
    /* BG1 switched to page 1 in the vertical blank: the next frame blue */
    nshvc_ext_layer_page(0, 1);
    wait_line(10);
    nshvc_wait_vblank();
    check(first_row(BLUE) == 0 && first_row(RED) < 0, "BG1 switched to page 1: the next frame is blue",
          first_row(RED), -1);
    /* page 1, shown, written directly at line 100's start: from the next
       line drawn, line 100 itself (drawn at its horizontal blank; the
       picture's row 99), red */
    wait_line(100);
    nshvc_ext_vram_direct(1);
    tile_by_port(1);
    nshvc_ext_vram_direct(-1);
    nshvc_wait_vblank();
    check(first_row(RED) == 99 && row_colour(98) == BLUE, "a write to the shown page at line 100: red from its row, 99",
          first_row(RED), 99);
    check(ppu_version_overflow == over, "no frame ran out of copies", (long long)(ppu_version_overflow - over), 0);

    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

int main(int argc, char **argv)
{
    nshvc_cart cart;
    if (argc > 1)
        mode = argv[1];
    memset(&cart, 0, sizeof cart);
    cart.title = "vram_direct";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
