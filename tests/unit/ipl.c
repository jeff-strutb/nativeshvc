/*
 * ipl.c: the sound unit's boot ROM taking an upload as games send one:
 * each byte and its index written together, a word to $2140 (the index
 * to $2140 first, the byte to $2141 after it), the S-CPU waiting for the
 * echo. Every byte must land in the APU RAM where it was sent.
 *
 *   ipl          runs the checks; exit 0 when every one holds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "snes.h"

#define DEST 0x0800
#define LEN 1024

static uint8_t sent[LEN];

static int port0_is(void *ctx) { return rd8(0x002140) == *(uint8_t *)ctx; }

static void abi_reset(void)
{
    uint8_t want;
    int i, bad = 0, first = -1;
    cpu.e = 0;
    want = 0xAA;
    nshvc_wait_until(port0_is, &want);
    /* the block's address, a byte not 0 (a block, not the jump), $CC */
    wr16(0x002142, DEST);
    wr8(0x002141, 0x01);
    wr8(0x002140, 0xCC);
    want = 0xCC;
    nshvc_wait_until(port0_is, &want);
    for (i = 0; i < LEN; i++) {
        /* the index and the byte in one word: $2140 first */
        wr16(0x002140, (uint16_t)((i & 0xFF) | sent[i] << 8));
        want = (uint8_t)i;
        nshvc_wait_until(port0_is, &want);
    }
    /* the boot ROM echoes the index before it stores the byte (as the
       console's does): the last store a few of its cycles on */
    nshvc_wait_vblank();
    for (i = 0; i < LEN; i++)
        if (snes_aram()[DEST + i] != sent[i] && !bad++)
            first = i;
    printf("%s %d of %d bytes land where they were sent", bad ? "FAIL" : "ok  ", LEN - bad, LEN);
    if (bad)
        printf(" (first wrong at %d: %02X, sent %02X)", first, snes_aram()[DEST + first], sent[first]);
    printf("\n%d check(s) failed\n", bad != 0);
    exit(bad ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

int main(void)
{
    nshvc_cart cart;
    int i;
    for (i = 0; i < LEN; i++)
        sent[i] = (uint8_t)(i * 151 + 7);
    memset(&cart, 0, sizeof cart);
    cart.title = "ipl";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
