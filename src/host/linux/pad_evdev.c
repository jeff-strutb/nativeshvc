/*
 * pad_evdev.c: game controllers on Linux through the kernel's evdev nodes (/dev/input/event*),
 * with the kernel's own constants written here so a game builds without kernel headers.
 *
 * Which nodes are controllers: a node with the gamepad's buttons (BTN_SOUTH) and a left stick,
 * from a driver whose layout is known (xpad; playstation and sony for DualShock and DualSense;
 * nintendo for the Switch controllers), is read by the kernel's button codes. Any other node is a
 * controller only when SDL_GameControllerDB (pad_db_linux.h) lists its vendor and product, and
 * is read through that entry's layout. Anything else, such as a motion sensor, a touch pad or a
 * mouse, is not one.
 *
 * A controller keeps its index (0-7) while it is connected. /dev/input is watched (inotify) so a
 * controller plugged in later appears and one unplugged is released.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "pad_map.h"
#include "pad_db_linux.h"

/* from <linux/input.h> */
struct ev_input_event { struct timeval time; uint16_t type, code; int32_t value; };
struct ev_input_id { uint16_t bustype, vendor, product, version; };
struct ev_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
#define EV_IOC(dir, nr, size) (((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ('E' << 8) | (unsigned)(nr))
#define EVIOCGID EV_IOC(2, 0x02, sizeof(struct ev_input_id))
#define EVIOCGNAME(len) EV_IOC(2, 0x06, len)
#define EVIOCGPHYS(len) EV_IOC(2, 0x07, len)
#define EVIOCGUNIQ(len) EV_IOC(2, 0x08, len)
#define EVIOCGPROP(len) EV_IOC(2, 0x09, len)
#define EVIOCGKEY(len) EV_IOC(2, 0x18, len)
#define EVIOCGBIT(ev, len) EV_IOC(2, 0x20 + (ev), len)
#define EVIOCGABS(abs) EV_IOC(2, 0x40 + (abs), sizeof(struct ev_absinfo))
enum { EV_SYN = 0, EV_KEY = 1, EV_ABS = 3, SYN_DROPPED = 3, INPUT_PROP_ACCELEROMETER = 6 };
enum { KEYS = 0x300, ABSES = 0x40 };
enum {
    ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RX = 3, ABS_RY = 4, ABS_RZ = 5, ABS_GAS = 9, ABS_BRAKE = 10,
    ABS_HAT0X = 0x10, ABS_HAT0Y = 0x11, ABS_HAT3Y = 0x17
};
enum {
    BTN_JOYSTICK = 0x120, BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_NORTH = 0x133, BTN_WEST = 0x134,
    BTN_TL = 0x136, BTN_TR = 0x137, BTN_TL2 = 0x138, BTN_TR2 = 0x139, BTN_SELECT = 0x13a,
    BTN_START = 0x13b, BTN_THUMBL = 0x13d, BTN_THUMBR = 0x13e,
    BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221, BTN_DPAD_LEFT = 0x222, BTN_DPAD_RIGHT = 0x223
};

enum { SLOTS = 8, KIND_POSITIONAL = 1, KIND_XPAD, KIND_MAPPED };

static struct slot {
    int      fd, kind;
    char     path[40];
    char     name[64];
    int      style;
    uint64_t id;
    pad_map  map;                      /* KIND_MAPPED */
    uint8_t  has_abs[ABSES];
    int32_t  amin[ABSES], amax[ABSES], aval[ABSES];
    uint8_t  key[KEYS];
    int      nbuttons, nabs;           /* KIND_MAPPED: SDL's numbering of the device's buttons and axes */
    uint16_t button_code[KEYS];
    uint8_t  axis_code[ABSES];
} slots[SLOTS];

static int  started, ino = -1;
static long long next_scan;           /* when /dev/input is next looked through; 0 now, -1 never */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int bit(const uint8_t *a, int n) { return (a[n >> 3] >> (n & 7)) & 1; }

static void slot_close(struct slot *s)
{
    if (s->fd >= 0)
        close(s->fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

/* the device's present buttons and axes, read again when the kernel has dropped events */
static void load_state(struct slot *s)
{
    uint8_t keys[KEYS / 8];
    int i;
    memset(s->key, 0, sizeof s->key);
    if (ioctl(s->fd, EVIOCGKEY(sizeof keys), keys) >= 0)
        for (i = 0; i < KEYS; i++)
            s->key[i] = (uint8_t)bit(keys, i);
    for (i = 0; i < ABSES; i++)
        if (s->has_abs[i]) {
            struct ev_absinfo a;
            if (ioctl(s->fd, EVIOCGABS(i), &a) >= 0) {
                s->amin[i] = a.minimum;
                s->amax[i] = a.maximum;
                s->aval[i] = a.value;
            }
        }
}

/* the name of the kernel driver bound to the device behind the node, "" if unknown */
static void driver_of(const char *event, char *out, size_t n)
{
    char link[128], target[128];
    ssize_t r;
    out[0] = 0;
    snprintf(link, sizeof link, "/sys/class/input/%s/device/device/driver", event);
    r = readlink(link, target, sizeof target - 1);
    if (r > 0) {
        const char *base;
        target[r] = 0;
        base = strrchr(target, '/');
        snprintf(out, n, "%s", base ? base + 1 : target);
    }
}

/* a joystick's or gamepad's buttons (BTN_JOYSTICK up to BTN_DIGI), as a touch pad's or a mouse's are not:
   the DualShock's touch pad is a node of its own, with a stick's axes and none of these */
static int has_pad_keys(const uint8_t *keys)
{
    int i;
    for (i = BTN_JOYSTICK; i < 0x140; i++)
        if (bit(keys, i))
            return 1;
    return 0;
}

/* make a slot of the node if it is a controller; 1 when it is one (fd kept in the slot) */
static int try_open(struct slot *s, const char *event)
{
    char path[48], driver[32], uniq[64] = "", phys[64] = "", text[160];
    uint8_t keys[KEYS / 8] = { 0 }, abs[ABSES / 8] = { 0 }, prop[8] = { 0 };
    struct ev_input_id id;
    int fd, i, n;
    snprintf(path, sizeof path, "/dev/input/%s", event);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return 0;
    memset(s, 0, sizeof *s);
    s->fd = fd;
    snprintf(s->path, sizeof s->path, "%s", path);
    if (ioctl(fd, EVIOCGID, &id) < 0 || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0)
        goto no;
    if (ioctl(fd, EVIOCGPROP(sizeof prop), prop) >= 0 && bit(prop, INPUT_PROP_ACCELEROMETER))
        goto no;
    for (i = 0; i < ABSES; i++)
        s->has_abs[i] = (uint8_t)bit(abs, i);
    driver_of(event, driver, sizeof driver);
    if (bit(keys, BTN_SOUTH) && s->has_abs[ABS_X] && s->has_abs[ABS_Y] &&
        (!strcmp(driver, "playstation") || !strcmp(driver, "sony") || !strcmp(driver, "nintendo")))
        s->kind = KIND_POSITIONAL;
    else if (bit(keys, BTN_SOUTH) && s->has_abs[ABS_X] && s->has_abs[ABS_Y] && !strcmp(driver, "xpad"))
        s->kind = KIND_XPAD;
    else if (s->has_abs[ABS_X] && s->has_abs[ABS_Y] && has_pad_keys(keys) &&
             pad_map_find(pad_db_linux, sizeof pad_db_linux / sizeof *pad_db_linux, id.vendor, id.product, id.version, &s->map)) {
        s->kind = KIND_MAPPED;
        /* SDL's numbering: buttons from BTN_JOYSTICK up, then those below it; axes by code, the hats left out */
        for (n = 0, i = BTN_JOYSTICK; i < KEYS; i++)
            if (bit(keys, i))
                s->button_code[n++] = (uint16_t)i;
        for (i = 0; i < BTN_JOYSTICK; i++)
            if (bit(keys, i))
                s->button_code[n++] = (uint16_t)i;
        s->nbuttons = n;
        for (n = 0, i = 0; i < ABSES; i++) {
            if (i == ABS_HAT0X) {
                i = ABS_HAT3Y;
                continue;
            }
            if (s->has_abs[i])
                s->axis_code[n++] = (uint8_t)i;
        }
        s->nabs = n;
    } else
        goto no;
    if (ioctl(fd, EVIOCGNAME(sizeof s->name - 1), s->name) < 0 || !s->name[0])
        snprintf(s->name, sizeof s->name, "%s", s->kind == KIND_MAPPED ? s->map.name : "Controller");
    ioctl(fd, EVIOCGUNIQ(sizeof uniq - 1), uniq);
    ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
    snprintf(text, sizeof text, "%04x:%04x:%s", id.vendor, id.product, uniq[0] ? uniq : phys);
    s->id = pad_hash(text);
    s->style = pad_style_of_vendor(id.vendor);
    load_state(s);
    return 1;
no:
    close(fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
    return 0;
}

static void scan(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        int k, free_k = -1, have = 0;
        char path[48];
        if (strncmp(e->d_name, "event", 5))
            continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        for (k = 0; k < SLOTS; k++) {
            if (slots[k].fd >= 0 && !strcmp(slots[k].path, path))
                have = 1;
            else if (slots[k].fd < 0 && free_k < 0)
                free_k = k;
        }
        if (!have && free_k >= 0)
            try_open(&slots[free_k], e->d_name);
    }
    closedir(d);
}

/* looks /dev/input through when it has changed (a node made or its permissions set, 300 ms later,
   as the system finishes it), and at least every 2 s when it cannot be watched */
static void rescan_if_due(void)
{
    long long now = now_ms();
    if (ino >= 0) {
        char buf[512];
        int changed = 0;
        ssize_t r;
        while ((r = read(ino, buf, sizeof buf)) > 0)
            changed = 1;
        if (changed)
            next_scan = now + 300;
    }
    if (next_scan >= 0 && now >= next_scan) {
        scan();
        next_scan = ino >= 0 ? -1 : now + 2000;
    }
}

static void init(void)
{
    int k;
    started = 1;
    for (k = 0; k < SLOTS; k++)
        slots[k].fd = -1;
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0 && inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
        close(ino);
        ino = -1;
    }
    next_scan = 0;
}

static float norm(const struct slot *s, int a)
{
    float span = (float)(s->amax[a] - s->amin[a]);
    float v = span > 0 ? ((float)(s->aval[a] - s->amin[a]) / span) * 2.0f - 1.0f : 0.0f;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

static float trigger(const struct slot *s, int a, int digital_code)
{
    if (a >= 0 && s->has_abs[a])
        return (norm(s, a) + 1.0f) * 0.5f;
    return s->key[digital_code] ? 1.0f : 0.0f;
}

static void state_standard(const struct slot *s, host_pad *p)
{
    static const struct { int code, bit; } fixed[] = {
        { BTN_SOUTH, 0 }, { BTN_EAST, 1 }, { BTN_TL, 4 }, { BTN_TR, 5 }, { BTN_SELECT, 6 }, { BTN_START, 7 },
        { BTN_THUMBL, 8 }, { BTN_THUMBR, 9 }, { BTN_DPAD_UP, 12 }, { BTN_DPAD_RIGHT, 13 },
        { BTN_DPAD_DOWN, 14 }, { BTN_DPAD_LEFT, 15 },
    };
    unsigned b = 0;
    size_t i;
    int lt = s->has_abs[ABS_BRAKE] ? ABS_BRAKE : s->has_abs[ABS_Z] ? ABS_Z : -1;
    int rt = s->has_abs[ABS_GAS] ? ABS_GAS : s->has_abs[ABS_RZ] ? ABS_RZ : -1;
    for (i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (s->key[fixed[i].code])
            b |= 1u << fixed[i].bit;
    /* the kernel's codes by place: north and west. xpad names its buttons for the Xbox's letters,
       so its code BTN_NORTH (BTN_X) is the west button (X) and BTN_WEST (BTN_Y) the north (Y) */
    if (s->key[BTN_NORTH]) b |= 1u << (s->kind == KIND_XPAD ? 2 : 3);
    if (s->key[BTN_WEST]) b |= 1u << (s->kind == KIND_XPAD ? 3 : 2);
    p->x = norm(s, ABS_X);
    p->y = norm(s, ABS_Y);
    if (s->has_abs[ABS_RX]) p->rx = norm(s, ABS_RX);
    if (s->has_abs[ABS_RY]) p->ry = norm(s, ABS_RY);
    p->lt = trigger(s, lt, BTN_TL2);
    p->rt = trigger(s, rt, BTN_TR2);
    if (p->lt > 0.5f) b |= 1u << 10;
    if (p->rt > 0.5f) b |= 1u << 11;
    if (s->has_abs[ABS_HAT0X]) {
        float hx = norm(s, ABS_HAT0X);
        if (hx > 0.5f) b |= 1u << 13;
        if (hx < -0.5f) b |= 1u << 15;
    }
    if (s->has_abs[ABS_HAT0Y]) {
        float hy = norm(s, ABS_HAT0Y);
        if (hy > 0.5f) b |= 1u << 14;
        if (hy < -0.5f) b |= 1u << 12;
    }
    p->buttons = b;
}

static void state_mapped(const struct slot *s, host_pad *p)
{
    uint8_t buttons[KEYS], hats[4] = { 0 };
    float axes[ABSES];
    pad_raw r;
    int i;
    for (i = 0; i < s->nbuttons; i++)
        buttons[i] = s->key[s->button_code[i]];
    for (i = 0; i < s->nabs; i++)
        axes[i] = norm(s, s->axis_code[i]);
    for (i = 0; i < 4; i++) {
        int ax = ABS_HAT0X + 2 * i, ay = ax + 1;
        if (s->has_abs[ax] && norm(s, ax) > 0.5f) hats[i] |= 2;
        if (s->has_abs[ax] && norm(s, ax) < -0.5f) hats[i] |= 8;
        if (s->has_abs[ay] && norm(s, ay) > 0.5f) hats[i] |= 4;
        if (s->has_abs[ay] && norm(s, ay) < -0.5f) hats[i] |= 1;
    }
    r.buttons = buttons;
    r.nbuttons = s->nbuttons;
    r.axes = axes;
    r.naxes = s->nabs;
    r.hats = hats;
    r.nhats = 4;
    pad_map_apply(&s->map, &r, p);
}

int host_pad_read(int i, host_pad *p)
{
    struct slot *s;
    memset(p, 0, sizeof *p);
    if (i < 0 || i >= SLOTS)
        return 0;
    if (!started)
        init();
    if (i == 0)
        rescan_if_due();
    s = &slots[i];
    if (s->fd < 0)
        return 0;
    for (;;) {
        struct ev_input_event ev[32];
        ssize_t n = read(s->fd, ev, sizeof ev);
        int k;
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        if (n <= 0) {                     /* unplugged (ENODEV) or gone: its slot is free */
            slot_close(s);
            return 0;
        }
        for (k = 0; k < (int)(n / (ssize_t)sizeof ev[0]); k++) {
            if (ev[k].type == EV_KEY && ev[k].code < KEYS)
                s->key[ev[k].code] = ev[k].value != 0;
            else if (ev[k].type == EV_ABS && ev[k].code < ABSES)
                s->aval[ev[k].code] = ev[k].value;
            else if (ev[k].type == EV_SYN && ev[k].code == SYN_DROPPED)
                load_state(s);
        }
    }
    if (s->kind == KIND_MAPPED)
        state_mapped(s, p);
    else
        state_standard(s, p);
    p->style = s->style;
    p->id = s->id;
    snprintf(p->name, sizeof p->name, "%s", s->name);
    return 1;
}
