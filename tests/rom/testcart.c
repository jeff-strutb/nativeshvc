/*
 * testcart.c: the test cartridges' C (tests/rom/snestest.py): the same
 * interpreter as the ROM's 65816 code, written against the machine model,
 * reading the same table of steps from the same ROM. Each access it makes
 * is one the assembly makes, in the same order; the time between them is
 * the assembly's, roughly (the steps' waits poll the beam, so a test does
 * not depend on the exact time).
 *
 *   testcart --rom ROM [nshvc_main's options]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* the 65816's clocks for an instruction of n bus cycles at 8 clocks and
   k internal cycles: what the steps between two accesses cost */
#define C(n, k) cyc((n) * 8u + (k) * 6u)

static uint16_t rd16_00(uint16_t a) { return rd16(a); }

/* The steps' interpreter, cycle for cycle as its 65816 code runs (SlowROM:
   8 clocks a code byte, 8 for work RAM, 6 for registers and internal
   cycles, 12 for $4000-$41FF): each instruction's clocks before an access
   stated before it. Native mode, 8-bit A, 16-bit X and Y, direct page 0,
   data bank 0. */
#define OP 8                /* a code byte */
#define IO 6                /* an internal cycle */

/* the dispatch's tests up to the step's (CMP #nn; BNE + taken for each
   before, then CMP, BNE not taken, JMP) */
static void dispatch(int k)
{
    cyc((uint32_t)k * (2 * OP + 2 * OP + IO));
    cyc(2 * OP + 2 * OP + 3 * OP);
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
        if (getenv("NSHVC_TEST_VRAM_PAGE")) {
            int k, pg = atoi(getenv("NSHVC_TEST_VRAM_PAGE"));
            nshvc_ext_vram(256);
            nshvc_ext_vram_page(pg);
            for (k = 0; k < 5; k++)
                nshvc_ext_layer_page(k, pg);
        }
    }
    /* SEI, CLC, XCE, REP #$10, SEP #$20, LDX #$01FF, TXS, LDA #$00, PHA */
    cyc(3 * (OP + IO) + 2 * (2 * OP + IO) + 3 * OP + (OP + IO) + 2 * OP + OP + IO);
    cpu.e = 0;
    cpu.s = 0x01FF;
    wr8(0x0001FF, 0x00);
    /* PLB */
    cyc(OP + IO + IO);
    rd8(0x0001FF);
    /* LDX #$9000, LDY #$0000 */
    cyc(2 * 3 * OP);
    x = 0x9000;
    y = 0;
    for (;;) {
        uint8_t op;
        /* next: LDA $0000,X; INX */
        cyc(3 * OP + IO + OP);
        op = rd8(x);
        cyc(OP + IO);
        x++;
        if (op == 0x01) {
            uint16_t a;
            uint8_t v;
            dispatch(0);
            cyc(2 * OP + IO);                       /* REP #$20 */
            cyc(3 * OP + IO + 2 * OP);              /* LDA $0000,X */
            a = rd16(x);
            cyc(2 * OP);                            /* STA $00 */
            wr16(0x000000, a);
            cyc(2 * OP + IO);                       /* SEP #$20 */
            cyc(3 * OP + IO + OP);                  /* LDA $0002,X */
            v = rd8((uint16_t)(x + 2));
            cyc(2 * OP);                            /* STA ($00) */
            rd8(0x000000);
            rd8(0x000001);
            wr8(a, v);
            cyc(3 * (OP + IO) + 3 * OP);            /* INX x 3, JMP next */
            x = (uint16_t)(x + 3);
        } else if (op == 0x04) {
            uint16_t line, at;
            dispatch(1);
            cyc(2 * OP + IO);
            cyc(3 * OP + IO + 2 * OP);
            line = rd16(x);
            cyc(2 * OP);
            wr16(0x000002, line);
            cyc(2 * OP + IO);
            cyc(2 * (OP + IO));                     /* INX x 2 */
            x = (uint16_t)(x + 2);
            do {
                uint8_t lo, hi;
                cyc(3 * OP);                        /* LDA $213F */
                rd8(0x00213F);
                cyc(3 * OP);                        /* LDA $2137 */
                rd8(0x002137);
                cyc(3 * OP);                        /* LDA $213D */
                lo = rd8(0x00213D);
                cyc(2 * OP);                        /* STA $04 */
                wr8(0x000004, lo);
                cyc(3 * OP);                        /* LDA $213D */
                hi = rd8(0x00213D);
                cyc(2 * OP);                        /* AND #$01 */
                hi &= 1;
                cyc(2 * OP);                        /* STA $05 */
                wr8(0x000005, hi);
                cyc(2 * OP + IO);                   /* REP #$20 */
                cyc(2 * OP);                        /* LDA $04 */
                at = rd16(0x000004);
                cyc(2 * OP);                        /* CMP $02 */
                rd16(0x000002);
                cyc(2 * OP + IO);                   /* SEP #$20 */
                cyc(at != line ? 2 * OP + IO : 2 * OP);   /* BNE .poll */
            } while (at != line);
            cyc(3 * OP);                            /* JMP next */
        } else if (op == 0x05) {
            uint8_t n;
            dispatch(2);
            cyc(3 * OP + IO + OP);                  /* LDA $0000,X */
            n = rd8(x);
            cyc(2 * OP);                            /* STA $06 */
            wr8(0x000006, n);
            cyc(OP + IO);                           /* INX */
            x++;
            do {
                uint8_t f;
                do {                                /* .out: LDA $4212; BMI .out */
                    cyc(3 * OP);
                    f = rd8(0x004212);
                    cyc(f & 0x80 ? 2 * OP + IO : 2 * OP);
                } while (f & 0x80);
                do {                                /* .in: LDA $4212; BPL .in */
                    cyc(3 * OP);
                    f = rd8(0x004212);
                    cyc(!(f & 0x80) ? 2 * OP + IO : 2 * OP);
                } while (!(f & 0x80));
                cyc(2 * OP);                        /* DEC $06 */
                n = rd8(0x000006);
                cyc(IO);
                n--;
                wr8(0x000006, n);
                cyc(n ? 2 * OP + IO : 2 * OP);      /* BNE .out */
            } while (n);
            cyc(3 * OP);                            /* JMP next */
        } else if (op == 0x06) {
            uint16_t a;
            uint8_t bank, v;
            dispatch(3);
            cyc(2 * OP + IO);
            cyc(3 * OP + IO + 2 * OP);
            a = rd16(x);
            cyc(2 * OP);
            wr16(0x000008, a);
            cyc(2 * OP + IO);
            cyc(3 * OP + IO + OP);
            bank = rd8((uint16_t)(x + 2));
            cyc(2 * OP);
            wr8(0x00000A, bank);
            cyc(3 * (OP + IO));                     /* INX x 3 */
            x = (uint16_t)(x + 3);
            cyc(2 * OP);                            /* LDA [$08] */
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            v = rd8((uint32_t)bank << 16 | a);
            cyc(OP + IO);                           /* PHX */
            wr8(0x0001FF, (uint8_t)(x >> 8));
            wr8(0x0001FE, (uint8_t)x);
            cyc(OP + IO);                           /* TYX */
            cyc(4 * OP);                            /* STA $7E2000,X */
            wr8(0x7E2000u + y, v);
            cyc(OP + IO + IO);                      /* PLX */
            rd8(0x0001FE);
            rd8(0x0001FF);
            cyc(OP + IO);                           /* INY */
            y++;
            cyc(3 * OP);                            /* JMP next */
        } else if (op == 0x07) {
            uint16_t a;
            uint8_t bank, v;
            dispatch(4);
            cyc(2 * OP + IO);
            cyc(3 * OP + IO + 2 * OP);
            a = rd16(x);
            cyc(2 * OP);
            wr16(0x000008, a);
            cyc(2 * OP + IO);
            cyc(3 * OP + IO + OP);
            bank = rd8((uint16_t)(x + 2));
            cyc(2 * OP);
            wr8(0x00000A, bank);
            cyc(3 * OP + IO + OP);
            v = rd8((uint16_t)(x + 3));
            cyc(2 * OP);                            /* STA [$08] */
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            wr8((uint32_t)bank << 16 | a, v);
            cyc(4 * (OP + IO) + 3 * OP);            /* INX x 4, JMP next */
            x = (uint16_t)(x + 4);
        } else if (op == 0x08) {
            dispatch(5);
            cyc(OP + IO);                           /* CLI */
            cpu.p &= (uint8_t)~P_I;
            cyc(3 * OP);
        } else if (op == 0x09) {
            dispatch(6);
            cpu_wai(0x008000);                      /* WAI */
            cyc(3 * OP);
        } else {
            /* halt: the dispatch's seven tests, then WAI; BRA halt */
            cyc(7 * (2 * OP + 2 * OP + IO));
            for (;;) {
                cpu_wai(0x008000);
                cyc(2 * OP + IO);
            }
        }
    }
}

/* The IRQ handler, cycle for cycle as its 65816 code runs (SlowROM: 8
   clocks a code byte, 8 for work RAM, 6 for registers and internal
   cycles): each instruction's clocks before an access stated before it,
   so every access lands where the original's does. Native mode, 16-bit
   index registers, direct page 0. */
static void push16_t(void)
{
    cyc(8 + 6);                    /* PHA/PHX/PHY: the opcode, an internal cycle */
    wr8(cpu.s, 0);
    wr8((uint16_t)(cpu.s - 1), 0);
    cpu.s = (uint16_t)(cpu.s - 2);
}

static void pull16_t(void)
{
    cyc(8 + 6 + 6);                /* PLY/PLX/PLA: the opcode, two internal cycles */
    rd8((uint16_t)(cpu.s + 1));
    rd8((uint16_t)(cpu.s + 2));
    cpu.s = (uint16_t)(cpu.s + 2);
}

static void abi_irq(void)
{
    uint16_t x;
    cyc(8 + 8 + 6);                /* REP #$30 */
    push16_t();
    push16_t();
    push16_t();
    cyc(8 + 8 + 6);                /* SEP #$20 */
    cyc(8 + 8 + 8);                /* LDA $4211 */
    rd8(0x004211);
    cyc(8 + 8);                    /* LDX $10 */
    x = rd16(0x000010);
    for (;;) {
        uint8_t op, v;
        uint16_t a;
        cyc(8 + 8 + 8 + 6 + 8);    /* LDA $0000,X: the step's code from ROM */
        op = rd8(x);
        if (!op) {
            cyc(8 + 8 + 6);        /* BEQ, taken */
            break;
        }
        cyc(8 + 8);                /* BEQ, not taken */
        cyc(8 + 8 + 6);            /* REP #$20 */
        cyc(8 + 8 + 8 + 6 + 8 + 8);/* LDA $0001,X */
        a = rd16((uint16_t)(x + 1));
        cyc(8 + 8);                /* STA $0C */
        wr16(0x00000C, a);
        cyc(8 + 8 + 6);            /* SEP #$20 */
        cyc(8 + 8 + 8 + 6 + 8);    /* LDA $0003,X */
        v = rd8((uint16_t)(x + 3));
        cyc(8 + 8);                /* STA ($0C) */
        rd8(0x00000C);
        rd8(0x00000D);
        wr8(a, v);
        cyc(4 * (8 + 6));          /* INX x 4 */
        cyc(8 + 8 + 6);            /* BRA .step */
        x = (uint16_t)(x + 4);
    }
    cyc(8 + 6);                    /* INX */
    x++;
    cyc(8 + 8);                    /* STX $10 */
    wr16(0x000010, x);
    cyc(8 + 8 + 6);                /* REP #$30 */
    pull16_t();
    pull16_t();
    pull16_t();
    /* RTI: the console's */
}

/* the NMI's steps (not timed to the cycle: they run in the vertical
   blank) */
static void push16(uint16_t v)
{
    wr8(cpu.s, (uint8_t)(v >> 8));
    cpu.s--;
    wr8(cpu.s, (uint8_t)v);
    cpu.s--;
    C(1, 1);
}

static void pull16(void)
{
    cpu.s++;
    rd8(cpu.s);
    cpu.s++;
    rd8(cpu.s);
    C(1, 2);
}

static uint16_t handler_steps(uint16_t x)
{
    for (;;) {
        uint8_t op = rd8(x);
        uint16_t a;
        uint8_t v;
        C(3, 0);
        if (!op)
            return x;
        a = rd16(x + 1);
        wr16(0x00000C, a);
        v = rd8((uint16_t)(x + 3));
        C(6, 0);
        wr8(a, v);
        x = (uint16_t)(x + 4);
        C(5, 4);
    }
}

/* the NMI: its steps from their start, and the IRQ's back to theirs */
static void abi_nmi(void)
{
    C(2, 0);
    push16(0);
    push16(0);
    push16(0);
    C(2, 0);
    rd8(0x004210);
    C(3, 0);
    wr16(0x000010, 0xE000);
    C(3, 0);
    handler_steps(0xF000);
    C(2, 0);
    pull16();
    pull16();
    pull16();
}

static uint8_t *rom;
static uint32_t rom_size;

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
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
        if (rom_size > 0x40FFDF && rom[0x40FFD5] == 0x25) {
            h = 0x40FFC0;
            map = NSHVC_EXHIROM;
        } else if (rom_size > 0xFFDF && rom[0xFFD5] == 0x21) {
            h = 0xFFC0;
            map = NSHVC_HIROM;
        }
        memset(&cart, 0, sizeof cart);
        cart.map = map;
        cart.sram_size = rom[h + 0x18] ? 1024u << rom[h + 0x18] : 0;
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
    cart.region = NSHVC_NTSC;
    cart.rom = rom;
    cart.rom_size = rom_size;
    cart.routines = table;
    cart.routine_count = 3;
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
