/*
 * keys_spc.c: tests/dsp/keys.py's SPC700 driver as C, as a decompilation's
 * C is under contract 2.0 (spc.h): every APU RAM and I/O access the
 * assembly makes, in its order, its poll of timer 0 a wait, its delay
 * loops (no access) no time. The test cartridge (tests/rom/testcart.c)
 * runs it where the S-CPU starts the uploaded program, $0200. Its tables
 * are read from the program as uploaded (keys.py: PATTERNS entries at
 * $0680, BITS at $06F8).
 */
#include <stddef.h>
#include <stdint.h>

#include "nshvc/spc.h"

#define PATTERNS 0x0680
#define BITS 0x06F8

/* MOV dp,#imm and MOV dp,A: the dummy read, then the write */
static void mov_dp(uint8_t d, uint8_t v)
{
    spc_rd(d);
    spc_wr(d, v);
}

static void dsp(uint8_t reg, uint8_t v)                    /* mov $F2,#reg; mov $F3,#v */
{
    mov_dp(0xF2, reg);
    mov_dp(0xF3, v);
}

static int ticked(void *ctx)                               /* mov a,$FD; beq wait */
{
    (void)ctx;
    return spc_rd(0xFD) != 0;
}

/* a voice set up and keyed on: its SRCN, its pitch, KON; its number and
   values from the pattern's table (`field`: the first voice's, $00, or
   the second's, $30) */
static void key_on(uint8_t x, uint8_t field)
{
    uint8_t v = spc_rd((uint16_t)(PATTERNS + field + x)), base = (uint8_t)(v << 4);
    mov_dp(0x11, base);
    mov_dp(0xF2, (uint8_t)(base | 4));
    mov_dp(0xF3, spc_rd((uint16_t)(PATTERNS + field + 0x10 + x)));
    mov_dp(0xF2, (uint8_t)(spc_rd(0x11) | 2));
    mov_dp(0xF3, spc_rd((uint16_t)(PATTERNS + field + 0x20 + x)));
    mov_dp(0xF2, (uint8_t)(spc_rd(0x11) | 3));
    mov_dp(0xF3, 0x10);
    mov_dp(0xF2, 0x4C);
    mov_dp(0xF3, spc_rd((uint16_t)(BITS + v)));
}

void keys_spc_main(void)
{
    int v;
    spc.sp = 0xEF;
    dsp(0x6C, 0x20);
    dsp(0x0C, 0x7F);
    dsp(0x1C, 0x7F);
    dsp(0x2C, 0x00);
    dsp(0x3C, 0x00);
    dsp(0x4D, 0x00);
    dsp(0x3D, 0x00);
    dsp(0x2D, 0x00);
    dsp(0x5D, 0x06);
    for (v = 0; v < 8; v++) {
        dsp((uint8_t)(v << 4 | 0), 0x40);
        dsp((uint8_t)(v << 4 | 1), 0x40);
        dsp((uint8_t)(v << 4 | 5), 0xFF);
        dsp((uint8_t)(v << 4 | 6), 0xF0);
    }
    mov_dp(0xFA, 0x50);
    mov_dp(0xF1, 0x01);
    mov_dp(0x10, 0x00);
    for (;;) {
        uint8_t x;
        spc_wait_until(ticked, NULL);
        spc_wr(0x10, (uint8_t)(spc_rd(0x10) + 1));        /* inc $10 */
        x = (uint8_t)(spc_rd(0x10) & 0x0F);                /* mov a,$10; and a,#$0F; mov x,a */
        /* KOF: its mask, then (after a pause the original spends) 0 */
        mov_dp(0xF2, 0x5C);
        mov_dp(0xF3, spc_rd((uint16_t)(PATTERNS + 0x60 + x)));
        mov_dp(0xF3, 0x00);
        key_on(x, 0x00);
        key_on(x, 0x30);
    }
}
