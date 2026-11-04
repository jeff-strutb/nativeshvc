/*
 * pause_check.c: the game's pause (nshvc_ext_pause), headless: a
 * cartridge whose NMI counts frames in work RAM, and a game's frame hook
 * that pauses at display frame 10 and goes on at frame 30. While paused
 * the machine must not run: the count, nshvc_frames and the sound queued
 * all held; then the count goes on a frame a frame from where it stopped,
 * and at the end the machine has made 20 frames fewer than were shown.
 *
 *   pause_check --headless --frames 60      exit 0 when every check holds
 */
#include <stdio.h>
#include <stdlib.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static void abi_nmi(void)
{
    uint8_t n;
    rd8(0x004210);
    n = rd8(0x000100);
    wr8(0x000100, (uint8_t)(n + 1));
}

static void abi_reset(void)
{
    cpu.e = 0;
    wr8(0x004200, 0x80);             /* the NMI */
    cpu_loop_forever(0x008010);
}

static int shown, fails;
static uint8_t count_at_pause;
static uint32_t frames_at_pause;
static int queued_at_pause;
static uint8_t last;

static void check(int ok, const char *what)
{
    if (!ok || shown == 59)
        printf("%s frame %d: %s\n", ok ? "ok  " : "FAIL", shown, what);
    fails += !ok;
}

static void on_frame(void)
{
    uint8_t n = nshvc_peek(0x7E0100);
    shown++;
    if (shown == 10) {
        nshvc_ext_pause(1, 12, 0x80000000u);
        count_at_pause = n;
        frames_at_pause = nshvc_frames();
        queued_at_pause = nshvc_audio_queued();
        check(nshvc_ext_paused(), "paused");
    } else if (shown > 10 && shown <= 30) {
        check(n == count_at_pause, "the count held while paused");
        check(nshvc_frames() == frames_at_pause, "nshvc_frames held while paused");
        check(nshvc_audio_queued() == queued_at_pause, "no sound made while paused");
        if (shown == 30) {
            nshvc_ext_pause(0, 0, 0);
            check(!nshvc_ext_paused(), "going on");
        }
    } else if (shown > 31) {
        check(n == (uint8_t)(last + 1), "a frame a frame after the pause");
    }
    if (shown == 59) {
        check(nshvc_frames() == 59 - 20, "the machine made 20 frames fewer than were shown");
        printf("%d check(s) failed\n", fails);
        exit(fails ? 1 : 0);
    }
    last = n;
}

static uint8_t rom[0x8000];
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x008020, abi_nmi, "nmi", 0 },
};

NSHVC_MAIN(argc, argv)
{
    nshvc_cart cart = { 0 };
    static const nshvc_hooks hooks = { .frame = on_frame };
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x20;
    rom[0x7FEB] = 0x80;
    rom[0x7FD5] = 0x20;
    cart.title = "pause";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
