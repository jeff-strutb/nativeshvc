/*
 * input.c: the native controls as the console's controllers. Whatever is
 * plugged into the controller ports (ports.c), the player's own devices
 * drive it:
 *
 *   joypad      the keyboard (player 0) and game controller N (player N)
 *   multitap    its four players, each a game controller (the keyboard
 *               player 0's too)
 *   mouse       the mouse's motion and buttons
 *   Super Scope the pointer, or a touch, where it aims; the left button
 *               (or the touch) the trigger, the right the cursor button,
 *               the middle (or T) the turbo switch, P the pause
 *   Justifier   the pointer or the first touch gun 0, a second touch gun
 *               1; the left button (or the touch) the trigger, the right
 *               (or Return) start
 *
 * The players are the controllers in port order, each port's device one
 * player (a multitap four): port 0's first. For each, the game can ask
 * which native device is driving it now (the last one used: a keyboard, a
 * game controller and its family, a mouse, a touch screen, a script) and
 * the name of the control that makes each of its buttons, and hear when
 * that changes (nshvc_hooks.input), to show the player the right names.
 *
 *   SNES joypad  keyboard          controller (by position)
 *   D-pad        arrows            d-pad or left stick
 *   B, A         Z, X              bottom, right face buttons
 *   Y, X         A, S              left, top face buttons
 *   L, R         Q, W              shoulders
 *   START        Return            menu
 *   SELECT       Right Shift, Tab  view
 *
 * The keys are the defaults until the game binds others
 * (nshvc_ext_key_bind).
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "host.h"
#include "input.h"

#define PLAYERS 8

/* ---- what drives each player ---- */
static nshvc_input_source source[PLAYERS];
static int source_changed[PLAYERS];
static host_pad pads[PLAYERS];
static int pad_on[PLAYERS];

static void use(int player, int kind, const host_pad *pad)
{
    nshvc_input_source s;
    if (player < 0 || player >= PLAYERS)
        return;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    if (pad) {
        s.style = pad->style == HOST_PAD_XBOX ? NSHVC_PAD_XBOX
                  : pad->style == HOST_PAD_PLAYSTATION ? NSHVC_PAD_PLAYSTATION
                  : pad->style == HOST_PAD_NINTENDO ? NSHVC_PAD_NINTENDO : NSHVC_PAD_GENERIC;
        snprintf(s.name, sizeof s.name, "%s", pad->name);
    } else {
        snprintf(s.name, sizeof s.name, "%s",
                 kind == NSHVC_INPUT_KEYBOARD ? "Keyboard" : kind == NSHVC_INPUT_MOUSE ? "Mouse"
                 : kind == NSHVC_INPUT_TOUCH ? "Touch screen" : kind == NSHVC_INPUT_SCRIPT ? "Script" : "");
    }
    if (memcmp(&s, &source[player], sizeof s)) {
        source[player] = s;
        source_changed[player] = 1;
    }
}

/* ---- the keyboard ---- */
static uint16_t keys;              /* player 0's joypad bits */
static int key_turbo, key_pause, key_start;

/* the keys and the joypad's controls they make: the defaults, then as the
   game binds them (nshvc_ext_key_bind); vk 0 a slot no key holds */
#define KEYS 16
static struct { int vk; uint16_t m; } k_map[KEYS] = {
    { 0x26, NSHVC_UP }, { 0x28, NSHVC_DOWN }, { 0x25, NSHVC_LEFT }, { 0x27, NSHVC_RIGHT }, { 'Z', NSHVC_B },
    { 'X', NSHVC_A }, { 'A', NSHVC_Y }, { 'S', NSHVC_X }, { 'Q', NSHVC_L }, { 'W', NSHVC_R },
    { 0x0D, NSHVC_START }, { 0x10, NSHVC_SELECT }, { 0x09, NSHVC_SELECT },
};

static int is_control(unsigned c)
{
    return c && !(c & (c - 1)) && (c & 0xFFF0);
}

int nshvc_ext_key_for(unsigned control)
{
    int i;
    for (i = 0; i < KEYS; i++)
        if (k_map[i].vk && k_map[i].m == control)
            return k_map[i].vk;
    return 0;
}

void nshvc_ext_key_bind(unsigned control, int vk)
{
    int i, free_slot = -1;
    if (!is_control(control) || vk < 0 || vk > 0xFE)
        return;
    for (i = 0; i < KEYS; i++)
        if (k_map[i].vk && (k_map[i].m == control || k_map[i].vk == vk)) {
            keys &= (uint16_t)~k_map[i].m;      /* nothing left held by a key moved */
            k_map[i].vk = 0;
        }
    for (i = 0; i < KEYS && free_slot < 0; i++)
        if (!k_map[i].vk)
            free_slot = i;
    if (vk && free_slot >= 0) {
        k_map[free_slot].vk = vk;
        k_map[free_slot].m = (uint16_t)control;
    }
}

/* a key's name, by its Win32 virtual key code */
const char *nshvc_ext_key_name(int vk)
{
    static char one[2];
    static const char *const f[12] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    static const char *const pad[10] = { "Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                         "Num 5", "Num 6", "Num 7", "Num 8", "Num 9" };
    static const struct { int vk; const char *name; } named[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Return" }, { 0x10, "Shift" }, { 0x11, "Control" },
        { 0x12, "Option" }, { 0x14, "Caps Lock" }, { 0x1B, "Escape" }, { 0x20, "Space" }, { 0x21, "Page Up" },
        { 0x22, "Page Down" }, { 0x23, "End" }, { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" },
        { 0x27, "Right" }, { 0x28, "Down" }, { 0x2D, "Insert" }, { 0x2E, "Delete" }, { 0x5B, "Command" },
        { 0x6A, "Num *" }, { 0x6B, "Num +" }, { 0x6D, "Num -" }, { 0x6E, "Num ." }, { 0x6F, "Num /" },
        { 0xA0, "Left Shift" }, { 0xA1, "Right Shift" }, { 0xA2, "Left Control" }, { 0xA3, "Right Control" },
        { 0xA4, "Left Option" }, { 0xA5, "Right Option" }, { 0xBA, ";" }, { 0xBB, "=" }, { 0xBC, "," },
        { 0xBD, "-" }, { 0xBE, "." }, { 0xBF, "/" }, { 0xC0, "`" }, { 0xDB, "[" }, { 0xDC, "\\" }, { 0xDD, "]" },
        { 0xDE, "'" },
    };
    size_t i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        one[0] = (char)vk;
        one[1] = 0;
        return one;
    }
    if (vk >= 0x70 && vk <= 0x7B)
        return f[vk - 0x70];
    if (vk >= 0x60 && vk <= 0x69)
        return pad[vk - 0x60];
    for (i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].vk == vk)
            return named[i].name;
    return "";
}

void input_key(int vk, int down)
{
    size_t i;
    uint16_t m = 0;
    for (i = 0; i < KEYS; i++)
        if (k_map[i].vk && k_map[i].vk == vk)
            m |= k_map[i].m;
    if (vk == 'T')
        key_turbo = down;
    if (vk == 'P')
        key_pause = down;
    if (vk == 0x0D)
        key_start = down;
    if (down)
        keys |= m;
    else
        keys &= (uint16_t)~m;
    if (down && (m || vk == 'T' || vk == 'P'))
        use(0, NSHVC_INPUT_KEYBOARD, NULL);
}

void input_focus_lost(void)
{
    keys = 0;
    key_turbo = key_pause = key_start = 0;
}

/* ---- the mouse and touches ---- */
static float mouse_x, mouse_y, mouse_dx, mouse_dy;
static int mouse_buttons, mouse_seen;
static struct { int id; float x, y; } touch[2];
static int touches;

void input_mouse(float x, float y, float dx, float dy, int buttons)
{
    mouse_x = x;
    mouse_y = y;
    mouse_dx += dx;
    mouse_dy += dy;
    if (buttons & ~mouse_buttons || dx || dy)
        mouse_seen = 1;
    mouse_buttons = buttons;
}

void input_touch(int id, int phase, float x, float y)
{
    int i;
    for (i = 0; i < touches; i++)
        if (touch[i].id == id)
            break;
    if (phase == 2) {
        if (i < touches) {
            touch[i] = touch[touches - 1];
            touches--;
        }
        return;
    }
    if (i == touches) {
        if (touches == 2)
            return;
        touches++;
    }
    touch[i].id = id;
    touch[i].x = x;
    touch[i].y = y;
}

/* ---- the game controllers ---- */
static uint16_t pad_joypad(const host_pad *p)
{
    uint16_t b = 0;
    if (p->buttons & 1u << 0) b |= NSHVC_B;
    if (p->buttons & 1u << 1) b |= NSHVC_A;
    if (p->buttons & 1u << 2) b |= NSHVC_Y;
    if (p->buttons & 1u << 3) b |= NSHVC_X;
    if (p->buttons & 1u << 4) b |= NSHVC_L;
    if (p->buttons & 1u << 5) b |= NSHVC_R;
    if (p->buttons & 1u << 6) b |= NSHVC_SELECT;
    if (p->buttons & 1u << 7) b |= NSHVC_START;
    if ((p->buttons & 1u << 12) || p->y < -0.5f) b |= NSHVC_UP;
    if ((p->buttons & 1u << 13) || p->x > 0.5f) b |= NSHVC_RIGHT;
    if ((p->buttons & 1u << 14) || p->y > 0.5f) b |= NSHVC_DOWN;
    if ((p->buttons & 1u << 15) || p->x < -0.5f) b |= NSHVC_LEFT;
    return b;
}

/* the controller bit each joypad button comes from (-1: the stick) */
static int pad_bit_of(uint16_t button)
{
    switch (button) {
    case NSHVC_B: return 0;
    case NSHVC_A: return 1;
    case NSHVC_Y: return 2;
    case NSHVC_X: return 3;
    case NSHVC_L: return 4;
    case NSHVC_R: return 5;
    case NSHVC_SELECT: return 6;
    case NSHVC_START: return 7;
    case NSHVC_UP: return 12;
    case NSHVC_RIGHT: return 13;
    case NSHVC_DOWN: return 14;
    case NSHVC_LEFT: return 15;
    }
    return -1;
}

/* a controller's button names by family, where the controller gives none */
static const char *pad_default_label(int style, int bit)
{
    static const char *const xbox[16] = { "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "LS", "RS", "LT", "RT",
                                          "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left" };
    static const char *const ps[16] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "Create", "Options",
                                        "L3", "R3", "L2", "R2", "D-pad Up", "D-pad Right", "D-pad Down",
                                        "D-pad Left" };
    static const char *const nin[16] = { "B", "A", "Y", "X", "L", "R", "-", "+", "Left Stick", "Right Stick", "ZL",
                                         "ZR", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left" };
    static const char *const gen[16] = { "Bottom", "Right", "Left", "Top", "Left Shoulder", "Right Shoulder",
                                         "Back", "Start", "Left Stick", "Right Stick", "Left Trigger",
                                         "Right Trigger", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left" };
    const char *const *t = style == NSHVC_PAD_XBOX ? xbox : style == NSHVC_PAD_PLAYSTATION ? ps
                           : style == NSHVC_PAD_NINTENDO ? nin : gen;
    return bit >= 0 && bit < 16 ? t[bit] : "";
}

/* ---- the players and the ports ---- */
/* the port and device player `player` is on (-1 none), its slot on a
   multitap */
static int player_port(int player, int *device, int *slot)
{
    int p, first = 0;
    for (p = 0; p < 2; p++) {
        int d = nshvc_plugged(p), n = d == NSHVC_PORT_MULTITAP ? 4 : 1;
        if (player < first + n) {
            *device = d;
            *slot = player - first;
            return p;
        }
        first += n;
    }
    return -1;
}

/* (x, y) in the window's points to the console's dot and line: the
   picture drawn at at[] (left, top, right, bottom) on a target `scale`
   pixels a point, `width` x `screen_height` with the console's column 0
   at column `left` and its first line at row `top`; 0 off the picture */
int input_point(const int *at, float scale, int width, int screen_height, int left, int top, float x, float y, int *dot,
                int *line)
{
    double u, v;
    if (at[2] <= at[0] || at[3] <= at[1])
        return 0;
    u = ((double)x * scale - at[0]) / (at[2] - at[0]);
    v = ((double)y * scale - at[1]) / (at[3] - at[1]);
    if (u < 0 || u >= 1 || v < 0 || v >= 1)
        return 0;
    *dot = (int)(u * width) - left;
    *line = (int)(v * screen_height) - top;
    return 1;
}

static void gun_aim(int gun, float x, float y, int *dot, int *line)
{
    (void)gun;
    if (!console_picture_at(x, y, dot, line)) {
        *dot = -1;
        *line = -1;
    }
}

/* each frame: the players' devices to the console's controllers */
void input_frame(const uint16_t *game_buttons)
{
    host_pad now[PLAYERS];
    int on[PLAYERS], k;
    for (k = 0; k < PLAYERS; k++)
        on[k] = host_pad_read(k, &now[k]);
    input_frame_pads(now, on, game_buttons);
}

void input_frame_pads(const host_pad *now, const int *on, const uint16_t *game_buttons)
{
    int player, p, k;
    uint16_t joy[PLAYERS] = { 0 };
    for (k = 0; k < PLAYERS; k++) {
        uint16_t was = pad_on[k] ? pad_joypad(&pads[k]) : 0;
        unsigned was_b = pad_on[k] ? pads[k].buttons : 0;
        pad_on[k] = on[k];
        if (on[k])
            pads[k] = now[k];
        if (pad_on[k]) {
            joy[k] = pad_joypad(&pads[k]);
            if ((joy[k] & ~was) || (pads[k].buttons & ~was_b))
                use(k, NSHVC_INPUT_GAMEPAD, &pads[k]);
        }
    }
    joy[0] |= keys;
    for (player = 0; player < PLAYERS; player++) {
        int device, slot;
        p = player_port(player, &device, &slot);
        if (p < 0)
            break;
        switch (device) {
        case NSHVC_PORT_JOYPAD:
        case NSHVC_PORT_MULTITAP:
            nshvc_set_joypad(player, (uint16_t)(joy[player] | game_buttons[player]));
            break;
        case NSHVC_PORT_MOUSE:
            nshvc_set_mouse(p, (int)mouse_dx, (int)mouse_dy, mouse_buttons & 3);
            if (mouse_seen)
                use(player, NSHVC_INPUT_MOUSE, NULL);
            break;
        case NSHVC_PORT_SUPERSCOPE:
        case NSHVC_PORT_JUSTIFIER:
        case NSHVC_PORT_JUSTIFIERS: {
            int g, x, y;
            for (g = 0; g < (device == NSHVC_PORT_JUSTIFIERS ? 2 : 1); g++) {
                unsigned b = 0;
                if (g < touches) {
                    gun_aim(g, touch[g].x, touch[g].y, &x, &y);
                    b = NSHVC_GUN_TRIGGER;
                    use(player, NSHVC_INPUT_TOUCH, NULL);
                } else if (g == 0) {
                    gun_aim(g, mouse_x, mouse_y, &x, &y);
                    if (mouse_buttons & 1) b |= NSHVC_GUN_TRIGGER;
                    if (device == NSHVC_PORT_SUPERSCOPE) {
                        if (mouse_buttons & 2) b |= NSHVC_GUN_CURSOR;
                        if ((mouse_buttons & 4) || key_turbo) b |= NSHVC_GUN_TURBO;
                        if (key_pause) b |= NSHVC_GUN_PAUSE;
                    } else if ((mouse_buttons & 2) || key_start) {
                        b |= NSHVC_GUN_START;
                    }
                    if (mouse_seen)
                        use(player, NSHVC_INPUT_MOUSE, NULL);
                } else {
                    x = y = -1;
                }
                nshvc_set_gun(g, x, y, b);
            }
            break;
        }
        }
    }
    mouse_dx = mouse_dy = 0;
    mouse_seen = 0;
}

/* a headless run's: the script drives the controllers */
void input_script(void)
{
    int k;
    for (k = 0; k < PLAYERS; k++)
        use(k, NSHVC_INPUT_SCRIPT, NULL);
}

/* the players whose device changed, to the game's hook */
void input_report(void (*hook)(int player, const nshvc_input_source *src))
{
    int k;
    for (k = 0; k < PLAYERS; k++)
        if (source_changed[k]) {
            source_changed[k] = 0;
            if (hook)
                hook(k, &source[k]);
        }
}

/* ---- nshvc.h ---- */
int nshvc_player_source(int player, nshvc_input_source *src)
{
    int device, slot;
    if (player < 0 || player >= PLAYERS || player_port(player, &device, &slot) < 0) {
        memset(src, 0, sizeof *src);
        return 0;
    }
    *src = source[player];
    return 1;
}

const char *nshvc_player_label(int player, unsigned control)
{
    int device, slot;
    const nshvc_input_source *s;
    size_t i;
    if (player < 0 || player >= PLAYERS || player_port(player, &device, &slot) < 0)
        return "";
    s = &source[player];
    if (device == NSHVC_PORT_MOUSE)
        return control == 1 ? "Left button" : control == 2 ? "Right button" : "";
    if (device >= NSHVC_PORT_SUPERSCOPE) {
        if (s->kind == NSHVC_INPUT_TOUCH)
            return control == NSHVC_GUN_TRIGGER ? "Tap" : "";
        switch (control) {
        case NSHVC_GUN_TRIGGER: return "Left button";
        case NSHVC_GUN_CURSOR: return "Right button";
        case NSHVC_GUN_TURBO: return "Middle button, T";
        case NSHVC_GUN_PAUSE: return "P";
        case NSHVC_GUN_START: return "Right button, Return";
        }
        return "";
    }
    if (s->kind == NSHVC_INPUT_GAMEPAD && pad_on[player]) {
        int bit = pad_bit_of((uint16_t)control);
        if (bit >= 0 && pads[player].label[bit][0])
            return pads[player].label[bit];
        return pad_default_label(s->style, bit);
    }
    if (player == 0)
        for (i = 0; i < KEYS; i++)
            if (k_map[i].vk && k_map[i].m == control)
                return nshvc_ext_key_name(k_map[i].vk);
    return "";
}
