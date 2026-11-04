/*
 * click_macos.m: the mouse's buttons as a game hears them (nshvc_hooks
 * .pointer), in a window (macOS): AppKit's own button events posted to the
 * program's window, each press and release handled after the button is
 * already up again (as a trackpad's tap, or any click let go before the
 * console's frame), so the buttons must come from each event itself, not
 * from the buttons held when it is handled. A left press, its release, a
 * right press and its release: the hook must hear buttons 1, 0, 2, 0, at
 * the place posted.
 *
 *   click_macos     opens a window, runs the checks, exits 0 when every
 *                   one holds
 */
#import <AppKit/AppKit.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static void abi_nmi(void) { rd8(0x004210); }

static void abi_reset(void)
{
    cpu.e = 0;
    wr8(0x004200, 0x80);
    for (;;)
        cpu_wai(0x008010);
}

static int frame, heard[16], n, fails;
static float heard_x[16], heard_y[16];

static int on_pointer(float x, float y, int buttons)
{
    /* only the events posted here: the real pointer's moves are not */
    if (!((fabsf(x - 40) < 0.01f && fabsf(y - 30) < 0.01f) || (fabsf(x - 60) < 0.01f && fabsf(y - 50) < 0.01f)))
        return 1;
    if (n < 16) {
        heard_x[n] = x;
        heard_y[n] = y;
        heard[n++] = buttons;
    }
    return 1;
}

static void post(NSWindow *w, NSEventType type, double x, double y)
{
    NSView *v = [w contentView];
    NSPoint p = [v convertPoint:NSMakePoint(x, [v bounds].size.height - y) toView:nil];
    NSEvent *e = [NSEvent mouseEventWithType:type location:p modifierFlags:0 timestamp:0
                                windowNumber:[w windowNumber] context:nil eventNumber:0 clickCount:1 pressure:1];
    [NSApp postEvent:e atStart:NO];
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static void on_frame(void)
{
    NSWindow *w = [NSApp mainWindow] ? [NSApp mainWindow] : [[NSApp windows] firstObject];
    frame++;
    if (!w || frame < 30)
        return;
    if (frame == 30) {
        n = 0;
        post(w, NSEventTypeLeftMouseDown, 40, 30);
        post(w, NSEventTypeLeftMouseUp, 40, 30);
        post(w, NSEventTypeRightMouseDown, 60, 50);
        post(w, NSEventTypeRightMouseUp, 60, 50);
        return;
    }
    if (frame < 34)
        return;
    {
        int i;
        for (i = 0; i < n; i++)
            printf("     heard (%.1f, %.1f) buttons %d\n", heard_x[i], heard_y[i], heard[i]);
    }
    check(n == 4, "four events heard");
    check(n == 4 && heard[0] == 1 && heard[1] == 0 && heard[2] == 2 && heard[3] == 0,
          "left press 1, release 0, right press 2, release 0");
    check(n == 4 && fabsf(heard_x[0] - 40) < 0.01f && fabsf(heard_y[0] - 30) < 0.01f && fabsf(heard_x[2] - 60) < 0.01f &&
              fabsf(heard_y[2] - 50) < 0.01f,
          "at the places posted, from the view's top left");
    printf("%d check(s) failed\n", fails);
    fflush(stdout);
    _exit(fails ? 1 : 0);
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
    cart.title = "click";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    hooks.frame = on_frame;
    hooks.pointer = on_pointer;
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
