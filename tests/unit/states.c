/*
 * states.c: a cartridge for the script's states and calls (tests/unit/
 * states.py): each NMI counts frames in work RAM and the buttons held, and
 * a routine a script calls adds to a second count, so a dump shows which
 * machine a step ran on.
 *
 *   states --headless --script SCRIPT --out DIR
 */
#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* the NMI: $0100 counts frames, $0102 adds the joypad's high byte */
static void abi_nmi(void)
{
    uint8_t n, b;
    rd8(0x004210);
    n = rd8(0x000100);
    wr8(0x000100, (uint8_t)(n + 1));
    b = rd8(0x004219);
    n = rd8(0x000102);
    wr8(0x000102, (uint8_t)(n + b));
}

/* $00:9000, called by a script: $0101 += $10 */
static void abi_bump(void)
{
    uint8_t n = rd8(0x000101);
    cyc(12);
    wr8(0x000101, (uint8_t)(n + 0x10));
    cyc(36);                         /* its RTS but for the pulls */
}

static void abi_reset(void)
{
    cpu.e = 0;
    wr8(0x004200, 0x81);             /* the NMI and the joypads' read */
    /* busy, so a script's call starts in the frame, not only at its NMI */
    for (;;) {
        rd8(0x000104);
        cyc(16);
    }
}

static uint8_t rom[0x8000];

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x008020, abi_nmi, "nmi", 0 },
    { 0x009000, abi_bump, "bump", 0 },
};

NSHVC_MAIN(argc, argv)
{
    nshvc_cart cart = { 0 };
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x20;
    rom[0x7FEB] = 0x80;
    rom[0x7FD5] = 0x20;
    cart.title = "states";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 3;
    return nshvc_main(&cart, argc, argv);
}
