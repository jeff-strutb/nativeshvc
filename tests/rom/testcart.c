/*
 * testcart.c: the test cartridges' C (tests/rom/snestest.py): the same
 * interpreter as the ROM's 65816 code, written against the machine model,
 * reading the same table of steps from the same ROM. Each access it makes
 * is one the assembly makes, in the same order, its read-modify-writes
 * (DEC) stated as such; its work takes no time, and its poll loops (a
 * line, the vertical blank, a value in memory) are waits (machine.h).
 *
 *   testcart --rom ROM [nshvc_main's options]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* .poll: the counters latched, the line read (STA $04, $05), compared
   with $02 */
static int at_line(void *ctx)
{
    uint8_t lo, hi;
    (void)ctx;
    rd8(0x00213F);
    rd8(0x002137);
    lo = rd8(0x00213D);
    wr8(0x000004, lo);
    hi = (uint8_t)(rd8(0x00213D) & 1);
    wr8(0x000005, hi);
    return rd16(0x000004) == rd16(0x000002);
}

/* LDA $4212: the vertical blank's flag as `want` (0x80 or 0) */
static int vblank_is(void *ctx) { return (rd8(0x004212) & 0x80) == *(uint8_t *)ctx; }

/* LDA ($00); CMP $02 */
static int holds(void *ctx)
{
    (void)ctx;
    rd8(0x000000);
    rd8(0x000001);
    return rd8(rd16(0x000000)) == rd8(0x000002);
}

static void abi_reset(void)
{
    uint16_t x, y;
    /* NSHVC_TEST_SCREEN=WxH: the cartridge asks for a larger screen (the
       extension's checks; nothing of the console's time) */
    {
        const char *e = getenv("NSHVC_TEST_SCREEN");
        int w, h;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2)
            nshvc_ext_screen(w, h);
        if (getenv("NSHVC_TEST_NOLIMITS"))
            nshvc_ext_sprite_limits(0);
        /* NSHVC_TEST_VRAM_PAGE=n: everything the cartridge puts in VRAM, and
           every layer's reads, in page n of 256 KB */
        if (getenv("NSHVC_TEST_VRAM_KB"))
            nshvc_ext_vram(atoi(getenv("NSHVC_TEST_VRAM_KB")));
        if (getenv("NSHVC_TEST_VRAM_PAGE")) {
            int k, pg = atoi(getenv("NSHVC_TEST_VRAM_PAGE"));
            nshvc_ext_vram(256);
            nshvc_ext_vram_page(pg);
            for (k = 0; k < 5; k++)
                nshvc_ext_layer_page(k, pg);
        }
    }
    /* SEI, CLC, XCE, REP #$10, SEP #$20, LDX #$01FF, TXS, LDA #$00, PHA */
    cpu.e = 0;
    cpu.s = 0x01FF;
    wr8(0x0001FF, 0x00);
    /* PLB */
    rd8(0x0001FF);
    /* LDX #$9000, LDY #$0000 */
    x = 0x9000;
    y = 0;
    for (;;) {
        uint8_t op;
        /* next: LDA $0000,X; INX */
        op = rd8(x);
        x++;
        if (op == 0x01) {
            uint16_t a;
            uint8_t v;
            a = rd16(x);
            wr16(0x000000, a);
            v = rd8((uint16_t)(x + 2));
            rd8(0x000000);
            rd8(0x000001);
            wr8(a, v);
            x = (uint16_t)(x + 3);
        } else if (op == 0x04) {
            wr16(0x000002, rd16(x));
            x = (uint16_t)(x + 2);
            nshvc_wait_until(at_line, NULL);
        } else if (op == 0x05) {
            uint8_t n;
            n = rd8(x);
            wr8(0x000006, n);
            x++;
            do {
                static uint8_t out = 0, in = 0x80;
                nshvc_wait_until(vblank_is, &out);  /* .out: LDA $4212; BMI .out */
                nshvc_wait_until(vblank_is, &in);   /* .in: LDA $4212; BPL .in */
                n = rd8_rmw(0x000006);
                n--;
                wr8(0x000006, n);
            } while (n);
        } else if (op == 0x06) {
            uint16_t a;
            uint8_t bank, v;
            a = rd16(x);
            wr16(0x000008, a);
            bank = rd8((uint16_t)(x + 2));
            wr8(0x00000A, bank);
            x = (uint16_t)(x + 3);
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            v = rd8((uint32_t)bank << 16 | a);
            wr8(0x0001FF, (uint8_t)(x >> 8));
            wr8(0x0001FE, (uint8_t)x);
            wr8(0x7E2000u + y, v);
            rd8(0x0001FE);
            rd8(0x0001FF);
            y++;
        } else if (op == 0x07) {
            uint16_t a;
            uint8_t bank, v;
            a = rd16(x);
            wr16(0x000008, a);
            bank = rd8((uint16_t)(x + 2));
            wr8(0x00000A, bank);
            v = rd8((uint16_t)(x + 3));
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            wr8((uint32_t)bank << 16 | a, v);
            x = (uint16_t)(x + 4);
        } else if (op == 0x08) {
            cpu.p &= (uint8_t)~P_I;
        } else if (op == 0x09) {
            cpu_wai(0x008000);                      /* WAI */
        } else if (op == 0x0A) {
            wr16(0x000000, rd16(x));
            wr8(0x000002, rd8((uint16_t)(x + 2)));
            x = (uint16_t)(x + 3);
            nshvc_wait_until(holds, NULL);
        } else if (op == 0x0B) {
            uint8_t n, m, t;
            n = rd8(x);
            wr8(0x000006, n);
            m = rd8((uint16_t)(x + 1));
            wr8(0x000007, m);
            x = (uint16_t)(x + 2);
            t = rd8(0x000006);
            while (t) {                         /* .a: DEC $06; BNE .a */
                t = rd8_rmw(0x000006);
                t--;
                wr8(0x000006, t);
            }
            t = rd8(0x000007);
            while (t) {                         /* .c: NOP; DEC $07; BNE .c */
                t = rd8_rmw(0x000007);
                t--;
                wr8(0x000007, t);
            }
        } else if (op == 0x0C) {
            uint8_t n, m;
            n = rd8(x);
            wr8(0x000006, n);
            m = rd8((uint16_t)(x + 1));
            wr8(0x000007, m);
            x = (uint16_t)(x + 2);
            do {
                do {
                    n = rd8_rmw(0x000006);
                    n--;
                    wr8(0x000006, n);
                } while (n);
                m = rd8_rmw(0x000007);
                m--;
                wr8(0x000007, m);
            } while (m);
        } else if (op == 0x0D) {
            uint8_t v;
            v = rd8(x);
            wr8(0x00420B, v);
            x++;
        } else if (op >= 0x0E && op <= 0x11) {
            /* the multiplier's and divider's results as the reads see them */
            uint8_t v;
            v = rd8(x);
            wr8(op == 0x0F ? 0x004206 : 0x004203, v);
            if (op == 0x11)
                wr8(0x004206, v);
            {
                static const uint16_t regs[4] = { 0x4214, 0x4215, 0x4216, 0x4217 };
                int k;
                for (k = 0; k < 4; k++) {
                    uint8_t r;
                    r = rd8(regs[k]);
                    wr8((uint32_t)(0x10 + k), r);
                }
            }
            x++;
        } else {
            /* halt: WAI; BRA halt */
            for (;;)
                cpu_wai(0x008000);
        }
    }
}

/* The handlers, their accesses as their 65816 code makes them. Native
   mode, 16-bit index registers, direct page 0. */
static void push16_t(void)
{
    wr8(cpu.s, 0);
    wr8((uint16_t)(cpu.s - 1), 0);
    cpu.s = (uint16_t)(cpu.s - 2);
}

static void pull16_t(void)
{
    rd8((uint16_t)(cpu.s + 1));
    rd8((uint16_t)(cpu.s + 2));
    cpu.s = (uint16_t)(cpu.s + 2);
}

/* The handlers' steps from x up to their 00 (.step: up to .done, BEQ
   taken): where they end */
static uint16_t handler_steps(uint16_t x)
{
    for (;;) {
        uint8_t op, v;
        uint16_t a;
        op = rd8(x);
        if (!op)
            return x;
        a = rd16((uint16_t)(x + 1));
        wr16(0x00000C, a);
        v = rd8((uint16_t)(x + 3));
        rd8(0x00000C);
        rd8(0x00000D);
        wr8(a, v);
        x = (uint16_t)(x + 4);
    }
}

/* the IRQ: its steps from where the last IRQ left off */
static void abi_irq(void)
{
    uint16_t x;
    push16_t();
    push16_t();
    push16_t();
    rd8(0x004211);
    x = handler_steps(rd16(0x000010));
    x++;
    wr16(0x000010, x);
    pull16_t();
    pull16_t();
    pull16_t();
    /* RTI: the console's */
}

/* the NMI: its steps from their start, and the IRQ's back to theirs */
static void abi_nmi(void)
{
    push16_t();
    push16_t();
    push16_t();
    rd8(0x004210);
    wr16(0x000010, 0xE000);
    handler_steps(0xF000);
    pull16_t();
    pull16_t();
    pull16_t();
    /* RTI: the console's */
}

static uint8_t *rom;
static uint32_t rom_size;

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
};

/* tests/dsp/keys.py's SPC700 driver (tests/dsp/keys_spc.c), where its
   cartridge starts it */
void keys_spc_main(void);
static const nshvc_spc_routine spc_routines[] = {
    { 0x0200, keys_spc_main, "keys", NULL },
};

int main(int argc, char **argv)
{
    nshvc_cart cart;
    const char *path = NULL;
    FILE *f;
    int i;
    uint16_t nmi, irq;
    static nshvc_routine table[3];
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            path = argv[i + 1];
    if (!path) {
        fprintf(stderr, "testcart: --rom ROM\n");
        return 2;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "testcart: cannot read %s\n", path);
        return 2;
    }
    rom = (uint8_t *)malloc(16u << 20);
    rom_size = (uint32_t)fread(rom, 1, 16u << 20, f);
    fclose(f);
    /* the mapping, from where snestest.py put the header (its map mode
       byte), and the cartridge's RAM from the header's size byte */
    {
        uint32_t h = 0x7FC0;
        nshvc_map map = NSHVC_LOROM;
        /* ExLoROM: a LoROM header past 4 MB, at the code's bank */
        if (rom_size > 0x407FDF && (rom[0x407FD5] & 0xEF) == 0x20) {
            h = 0x407FC0;
            map = NSHVC_EXLOROM;
        } else if (rom_size > 0x40FFDF && rom[0x40FFD5] == 0x25) {
            h = 0x40FFC0;
            map = NSHVC_EXHIROM;
        } else if (rom_size > 0xFFDF && rom[0xFFD5] == 0x21) {
            h = 0xFFC0;
            map = NSHVC_HIROM;
        }
        memset(&cart, 0, sizeof cart);
        cart.map = map;
        cart.sram_size = rom[h + 0x18] ? 1024u << rom[h + 0x18] : 0;
        /* the header's country: Europe and its others are PAL */
        cart.region = rom[h + 0x19] >= 0x02 && rom[h + 0x19] <= 0x0C ? NSHVC_PAL : NSHVC_NTSC;
        /* the native mode vectors: the NMI's and the IRQ's handlers */
        nmi = (uint16_t)(rom[h + 0x2A] | rom[h + 0x2B] << 8);
        irq = (uint16_t)(rom[h + 0x2E] | rom[h + 0x2F] << 8);
    }
    table[0] = routines[0];
    table[1].addr = nmi;
    table[1].abi = abi_nmi;
    table[1].name = "nmi";
    table[2].addr = irq;
    table[2].abi = abi_irq;
    table[2].name = "irq";
    cart.title = "nativeshvc test";
    cart.save_name = "nshvc_test";
    cart.rom = rom;
    cart.rom_size = rom_size;
    cart.routines = table;
    cart.routine_count = 3;
    cart.spc_routines = spc_routines;
    cart.spc_routine_count = 1;
    for (i = 0; i < 3; i++) {
        int j;
        for (j = i + 1; j < 3; j++)
            if (table[j].addr < table[i].addr) {
                nshvc_routine t = table[i];
                table[i] = table[j];
                table[j] = t;
            }
    }
    return nshvc_main(&cart, argc, argv);
}
