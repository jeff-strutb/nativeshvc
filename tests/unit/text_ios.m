/*
 * text_ios.m: text entry on iOS (nshvc_ext_text_input), in the simulator
 * (tests/unit/text_ios.sh packages and runs it). A cartridge that does
 * nothing; the check is the front end's. The simulator always has the
 * Mac's keyboard, so the check makes GameController report none and posts
 * the system's notices as a keyboard comes and goes, and at each step
 * finds the first responder:
 *
 *   no keyboard, text entry on   the system keyboard's responder (the
 *                                host's PortText); typed into ("aZ 9",
 *                                Return, "#", Backspace), the key hook gets
 *                                A, Z, space, 9, Return, Backspace, each a
 *                                press then a release, nothing for "#"
 *   a keyboard attached          it lets go (the keyboard's presses come
 *                                as always); nshvc_keyboard_attached 1
 *   the keyboard gone again      it takes the text again
 *   text entry off               it lets go
 *
 * It writes each step and "text_ios: ok" or "text_ios: FAIL" to text.txt in
 * the save folder and on stderr, and exits 0 or 1.
 */
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static __weak UIResponder *s_first;

@interface UIResponder (TextIosCheck)
- (void)textIosCapture:(id)sender;
@end

@implementation UIResponder (TextIosCheck)
- (void)textIosCapture:(id)sender
{
    (void)sender;
    s_first = self;
}
@end

static FILE *log_f;
static int keys[64][2], nkeys, frames, fails;
static char first[128];
static int typed;

static void say(const char *fmt, const char *a, int b)
{
    fprintf(stderr, fmt, a, b);
    if (log_f) {
        fprintf(log_f, fmt, a, b);
        fflush(log_f);
    }
}

static int on_key(int vk, int down)
{
    if (nkeys < 64) {
        keys[nkeys][0] = vk;
        keys[nkeys][1] = down;
        nkeys++;
    }
    return 1;
}

/* the simulator always has the Mac's keyboard: GameController made to
   report none (+[GCKeyboard coalescedKeyboard] nil) for the steps that
   need it, and the system's notices posted as a keyboard comes and goes */
static IMP s_real_kb;
static id no_keyboard(id self, SEL sel)
{
    (void)self;
    (void)sel;
    return nil;
}

static void keyboard(int on)
{
    Method m = class_getClassMethod([GCKeyboard class], @selector(coalescedKeyboard));
    if (!s_real_kb)
        s_real_kb = method_getImplementation(m);
    method_setImplementation(m, on ? s_real_kb : (IMP)no_keyboard);
    [[NSNotificationCenter defaultCenter]
        postNotificationName:on ? GCKeyboardDidConnectNotification : GCKeyboardDidDisconnectNotification
                      object:nil];
}

/* (main thread) the first responder's class into first; typed into when
   type is set and it takes text */
static void probe(int type)
{
    s_first = nil;
    [[UIApplication sharedApplication] sendAction:@selector(textIosCapture:) to:nil from:nil forEvent:nil];
    snprintf(first, sizeof first, "%s", s_first ? NSStringFromClass([s_first class]).UTF8String : "none");
    if (type && [s_first conformsToProtocol:@protocol(UIKeyInput)]) {
        id<UIKeyInput> k = (id<UIKeyInput>)s_first;
        [k insertText:@"aZ 9\n#"];
        [k deleteBackward];
        typed = 1;
    }
}

/* the first responder (probed a frame or more before) is or is not the
   keyboard's (PortText), and the hardware keyboard reported */
static void expect(const char *step, int text, int kb)
{
    int ok = (strcmp(first, "PortText") == 0) == text && nshvc_keyboard_attached() == kb;
    char d[256];
    snprintf(d, sizeof d, "%s: first responder %s, keyboard attached %d", step, first, nshvc_keyboard_attached());
    say("%s%.0d\n", ok ? "ok" : "FAIL", 0);
    say("  %s%.0d\n", d, 0);
    fails += !ok;
}

static void on_frame(void)
{
    static const int want[6] = { 'A', 'Z', 0x20, '9', 0x0D, 0x08 };
    int i;
    frames++;
    if (!log_f) {
        char path[1024];
        snprintf(path, sizeof path, "%s/text.txt", nshvc_save_dir());
        log_f = fopen(path, "w");
    }
    switch (frames) {
    case 20:   /* no keyboard; text entry on */
        dispatch_async(dispatch_get_main_queue(), ^{ keyboard(0); });
        break;
    case 35:
        nshvc_ext_text_input(1);
        break;
    case 50:   /* the system keyboard's responder first; typed into */
        dispatch_async(dispatch_get_main_queue(), ^{ probe(1); });
        break;
    case 65:
        expect("no keyboard, text entry on", 1, 0);
        if (!typed || nkeys != 12) {
            say("FAIL keys: %s%d, want 12\n", typed ? "" : "nothing typed, ", nkeys);
            fails++;
        }
        for (i = 0; i < nkeys && i < 12; i++)
            if (keys[i][0] != want[i / 2] || keys[i][1] != !(i & 1)) {
                char d[32];
                snprintf(d, sizeof d, "%02X %s", keys[i][0], keys[i][1] ? "down" : "up");
                say("FAIL key %s at %d\n", d, i);
                fails++;
            }
        if (nkeys == 12)
            say("ok   keys: %sA Z space 9 Return Backspace, each down then up (%d)\n", "", nkeys);
        /* a keyboard attached: the responder lets go */
        dispatch_async(dispatch_get_main_queue(), ^{ keyboard(1); });
        break;
    case 80:
        dispatch_async(dispatch_get_main_queue(), ^{ probe(0); });
        break;
    case 95:
        expect("keyboard attached, text entry on", 0, 1);
        dispatch_async(dispatch_get_main_queue(), ^{ keyboard(0); });
        break;
    case 110:
        dispatch_async(dispatch_get_main_queue(), ^{ probe(0); });
        break;
    case 125:
        expect("keyboard gone again", 1, 0);
        nshvc_ext_text_input(0);
        break;
    case 140:
        dispatch_async(dispatch_get_main_queue(), ^{ probe(0); });
        break;
    case 155:
        expect("text entry off", 0, 0);
        dispatch_async(dispatch_get_main_queue(), ^{ keyboard(1); });
        break;
    case 170:
        say("text_ios: %s%.0d\n", fails ? "FAIL" : "ok", 0);
        exit(fails ? 1 : 0);
    }
}

static uint8_t rom[0x8000];

static void abi_reset(void)
{
    wr8(0x002100, 0x0F);
    for (;;)
        nshvc_wait_vblank();
}

static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

NSHVC_MAIN(argc, argv)
{
    nshvc_cart cart;
    nshvc_hooks hooks;
    memset(&cart, 0, sizeof cart);
    memset(&hooks, 0, sizeof hooks);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FD5] = 0x20;
    cart.title = "text";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    hooks.frame = on_frame;
    hooks.key = on_key;
    nshvc_set_hooks(&hooks);
    return nshvc_main(&cart, argc, argv);
}
