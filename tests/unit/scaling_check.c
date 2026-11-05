/*
 * scaling_check.c: nothing in the console runs unless something observes it,
 * so a frame's cost is what the game does, not what the console's time holds.
 * With the costs off and the sound silent, a frame of a cartridge doing nothing
 * but wait: its cost with the display on, with the NMI and the joypads' read,
 * and with the IRQ (masked or not), against the same with all of them off, at
 * most 25% over; and one frame alone against one among a hundred. An HDMA byte
 * each line is the game's own work: printed, not judged.
 *
 * What it costs is the host's time: the best of 5 batches of 200 frames. Run
 * with nothing else running; in PPSSPP it is an instruction count, not a
 * PSP's time. The sound unit's timers are not here: they need a driver.
 *
 *   scaling_check              exit 0 when every one holds
 *   scaling_check SCENE N      N frames of the scene alone, for a profiler
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "host.h"

#define OVER_MOST 1.25
#define BATCHES 5
#define BATCH_FRAMES 200

enum { S_IDLE, S_DISPLAY, S_NMI, S_IRQ_MASKED, S_IRQ, S_HDMA, NSCENES };
static const char *const scene_name[NSCENES] = { "idle", "display", "nmi+joypads", "irq masked", "irq", "hdma" };
static volatile int scene_asked;

static void abi_nmi(void) { rd8(0x004210); }
static void abi_irq(void) { rd8(0x004211); }

static void scene_set(int s)
{
    int i;
    wr8(0x00420C, 0x00);
    wr8(0x004200, 0x00);
    wr8(0x002100, s == S_DISPLAY || s == S_HDMA ? 0x0F : 0x80);
    wr8(0x00212C, s == S_DISPLAY || s == S_HDMA ? 0x01 : 0x00);
    cpu.p = (uint8_t)(s == S_IRQ ? cpu.p & ~P_I : cpu.p | P_I);
    if (s == S_NMI)
        wr8(0x004200, 0x81);
    if (s == S_IRQ_MASKED || s == S_IRQ) {
        wr8(0x004209, 100);
        wr8(0x00420A, 0);
        wr8(0x004200, 0x20);
    }
    if (s == S_HDMA) {
        /* a byte to BG1's scroll each line: runs of 127 and 97, repeated */
        uint32_t t = 0x7E2000;
        wr8(t++, 0xFF);
        for (i = 0; i < 127; i++)
            wr8(t++, (uint8_t)i);
        wr8(t++, 0xE1);
        for (i = 0; i < 97; i++)
            wr8(t++, (uint8_t)i);
        wr8(t, 0);
        wr8(0x004300, 0x00);
        wr8(0x004301, 0x0D);
        wr8(0x004302, 0x00);
        wr8(0x004303, 0x20);
        wr8(0x004304, 0x7E);
        wr8(0x00420C, 0x01);
    }
}

static void abi_reset(void)
{
    int at = -1;
    cpu.e = 0;
    nshvc_set_cpu_costs(0);
    for (;;) {
        if (at != scene_asked) {
            at = scene_asked;
            scene_set(at);
        }
        nshvc_wait_vblank();
    }
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FEA] = 0x00, [0x7FEB] = 0x90,
                                      [0x7FEE] = 0x00, [0x7FEF] = 0xA0, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x009000, abi_nmi, "nmi", 0 },
    { 0x00A000, abi_irq, "irq", 0 },
};

/* n frames, the sound taken as it is made (as a host does): nanoseconds */
static uint64_t frames_cost(int n)
{
    static int16_t lr[2 * 4096];
    uint64_t t0 = host_ticks_ns();
    int i;
    for (i = 0; i < n; i++) {
        nshvc_run_frame();
        nshvc_audio_take(lr, 4096);
    }
    return host_ticks_ns() - t0;
}

/* a frame's nanoseconds in the scene: the best of the batches */
static double scene_cost(int s)
{
    double best = 1e18;
    int b;
    scene_asked = s;
    frames_cost(10);
    for (b = 0; b < BATCHES; b++) {
        double c = (double)frames_cost(BATCH_FRAMES) / BATCH_FRAMES;
        if (c < best)
            best = c;
    }
    return best;
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    double base, alone = 1e18, among;
    int s, i, b, fails = 0;
    memset(&cart, 0, sizeof cart);
    cart.title = "scaling_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 3;
    nshvc_power(&cart);
    if (argc > 2) {
        /* (for a profiler: one scene, and nothing else) */
        for (s = 0; s < NSCENES; s++)
            if (!strcmp(argv[1], scene_name[s])) {
                scene_asked = s;
                frames_cost(atoi(argv[2]));
                return 0;
            }
        return 2;
    }
    base = scene_cost(S_IDLE);
    printf("scaling_check: %-12s %9.1f us a frame\n", scene_name[S_IDLE], base / 1000);
    for (s = S_DISPLAY; s < S_HDMA; s++) {
        double c = scene_cost(s);
        int ok = c <= base * OVER_MOST;
        printf("scaling_check: %-12s %9.1f us a frame, %.2f of idle%s\n", scene_name[s], c / 1000, c / base, ok ? "" : " FAIL");
        fails += !ok;
    }
    {
        double c = scene_cost(S_HDMA), d = scene_cost(S_DISPLAY);
        printf("scaling_check: %-12s %9.1f us a frame, %.3f us a line over the display's (the game's work, not judged)\n",
               scene_name[S_HDMA], c / 1000, (c - d) / 1000 / 224);
    }
    /* a frame alone, against a frame of a hundred: each the best of the
       batches, 100 frames each, timed one at a time or together */
    scene_asked = S_IDLE;
    frames_cost(10);
    among = 1e18;
    for (b = 0; b < BATCHES; b++) {
        double one = 0, all;
        for (i = 0; i < 100; i++)
            one += (double)frames_cost(1);
        one /= 100;
        all = (double)frames_cost(100) / 100;
        if (one < alone)
            alone = one;
        if (all < among)
            among = all;
    }
    {
        double hi = alone > among ? alone : among, lo = alone > among ? among : alone;
        int ok = hi <= lo * OVER_MOST;
        printf("scaling_check: a frame alone %.1f us, among 100 %.1f us%s\n", alone / 1000, among / 1000, ok ? "" : " FAIL");
        fails += !ok;
    }
    printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
