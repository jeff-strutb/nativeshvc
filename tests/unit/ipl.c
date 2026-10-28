/*
 * ipl.c: the sound unit's boot ROM taking an upload as games send one:
 * each byte and its index written together, a word to $2140 (the index
 * to $2140 first, the byte to $2141 after it), the S-CPU waiting for the
 * echo. Every byte must land in the APU RAM where it was sent.
 *
 * And the same upload taken by a driver's own C (the boot ROM's protocol
 * written in the game's SPC700 C, as a driver that reloads does): its C
 * takes no time, and polls with spc_wait_until.
 *
 *   ipl          runs the checks; exit 0 when every one holds
 *   ipl --driver the driver's C takes the second upload
 *   ipl --driver --profile   and the S-CPU with a cost profile (a weight
 *                for each of its accesses: a word's two bytes still go
 *                out in consecutive cycles)
 *   --cpu-profile        the S-CPU's weights alone (none for the SPC700):
 *                each word's bytes still one bus cycle apart (6 clocks),
 *                or 46 where the refresh falls between them; the driver's
 *                C, untimed, then reads some of those words' bytes before
 *                they land, and only those (what the SPC700's weights fix)
 *   --sweep      a little more of the S-CPU's work before each word, a
 *                different amount each time, 4,096 bytes: some words
 *                straddle the work RAM's refresh (their bytes 46 clocks
 *                apart, as on the console). The driver takes them right
 *                with the profile's weights for its own work (--profile),
 *                as the console's driver does with its instructions' time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "nshvc/spc.h"
#include "snes.h"

#define DEST 0x0800
#define LEN 4096

static uint8_t sent[LEN];
static int driver_mode, sweep, cpu_only;

/* each word's two bytes: the clocks from the index's write to the byte's */
static long gap[LEN];
static int word = -1;
static uint64_t index_at;
static void on_write(uint32_t addr, uint8_t value)
{
    (void)value;
    if ((addr & 0xFFFF) == 0x2140)
        index_at = snes_clock;
    else if ((addr & 0xFFFF) == 0x2141 && word >= 0)
        gap[word] = (long)(snes_clock - index_at);
}

/* ---- the driver: the boot ROM's protocol in C, untimed ---- */
static int f4_is(void *ctx) { return spc_rd(0xF4) == *(uint8_t *)ctx; }

static void driver_main(void)
{
    uint8_t want, y;
    uint16_t dest;
    spc_wr(0xF4, 0xAA);
    spc_wr(0xF5, 0xBB);
    want = 0xCC;
    spc_wait_until(f4_is, &want);
    dest = (uint16_t)(spc_rd(0xF6) | spc_rd(0xF7) << 8);
    spc_wr(0xF4, 0xCC);
    for (y = 0;; y++) {
        want = y;
        spc_wait_until(f4_is, &want);       /* the index */
        spc_wr((uint16_t)(dest + y), spc_rd(0xF5));
        spc_wr(0xF4, y);                    /* echoed */
        if (dest + y + 1 >= DEST + LEN)
            break;
        if (y == 0xFF)
            dest = (uint16_t)(dest + 0x100);
    }
    for (;;) {
        static uint8_t never = 0x5A;
        spc_wait_until(f4_is, &never);
    }
}

static int port0_is(void *ctx) { return rd8(0x002140) == *(uint8_t *)ctx; }

/* an upload by the boot ROM's protocol: `data`, `len` bytes to `dest`, the
   index and byte a word to $2140; then, `jump`, the jump to dest */
static void upload(uint16_t dest, const uint8_t *data, int len, int jump)
{
    uint8_t want;
    int i;
    wr16(0x002142, dest);
    wr8(0x002141, 0x01);
    wr8(0x002140, 0xCC);
    want = 0xCC;
    nshvc_wait_until(port0_is, &want);
    for (i = 0; i < len; i++) {
        int k;
        for (k = 0; sweep && k < (i * 7) % 23; k++)
            rd8(0x7E0000);
        word = len == LEN ? i : -1;
        wr16(0x002140, (uint16_t)((i & 0xFF) | data[i] << 8));
        word = -1;
        want = (uint8_t)i;
        nshvc_wait_until(port0_is, &want);
    }
    if (jump) {
        wr16(0x002142, dest);
        wr8(0x002141, 0x00);
        wr8(0x002140, (uint8_t)(len + 2));
    }
}

static void abi_reset(void)
{
    uint8_t want;
    int i, bad = 0, first = -1, apart = 0, split = 0, bad_whole = 0;
    cpu.e = 0;
    want = 0xAA;
    nshvc_wait_until(port0_is, &want);
    if (driver_mode) {
        /* the driver up and started by the boot ROM, then the upload to it */
        static const uint8_t stub[1] = { 0x00 };
        upload(0x0200, stub, 1, 1);
        want = 0xAA;
        nshvc_wait_until(port0_is, &want);
        upload(DEST, sent, LEN, 0);
        goto check;
    }
    /* the block's address, a byte not 0 (a block, not the jump), $CC */
    wr16(0x002142, DEST);
    wr8(0x002141, 0x01);
    wr8(0x002140, 0xCC);
    want = 0xCC;
    nshvc_wait_until(port0_is, &want);
    for (i = 0; i < LEN; i++) {
        /* the index and the byte in one word: $2140 first */
        word = i;
        wr16(0x002140, (uint16_t)((i & 0xFF) | sent[i] << 8));
        word = -1;
        want = (uint8_t)i;
        nshvc_wait_until(port0_is, &want);
    }
    /* the boot ROM echoes the index before it stores the byte (as the
       console's does): the last store a few of its cycles on */
check:
    nshvc_wait_vblank();
    for (i = 0; i < LEN; i++) {
        int wrong = snes_aram()[DEST + i] != sent[i];
        if (wrong && !bad++)
            first = i;
        apart += gap[i] != 6 && gap[i] != 46;
        split += gap[i] == 46;
        bad_whole += wrong && gap[i] != 46;
    }
    printf("%s each word's bytes 6 clocks apart, or 46 across the refresh (%d split; %d otherwise)\n",
           apart ? "FAIL" : "ok  ", split, apart);
    if (cpu_only) {
        printf("%s the driver's C with no weights of its own: only split words' bytes land wrong "
               "(%d of %d split; %d not split)\n", bad_whole ? "FAIL" : "ok  ", bad - bad_whole, split, bad_whole);
        printf("%d check(s) failed\n", (apart != 0) + (bad_whole != 0));
        exit(apart || bad_whole ? 1 : 0);
    }
    printf("%s %d of %d bytes land where they were sent", bad ? "FAIL" : "ok  ", LEN - bad, LEN);
    if (bad)
        printf(" (first wrong at %d: %02X, sent %02X)", first, snes_aram()[DEST + first], sent[first]);
    printf("\n%d check(s) failed\n", (bad != 0) + (apart != 0));
    exit(bad || apart ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

static const nshvc_spc_routine spc_routines[] = { { 0x0200, driver_main, "driver", NULL } };

/* a real game's weights: Stunt Race FX's, as tools/nshvc-profile measures
   them on its attract mode. The S-CPU's in tenths of a clock (a read of
   I/O 72.5, more than three SPC700 cycles); the SPC700's in tenths of its
   cycle */
static const nshvc_cost_profile profile = {
    NSHVC_COST_PROFILE_VERSION, { { 224, 946, 297, 725 }, { 127, 0, 485, 113 } }, 0, NULL, 0, { 0, 0, NULL },
    { { 49, 39 }, { 14, 1 } },
};
/* the S-CPU's weights alone */
static const nshvc_cost_profile cpu_profile = {
    NSHVC_COST_PROFILE_VERSION, { { 224, 946, 297, 725 }, { 127, 0, 485, 113 } }, 0, NULL, 0, { 0, 0, NULL },
    { { 0, 0 }, { 0, 0 } },
};

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int i;
    driver_mode = argc > 1 && !strcmp(argv[1], "--driver");
    for (i = 0; i < LEN; i++)
        sent[i] = (uint8_t)(i * 151 + 7);
    memset(&cart, 0, sizeof cart);
    cart.title = "ipl";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--profile"))
            cart.cost_profile = &profile;
        if (!strcmp(argv[i], "--cpu-profile")) {
            cart.cost_profile = &cpu_profile;
            cpu_only = 1;
        }
        if (!strcmp(argv[i], "--sweep"))
            sweep = 1;
    }
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    cart.spc_routines = spc_routines;
    cart.spc_routine_count = 1;
    nshvc_set_write_hook(on_write);
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
