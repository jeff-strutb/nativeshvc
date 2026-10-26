/*
 * hello.c: the smallest cartridge for nativeshvc, as a decompilation would
 * describe one: its code as C against the machine model (machine.h), its
 * routines by address, its ROM, and nshvc_main. It shows a colour, and B
 * held on controller 1 changes it: everything through the console's
 * registers, as a game's code reaches them.
 *
 *   hello                    a window
 *   hello --headless --frames 60 --script SCRIPT --out DIR
 */
#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "nshvc/hw.h"

/* The NMI: each frame, the backdrop's colour from the buttons the joypads'
   automatic read took (B held: red, else blue). */
static void abi_nmi(void)
{
    uint8_t hi;
    rd8(0x004210);                   /* the NMI flag read, as a handler does */
    hi = rd8(JOY1L + 1);             /* $4219: B Y SELECT START up down left right */
    wr8(CGADD, 0);
    if (hi & 0x80) {                 /* B */
        wr8(CGDATA, 0x1F);
        wr8(CGDATA, 0x00);
    } else {
        wr8(CGDATA, 0x00);
        wr8(CGDATA, 0x7C);
    }
}

/* The reset: the screen on, the NMI and the joypads' automatic read on,
   then the CPU waits for interrupts for ever. */
static void abi_reset(void)
{
    cpu.e = 0;
    wr8(INIDISP, 0x80);
    wr8(CGADD, 0);
    wr8(CGDATA, 0x00);
    wr8(CGDATA, 0x7C);
    wr8(TM, 0x00);
    wr8(INIDISP, 0x0F);
    wr8(0x004200, 0x81);             /* NMITIMEN: the NMI, the joypads' read */
    for (;;)
        cpu_wai(0x008010);
}

/* The ROM: only its vectors (the code is the C). Native NMI at $FFEA,
   reset at $FFFC. */
static uint8_t rom[0x8000];
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "Reset", 0 },
    { 0x008020, abi_nmi, "Nmi", 0 },
};

NSHVC_MAIN(argc, argv)
{
    static nshvc_cart cart;
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x20;
    rom[0x7FEB] = 0x80;
    cart.title = "Hello";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    return nshvc_main(&cart, argc, argv);
}
