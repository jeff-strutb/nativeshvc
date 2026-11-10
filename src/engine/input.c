/* input.c: every keyboard, controller, mouse and touch screen, as each device
 * has it. The engine maps none of it to a game's controls; the game decides.
 * The system thread hands in events; the game thread samples the state once
 * per update, so a recorded run replays exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_window.h"
#include "engine.h"
#include "host.h"

#define CONTROLLERS 8

static host_mutex *lock;

/* the system thread's, under lock */
static uint8_t key_down[256];
static uint32_t key_pressed[8], key_released[8];   /* since the last sample */
static float mouse_x, mouse_y, mouse_dx, mouse_dy, wheel_x, wheel_y;
static int mouse_buttons, mouse_here;
static struct { int id; float x, y; } touch[16];
static int touches;
static char typed[64];
static uint32_t ntyped;

/* the game thread's */
static host_pad pads[CONTROLLERS];
static int pad_on[CONTROLLERS];

/* --input: the devices held from each listed update on */
static FILE *replay;
static uint64_t replay_next = UINT64_MAX;
static nshvc_input replay_line, replay_held;
static uint32_t replay_keys[8];

void eng_input_start(const char *replay_path)
{
    lock = host_mutex_new();
    if (replay_path && !(replay = fopen(replay_path, "r")))
        fprintf(stderr, "nativeshvc: cannot read %s\n", replay_path);
}

/* on the system thread */
void eng_input_event(const host_event *e)
{
    host_mutex_lock(lock);
    switch (e->type) {
    case HOST_EV_KEY:
        if (e->vk > 0 && e->vk < 256) {
            if (e->down && !key_down[e->vk])
                key_pressed[e->vk >> 5] |= 1u << (e->vk & 31);
            if (!e->down && key_down[e->vk])
                key_released[e->vk >> 5] |= 1u << (e->vk & 31);
            key_down[e->vk] = (uint8_t)(e->down != 0);
        }
        /* what types: 'A'-'Z', digits, space, Backspace, Return */
        if (e->down && (e->vk == 0x20 || e->vk == 0x08 || e->vk == 0x0D || (e->vk >= '0' && e->vk <= '9') || (e->vk >= 'A' && e->vk <= 'Z')))
            if (ntyped < sizeof typed)
                typed[ntyped++] = (char)e->vk;
        break;
    case HOST_EV_FOCUS:
        if (!e->down) {
            int k;
            for (k = 0; k < 256; k++)
                if (key_down[k])
                    key_released[k >> 5] |= 1u << (k & 31);
            memset(key_down, 0, sizeof key_down);
        }
        break;
    case HOST_EV_WHEEL:
        wheel_x += e->dx;
        wheel_y += e->dy;
        break;
    case HOST_EV_MOUSE:
        mouse_here = 1;
        mouse_x = e->x;
        mouse_y = e->y;
        mouse_dx += e->dx;
        mouse_dy += e->dy;
        mouse_buttons = e->buttons;
        break;
    case HOST_EV_TOUCH: {
        int i;
        for (i = 0; i < touches; i++)
            if (touch[i].id == e->id)
                break;
        if (e->phase == 2) {
            if (i < touches) {
                touch[i] = touch[touches - 1];
                touches--;
            }
        } else if (i < touches || touches < 16) {
            if (i == touches)
                touches++;
            touch[i].id = e->id;
            touch[i].x = e->x;
            touch[i].y = e->y;
        }
        break;
    }
    }
    host_mutex_unlock(lock);
}

/* one --input line: "UPDATE item...", the devices held from that update on.
   Items: key=VK (hex), controller=N,BUTTONS (hex), stick=N,LX,LY,RX,RY,
   trigger=N,LT,RT, pointer=X,Y[,BUTTONS], touch=ID,X,Y. 0 at the end */
static int replay_read(void)
{
    char line[1024], *c, *w;
    for (;;) {
        if (!fgets(line, sizeof line, replay))
            return 0;
        if ((c = strchr(line, '#')))
            *c = 0;
        c = line;
        while (*c == ' ' || *c == '\t')
            c++;
        if (*c && *c != '\n' && *c != '\r')
            break;
    }
    replay_next = strtoull(c, &c, 10);
    memset(&replay_line, 0, sizeof replay_line);
    for (;;) {
        unsigned n = 0, b = 0;
        int id = 0;
        float a[4] = { 0, 0, 0, 0 };
        /* the next word */
        while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            c++;
        if (!*c)
            break;
        w = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
            c++;
        if (*c)
            *c++ = 0;
        if (sscanf(w, "key=%x", &n) == 1 && n > 0 && n < 256) {
            replay_line.keys[n >> 5] |= 1u << (n & 31);
        } else if (sscanf(w, "controller=%u,%x", &n, &b) == 2 && n < CONTROLLERS) {
            replay_line.controller[n].on = 1;
            replay_line.controller[n].buttons = (uint16_t)b;
        } else if (sscanf(w, "stick=%u,%f,%f,%f,%f", &n, &a[0], &a[1], &a[2], &a[3]) >= 3 && n < CONTROLLERS) {
            replay_line.controller[n].on = 1;
            replay_line.controller[n].lx = a[0];
            replay_line.controller[n].ly = a[1];
            replay_line.controller[n].rx = a[2];
            replay_line.controller[n].ry = a[3];
        } else if (sscanf(w, "trigger=%u,%f,%f", &n, &a[0], &a[1]) == 3 && n < CONTROLLERS) {
            replay_line.controller[n].on = 1;
            replay_line.controller[n].lt = a[0];
            replay_line.controller[n].rt = a[1];
        } else if (sscanf(w, "pointer=%f,%f,%x", &a[0], &a[1], &b) >= 2) {
            replay_line.pointer.x = a[0];
            replay_line.pointer.y = a[1];
            replay_line.pointer.buttons = (uint8_t)(b & 7);
            replay_line.pointer.inside = 1;
        } else if (sscanf(w, "touch=%d,%f,%f", &id, &a[0], &a[1]) == 3 && replay_line.touches < 10) {
            replay_line.touch[replay_line.touches].id = id;
            replay_line.touch[replay_line.touches].x = a[0];
            replay_line.touch[replay_line.touches].y = a[1];
            replay_line.touches++;
        } else {
            fprintf(stderr, "nativeshvc: --input: update %llu: %s is not an input\n", (unsigned long long)replay_next, w);
        }
    }
    return 1;
}

/* the replayed devices for update n */
static void replay_sample(nshvc_input *in, uint64_t n)
{
    int k;
    for (;;) {
        if (replay_next == UINT64_MAX && !replay_read())
            break;
        if (replay_next > n)
            break;
        replay_held = replay_line;
        replay_next = UINT64_MAX;
    }
    *in = replay_held;
    for (k = 0; k < 8; k++) {
        in->keys_pressed[k] = in->keys[k] & ~replay_keys[k];
        in->keys_released[k] = replay_keys[k] & ~in->keys[k];
        replay_keys[k] = in->keys[k];
    }
}

/* on the game thread: the input for update `n` */
void eng_input_sample(nshvc_input *in, uint64_t n, int headless)
{
    uint32_t p;
    memset(in, 0, sizeof *in);
    if (replay) {
        replay_sample(in, n);
        return;
    }
    if (headless)
        return;
    for (p = 0; p < CONTROLLERS; p++) {
        host_pad now;
        pad_on[p] = host_pad_read((int)p, &now);
        if (pad_on[p]) {
            pads[p] = now;
            in->controller[p].on = 1;
            in->controller[p].style = (uint8_t)(now.style == HOST_PAD_XBOX ? NSHVC_STYLE_XBOX
                                                : now.style == HOST_PAD_PLAYSTATION ? NSHVC_STYLE_PLAYSTATION
                                                : now.style == HOST_PAD_NINTENDO ? NSHVC_STYLE_NINTENDO : NSHVC_STYLE_GENERIC);
            in->controller[p].buttons = (uint16_t)now.buttons;
            in->controller[p].lx = now.x;
            in->controller[p].ly = now.y;
            in->controller[p].rx = now.rx;
            in->controller[p].ry = now.ry;
            in->controller[p].lt = now.lt;
            in->controller[p].rt = now.rt;
        }
    }
    host_mutex_lock(lock);
    {
        int k;
        for (k = 1; k < 256; k++)
            if (key_down[k])
                in->keys[k >> 5] |= 1u << (k & 31);
        memcpy(in->keys_pressed, key_pressed, sizeof key_pressed);
        memcpy(in->keys_released, key_released, sizeof key_released);
        memset(key_pressed, 0, sizeof key_pressed);
        memset(key_released, 0, sizeof key_released);
    }
    {
        /* the pointer and touches, from points to window pixels */
        float pw = 0, ph = 0, sx = 1, sy = 1;
        int ww = 0, wh = 0, t;
        host_window_sizes(&ww, &wh, &pw, &ph);   /* from the same resize */
        if (pw > 0 && ph > 0 && ww > 0 && wh > 0) {
            sx = (float)ww / pw;
            sy = (float)wh / ph;
        }
        in->pointer.x = mouse_x * sx;
        in->pointer.y = mouse_y * sy;
        in->pointer.dx = mouse_dx * sx;
        in->pointer.dy = mouse_dy * sy;
        in->pointer.wheel_x = wheel_x;
        in->pointer.wheel_y = wheel_y;
        in->pointer.buttons = (uint8_t)(mouse_buttons & 7);
        in->pointer.inside = (uint8_t)(mouse_here && mouse_x >= 0 && mouse_y >= 0 && mouse_x < pw && mouse_y < ph);
        mouse_dx = mouse_dy = 0;
        wheel_x = wheel_y = 0;
        for (t = 0; t < touches && t < 10; t++) {
            in->touch[t].id = touch[t].id;
            in->touch[t].x = touch[t].x * sx;
            in->touch[t].y = touch[t].y * sy;
        }
        in->touches = (uint8_t)t;
    }
    host_mutex_unlock(lock);
}

/* ---- calls ---- */
const char *nshvc_key_name(nshvc_key vk)
{
    static const char *const f[12] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    static const char *const one[36] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G", "H",
                                         "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
    static const struct { int vk; const char *name; } named[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Return" }, { 0x10, "Shift" }, { 0x11, "Control" },
        { 0x12, "Option" }, { 0x14, "Caps Lock" }, { 0x1B, "Escape" }, { 0x20, "Space" }, { 0x25, "Left" },
        { 0x26, "Up" }, { 0x27, "Right" }, { 0x28, "Down" }, { 0xBA, ";" }, { 0xBC, "," }, { 0xBE, "." }, { 0xBF, "/" },
    };
    size_t i;
    if (vk >= '0' && vk <= '9')
        return one[vk - '0'];
    if (vk >= 'A' && vk <= 'Z')
        return one[10 + vk - 'A'];
    if (vk >= 0x70 && vk <= 0x7B)
        return f[vk - 0x70];
    for (i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].vk == vk)
            return named[i].name;
    return "";
}

const char *nshvc_controller_name(uint32_t c)
{
    if (!eng_on("nshvc_controller_name", ENG_GAME) || c >= CONTROLLERS || !pad_on[c])
        return "";
    return pads[c].name;
}

const char *nshvc_controller_label(uint32_t c, uint32_t ctl)
{
    static const char *const xbox[16] = { "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "LS", "RS", "LT", "RT", "Up", "Right", "Down", "Left" };
    static const char *const ps[16] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "Create", "Options", "L3", "R3", "L2", "R2", "Up", "Right", "Down", "Left" };
    static const char *const nin[16] = { "B", "A", "Y", "X", "L", "R", "-", "+", "LS", "RS", "ZL", "ZR", "Up", "Right", "Down", "Left" };
    static const char *const gen[16] = { "Bottom", "Right", "Left", "Top", "L", "R", "Back", "Start", "LS", "RS", "LT", "RT", "Up", "Right", "Down", "Left" };
    int b;
    if (!eng_on("nshvc_controller_label", ENG_GAME) || c >= CONTROLLERS || !pad_on[c])
        return "";
    for (b = 0; b < 16 && ctl != 1u << b; b++)
        ;
    if (b == 16) {
        eng_report("nshvc_controller_label", "controller %u, button 0x%x", c, ctl);
        return "";
    }
    if (pads[c].label[b][0])
        return pads[c].label[b];
    return (pads[c].style == HOST_PAD_XBOX ? xbox : pads[c].style == HOST_PAD_PLAYSTATION ? ps : pads[c].style == HOST_PAD_NINTENDO ? nin : gen)[b];
}

int nshvc_window_to_picture(float x, float y, int32_t *picture_x, int32_t *picture_y)
{
    int ww = 0, wh = 0, sw = 256, sh = 224, vx, vy, vw, vh;
    if (!eng_on("nshvc_window_to_picture", ENG_GAME))
        return 0;
    host_window_pixels(&ww, &wh);
    eng_picture_size(&sw, &sh);
    if (ww <= 0 || wh <= 0) {
        /* no window: the picture is the window, a pixel for a pixel */
        vx = vy = 0;
        vw = sw;
        vh = sh;
    } else {
        rr_view(eng_picture_aspect(), ww, wh, &vx, &vy, &vw, &vh);
    }
    x -= (float)vx;
    y -= (float)vy;
    if (x < 0 || y < 0 || x >= (float)vw || y >= (float)vh || vw <= 0 || vh <= 0)
        return 0;
    if (picture_x)
        *picture_x = (int32_t)(x * (float)sw / (float)vw);
    if (picture_y)
        *picture_y = (int32_t)(y * (float)sh / (float)vh);
    return 1;
}

uint32_t nshvc_text_read(char *buffer, uint32_t max)
{
    uint32_t n;
    if (!eng_on("nshvc_text_read", ENG_GAME))
        return 0;
    host_mutex_lock(lock);
    n = ntyped < max ? ntyped : max;
    memcpy(buffer, typed, n);
    memmove(typed, typed + n, ntyped - n);
    ntyped -= n;
    host_mutex_unlock(lock);
    return n;
}
