/*
 * pointer_macos.m: a light gun aimed with the real pointer, in a window
 * (macOS): mouse events posted to the program's own window through AppKit
 * at known places on the picture, and a Super Scope cartridge reading the
 * counters the gun latched ($213C, $213D) and whether it latched ($213F).
 * Each place's dot and line are worked out here from the window alone: the
 * picture is the largest 4:3 that fits, centred.
 *
 *   pointer_macos     opens a window, runs the checks, exits 0 when every
 *                     one holds
 */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* the NMI: the gun's latch, as a game reads it, into $0200-$0204 */
static void abi_nmi(void)
{
    uint8_t lo, hi;
    rd8(0x004210);
    wr8(0x000200, (uint8_t)(rd8(0x00213F) >> 6 & 1));   /* latched this frame; the read clears it */
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

/* the places, as fractions of the picture (u across, v down), and one off
   it */
static const struct { double u, v; int on; } places[] = {
    { 0.5, 0.5, 1 }, { 0.1, 0.2, 1 }, { 0.9, 0.8, 1 }, { 0.002, 0.003, 1 }, { 0.997, 0.996, 1 },
    { 0.25, 0.75, 1 }, { 0.5, -0.05, 0 },
};
#define PLACES (int)(sizeof places / sizeof places[0])
static int frame, place, fails, settle;

static void post_move(NSWindow *w, double x, double y)
{
    NSView *v = [w contentView];
    /* the view's top left origin, as the host reports points */
    NSPoint p = [v convertPoint:NSMakePoint(x, [v bounds].size.height - y) toView:nil];
    NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:p modifierFlags:0 timestamp:0
                                 windowNumber:[w windowNumber] context:nil eventNumber:0 clickCount:0 pressure:0];
    [NSApp postEvent:e atStart:NO];
}

static void on_frame(void)
{
    NSWindow *w = [NSApp mainWindow] ? [NSApp mainWindow] : [[NSApp windows] firstObject];
    const uint8_t *ram = nshvc_wram();
    frame++;
    if (!w || frame < 30)
        return;
    if (settle == 0) {
        NSSize s = [[w contentView] bounds].size;
        double pw = s.width, ph = s.height, x, y;
        if (pw / ph > 4.0 / 3.0)
            pw = ph * 4.0 / 3.0;
        else
            ph = pw * 3.0 / 4.0;
        x = (s.width - pw) / 2 + places[place].u * pw;
        y = (s.height - ph) / 2 + places[place].v * ph;
        post_move(w, x, y);
        settle = 1;
        return;
    }
    /* the pointer read at a frame's end, the gun set, latched at the next
       frame's vertical blank, read by its NMI */
    if (++settle < 4)
        return;
    {
        int dot = (int)(places[place].u * 256), line = (int)(places[place].v * 224);
        int latched = ram[0x200], h = ram[0x201] | ram[0x202] << 8, v = ram[0x203] | ram[0x204] << 8;
        int ok = places[place].on ? latched && h == dot && v == line : !latched;
        printf("%s (%.3f, %.3f): %s dot %d line %d; the console latched %d: dot %d line %d\n", ok ? "ok  " : "FAIL",
               places[place].u, places[place].v, places[place].on ? "on the picture," : "off the picture,", dot,
               line, latched, h, v);
        fails += !ok;
    }
    settle = 0;
    if (++place == PLACES) {
        printf("%d check(s) failed\n", fails);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
}

static uint8_t rom[0x8000];
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x008020, abi_nmi, "nmi", 0 },
};

int main(int argc, char **argv)
{
    nshvc_cart cart = { 0 };
    nshvc_hooks hooks = { 0 };
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
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
