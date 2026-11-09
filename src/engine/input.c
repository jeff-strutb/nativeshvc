/* input.c: every keyboard, controller, mouse and touch screen mapped to the
 * game's controls. The system thread hands in events; the game thread samples
 * the state once per update, so a recorded run replays exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../render/rr_window.h"
#include "engine.h"
#include "host.h"

#define PLAYERS 8
#define BUTTONS 12

static const uint16_t bit_of[BUTTONS] = { NSHVC_B, NSHVC_Y, NSHVC_SELECT, NSHVC_START, NSHVC_UP, NSHVC_DOWN, NSHVC_LEFT,
                                          NSHVC_RIGHT, NSHVC_A, NSHVC_X, NSHVC_L, NSHVC_R };
/* the controller's bit for each of those */
static const int8_t pad_bit[BUTTONS] = { 0, 2, 6, 7, 12, 14, 15, 13, 1, 3, 4, 5 };

static host_mutex *lock;

/* the system thread's, under lock */
static int vk_of[BUTTONS] = { 'Z', 'A', 0x10, 0x0D, 0x26, 0x28, 0x25, 0x27, 'X', 'S', 'Q', 'W' };
static uint8_t key_down[256];
static float mouse_x, mouse_y, mouse_dx, mouse_dy;
static int mouse_buttons, mouse_seen, key_seen;
static struct { int id; float x, y; } touch[16];
static int touches;
static int touch_host;
static char typed[64];
static uint32_t ntyped;

/* the game thread's */
static nshvc_device devices[PLAYERS];
static uint32_t nplayers = 1;
static host_pad pads[PLAYERS];
static int pad_on[PLAYERS];
static nshvc_source sources[PLAYERS];
static FILE *replay;
static uint64_t replay_next = UINT64_MAX;
static uint16_t replay_pads[PLAYERS], replay_held[PLAYERS];

void eng_input_start(const char *replay_path)
{
    lock = host_mutex_new();
    if (replay_path && !(replay = fopen(replay_path, "r")))
        fprintf(stderr, "nativeshvc: cannot read %s\n", replay_path);
}

static const char *const vk_names(int vk, char *one)
{
    static const char *const f[12] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    static const struct { int vk; const char *name; } named[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Return" }, { 0x10, "Shift" }, { 0x11, "Control" },
        { 0x12, "Option" }, { 0x14, "Caps Lock" }, { 0x1B, "Escape" }, { 0x20, "Space" }, { 0x25, "Left" },
        { 0x26, "Up" }, { 0x27, "Right" }, { 0x28, "Down" }, { 0xBA, ";" }, { 0xBC, "," }, { 0xBE, "." }, { 0xBF, "/" },
    };
    size_t i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        one[0] = (char)vk;
        one[1] = 0;
        return one;
    }
    if (vk >= 0x70 && vk <= 0x7B)
        return f[vk - 0x70];
    for (i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].vk == vk)
            return named[i].name;
    return "";
}

/* the on-screen controls' places on a touch screen, as fractions of the
   window: centre x, y, radius (of the height) */
typedef struct { float x, y, r; int button; } Zone;
static const Zone zones[] = {
    { 0.10f, 0.62f, 0.07f, 4 }, { 0.10f, 0.86f, 0.07f, 5 }, { 0.04f, 0.74f, 0.07f, 6 }, { 0.16f, 0.74f, 0.07f, 7 },
    { 0.84f, 0.86f, 0.08f, 0 }, { 0.93f, 0.74f, 0.08f, 8 }, { 0.75f, 0.74f, 0.08f, 1 }, { 0.84f, 0.62f, 0.08f, 9 },
    { 0.06f, 0.30f, 0.08f, 10 }, { 0.94f, 0.30f, 0.08f, 11 }, { 0.42f, 0.92f, 0.05f, 2 }, { 0.58f, 0.92f, 0.05f, 3 },
};
#define ZONES ((int)(sizeof zones / sizeof zones[0]))

static uint16_t touch_buttons(void)
{
    float w = 0, h = 0;
    uint16_t out = 0;
    int t, z;
    host_window_points(&w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    for (t = 0; t < touches; t++)
        for (z = 0; z < ZONES; z++) {
            float dx = touch[t].x - zones[z].x * w, dy = touch[t].y - zones[z].y * h, r = zones[z].r * h * 1.4f;
            if (dx * dx + dy * dy <= r * r)
                out |= bit_of[zones[z].button];
        }
    return out;
}

/* on the system thread */
void eng_input_event(const host_event *e)
{
    host_mutex_lock(lock);
    switch (e->type) {
    case HOST_EV_KEY:
        if (e->vk >= 0 && e->vk < 256)
            key_down[e->vk] = (uint8_t)(e->down != 0);
        if (e->down) {
            key_seen = 1;
            if (e->vk == 0x20 || e->vk == 0x08 || e->vk == 0x0D || (e->vk >= '0' && e->vk <= '9') || (e->vk >= 'A' && e->vk <= 'Z'))
                if (ntyped < sizeof typed)
                    typed[ntyped++] = (char)e->vk;
        }
        break;
    case HOST_EV_FOCUS:
        if (!e->down)
            memset(key_down, 0, sizeof key_down);
        break;
    case HOST_EV_MOUSE:
        mouse_x = e->x;
        mouse_y = e->y;
        mouse_dx += e->dx;
        mouse_dy += e->dy;
        if ((e->buttons & ~mouse_buttons) || e->dx != 0 || e->dy != 0)
            mouse_seen = 1;
        mouse_buttons = e->buttons;
        break;
    case HOST_EV_TOUCH: {
        int i;
        touch_host = 1;
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

static uint16_t pad_bits(const host_pad *p)
{
    uint16_t b = 0;
    int i;
    for (i = 0; i < BUTTONS; i++)
        if (p->buttons >> pad_bit[i] & 1u)
            b |= bit_of[i];
    if (p->y < -0.5f) b |= NSHVC_UP;
    if (p->y > 0.5f) b |= NSHVC_DOWN;
    if (p->x < -0.5f) b |= NSHVC_LEFT;
    if (p->x > 0.5f) b |= NSHVC_RIGHT;
    return b;
}

static void set_source(uint32_t player, int kind, const host_pad *p)
{
    nshvc_source *s = &sources[player];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    if (p) {
        s->style = p->style;
        snprintf(s->name, sizeof s->name, "%s", p->name);
    } else {
        snprintf(s->name, sizeof s->name, "%s", kind == NSHVC_SOURCE_KEYBOARD ? "Keyboard" : kind == NSHVC_SOURCE_MOUSE ? "Mouse"
                 : kind == NSHVC_SOURCE_TOUCH ? "Touch screen" : "");
    }
}

/* the pointer in the screen's pixels */
static void pointer_screen(float px, float py, int32_t *x, int32_t *y, uint8_t *on)
{
    float w = 0, h = 0;
    int sw = 256, sh = 224;
    host_window_points(&w, &h);
    eng_picture_size(&sw, &sh);
    if (w <= 0 || h <= 0 || px < 0 || py < 0 || px >= w || py >= h) {
        *x = *y = -1;
        *on = 0;
        return;
    }
    {
        int vx, vy, vw, vh;
        rr_view(eng_picture_aspect(), (int)w, (int)h, &vx, &vy, &vw, &vh);
        px -= (float)vx;
        py -= (float)vy;
        if (px < 0 || py < 0 || px >= (float)vw || py >= (float)vh) {
            *x = *y = -1;
            *on = 0;
            return;
        }
        *x = (int32_t)(px * (float)sw / (float)vw);
        *y = (int32_t)(py * (float)sh / (float)vh);
    }
    *on = 1;
}

/* on the game thread: the input for update `n` */
void eng_input_sample(nshvc_input *in, uint64_t n, int headless)
{
    uint32_t p;
    memset(in, 0, sizeof *in);
    if (replay) {
        for (;;) {
            char line[256];
            if (replay_next == UINT64_MAX) {
                char *c;
                int k = 0;
                if (!fgets(line, sizeof line, replay))
                    break;
                replay_next = strtoull(line, &c, 10);
                memset(replay_pads, 0, sizeof replay_pads);
                while (k < PLAYERS && *c) {
                    char *e;
                    unsigned long v = strtoul(c, &e, 16);
                    if (e == c)
                        break;
                    replay_pads[k++] = (uint16_t)v;
                    c = e;
                }
            }
            if (replay_next > n)
                break;
            memcpy(replay_held, replay_pads, sizeof replay_held);
            replay_next = UINT64_MAX;
        }
        memcpy(in->pad, replay_held, sizeof in->pad);
        return;
    }
    if (headless)
        return;
    for (p = 0; p < PLAYERS; p++) {
        host_pad now;
        uint16_t was = pad_on[p] ? pad_bits(&pads[p]) : 0;
        pad_on[p] = host_pad_read((int)p, &now);
        if (pad_on[p]) {
            pads[p] = now;
            in->pad[p] = pad_bits(&now);
            if (in->pad[p] & ~was)
                set_source(p, NSHVC_SOURCE_CONTROLLER, &now);
        }
    }
    host_mutex_lock(lock);
    {
        int b;
        for (b = 0; b < BUTTONS; b++)
            if (vk_of[b] && key_down[vk_of[b]])
                in->pad[0] |= bit_of[b];
        if (vk_of[2] == 0x10 && key_down[0x09])
            in->pad[0] |= NSHVC_SELECT;
    }
    if (key_seen) {
        set_source(0, NSHVC_SOURCE_KEYBOARD, NULL);
        key_seen = 0;
    }
    if (touch_host) {
        uint16_t t = touch_buttons();
        in->pad[0] |= t;
        if (t)
            set_source(0, NSHVC_SOURCE_TOUCH, NULL);
    }
    {
        uint32_t m = 0, g = 0;
        for (p = 0; p < nplayers; p++) {
            if (devices[p] == NSHVC_MOUSE && m < 2) {
                int dx = (int)mouse_dx, dy = (int)mouse_dy;
                in->mouse[m].dx = (int16_t)(dx < -32768 ? -32768 : dx > 32767 ? 32767 : dx);
                in->mouse[m].dy = (int16_t)(dy < -32768 ? -32768 : dy > 32767 ? 32767 : dy);
                in->mouse[m].buttons = (uint8_t)(mouse_buttons & 3);
                if (mouse_seen)
                    set_source(p, NSHVC_SOURCE_MOUSE, NULL);
                m++;
            } else if ((devices[p] == NSHVC_SUPERSCOPE || devices[p] == NSHVC_JUSTIFIER) && g < 2) {
                if ((int)g < touches) {
                    pointer_screen(touch[g].x, touch[g].y, &in->gun[g].x, &in->gun[g].y, &in->gun[g].on_screen);
                    in->gun[g].trigger = 1;
                    set_source(p, NSHVC_SOURCE_TOUCH, NULL);
                } else if (g == 0) {
                    pointer_screen(mouse_x, mouse_y, &in->gun[g].x, &in->gun[g].y, &in->gun[g].on_screen);
                    in->gun[g].trigger = mouse_buttons & 1;
                    if (devices[p] == NSHVC_SUPERSCOPE) {
                        in->gun[g].cursor = (mouse_buttons >> 1) & 1;
                        in->gun[g].turbo = ((mouse_buttons >> 2) & 1) | key_down['T'];
                        in->gun[g].pause = key_down['P'];
                    } else {
                        in->gun[g].start = ((mouse_buttons >> 1) & 1) | key_down[0x0D];
                    }
                    if (mouse_seen)
                        set_source(p, NSHVC_SOURCE_MOUSE, NULL);
                } else {
                    in->gun[g].x = in->gun[g].y = -1;
                }
                g++;
            }
        }
    }
    mouse_dx = mouse_dy = 0;
    mouse_seen = 0;
    host_mutex_unlock(lock);
}

/* ---- calls ---- */
void nshvc_players(uint32_t count, const nshvc_device *d)
{
    if (!eng_on("nshvc_players", ENG_GAME))
        return;
    if (count > PLAYERS || (count && !d)) {
        eng_report("nshvc_players", "%u players", count);
        return;
    }
    nplayers = count;
    if (count)
        memcpy(devices, d, count * sizeof *d);
}

static int index_of(uint32_t button)
{
    int i;
    for (i = 0; i < BUTTONS; i++)
        if (bit_of[i] == button)
            return i;
    return -1;
}

const char *nshvc_control_name(uint32_t player, uint32_t button)
{
    static char one[2];
    static const char *const xbox[16] = { "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "LS", "RS", "LT", "RT", "Up", "Right", "Down", "Left" };
    static const char *const ps[16] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "Create", "Options", "L3", "R3", "L2", "R2", "Up", "Right", "Down", "Left" };
    static const char *const nin[16] = { "B", "A", "Y", "X", "L", "R", "-", "+", "LS", "RS", "ZL", "ZR", "Up", "Right", "Down", "Left" };
    static const char *const gen[16] = { "Bottom", "Right", "Left", "Top", "L", "R", "Back", "Start", "LS", "RS", "LT", "RT", "Up", "Right", "Down", "Left" };
    int i = index_of(button);
    if (!eng_on("nshvc_control_name", ENG_GAME) || i < 0 || player >= PLAYERS)
        return "";
    if (sources[player].kind == NSHVC_SOURCE_CONTROLLER && pad_on[player]) {
        int b = pad_bit[i];
        const char *const *t = pads[player].style == HOST_PAD_XBOX ? xbox : pads[player].style == HOST_PAD_PLAYSTATION ? ps
                               : pads[player].style == HOST_PAD_NINTENDO ? nin : gen;
        return pads[player].label[b][0] ? pads[player].label[b] : t[b];
    }
    if (sources[player].kind == NSHVC_SOURCE_TOUCH)
        return "Touch";
    return player == 0 ? vk_names(vk_of[i], one) : "";
}

nshvc_source nshvc_control_source(uint32_t player)
{
    nshvc_source none;
    memset(&none, 0, sizeof none);
    if (!eng_on("nshvc_control_source", ENG_GAME) || player >= PLAYERS)
        return none;
    return sources[player];
}

void nshvc_control_bind(uint32_t player, uint32_t button, nshvc_key key)
{
    int i = index_of(button), k;
    if (!eng_on("nshvc_control_bind", ENG_GAME))
        return;
    if (i < 0 || player != 0 || key < 0 || key > 255) {
        eng_report("nshvc_control_bind", "player %u, button %u, key %d", player, button, key);
        return;
    }
    host_mutex_lock(lock);
    for (k = 0; k < BUTTONS; k++)
        if (vk_of[k] == key)
            vk_of[k] = 0;
    vk_of[i] = key;
    host_mutex_unlock(lock);
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

/* the on-screen controls: 1 once a touch screen is in use */
int eng_input_touch_host(void)
{
    int on;
    host_mutex_lock(lock);
    on = touch_host;
    host_mutex_unlock(lock);
    return on;
}

int eng_input_zones(void) { return ZONES; }
void eng_input_zone(int z, float *x, float *y, float *r)
{
    *x = zones[z].x;
    *y = zones[z].y;
    *r = zones[z].r;
}
