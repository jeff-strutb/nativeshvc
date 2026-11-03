/*
 * pace_macos.m: the pictures against the display, in a window (macOS,
 * the Metal renderer or Vulkan through MoltenVK). A cartridge whose
 * backdrop changes every frame; the check takes over -[CAMetalLayer
 * nextDrawable] to time each picture from the drawable asked for to the
 * drawable on the glass (its presentedTime), and how long the ask took.
 * The console keeps its own time (a frame every 16,639,267 ns), whatever
 * the display's refresh; the display never paces it and no picture waits
 * behind others.
 *
 * PACE_STEP=1 makes every third frame's game run 5 ms longer, as a game
 * whose frames alternate light and heavy work: the pictures still reach
 * the display a frame apart.
 *
 * For PACE_FRAMES frames (1500 by default, 25 s) it prints, in blocks of
 * 300 pictures, the median and the 90th percentile of the time to the
 * glass and the pictures the display never showed, then holds that, past
 * the first 60 pictures (the lead measured):
 *
 *   - no picture waits behind another for long: every picture's time to
 *     the glass within a refresh and 1 ms of the least of the 120 from it
 *     (a compositor that keeps pictures a refresh longer for a while, the
 *     Mac's window server now and then, moves them all alike; pictures
 *     waiting behind one are found and emptied within that); but the
 *     four from a picture the compositor kept a refresh longer (those
 *     already sent wait behind it until it is seen)
 *   - no ask for a drawable longer than 2 ms (the display not pacing)
 *   - the frames 16,639,267 ns apart (the slope of their times from frame
 *     200), within 0.02%
 *   - the frames whose picture was held back no more than the console's
 *     extra frames over the display's refreshes, sixty (the lead's
 *     measuring: a probe each two or three frames, longer while the
 *     window server settles), one each 60 while the compositor keeps
 *     pictures longer
 *     and three each time it changes; the pictures sent but never shown
 *     no more than two
 *
 * Exit 0 when all hold.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

#define MAXP 8192
#define BLOCK 300
#define FRAME_NS 16639267.0

static double asked[MAXP], waited[MAXP];
static double shown[MAXP];   /* 0 until presented; -1 never shown */
static int npics;
static double frame_t[MAXP];
static int nframes, want_frames = 1500;

static IMP s_next;
static id timed_next(id self, SEL sel)
{
    double t0 = CACurrentMediaTime();
    id<CAMetalDrawable> d = ((id(*)(id, SEL))s_next)(self, sel);
    double t1 = CACurrentMediaTime();
    if (d && npics < MAXP) {
        int i = npics++;
        asked[i] = t1;
        waited[i] = t1 - t0;
        [d addPresentedHandler:^(id<MTLDrawable> p) {
            double t = p.presentedTime;
            shown[i] = t > 0 ? t : -1;
        }];
    }
    return d;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(void)
{
    double refresh = 1.0 / (double)[[NSScreen mainScreen] maximumFramesPerSecond];
    double lat[MAXP], least = 1e9, most = 0, max_wait = 0, mean, extra;
    int fails = 0, b, i, never = 0, held, ok, late = 0, skips = 0, deeper = 0, skipped = -100;
    /* the first 60 pictures measure the lead; the last 10 may not be on
       the glass yet */
    int from = 60, to = npics - 10;
    printf("display refresh %.3f ms; %d pictures, %d frames\n", refresh * 1e3, npics, nframes);
    if (getenv("PACE_DUMP")) {
        /* each picture: asked, the ask's wait, on the glass (s; -1 never) */
        FILE *d = fopen(getenv("PACE_DUMP"), "w");
        for (i = 0; d && i < npics; i++)
            fprintf(d, "%d %.6f %.6f %.6f\n", i, asked[i], waited[i], shown[i]);
        if (d)
            fclose(d);
    }
    for (b = from; b + BLOCK <= to; b += BLOCK) {
        int n = 0, drop = 0;
        for (i = b; i < b + BLOCK; i++)
            if (shown[i] > 0)
                lat[n++] = (shown[i] - asked[i]) * 1e3;
            else
                drop++;
        qsort(lat, (size_t)n, sizeof lat[0], cmp);
        printf("     pictures %4d-%4d: to the glass median %.1f ms, 90%% %.1f ms; %d never shown\n", b,
               b + BLOCK - 1, n ? lat[n / 2] : 0, n ? lat[n * 9 / 10] : 0, drop);
    }
    for (i = from; i < to; i++)
        if (shown[i] > 0 && shown[i] - asked[i] < least)
            least = shown[i] - asked[i];
    for (i = from; i < to; i++) {
        double floor = 1e9;
        int j;
        if (shown[i] > 0) {
            double l = shown[i] - asked[i];
            most = l > most ? l : most;
            for (j = i; j < i + 120 && j < to; j++)
                if (shown[j] > 0 && shown[j] - asked[j] < floor)
                    floor = shown[j] - asked[j];
            if (l > floor + refresh + 0.001 && i - skipped > 4)
                late++;
            if (l >= least + refresh - 0.001)
                deeper++;
        } else
            never++;
        /* the compositor holding the last picture a refresh longer
           though the next was sent a frame after it */
        if (i > from && shown[i] > 0 && shown[i - 1] > 0 && asked[i] - asked[i - 1] < FRAME_NS * 1.5e-9 &&
            shown[i] - shown[i - 1] > refresh * 1.5)
            skips++, skipped = i;
        if (waited[i] > max_wait)
            max_wait = waited[i];
    }
    printf("%s to the glass %.1f to %.1f ms; %d pictures a refresh over the least of the 120 from them (none "
           "waited long); the compositor kept a picture longer %d times, %d pictures a refresh deeper\n",
           late ? "FAIL" : "ok  ", least * 1e3, most * 1e3, late, skips, deeper);
    fails += late != 0;
    printf("%s longest ask for a drawable %.2f ms\n", max_wait * 1e3 <= 2.0 ? "ok  " : "FAIL", max_wait * 1e3);
    fails += max_wait * 1e3 > 2.0;
    {
        /* the frames' spacing: the least-squares slope of their NMIs'
           times, from frame 200 (the lead measured: the probes' waits
           behind) */
        double sx = 0, sy = 0, sxx = 0, sxy = 0, k = 0;
        for (i = 200; i < nframes; i++, k++) {
            sx += i;
            sy += frame_t[i] - frame_t[200];
            sxx += (double)i * i;
            sxy += i * (frame_t[i] - frame_t[200]);
        }
        mean = (k * sxy - sx * sy) / (k * sxx - sx * sx) * 1e9;
    }
    ok = mean >= FRAME_NS * 0.9998 && mean <= FRAME_NS * 1.0002;
    printf("%s frames %.0f ns apart (the console's %.0f, %+.3f%%)\n", ok ? "ok  " : "FAIL", mean, FRAME_NS,
           (mean / FRAME_NS - 1) * 100);
    fails += !ok;
    held = nframes - npics;
    extra = refresh > FRAME_NS * 1e-9 ? nframes * (1 - FRAME_NS * 1e-9 / refresh) : 0;
    {
        int allowed = (int)ceil(extra) + 60 + deeper / 60 + 3 * skips;
        ok = held <= allowed && never <= 2;
        printf("%s %d frames held back (allowed %d: the console's extra frames %.1f, the lead's measuring, the "
               "compositor's changes), %d sent never shown\n",
               ok ? "ok  " : "FAIL", held, allowed, extra, never);
    }
    fails += !ok;
    printf("pace_macos: %s\n", fails ? "FAIL" : "ok");
    fflush(stdout);
    _exit(fails ? 1 : 0);
}

static void on_frame(void)
{
    if (nframes < MAXP)
        nframes++;
    wr8(0x7E0100, (uint8_t)(rd8(0x7E0100) + 1));
    if (nframes == want_frames)
        /* a few refreshes more for the last pictures to reach the glass */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ report(); });
}

/* PACE_STEP=1: every third frame's game runs 5 ms longer (a heavy step
   among light ones) */
static int step;

/* the backdrop a new colour each frame */
static void abi_nmi(void)
{
    uint8_t c = rd8(0x7E0100);
    /* each frame's time where the console has it: its NMI */
    if (nframes < MAXP)
        frame_t[nframes] = CACurrentMediaTime();
    rd8(0x004210);
    if (step && c % 3 == 0) {
        double until = CACurrentMediaTime() + 0.005;
        while (CACurrentMediaTime() < until)
            ;
    }
    wr8(0x002121, 0);
    wr8(0x002122, c);
    wr8(0x002122, (uint8_t)(c >> 3 & 0x7F));
}

static void abi_reset(void)
{
    cpu.e = 0;
    wr8(0x002100, 0x0F);
    wr8(0x004200, 0x80);
    for (;;)
        cpu_wai(0x008010);
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
    Method m = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    const char *f = getenv("PACE_FRAMES");
    if (f && atoi(f) > 60 && atoi(f) < MAXP)
        want_frames = atoi(f);
    step = getenv("PACE_STEP") != NULL;
    s_next = method_setImplementation(m, (IMP)timed_next);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x20;
    rom[0x7FEB] = 0x80;
    rom[0x7FD5] = 0x20;
    cart.title = "pace";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    hooks.frame = on_frame;
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
