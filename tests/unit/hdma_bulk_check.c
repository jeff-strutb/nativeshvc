/*
 * hdma_bulk_check.c: a wait's HDMA lines made in bulk, and those that move
 * nothing passed at once (bus.c hdma_bulk), against the same lines made one
 * at a time. A read hook sends the console to one at a time, the path
 * tests/rom/run.py holds to the reference; the replay there cannot reach the
 * bulk path, its chip and its accesses every few clocks stopping it. So each
 * scene here is a cartridge waiting a frame at a time with its costs off, its
 * tables of every shape, and each frame prints what the console then shows:
 * the clock, the data bus, each channel's table address, count and indirect
 * address, a hash of the frame's description, CGRAM, VRAM and the work RAM a
 * channel writes.
 *
 *   hdma_bulk_check            the lines in bulk
 *   hdma_bulk_check --stepped  one at a time
 *
 * tests/unit/hdma_bulk.py runs both and compares them.
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "nshvc/machine.h"
#include "ppu.h"

#define WT 0x7E2000u          /* a table in work RAM */
#define WD 0x7E2800u          /* an indirect table's bytes */
#define WA 0x7E3000u          /* where a channel writes the A bus */
#define WA_LEN 256
#define ROM_TABLE 0x0100u     /* a table in the ROM, at $00:8100 */
#define FRAMES 3

enum {
    S_REPEAT, S_RUNS, S_RUNS_ROM, S_INDIRECT, S_CHANNELS, S_ENDS, S_128, S_TO_A, S_IRQ, S_IRQ_MASKED,
    S_IRQ_WRITES, S_NMI, S_OVERSCAN, S_CGRAM, S_VRAM, S_OPEN_BUS, NSCENES
};
static const char *const scene_name[NSCENES] = {
    "repeat", "runs", "runs_rom", "indirect", "channels", "ends", "run_128", "to_a", "irq", "irq_masked",
    "irq_writes", "nmi", "overscan", "cgram", "vram", "open_bus"
};

static uint8_t rom[0x8000];
static int scene;
static uint32_t irq_line;

static struct {
    uint64_t clock;
    uint8_t bus, ch[8][5];
    uint32_t a_hash;
} seen;

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static void abi_nmi(void) { rd8(0x004210); }

static void abi_irq(void)
{
    rd8(0x004211);
    if (scene == S_IRQ_WRITES) {
        /* a count written among the lines, and a channel turned off */
        wr8(0x00430A, 0x05);
        wr8(0x00431A, 0x83);
        wr8(0x00420C, 0x03);
    }
}

/* lengths of runs of no repeat, each with `each` bytes, then the end */
static void runs_table(uint32_t t, const uint8_t *lens, int n, int each)
{
    int i, k;
    for (i = 0; i < n; i++) {
        wr8(t++, lens[i]);
        for (k = 0; k < each; k++)
            wr8(t++, (uint8_t)(i * 37 + k * 11 + 1));
    }
    wr8(t, 0);
}

static void channel(int c, uint8_t dmap, uint8_t bbad, uint32_t table)
{
    uint32_t r = 0x004300u + (uint32_t)c * 0x10u;
    wr8(r, dmap);
    wr8(r + 1, bbad);
    wr8(r + 2, (uint8_t)table);
    wr8(r + 3, (uint8_t)(table >> 8));
    wr8(r + 4, (uint8_t)(table >> 16));
}

static void scene_set(int s)
{
    static const uint8_t mixed[] = { 0x7F, 0x50, 0x20, 0x01, 0x02, 0x03, 0x0A };
    static const uint8_t slow[] = { 0x41, 0x3D, 0x7F };
    static const uint8_t quick[] = { 0x07, 0x09, 0x0B, 0x0D, 0x11, 0x13, 0x17, 0x1D, 0x1F, 0x25 };
    uint32_t t = WT;
    int i;
    wr8(0x00420C, 0x00);
    wr8(0x004200, 0x00);
    wr8(0x002133, 0x00);
    wr8(0x002100, 0x0F);
    wr8(0x00212C, 0x01);
    cpu.p = (uint8_t)(cpu.p | P_I);
    switch (s) {
    case S_REPEAT:
        wr8(t++, 0xFF);
        for (i = 0; i < 127; i++)
            wr8(t++, (uint8_t)i);
        wr8(t++, 0xE1);
        for (i = 0; i < 97; i++)
            wr8(t++, (uint8_t)i);
        wr8(t, 0);
        channel(0, 0x00, 0x0D, WT);
        wr8(0x00420C, 0x01);
        break;
    case S_RUNS:
        runs_table(WT, mixed, 7, 2);
        channel(0, 0x02, 0x0E, WT);
        wr8(0x00420C, 0x01);
        break;
    case S_RUNS_ROM:
        channel(0, 0x02, 0x0E, 0x008000u | ROM_TABLE);
        wr8(0x00420C, 0x01);
        break;
    case S_INDIRECT:
        for (i = 0; i < 7; i++) {
            wr8(t++, mixed[i]);
            wr8(t++, (uint8_t)(WD + (uint32_t)i * 2));
            wr8(t++, (uint8_t)((WD + (uint32_t)i * 2) >> 8));
            wr8(WD + (uint32_t)i * 2, (uint8_t)(i * 29 + 3));
            wr8(WD + (uint32_t)i * 2 + 1, (uint8_t)(i * 13 + 5));
        }
        wr8(t, 0);
        channel(0, 0x42, 0x0E, WT);
        wr8(0x004307, (uint8_t)(WD >> 16));
        wr8(0x00420C, 0x01);
        break;
    case S_CHANNELS:
        runs_table(WT, slow, 3, 2);
        runs_table(WT + 0x100, quick, 10, 2);
        wr8(WT + 0x200, 0x7F);
        wr8(WT + 0x201, 0x55);
        wr8(WT + 0x202, 0);
        channel(0, 0x02, 0x0E, WT);
        channel(1, 0x02, 0x0D, WT + 0x100);
        channel(3, 0x00, 0x32, WT + 0x200);
        wr8(0x00420C, 0x0B);
        break;
    case S_ENDS:
        wr8(t++, 0x40);
        wr8(t++, 0x21);
        wr8(t, 0);
        channel(0, 0x00, 0x0D, WT);
        wr8(0x00420C, 0x01);
        break;
    case S_128:
        wr8(t++, 0x80);
        for (i = 0; i < 127; i++)
            wr8(t++, (uint8_t)(i + 1));
        wr8(t, 0);
        channel(0, 0x00, 0x0D, WT);
        wr8(0x00420C, 0x01);
        break;
    case S_TO_A:
        wr8(0x004202, 0x12);
        wr8(0x004203, 0x34);
        /* the table's run lengths, each with a byte's room: the channel's
           byte lands there from the B bus */
        t = WA;
        for (i = 0; i < 7; i++) {
            wr8(t++, mixed[i]);
            t++;
        }
        wr8(t, 0);
        channel(0, 0x80, 0x34, WA);
        channel(1, 0x02, 0x0E, WT + 0x100);
        runs_table(WT + 0x100, mixed, 7, 2);
        wr8(0x00420C, 0x03);
        break;
    case S_IRQ:
    case S_IRQ_MASKED:
    case S_IRQ_WRITES:
        runs_table(WT, mixed, 7, 2);
        runs_table(WT + 0x100, slow, 3, 2);
        channel(0, 0x02, 0x0E, WT);
        channel(1, 0x02, 0x0D, WT + 0x100);
        wr8(0x004209, (uint8_t)irq_line);
        wr8(0x00420A, (uint8_t)(irq_line >> 8));
        wr8(0x004200, 0x20);
        if (s != S_IRQ_MASKED)
            cpu.p = (uint8_t)(cpu.p & ~P_I);
        wr8(0x00420C, s == S_IRQ_WRITES ? 0x01 : 0x03);
        break;
    case S_NMI:
        runs_table(WT, mixed, 7, 2);
        channel(0, 0x02, 0x0E, WT);
        wr8(0x004200, 0x81);
        wr8(0x00420C, 0x01);
        break;
    case S_OVERSCAN:
        /* a transfer that moves the deadlines */
        wr8(t++, 0x60);
        wr8(t++, 0x04);
        wr8(t++, 0x60);
        wr8(t++, 0x00);
        wr8(t, 0);
        channel(0, 0x00, 0x33, WT);
        wr8(0x00420C, 0x01);
        break;
    case S_CGRAM:
        runs_table(WT, quick, 10, 2);
        channel(0, 0x02, 0x22, WT);
        wr8(0x002121, 0x10);
        wr8(0x00420C, 0x01);
        break;
    case S_VRAM:
        runs_table(WT, quick, 10, 2);
        channel(0, 0x01, 0x18, WT);
        wr8(0x002115, 0x80);
        wr8(0x002116, 0x00);
        wr8(0x002117, 0x10);
        wr8(0x002100, 0x80);
        wr8(0x00420C, 0x01);
        break;
    case S_OPEN_BUS:
        /* a table read from no memory: the data bus's own value */
        channel(0, 0x00, 0x0D, 0x004000u);
        wr8(0x00420C, 0x01);
        break;
    }
}

static void abi_reset(void)
{
    int at = -1;
    int i;
    cpu.e = 0;
    nshvc_set_cpu_costs(0);
    for (;;) {
        if (at != scene) {
            at = scene;
            scene_set(at);
        }
        nshvc_wait_vblank();
        /* the data bus first: the reads after it change it ($43xC reads it) */
        seen.bus = rd8(0x00430C);
        seen.clock = nshvc_cart_clock();
        for (i = 0; i < 8; i++) {
            uint32_t r = 0x004300u + (uint32_t)i * 0x10u;
            seen.ch[i][0] = rd8(r + 8);
            seen.ch[i][1] = rd8(r + 9);
            seen.ch[i][2] = rd8(r + 10);
            seen.ch[i][3] = rd8(r + 5);
            seen.ch[i][4] = rd8(r + 6);
        }
        seen.a_hash = 2166136261u;
        for (i = 0; i < WA_LEN; i++) {
            uint8_t v = rd8(WA + (uint32_t)i);
            seen.a_hash = fnv(seen.a_hash, &v, 1);
        }
    }
}

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x009000, abi_nmi, "nmi", 0 },
    { 0x00A000, abi_irq, "irq", 0 },
};

static void no_hook(uint32_t addr, uint8_t value)
{
    (void)addr;
    (void)value;
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int s, f, i;
    static const uint8_t table[] = { 0x7F, 0x10, 0x20, 0x50, 0x30, 0x40, 0x20, 0x50, 0x60, 0x01, 0x70, 0x80,
                                     0x02, 0x90, 0xA0, 0x03, 0xB0, 0xC0, 0x0A, 0xD0, 0xE0, 0x00 };
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x00;
    rom[0x7FEB] = 0x90;
    rom[0x7FEE] = 0x00;
    rom[0x7FEF] = 0xA0;
    rom[0x7FD5] = 0x20;
    memcpy(rom + ROM_TABLE, table, sizeof table);
    irq_line = 100;
    memset(&cart, 0, sizeof cart);
    cart.title = "hdma_bulk_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 3;
    if (argc > 1 && !strcmp(argv[1], "--stepped"))
        nshvc_set_read_hook(no_hook);
    nshvc_power(&cart);
    for (s = 0; s < NSCENES; s++) {
        scene = s;
        for (f = 0; f < FRAMES; f++) {
            size_t words = 0;
            const uint32_t *d;
            uint32_t h;
            nshvc_run_frame();
            d = ppu_compose(ppu_frame(), &words);
            h = fnv(2166136261u, d, words * sizeof *d);
            printf("%s %d clock %llu bus %02X", scene_name[s], f, (unsigned long long)seen.clock, seen.bus);
            for (i = 0; i < 4; i++)
                printf(" %02X%02X/%02X/%02X%02X", seen.ch[i][1], seen.ch[i][0], seen.ch[i][2], seen.ch[i][4], seen.ch[i][3]);
            printf(" frame %08X cgram %08X vram %08X a %08X\n", (unsigned)h, (unsigned)fnv(2166136261u, ppu_cgram(), 512),
                   (unsigned)fnv(2166136261u, ppu_vram(), 65536), (unsigned)seen.a_hash);
        }
    }
    return 0;
}
