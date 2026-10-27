/*
 * fuzz_spc.c: tests/dsp/fuzz.py's SPC700 program as C, as a cycle exact
 * decompilation's C is: every APU RAM and I/O access the assembly makes,
 * dummy reads included, in its order, and the cycles between them (spc.h:
 * spc_cyc, the cycles up to and including each access's own). The test
 * cartridge (tests/rom/testcart.c) runs it where the S-CPU starts the
 * uploaded program, $0200. The seed is read from the program's own bytes
 * as uploaded (its two MOV dp,#imm at $0204 and $0207).
 */
#include <stdint.h>

#include "nshvc/spc.h"

/* the APU RAM as it stands, read without an access (the seed is in the
   program's own code: fetched, not read as data) */
const uint8_t *snes_aram(void);

static uint8_t a, x, y;
static int c;

/* the instruction forms the program uses, each with its cycles */
static void op2(void) { spc_cyc(2); }                     /* an implied or immediate form */
static void branch(int taken) { spc_cyc(taken ? 4 : 2); }

static void mov_dp_imm(uint8_t d, uint8_t v)               /* MOV dp,#imm: 5 */
{
    spc_cyc(4);
    spc_rd(d);
    spc_cyc(1);
    spc_wr(d, v);
}

static void mov_dp_a(uint8_t d)                            /* MOV dp,A: 4 */
{
    spc_cyc(3);
    spc_rd(d);
    spc_cyc(1);
    spc_wr(d, a);
}

static uint8_t rd_dp(uint8_t d)                            /* MOV A,dp; ADC A,dp: 3 */
{
    spc_cyc(3);
    return spc_rd(d);
}

static uint16_t pointer(uint8_t d)                         /* [dp]+Y's address */
{
    uint16_t lo, hi;
    spc_cyc(3);
    lo = spc_rd(d);
    spc_cyc(1);
    hi = spc_rd((uint8_t)(d + 1));
    return (uint16_t)((lo | hi << 8) + y);
}

static void mov_ind_y_a(uint8_t d)                         /* MOV [dp]+Y,A: 7 */
{
    uint16_t at = pointer(d);
    spc_cyc(2);
    spc_rd(at);
    spc_cyc(1);
    spc_wr(at, a);
}

static void mov_a_ind_y(uint8_t d)                         /* MOV A,[dp]+Y: 6 */
{
    uint16_t at = pointer(d);
    spc_cyc(2);
    a = spc_rd(at);
}

static void rand8(void)
{
    x = 8;                                                 /* MOV X,#8 */
    op2();
    do {
        uint8_t v;
        spc_cyc(3);                                        /* LSR $01 */
        v = spc_rd(0x01);
        c = v & 1;
        spc_cyc(1);
        spc_wr(0x01, (uint8_t)(v >> 1));
        spc_cyc(3);                                        /* ROR $00 */
        v = spc_rd(0x00);
        {
            int out = v & 1;
            spc_cyc(1);
            spc_wr(0x00, (uint8_t)(v >> 1 | c << 7));
            c = out;
        }
        branch(!c);                                        /* BCC */
        if (c) {
            a = rd_dp(0x01);                               /* MOV A,$01 */
            a ^= 0xB4;                                     /* EOR A,#$B4 */
            op2();
            mov_dp_a(0x01);                                /* MOV $01,A */
        }
        x--;                                               /* DEC X */
        op2();
        branch(x != 0);                                    /* BNE */
    } while (x);
    a = rd_dp(0x00);                                       /* MOV A,$00 */
}

/* MOV $F2,#reg; MOV $F3,#v */
static void dsp_imm(uint8_t reg, uint8_t v)
{
    mov_dp_imm(0xF2, reg);
    mov_dp_imm(0xF3, v);
}

void fuzz_spc_main(void)
{
    uint8_t seed_lo = snes_aram()[0x0204], seed_hi = snes_aram()[0x0207];
    x = 0xEF;                                              /* MOV X,#$EF */
    op2();
    spc.sp = x;                                            /* MOV SP,X */
    op2();
    mov_dp_imm(0x00, seed_lo);
    mov_dp_imm(0x01, seed_hi);
    mov_dp_imm(0x02, 0x00);
    mov_dp_imm(0x03, 0x10);
    y = 0;                                                 /* MOV Y,#0 */
    op2();
    for (;;) {                                             /* fill */
        rand8();
        mov_ind_y_a(0x02);
        y++;                                               /* INC Y */
        op2();
        branch(y != 0);                                    /* BNE fill */
        if (y)
            continue;
        {
            uint8_t v;
            spc_cyc(3);                                    /* INC $03 */
            v = spc_rd(0x03);
            spc_cyc(1);
            spc_wr(0x03, (uint8_t)(v + 1));
        }
        a = rd_dp(0x03);                                   /* MOV A,$03 */
        op2();                                             /* CMP A,#$80 */
        branch(a != 0x80);                                 /* BNE fill */
        if (a == 0x80)
            break;
    }
    mov_dp_imm(0x06, 0x80);
    dsp_imm(0x6D, 0x80);
    dsp_imm(0x7D, 0x02);
    x = 0x20;                                              /* MOV X,#$20 */
    op2();
    do {                                                   /* settle */
        x--;
        op2();
        branch(x != 0);
    } while (x);
    dsp_imm(0x6C, 0x00);
    dsp_imm(0x5D, 0x20);
    dsp_imm(0x0C, 0x50);
    dsp_imm(0x1C, 0x50);
    dsp_imm(0x2C, 0x30);
    dsp_imm(0x3C, 0xD0);
    dsp_imm(0x4C, 0xFF);
    mov_dp_imm(0xFA, 0x10);
    mov_dp_imm(0xFB, 0xC0);
    mov_dp_imm(0xFC, 0x07);
    mov_dp_imm(0xF1, 0x07);
    for (;;) {                                             /* main */
        uint8_t reg;
        rand8();
        a &= 0x7F;                                         /* AND A,#$7F */
        op2();
        mov_dp_a(0x04);
        rand8();
        mov_dp_a(0x05);
        reg = a = rd_dp(0x04);                             /* MOV A,$04 */
        op2();                                             /* CMP A,#$6D */
        branch(reg != 0x6D);
        if (reg == 0x6D) {
            a = rd_dp(0x05);
            a = (uint8_t)((a & 0x3F) | 0x80);              /* AND, OR */
            op2();
            op2();
            mov_dp_a(0x05);
            mov_dp_a(0x06);
            spc_cyc(4);                                    /* BRA keep_end */
        } else {
            op2();                                         /* CMP A,#$7D */
            branch(reg != 0x7D);
            if (reg == 0x7D) {
                a = rd_dp(0x05);
                a &= 0x07;
                op2();
                mov_dp_a(0x05);
                spc_cyc(4);
            } else {
                op2();                                     /* CMP A,#$5D */
                branch(reg != 0x5D);
                if (reg == 0x5D) {
                    a = rd_dp(0x05);
                    a = (uint8_t)((a & 0x3F) | 0x10);
                    op2();
                    op2();
                    mov_dp_a(0x05);
                    spc_cyc(4);
                } else {
                    op2();                                 /* CMP A,#$6C */
                    branch(reg != 0x6C);
                    if (reg == 0x6C) {
                        rand8();
                        a &= 0x0F;                         /* AND A,#$0F */
                        op2();
                        branch(a == 0);                    /* BEQ keep_end */
                        if (a) {
                            a = rd_dp(0x05);
                            a &= 0x3F;
                            op2();
                            mov_dp_a(0x05);
                        }
                    }
                }
            }
        }
        a = rd_dp(0x04);                                   /* keep_end */
        mov_dp_a(0xF2);
        a = rd_dp(0x05);
        mov_dp_a(0xF3);
        rand8();                                           /* a register read back */
        a &= 0x7F;
        op2();
        mov_dp_a(0xF2);
        a = rd_dp(0xF3);
        rand8();                                           /* a byte of the echo's line */
        mov_dp_a(0xEC);
        rand8();
        a &= 0x07;
        op2();
        op2();                                             /* CLRC */
        a = (uint8_t)(a + rd_dp(0x06));                    /* ADC A,$06 */
        mov_dp_a(0xED);
        y = 0;
        op2();
        mov_a_ind_y(0xEC);
        a = rd_dp(0xFD);                                   /* the timers */
        a = rd_dp(0xFE);
        a = rd_dp(0xFF);
        rand8();                                           /* the ports */
        mov_dp_a(0xF4);
        rand8();
        mov_dp_a(0xF7);
        a = rd_dp(0xF4);
        a = rd_dp(0xF5);
        a = rd_dp(0xF6);
        a = rd_dp(0xF7);
        rand8();                                           /* one pass in 32: the timers */
        a &= 0x1F;
        op2();
        branch(a != 0);
        if (!a) {
            rand8();
            mov_dp_a(0xFA);
            rand8();
            mov_dp_a(0xFB);
            rand8();
            mov_dp_a(0xFC);
            rand8();
            a &= 0x07;
            op2();
            mov_dp_a(0xF1);
        }
        rand8();                                           /* a pause */
        a &= 0x1F;
        op2();
        x = a;                                             /* MOV X,A */
        op2();
        x++;                                               /* INC X */
        op2();
        do {
            x--;
            op2();
            branch(x != 0);
        } while (x);
        spc_cyc(3);                                        /* JMP main */
    }
}
