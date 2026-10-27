/*
 * pointer_ios.c: a light gun aimed by touch, on iOS (tests/unit/
 * pointer_ios.sh packages it for the simulator): a Super Scope cartridge
 * that reads the counters the gun latched ($213C, $213D) and whether it
 * latched ($213F). For each touch, as it lifts, it writes where the touch
 * was (the view's points, as the host reported it) and what the console
 * latched while it was held, to pointer.txt in the save folder, after a
 * first line with the view's size; pointer_ios.sh checks each against the
 * dot and line the place on the picture is (the largest 4:3 that fits,
 * centred).
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static void abi_nmi(void)
{
    uint8_t lo, hi;
    rd8(0x004210);
    wr8(0x000200, (uint8_t)(rd8(0x00213F) >> 6 & 1));
    lo = rd8(0x00213C);
    hi = rd8(0x00213C);
    wr8(0x000201, lo);
    wr8(0x000202, (uint8_t)(hi & 1));
    lo = rd8(0x00213D);
    hi = rd8(0x00213D);
    wr8(0x000203, lo);
    wr8(0x000204, (uint8_t)(hi & 1));
    rd8(0x00213F);
}

static void abi_reset(void)
{
    cpu.e = 0;
    wr8(0x002100, 0x0F);
    wr8(0x004200, 0x80);
    for (;;)
        cpu_wai(0x008010);
}

static FILE *log_f;
static int touching, held;
static float tx, ty;
static int last_latched, last_h, last_v;

static void on_touch(const nshvc_touch *t, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (t[i].id != 0)
            continue;
        tx = t[i].x;
        ty = t[i].y;
        if (t[i].phase == 0) {
            touching = 1;
            held = 0;
        } else if (t[i].phase == 2 && touching) {
            touching = 0;
            if (log_f) {
                fprintf(log_f, "touch %.2f %.2f held %d latched %d dot %d line %d\n", tx, ty, held, last_latched,
                        last_h, last_v);
                fflush(log_f);
            }
        }
    }
}

static void on_frame(void)
{
    const uint8_t *ram = nshvc_wram();
    if (!log_f) {
        char path[1024];
        float w, h;
        snprintf(path, sizeof path, "%s/pointer.txt", nshvc_save_dir());
        log_f = fopen(path, "w");
        nshvc_window_size(&w, &h);
        if (log_f) {
            fprintf(log_f, "view %.2f %.2f\n", w, h);
            fflush(log_f);
        }
    }
    /* the latch two frames into the touch: the gun set at a frame's end,
       latched at the next vertical blank, read by its NMI */
    if (touching && ++held >= 3) {
        last_latched = ram[0x200];
        last_h = ram[0x201] | ram[0x202] << 8;
        last_v = ram[0x203] | ram[0x204] << 8;
    }
}

static uint8_t rom[0x8000];
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x008020, abi_nmi, "nmi", 0 },
};

NSHVC_MAIN(argc, argv)
{
    nshvc_cart cart;
    nshvc_hooks hooks;
    memset(&cart, 0, sizeof cart);
    memset(&hooks, 0, sizeof hooks);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x20;
    rom[0x7FEB] = 0x80;
    rom[0x7FD5] = 0x20;
    cart.title = "pointer";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    cart.ports[0] = NSHVC_PORT_JOYPAD;
    cart.ports[1] = NSHVC_PORT_SUPERSCOPE;
    hooks.frame = on_frame;
    hooks.touch = on_touch;
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
