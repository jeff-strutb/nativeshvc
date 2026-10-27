/*
 * gsu_ext.c: nshvc_ext_gsu_screen and Super FX RAM past 128 KB, checked on
 * a cartridge of a few lines of C: a Super FX program plots across a
 * 512 x 224 screen at 4 bits a pixel (gsu_plot_at), and every pixel's
 * bits are found where the layout puts them, through the S-CPU's bus
 * (banks $70-$73); the GSU's own screen, unextended, as before.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/superfx.h"
#include "nshvc/machine.h"
#include "nshvc/gsu.h"

enum { W = 512, H = 224 };
static int fails;
static const struct { uint16_t x, y; uint8_t c; } pts[] = {
    { 0, 0, 0x0B }, { 7, 7, 0x05 }, { 300, 100, 0x0F }, { 511, 223, 0x09 }, { 256, 8, 0x03 }, { 400, 200, 0x0C },
};

static int big;

static void abi_program(void)
{
    unsigned i;
    gsu.por = 0;
    if (big) {
        /* 8 bits a pixel on 1024 x 512: the last pixel 512 KB in */
        gsu.colr = 0xA5;
        gsu_plot_at(1023, 511);
        gsu_rpix_at(0, 0);
        return;
    }
    for (i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        gsu.colr = pts[i].c;
        gsu_plot_at(pts[i].x, pts[i].y);
    }
    gsu_rpix_at(0, 0);       /* the pixel caches flushed */
}

/* where the layout puts pixel (x, y)'s bit plane p (4 bits a pixel) */
static uint32_t plane_addr(unsigned x, unsigned y, unsigned p)
{
    unsigned cn = (x >> 3) * (H >> 3) + (y >> 3);
    return 0x700000u + cn * 32 + (y & 7) * 2 + (p >> 1) * 16 + (p & 1);
}

static void abi_reset(void)
{
    unsigned i, p;
    nshvc_ext_gsu_screen(W, H);
    wr8(0x00303A, 0x18 | 0x01);    /* SCMR: the GSU holds ROM and RAM, 4 bits a pixel */
    wr8(0x003038, 0x00);           /* SCBR */
    wr8(0x003034, 0x01);           /* PBR */
    wr8(0x00301E, 0x00);
    wr8(0x00301F, 0x80);           /* R15: the program at $01:8000 starts */
    while (rd8(0x003030) & 0x20)
        cyc(16);
    wr8(0x00303A, 0x01);           /* the S-CPU holds the RAM again */
    for (i = 0; i < sizeof pts / sizeof pts[0]; i++)
        for (p = 0; p < 4; p++) {
            uint32_t a = plane_addr(pts[i].x, pts[i].y, p);
            int bit = rd8(a) >> (7 - (pts[i].x & 7)) & 1;
            if (bit != (pts[i].c >> p & 1)) {
                printf("FAIL (%u, %u) plane %u at $%06X: %d\n", pts[i].x, pts[i].y, p, (unsigned)a, bit);
                fails++;
            }
        }
    /* past bank $71: 1024 x 512 at 8 bits a pixel, pixel (1023, 511) */
    big = 1;
    nshvc_ext_gsu_screen(1024, 512);
    wr8(0x00303A, 0x18 | 0x03);
    wr8(0x00301E, 0x00);
    wr8(0x00301F, 0x80);
    while (rd8(0x003030) & 0x20)
        cyc(16);
    wr8(0x00303A, 0x03);
    {
        unsigned cn = (1023 >> 3) * (512 >> 3) + (511 >> 3), ok = 1;
        uint32_t a = 0x700000u + cn * 64 + (511 & 7) * 2;
        for (p = 0; p < 8; p++)
            if ((rd8(a + (p >> 1) * 16 + (p & 1)) >> 0 & 1) != (0xA5u >> p & 1))
                ok = 0;
        printf("%s (1023, 511) at 8 bits a pixel lands at $%06X (bank $%02X)\n", ok ? "ok  " : "FAIL", (unsigned)a,
               (unsigned)(a >> 16));
        if (!ok || (a >> 16) != 0x77)
            fails++;
    }
    /* a program run with interrupts held leaves the Super FX's interrupt as
       the hold found it: SFR's IRQ bit (bit 15, cleared by the read of
       $3031) set by the STOP without a hold, clear after one */
    {
        int i2;
        for (i2 = 0; i2 < 2; i2++) {
            int irq;
            (void)rd8(0x003031);
            if (i2)
                nshvc_hold_interrupts(1);
            wr8(0x00303A, 0x18 | 0x03);
            wr8(0x00301E, 0x00);
            wr8(0x00301F, 0x80);
            while (rd8(0x003030) & 0x20)
                cyc(16);
            wr8(0x00303A, 0x03);
            if (i2)
                nshvc_hold_interrupts(0);
            irq = rd8(0x003031) >> 7 & 1;
            printf("%s %s: SFR's IRQ bit %d\n", irq == !i2 ? "ok  " : "FAIL",
                   i2 ? "a program run held leaves no interrupt" : "a program's STOP raises the interrupt", irq);
            if (irq != !i2)
                fails++;
        }
    }
    /* a byte written by the S-CPU at $7D:FFFF lands in the last of 14 banks */
    wr8(0x7DFFFF, 0x5A);
    if (nshvc_sram()[0xDFFFF] != 0x5A) {
        printf("FAIL $7D:FFFF\n");
        fails++;
    }
    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static uint8_t rom[0x20000];
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x018000, abi_program, "program", 0 },
};

int main(void)
{
    nshvc_cart cart;
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    memset(&cart, 0, sizeof cart);
    cart.title = "gsu_ext";
    cart.map = NSHVC_SUPERFX;
    cart.chip = &nshvc_superfx;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.sram_size = 0xE0000;
    cart.routines = routines;
    cart.routine_count = 2;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
