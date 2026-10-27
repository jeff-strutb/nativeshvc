/*
 * ports.c: the controller ports and what plugs into them. Each port has a
 * latch line (the CPU's $4016 bit 0 and the automatic read share it), a
 * clock that each read of the port makes, two data lines back and the
 * IOBit pin (WRIO's bit 6 for port 1, bit 7 for port 2, which port 2's
 * light guns pull to latch the PPU's counters). bus.c drives the lines;
 * each device here answers them as the device itself does:
 *
 *   joypad      12 buttons, B first, then 4 signature bits (0), then 1s
 *   mouse       32 bits: 8 zeros, the buttons, its speed, the signature
 *               (0001), then Y and X as sign and 7 bits of motion; a clock
 *               while latched steps the speed (1, 1.5, 2 times)
 *   multitap    four joypads, two at once on the two data lines; IOBit
 *               picks the pair (high: the first two, low: the others);
 *               while latched, data line 2 reads 1 (its detection)
 *   Super Scope 8 bits: trigger, cursor, turbo, pause, 0, 0, off screen,
 *               noise; on port 2 it latches the counters where it points
 *   Justifier   32 bits: 12 zeros, the signature (1110), the ID (01010101),
 *               both triggers, both starts, which gun the latch is for;
 *               the guns take turns, one each latch
 *
 * A mouse reports the frame's motion at each latch in the frame.
 *
 * The native controls (keyboard, game controllers, mouse, touch: front
 * end) set what each device's player holds: nshvc.h's nshvc_set_joypad,
 * nshvc_set_mouse and nshvc_set_gun.
 */
#include <string.h>

#include "nshvc/nshvc.h"
#include "snes_internal.h"

#define PADS 8

static int plugged[2];
/* what each player holds: the players in port order, each port's device
   one (a multitap four), port 1's first */
static uint16_t held[PADS];
/* the mice: this frame's motion, the buttons */
static int mouse_dx[2], mouse_dy[2], mouse_buttons[2];
/* the light guns: where each points (dots, lines) and its buttons */
static int gun_x[2], gun_y[2];
static unsigned gun_buttons[2];

/* each port's device state */
static struct port {
    int latched;
    unsigned counter, counter2;
    /* joypad, multitap: the buttons taken as the latch fell */
    uint16_t pad[4];
    /* mouse */
    unsigned speed;
    int mx, my, mdx, mdy, ml, mr;
    /* Super Scope */
    int sx, sy, trigger, cursor, turbo, pause, offscreen, oldturbo, triggerlock, pauselock;
    /* Justifier(s) */
    int jx[2], jy[2], jtrigger[2], jstart[2], active;
} port[2];

/* the joypads before port p's */
static int pad_base(int p)
{
    return p == 0 ? 0 : plugged[0] == NSHVC_PORT_MULTITAP ? 4 : 1;
}

static int iobit(int p) { return snes_wrio() >> (p ? 7 : 6) & 1; }

static void device_reset(int p)
{
    struct port *s = &port[p];
    int g;
    memset(s, 0, sizeof *s);
    /* a light gun starts pointing at the screen's middle */
    s->sx = 256 / 2;
    s->sy = 240 / 2;
    s->jx[0] = s->jx[1] = 256 / 2;
    s->jy[0] = s->jy[1] = 240 / 2;
    if (plugged[p] == NSHVC_PORT_JUSTIFIERS) {
        s->jx[0] -= 16;
        s->jx[1] += 16;
    } else {
        s->jx[1] = s->jy[1] = -1;
    }
    /* a light gun plugged in is aimed where it starts */
    if (plugged[p] >= NSHVC_PORT_SUPERSCOPE)
        for (g = 0; g < 2; g++) {
            gun_x[g] = plugged[p] == NSHVC_PORT_JUSTIFIERS ? s->jx[g] : 256 / 2;
            gun_y[g] = 240 / 2;
        }
}

void ports_reset(const int *ports)
{
    int p;
    for (p = 0; p < 2; p++) {
        plugged[p] = ports ? ports[p] : NSHVC_PORT_JOYPAD;
        mouse_dx[p] = mouse_dy[p] = mouse_buttons[p] = 0;
        gun_x[p] = 256 / 2;
        gun_y[p] = 240 / 2;
        gun_buttons[p] = 0;
        device_reset(p);
    }
    memset(held, 0, sizeof held);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the mouse takes the frame's motion as the latch changes, scaled by its
   speed and held to 127 a reading */
static void mouse_take(int p)
{
    struct port *s = &port[p];
    int x = mouse_dx[p], y = mouse_dy[p];
    s->ml = mouse_buttons[p] & 1;
    s->mr = mouse_buttons[p] >> 1 & 1;
    s->mdx = x < 0;
    s->mdy = y < 0;
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    if (s->speed == 1) { x = x * 3 / 2; y = y * 3 / 2; }
    if (s->speed == 2) { x *= 2; y *= 2; }
    s->mx = x > 127 ? 127 : x;
    s->my = y > 127 ? 127 : y;
}

static void device_latch(int p, int level)
{
    struct port *s = &port[p];
    int k, base;
    if (s->latched == level)
        return;
    s->latched = level;
    s->counter = s->counter2 = 0;
    switch (plugged[p]) {
    case NSHVC_PORT_JOYPAD:
        if (!level)
            s->pad[0] = held[pad_base(p)];
        break;
    case NSHVC_PORT_MULTITAP:
        base = pad_base(p);
        if (!level)
            for (k = 0; k < 4; k++)
                s->pad[k] = base + k < PADS ? held[base + k] : 0;
        break;
    case NSHVC_PORT_MOUSE:
        mouse_take(p);
        break;
    case NSHVC_PORT_JUSTIFIER:
    case NSHVC_PORT_JUSTIFIERS:
        if (!level)
            s->active = !s->active;
        break;
    }
}

void ports_latch(int level)
{
    device_latch(0, level);
    device_latch(1, level);
}

/* a joypad's bit n (B, Y, SELECT, START, up, down, left, right, A, X, L,
   R): the pad's own bits; its cross holds no two opposite directions */
static unsigned pad_bit(uint16_t b, unsigned n)
{
    switch (n) {
    case 4: return (b & NSHVC_UP) && !(b & NSHVC_DOWN);
    case 5: return (b & NSHVC_DOWN) && !(b & NSHVC_UP);
    case 6: return (b & NSHVC_LEFT) && !(b & NSHVC_RIGHT);
    case 7: return (b & NSHVC_RIGHT) && !(b & NSHVC_LEFT);
    }
    return n < 12 ? b >> (15 - n) & 1 : 0;
}

static unsigned scope_data(int p)
{
    struct port *s = &port[p];
    if (s->counter >= 8)
        return 1;
    if (s->counter == 0) {
        unsigned b = gun_buttons[0];
        int t = (b & NSHVC_GUN_TRIGGER) != 0, tu = (b & NSHVC_GUN_TURBO) != 0, pa = (b & NSHVC_GUN_PAUSE) != 0;
        /* turbo is a switch, each press turning it over; with it on the
           trigger fires while held, else once a press; pause once a press */
        if (tu && !s->oldturbo)
            s->turbo = !s->turbo;
        s->oldturbo = tu;
        s->trigger = 0;
        if (t && (s->turbo || !s->triggerlock))
            s->trigger = s->triggerlock = 1;
        else if (!t)
            s->triggerlock = 0;
        s->cursor = (b & NSHVC_GUN_CURSOR) != 0;
        s->pause = 0;
        if (pa && !s->pauselock)
            s->pause = s->pauselock = 1;
        else if (!pa)
            s->pauselock = 0;
        s->offscreen = s->sx < 0 || s->sy < 0 || s->sx >= 256 || s->sy >= (int)snes_vdisp();
    }
    switch (s->counter++) {
    case 0: return s->offscreen ? 0 : (unsigned)s->trigger;
    case 1: return (unsigned)s->cursor;
    case 2: return (unsigned)s->turbo;
    case 3: return (unsigned)s->pause;
    case 6: return (unsigned)s->offscreen;
    }
    return 0;
}

static unsigned justifier_data(int p)
{
    struct port *s = &port[p];
    unsigned n;
    if (s->counter >= 32)
        return 1;
    if (s->counter == 0) {
        s->jtrigger[0] = (gun_buttons[0] & NSHVC_GUN_TRIGGER) != 0;
        s->jstart[0] = (gun_buttons[0] & NSHVC_GUN_START) != 0;
        if (plugged[p] == NSHVC_PORT_JUSTIFIERS) {
            s->jtrigger[1] = (gun_buttons[1] & NSHVC_GUN_TRIGGER) != 0;
            s->jstart[1] = (gun_buttons[1] & NSHVC_GUN_START) != 0;
        }
    }
    n = s->counter++;
    if (n >= 12 && n <= 14)
        return 1;                                /* the signature, 1110 */
    if (n >= 16 && n <= 23)
        return n & 1;                            /* the ID, 01010101 */
    switch (n) {
    case 24: return (unsigned)s->jtrigger[0];
    case 25: return (unsigned)s->jtrigger[1];
    case 26: return (unsigned)s->jstart[0];
    case 27: return (unsigned)s->jstart[1];
    case 28: return (unsigned)s->active;
    }
    return 0;
}

unsigned ports_data(int p)
{
    struct port *s = &port[p];
    unsigned n;
    switch (plugged[p]) {
    case NSHVC_PORT_JOYPAD:
        if (s->counter >= 16)
            return 1;
        if (s->latched)
            return held[pad_base(p)] & NSHVC_B ? 1 : 0;
        return pad_bit(s->pad[0], s->counter++);
    case NSHVC_PORT_MULTITAP: {
        unsigned *c = iobit(p) ? &s->counter : &s->counter2;
        int a = iobit(p) ? 0 : 2;
        if (s->latched)
            return 2;
        if (*c >= 16)
            return 3;
        n = (*c)++;
        if (n >= 12)
            return 0;
        return pad_bit(s->pad[a], n) | pad_bit(s->pad[a + 1], n) << 1;
    }
    case NSHVC_PORT_MOUSE:
        if (s->latched) {
            s->speed = (s->speed + 1) % 3;
            return 0;
        }
        if (s->counter >= 32)
            return 1;
        n = s->counter++;
        if (n < 8)
            return 0;
        switch (n) {
        case 8: return (unsigned)s->mr;
        case 9: return (unsigned)s->ml;
        case 10: return s->speed >> 1 & 1;
        case 11: return s->speed & 1;
        case 15: return 1;
        case 16: return (unsigned)s->mdy;
        case 24: return (unsigned)s->mdx;
        }
        if (n >= 17 && n <= 23)
            return (unsigned)s->my >> (23 - n) & 1;
        if (n >= 25)
            return (unsigned)s->mx >> (31 - n) & 1;
        return 0;
    case NSHVC_PORT_SUPERSCOPE:
        return scope_data(p);
    case NSHVC_PORT_JUSTIFIER:
    case NSHVC_PORT_JUSTIFIERS:
        return justifier_data(p);
    }
    return 0;
}

/* line vdisp begins (the picture done): a light gun on port 2 that points
   at the picture latches the counters there */
void ports_frame(void)
{
    struct port *s = &port[1];
    int vdisp = (int)snes_vdisp(), g, x, y;
    /* the frame's mouse motion is spent */
    mouse_dx[0] = mouse_dy[0] = mouse_dx[1] = mouse_dy[1] = 0;
    switch (plugged[1]) {
    case NSHVC_PORT_SUPERSCOPE:
        s->sx = clamp(gun_x[0], -16, 256 + 16);
        s->sy = clamp(gun_y[0], -16, vdisp + 16);
        s->offscreen = s->sx < 0 || s->sy < 0 || s->sx >= 256 || s->sy >= vdisp;
        if (!s->offscreen)
            ppu_latch_counters((uint16_t)s->sx, (uint16_t)s->sy);
        break;
    case NSHVC_PORT_JUSTIFIER:
    case NSHVC_PORT_JUSTIFIERS:
        g = s->active;
        if (g == 1 && plugged[1] == NSHVC_PORT_JUSTIFIER) {
            x = s->jx[1];
            y = s->jy[1];
        } else {
            s->jx[g] = x = clamp(gun_x[g], -16, 256 + 16);
            s->jy[g] = y = clamp(gun_y[g], -16, vdisp + 16);
        }
        if (!(x < 0 || y < 0 || x >= 256 || y >= vdisp))
            ppu_latch_counters((uint16_t)x, (uint16_t)y);
        break;
    }
}

/* ---- nshvc.h ---- */
void nshvc_plug(int p, int device)
{
    if (p < 0 || p > 1 || device < NSHVC_PORT_JOYPAD || device > NSHVC_PORT_JUSTIFIERS)
        return;
    plugged[p] = device;
    device_reset(p);
}
int nshvc_plugged(int p) { return p == 0 || p == 1 ? plugged[p] : NSHVC_PORT_NONE; }

void nshvc_set_joypad(int pad, uint16_t buttons)
{
    if (pad >= 0 && pad < PADS)
        held[pad] = (uint16_t)(buttons & 0xFFF0);
}

void nshvc_set_mouse(int p, int dx, int dy, int buttons)
{
    if (p != 0 && p != 1)
        return;
    mouse_dx[p] = dx;
    mouse_dy[p] = dy;
    mouse_buttons[p] = buttons;
}

void nshvc_set_gun(int gun, int x, int y, unsigned buttons)
{
    if (gun != 0 && gun != 1)
        return;
    gun_x[gun] = x;
    gun_y[gun] = y;
    gun_buttons[gun] = buttons;
}
