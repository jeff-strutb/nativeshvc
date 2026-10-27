/*
 * testcart.c: the test cartridges' C (tests/rom/snestest.py): the same
 * interpreter as the ROM's 65816 code, written against the machine model,
 * reading the same table of steps from the same ROM. Each access it makes
 * is one the assembly makes, in the same order; the time between them is
 * the assembly's, and its read-modify-writes (DEC) are stated as such, so
 * an interrupt is taken between its operations as the console takes one
 * (machine.h); the steps' waits poll the beam, so a test does not depend
 * on the exact time).
 *
 *   testcart --rom ROM [nshvc_main's options]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* an instruction's time before its access */
#define I(clocks) cyc(clocks)
/* the same, the instruction's last cycle (last clocks) in it */
#define L(clocks, last) ((void)(last), cyc(clocks))
/* a two-cycle implied instruction (INX, TYX, CLI...): its opcode, then
   the internal cycle the CPU makes a read when an interrupt follows */
#define IMP() (cyc(OP), cpu_implied(0x008000))

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
    int i;
    for (i = 0; i < k; i++) {
        L(2 * OP, OP);                  /* CMP #nn */
        L(2 * OP + IO, IO);             /* BNE +, taken */
    }
    L(2 * OP, OP);                      /* CMP #nn */
    L(2 * OP, OP);                      /* BNE +, not taken */
    L(3 * OP, OP);                  /* JMP op */
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
        L(3 * OP + IO + OP, OP);
        op = rd8(x);
        IMP();
        x++;
        if (op == 0x01) {
            uint16_t a;
            uint8_t v;
            dispatch(0);
            L(2 * OP + IO, IO);                       /* REP #$20 */
            L(3 * OP + IO + 2 * OP, OP);              /* LDA $0000,X */
            a = rd16(x);
            I(2 * OP);                            /* STA $00 */
            wr16(0x000000, a);
            L(2 * OP + IO, IO);                       /* SEP #$20 */
            L(3 * OP + IO + OP, OP);                  /* LDA $0002,X */
            v = rd8((uint16_t)(x + 2));
            I(2 * OP);                            /* STA ($00) */
            rd8(0x000000);
            rd8(0x000001);
            wr8(a, v);
            IMP(); IMP(); IMP();     /* INX x 3 */
            L(3 * OP, OP);                              /* JMP next */
            x = (uint16_t)(x + 3);
        } else if (op == 0x04) {
            uint16_t line, at;
            dispatch(1);
            L(2 * OP + IO, IO);
            L(3 * OP + IO + 2 * OP, OP);
            line = rd16(x);
            I(2 * OP);
            wr16(0x000002, line);
            L(2 * OP + IO, IO);
            IMP(); IMP();                 /* INX x 2 */
            x = (uint16_t)(x + 2);
            do {
                uint8_t lo, hi;
                I(3 * OP);                        /* LDA $213F */
                rd8(0x00213F);
                I(3 * OP);                        /* LDA $2137 */
                rd8(0x002137);
                I(3 * OP);                        /* LDA $213D */
                lo = rd8(0x00213D);
                I(2 * OP);                        /* STA $04 */
                wr8(0x000004, lo);
                I(3 * OP);                        /* LDA $213D */
                hi = rd8(0x00213D);
                L(2 * OP, OP);                        /* AND #$01 */
                hi &= 1;
                I(2 * OP);                        /* STA $05 */
                wr8(0x000005, hi);
                L(2 * OP + IO, IO);                   /* REP #$20 */
                I(2 * OP);                        /* LDA $04 */
                at = rd16(0x000004);
                I(2 * OP);                        /* CMP $02 */
                rd16(0x000002);
                L(2 * OP + IO, IO);                   /* SEP #$20 */
                L(at != line ? 2 * OP + IO : 2 * OP, at != line ? IO : OP);   /* BNE .poll */
            } while (at != line);
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x05) {
            uint8_t n;
            dispatch(2);
            L(3 * OP + IO + OP, OP);                  /* LDA $0000,X */
            n = rd8(x);
            I(2 * OP);                            /* STA $06 */
            wr8(0x000006, n);
            IMP();                           /* INX */
            x++;
            do {
                uint8_t f;
                do {                                /* .out: LDA $4212; BMI .out */
                    I(3 * OP);
                    f = rd8(0x004212);
                    L(f & 0x80 ? 2 * OP + IO : 2 * OP, f & 0x80 ? IO : OP);
                } while (f & 0x80);
                do {                                /* .in: LDA $4212; BPL .in */
                    I(3 * OP);
                    f = rd8(0x004212);
                    L(!(f & 0x80) ? 2 * OP + IO : 2 * OP, !(f & 0x80) ? IO : OP);
                } while (!(f & 0x80));
                I(2 * OP);                          /* DEC $06 */
                n = rd8_rmw(0x000006);
                cyc(IO);
                n--;
                wr8(0x000006, n);
                L(n ? 2 * OP + IO : 2 * OP, n ? IO : OP);      /* BNE .out */
            } while (n);
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x06) {
            uint16_t a;
            uint8_t bank, v;
            dispatch(3);
            L(2 * OP + IO, IO);
            L(3 * OP + IO + 2 * OP, OP);
            a = rd16(x);
            I(2 * OP);
            wr16(0x000008, a);
            L(2 * OP + IO, IO);
            L(3 * OP + IO + OP, OP);
            bank = rd8((uint16_t)(x + 2));
            I(2 * OP);
            wr8(0x00000A, bank);
            IMP(); IMP(); IMP();     /* INX x 3 */
            x = (uint16_t)(x + 3);
            I(2 * OP);                            /* LDA [$08] */
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            v = rd8((uint32_t)bank << 16 | a);
            I(OP + IO);                           /* PHX */
            wr8(0x0001FF, (uint8_t)(x >> 8));
            wr8(0x0001FE, (uint8_t)x);
            IMP();                           /* TYX */
            I(4 * OP);                            /* STA $7E2000,X */
            wr8(0x7E2000u + y, v);
            I(OP + IO + IO);                      /* PLX */
            rd8(0x0001FE);
            rd8(0x0001FF);
            IMP();                           /* INY */
            y++;
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x07) {
            uint16_t a;
            uint8_t bank, v;
            dispatch(4);
            L(2 * OP + IO, IO);
            L(3 * OP + IO + 2 * OP, OP);
            a = rd16(x);
            I(2 * OP);
            wr16(0x000008, a);
            L(2 * OP + IO, IO);
            L(3 * OP + IO + OP, OP);
            bank = rd8((uint16_t)(x + 2));
            I(2 * OP);
            wr8(0x00000A, bank);
            L(3 * OP + IO + OP, OP);
            v = rd8((uint16_t)(x + 3));
            I(2 * OP);                            /* STA [$08] */
            rd8(0x000008);
            rd8(0x000009);
            rd8(0x00000A);
            wr8((uint32_t)bank << 16 | a, v);
            IMP(); IMP(); IMP(); IMP();   /* INX x 4 */
            L(3 * OP, OP);                              /* JMP next */
            x = (uint16_t)(x + 4);
        } else if (op == 0x08) {
            dispatch(5);
            IMP();                           /* CLI */
            cpu.p &= (uint8_t)~P_I;
            L(3 * OP, OP);
        } else if (op == 0x09) {
            dispatch(6);
            cpu_wai(0x008000);                      /* WAI */
            L(3 * OP, OP);
        } else if (op == 0x0A) {
            uint16_t a;
            uint8_t v, got;
            dispatch(7);
            L(2 * OP + IO, IO);                       /* REP #$20 */
            L(3 * OP + IO + 2 * OP, OP);              /* LDA $0000,X */
            a = rd16(x);
            I(2 * OP);                            /* STA $00 */
            wr16(0x000000, a);
            L(2 * OP + IO, IO);                       /* SEP #$20 */
            L(3 * OP + IO + OP, OP);                  /* LDA $0002,X */
            v = rd8((uint16_t)(x + 2));
            I(2 * OP);                            /* STA $02 */
            wr8(0x000002, v);
            IMP(); IMP(); IMP();     /* INX x 3 */
            x = (uint16_t)(x + 3);
            do {
                I(2 * OP);                        /* LDA ($00) */
                rd8(0x000000);
                rd8(0x000001);
                got = rd8(a);
                I(2 * OP);                        /* CMP $02 */
                rd8(0x000002);
                L(got != v ? 2 * OP + IO : 2 * OP, got != v ? IO : OP);   /* BNE .p */
            } while (got != v);
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x0B) {
            uint8_t n, m, t;
            dispatch(8);
            L(3 * OP + IO + OP, OP);                  /* LDA $0000,X */
            n = rd8(x);
            I(2 * OP);                            /* STA $06 */
            wr8(0x000006, n);
            L(3 * OP + IO + OP, OP);                  /* LDA $0001,X */
            m = rd8((uint16_t)(x + 1));
            I(2 * OP);                            /* STA $07 */
            wr8(0x000007, m);
            IMP(); IMP();                 /* INX x 2 */
            x = (uint16_t)(x + 2);
            I(2 * OP);                            /* LDA $06 */
            t = rd8(0x000006);
            L(!t ? 2 * OP + IO : 2 * OP, !t ? IO : OP);    /* BEQ .b */
            while (t) {                         /* .a: DEC $06; BNE .a */
                I(2 * OP);
                t = rd8_rmw(0x000006);
                cyc(IO);
                t--;
                wr8(0x000006, t);
                L(t ? 2 * OP + IO : 2 * OP, t ? IO : OP);
            }
            I(2 * OP);                            /* LDA $07 */
            t = rd8(0x000007);
            L(!t ? 2 * OP + IO : 2 * OP, !t ? IO : OP);    /* BEQ .d */
            while (t) {                         /* .c: NOP; DEC $07; BNE .c */
                IMP();
                I(2 * OP);
                t = rd8_rmw(0x000007);
                cyc(IO);
                t--;
                wr8(0x000007, t);
                L(t ? 2 * OP + IO : 2 * OP, t ? IO : OP);
            }
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x0C) {
            uint8_t n, m;
            dispatch(9);
            L(3 * OP + IO + OP, OP);                  /* LDA $0000,X */
            n = rd8(x);
            I(2 * OP);                            /* STA $06 */
            wr8(0x000006, n);
            L(3 * OP + IO + OP, OP);                  /* LDA $0001,X */
            m = rd8((uint16_t)(x + 1));
            I(2 * OP);                            /* STA $07 */
            wr8(0x000007, m);
            IMP(); IMP();                 /* INX x 2 */
            x = (uint16_t)(x + 2);
            do {
                do {
                    cycs(0x86868686);               /* NOP x 4, a stretch of eight cycles */
                    cycs(0x88);                     /* DEC $06 */
                    n = rd8_rmw(0x000006);
                    cycs(0x6);
                    n--;
                    wr8(0x000006, n);
                    cycs(n ? 0x886 : 0x88);         /* BNE .l */
                } while (n);
                cycs(0x88);                         /* DEC $07 */
                m = rd8_rmw(0x000007);
                cycs(0x6);
                m--;
                wr8(0x000007, m);
                cycs(m ? 0x886 : 0x88);             /* BNE .l */
            } while (m);
            L(3 * OP, OP);                            /* JMP next */
        } else if (op == 0x0D) {
            uint8_t v;
            dispatch(10);
            L(3 * OP + IO + OP, OP);                  /* LDA $0000,X */
            v = rd8(x);
            I(3 * OP);                            /* STA $420B */
            wr8(0x00420B, v);
            cycs(0x86868686);                       /* NOP x 4: the DMA starts in it */
            IMP();                           /* INX */
            x++;
            L(3 * OP, OP);                            /* JMP next */
        } else if (op >= 0x0E && op <= 0x11) {
            /* the multiplier's and divider's steps as the reads see them:
               every cycle stated (cycs) */
            uint8_t v;
            dispatch(11 + (op - 0x0E));
            cycs(0x8886);                           /* LDA $0000,X: op, address, index */
            cycs(0x8);                              /* its read, from the table in ROM */
            v = rd8(x);
            cycs(0x888);                            /* STA $4203 / $4206 */
            wr8(op == 0x0F ? 0x004206 : 0x004203, v);
            if (op == 0x10)
                cycs(0x86);                         /* NOP */
            if (op == 0x11) {
                cycs(0x888);                        /* STA $4206 */
                wr8(0x004206, v);
            }
            cycs(0x888);                            /* JMP alu_reads */
            {
                static const uint16_t regs[4] = { 0x4214, 0x4215, 0x4216, 0x4217 };
                int k;
                for (k = 0; k < 4; k++) {
                    uint8_t r;
                    cycs(0x888);                    /* LDA $421x */
                    r = rd8(regs[k]);
                    cycs(0x88);                     /* STA $1x */
                    wr8((uint32_t)(0x10 + k), r);
                }
            }
            cycs(0x86);                             /* INX */
            x++;
            cycs(0x888);                            /* JMP next */
        } else {
            /* halt: the dispatch's fifteen tests, each CMP #nn and BNE + taken,
               then WAI; BRA halt */
            int k;
            for (k = 0; k < 15; k++) {
                L(2 * OP, OP);                  /* CMP #nn */
                L(2 * OP + IO, IO);             /* BNE +, taken */
            }
            for (;;) {
                cpu_wai(0x008000);
                L(2 * OP + IO, IO);
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
    I(8 + 6);                    /* PHA/PHX/PHY: the opcode, an internal cycle */
    wr8(cpu.s, 0);
    wr8((uint16_t)(cpu.s - 1), 0);
    cpu.s = (uint16_t)(cpu.s - 2);
}

static void pull16_t(void)
{
    I(8 + 6 + 6);                /* PLY/PLX/PLA: the opcode, two internal cycles */
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
        L(8 + 8 + 8 + 6 + 8, 8);    /* LDA $0000,X: the step's code from ROM */
        op = rd8(x);
        if (!op) {
            L(8 + 8 + 6, 6);        /* BEQ, taken */
            return x;
        }
        L(8 + 8, 8);                /* BEQ, not taken */
        L(8 + 8 + 6, 6);            /* REP #$20 */
        L(8 + 8 + 8 + 6 + 8 + 8, 8);/* LDA $0001,X */
        a = rd16((uint16_t)(x + 1));
        I(8 + 8);                   /* STA $0C */
        wr16(0x00000C, a);
        L(8 + 8 + 6, 6);            /* SEP #$20 */
        L(8 + 8 + 8 + 6 + 8, 8);    /* LDA $0003,X */
        v = rd8((uint16_t)(x + 3));
        I(8 + 8);                   /* STA ($0C) */
        rd8(0x00000C);
        rd8(0x00000D);
        wr8(a, v);
        IMP(); IMP(); IMP(); IMP(); /* INX x 4 */
        L(8 + 8 + 6, 6);            /* BRA .step */
        x = (uint16_t)(x + 4);
    }
}

/* the IRQ: its steps from where the last IRQ left off */
static void abi_irq(void)
{
    uint16_t x;
    L(8 + 8 + 6, 6);                /* REP #$30 */
    push16_t();
    push16_t();
    push16_t();
    L(8 + 8 + 6, 6);                /* SEP #$20 */
    I(8 + 8 + 8);                   /* LDA $4211 */
    rd8(0x004211);
    I(8 + 8);                       /* LDX $10 */
    x = handler_steps(rd16(0x000010));
    IMP();                          /* INX */
    x++;
    I(8 + 8);                       /* STX $10 */
    wr16(0x000010, x);
    L(8 + 8 + 6, 6);                /* REP #$30 */
    pull16_t();
    pull16_t();
    pull16_t();
    /* RTI: the console's */
}

/* the NMI: its steps from their start, and the IRQ's back to theirs */
static void abi_nmi(void)
{
    L(8 + 8 + 6, 6);                /* REP #$30 */
    push16_t();
    push16_t();
    push16_t();
    L(8 + 8 + 6, 6);                /* SEP #$20 */
    I(8 + 8 + 8);                   /* LDA $4210 */
    rd8(0x004210);
    L(8 + 8 + 8, 8);                /* LDX #$E000 */
    I(8 + 8);                       /* STX $10 */
    wr16(0x000010, 0xE000);
    L(8 + 8 + 8, 8);                /* LDX #$F000 */
    handler_steps(0xF000);
    L(8 + 8 + 6, 6);                /* REP #$30 */
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

/* tests/dsp/fuzz.py's SPC700 program (tests/dsp/fuzz_spc.c), where its
   cartridges start it */
void fuzz_spc_main(void);
static const nshvc_spc_routine spc_routines[] = {
    { 0x0200, fuzz_spc_main, "fuzz", NULL },
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
