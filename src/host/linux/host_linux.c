/*
 * host_linux.c: a window on X11, keyboard and mouse, controllers through
 * /dev/input/event* (pad_evdev.c), and sound through ALSA. X11 and ALSA are loaded at run time,
 * so a game builds without their development packages and a machine without
 * them reports it. Key codes are mapped to Windows virtual key codes, as on
 * every host.
 *
 * Full screen is the window manager's (_NET_WM_STATE_FULLSCREEN, the
 * standard every desktop's and SteamOS's gamescope follow): the window asks,
 * the manager sizes it to the display. F11 and Alt+Enter switch it, as on
 * Windows (the game never sees them), and so does the game
 * (host_window_set_full_screen).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "host.h"

/* ---- X11, by hand: the few types and calls used ---- */
typedef struct _XDisplay Display;
typedef unsigned long XWindow;
typedef unsigned long Atom;
typedef unsigned long KeySym;
typedef union {
    int type;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow w; XWindow root, sub; unsigned long time; int x, y, x_root, y_root; unsigned state; unsigned keycode; int same; } key;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow w; XWindow root, sub; unsigned long time; int x, y, x_root, y_root; unsigned state; unsigned button; int same; } button;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow w; XWindow root, sub; unsigned long time; int x, y, x_root, y_root; unsigned state; char hint; int same; } motion;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow event, w; int x, y, width, height, border; XWindow above; int override_redirect; } configure;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow w; Atom message_type; int format; union { long l[5]; } data; } client;
    struct { int type; unsigned long serial; int send_event; Display *d; XWindow w; Atom atom; unsigned long time; int state; } property;
    long pad[24];
} XEvent;

#define X_KeyPress 2
#define X_KeyRelease 3
#define X_ButtonPress 4
#define X_ButtonRelease 5
#define X_MotionNotify 6
#define X_FocusIn 9
#define X_FocusOut 10
#define X_MapNotify 19
#define X_ConfigureNotify 22
#define X_PropertyNotify 28
#define X_ClientMessage 33
#define X_MASK (1L | 2L | 4L | 8L | (1L << 6) | (1L << 17) | (1L << 21) | (1L << 22))

static Display *(*pXOpenDisplay)(const char *);
static int (*pXInitThreads)(void);
static int (*pXDefaultScreen)(Display *);
static XWindow (*pXRootWindow)(Display *, int);
static XWindow (*pXCreateSimpleWindow)(Display *, XWindow, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
static int (*pXSelectInput)(Display *, XWindow, long);
static int (*pXMapWindow)(Display *, XWindow);
static int (*pXStoreName)(Display *, XWindow, const char *);
static Atom (*pXInternAtom)(Display *, const char *, int);
static int (*pXSetWMProtocols)(Display *, XWindow, Atom *, int);
static int (*pXPending)(Display *);
static int (*pXNextEvent)(Display *, XEvent *);
static KeySym (*pXLookupKeysym)(void *, int);
static int (*pXConnectionNumber)(Display *);
static int (*pXFlush)(Display *);
static int (*pXDestroyWindow)(Display *, XWindow);
static int (*pXkbSetDetectableAutoRepeat)(Display *, int, int *);
static int (*pXChangeProperty)(Display *, XWindow, Atom, Atom, int, int, const unsigned char *, int);
static int (*pXSendEvent)(Display *, XWindow, int, long, XEvent *);
static int (*pXGetWindowProperty)(Display *, XWindow, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *,
                                  unsigned char **);
static int (*pXFree)(void *);

static Display *dpy;
static XWindow win, root;
static Atom wm_delete, wm_state, wm_full;
static int win_w, win_h;               /* the event thread's own */
static _Atomic uint64_t win_px;         /* the size published for every thread: w << 32 | h, never half new */
static _Atomic float scale_now = 1;     /* (6.9) Xft.dpi over 96, from the root window's RESOURCE_MANAGER; 1 where it has none */
static Atom wm_resources;
static void scale_read(void);
static char app_dir[128] = "game", app_title[256] = "nativeshvc", save_dir[1024], res_dir[1024];

static int load_x11(void)
{
    void *h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h)
        return 0;
#define L(n) if (!(p##n = dlsym(h, #n))) return 0;
    L(XInitThreads) L(XOpenDisplay) L(XDefaultScreen) L(XRootWindow) L(XCreateSimpleWindow) L(XSelectInput) L(XMapWindow) L(XStoreName)
    L(XInternAtom) L(XSetWMProtocols) L(XPending) L(XNextEvent) L(XLookupKeysym) L(XConnectionNumber) L(XFlush) L(XDestroyWindow)
    L(XSendEvent) L(XGetWindowProperty) L(XFree)
#undef L
    /* a held key's repeats as presses alone, not a release and a press */
    pXkbSetDetectableAutoRepeat = (int (*)(Display *, int, int *))dlsym(h, "XkbSetDetectableAutoRepeat");
    pXChangeProperty = (int (*)(Display *, XWindow, Atom, Atom, int, int, const unsigned char *, int))dlsym(h, "XChangeProperty");
    return 1;
}

void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        snprintf(app_dir, sizeof app_dir, "%s", dir);
    if (title)
        snprintf(app_title, sizeof app_title, "%s", title);
}

void host_init(int argc, char **argv)
{
    const char *e = getenv("PORT_SAVEDIR"), *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
    ssize_t n;
    (void)argc;
    (void)argv;
    if (e) {
        snprintf(save_dir, sizeof save_dir, "%s", e);
    } else if (xdg) {
        snprintf(save_dir, sizeof save_dir, "%s/%s", xdg, app_dir);
    } else {
        char base[900];
        snprintf(base, sizeof base, "%s/.local", home ? home : ".");
        host_mkdir(base);
        snprintf(base, sizeof base, "%s/.local/share", home ? home : ".");
        host_mkdir(base);
        snprintf(save_dir, sizeof save_dir, "%s/%s", base, app_dir);
    }
    host_mkdir(save_dir);
    e = getenv("PORT_RESDIR");
    if (e) {
        snprintf(res_dir, sizeof res_dir, "%s", e);
    } else if ((n = readlink("/proc/self/exe", res_dir, sizeof res_dir - 1)) > 0) {
        char *s;
        res_dir[n] = 0;
        if ((s = strrchr(res_dir, '/')))
            *s = 0;
    } else {
        snprintf(res_dir, sizeof res_dir, ".");
    }
}

void host_shutdown(void) {}
const char *host_save_dir(void) { return save_dir; }
const char *host_resource_dir(void) { return res_dir; }

/* the window's icon: icon.argb beside the program (nshvc_game's ICON), each
   size its width, height and ARGB pixels as 32-bit little-endian words, set as
   _NET_WM_ICON (which Xlib takes as longs) */
static void window_icon(void)
{
    char path[1100];
    FILE *f;
    long size;
    unsigned char *raw;
    unsigned long *words;
    long i, n;
    if (!pXChangeProperty)
        return;
    snprintf(path, sizeof path, "%s/icon.argb", res_dir);
    if (!(f = fopen(path, "rb")))
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    n = size / 4;
    raw = malloc((size_t)size + 1);
    words = malloc(sizeof *words * (size_t)(n + 1));
    if (raw && words && size > 8 && fread(raw, 1, (size_t)size, f) == (size_t)size) {
        for (i = 0; i < n; i++)
            words[i] = (unsigned long)raw[4 * i] | (unsigned long)raw[4 * i + 1] << 8 | (unsigned long)raw[4 * i + 2] << 16
                       | (unsigned long)raw[4 * i + 3] << 24;
        pXChangeProperty(dpy, win, pXInternAtom(dpy, "_NET_WM_ICON", 0), pXInternAtom(dpy, "CARDINAL", 0), 32, 0,
                         (const unsigned char *)words, (int)n);
    }
    free(raw);
    free(words);
    fclose(f);
}

int host_window_open(int width, int height, const char *title)
{
    int s;
    /* the display is used from two threads: this one's events, and the
       renderer's presenting to a surface on it (VK_KHR_xlib_surface asks for
       this, before any other Xlib call) */
    if (!load_x11() || !pXInitThreads() || !(dpy = pXOpenDisplay(NULL)))
        return 0;
    if (pXkbSetDetectableAutoRepeat)
        pXkbSetDetectableAutoRepeat(dpy, 1, NULL);
    s = pXDefaultScreen(dpy);
    root = pXRootWindow(dpy, s);
    wm_state = pXInternAtom(dpy, "_NET_WM_STATE", 0);
    wm_full = pXInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", 0);
    win_w = width * 3;
    win_h = height * 3;
    atomic_store(&win_px, (uint64_t)(uint32_t)win_w << 32 | (uint32_t)win_h);
    win = pXCreateSimpleWindow(dpy, root, 0, 0, (unsigned)win_w, (unsigned)win_h, 0, 0, 0);
    pXSelectInput(dpy, win, X_MASK);
    pXStoreName(dpy, win, title ? title : app_title);
    wm_delete = pXInternAtom(dpy, "WM_DELETE_WINDOW", 0);
    pXSetWMProtocols(dpy, win, &wm_delete, 1);
    window_icon();
    wm_resources = pXInternAtom(dpy, "RESOURCE_MANAGER", 0);
    pXSelectInput(dpy, root, 1L << 22);     /* PropertyChange: the player changing the setting */
    scale_read();
    pXFlush(dpy);
    return 1;
}

/* on the event thread: Xft.dpi in the X resources the session set on the root window */
static void scale_read(void)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    float s = 1;
    if (pXGetWindowProperty(dpy, root, wm_resources, 0, 65536, 0, 31 /* XA_STRING */, &type, &format, &n, &after, &data) == 0 && data) {
        const char *p = strstr((const char *)data, "Xft.dpi:");
        if (p) {
            float dpi = (float)atof(p + 8);
            if (dpi >= 48 && dpi <= 960)
                s = dpi / 96.0f;
        }
        pXFree(data);
    }
    atomic_store(&scale_now, s);
}
float host_display_scale(void) { return atomic_load(&scale_now); }

void host_window_close(void)
{
    if (dpy && win)
        pXDestroyWindow(dpy, win);
    win = 0;
}

int host_window_visible(void) { return win != 0; }
void *host_window_handle(void) { return (void *)win; }
int host_windowed(void) { return win != 0; }
/* the window lasts until it is closed, after the renderer: nothing to wait for */
void *host_window_take(int *w, int *h)
{
    int a, b;
    host_window_pixels(&a, &b);
    if (w)
        *w = a;
    if (h)
        *h = b;
    return (void *)win;
}
void host_window_give(void *kept) { (void)kept; }
/* the events are read on the thread that set it */
static void (*watch)(void);
void host_window_watch(void (*changed)(void)) { watch = changed; }
void *host_x11_display(void) { return dpy; }
void host_window_pixels(int *w, int *h)
{
    uint64_t v = atomic_load(&win_px);
    *w = (int)(uint32_t)(v >> 32);
    *h = (int)(uint32_t)v;
}
/* points are pixels here: both from one reading */
void host_window_sizes(int *pw, int *ph, float *tw, float *th)
{
    host_window_pixels(pw, ph);
    *tw = (float)*pw;
    *th = (float)*ph;
}

void host_window_points(float *w, float *h)
{
    int a, b;
    host_window_pixels(&a, &b);
    *w = (float)a;
    *h = (float)b;
}
/* full screen. full_now: which it is or is going to, for any thread. On
   the event thread: shown once host_window_show has mapped it, mapped once
   the window manager has shown it (it takes requests for a window it
   manages); full_asked what was last asked of it (-1 nothing outstanding),
   until its _NET_WM_STATE shows it, so that the state it sets meanwhile for
   other reasons does not read as the switch undone */
static _Atomic int full_now;
static int shown, mapped, full_asked = -1;

static void full_screen_ask(int on)
{
    XEvent e;
    memset(&e, 0, sizeof e);
    e.client.type = X_ClientMessage;
    e.client.d = dpy;
    e.client.w = win;
    e.client.message_type = wm_state;
    e.client.format = 32;
    e.client.data.l[0] = on;                     /* _NET_WM_STATE_REMOVE 0, _ADD 1 */
    e.client.data.l[1] = (long)wm_full;
    e.client.data.l[3] = 1;                      /* asked by an application */
    /* to the root, for the manager: SubstructureNotify | SubstructureRedirect */
    pXSendEvent(dpy, root, 0, (1L << 19) | (1L << 20), &e);
    pXFlush(dpy);
}

/* 1 when the window's _NET_WM_STATE holds _NET_WM_STATE_FULLSCREEN */
static int full_screen_state(void)
{
    Atom type = 0;
    int format = 0, full = 0;
    unsigned long n = 0, after = 0, i;
    unsigned char *data = NULL;
    if (pXGetWindowProperty(dpy, win, wm_state, 0, 64, 0, 4 /* XA_ATOM */, &type, &format, &n, &after, &data) == 0 && data) {
        if (format == 32)
            for (i = 0; i < n; i++)
                if (((unsigned long *)data)[i] == wm_full)
                    full = 1;
        pXFree(data);
    }
    return full;
}

/* mapped as set: full screen asked before by the window's own
   _NET_WM_STATE, which a manager reads as it maps a window (EWMH's way to
   open one full screen; gamescope sizes a game's window to the display) */
void host_window_show(void)
{
    if (!win || shown)
        return;
    shown = 1;
    if (atomic_load(&full_now) && pXChangeProperty)
        pXChangeProperty(dpy, win, wm_state, 4 /* XA_ATOM */, 32, 0 /* PropModeReplace */, (const unsigned char *)&wm_full, 1);
    full_asked = -1;
    pXMapWindow(dpy, win);
    pXFlush(dpy);
}

int host_full_screen_switch(void) { return win != 0; }
int host_window_full_screen(void) { return atomic_load(&full_now); }
void host_window_set_full_screen(int on)
{
    on = on != 0;
    if (!win)
        return;
    atomic_store(&full_now, on);
    if (!shown)                         /* opened so (host_window_show) */
        return;
    full_asked = on;
    if (mapped)
        full_screen_ask(on);
}

void host_message_box(const char *text, const char *caption) { fprintf(stderr, "%s: %s\n", caption ? caption : "message", text ? text : ""); }
void host_safe_area(float *l, float *t, float *r, float *b) { *l = *t = *r = *b = 0; }
void host_text_input(int on) { (void)on; }
int host_keyboard_attached(void) { return 1; }

static int vk_of(KeySym k)
{
    if (k >= 'a' && k <= 'z')
        return (int)(k - 'a' + 'A');
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ')
        return (int)k;
    switch (k) {
    case '`': return 0xC0;
    case '-': return 0xBD;
    case '=': return 0xBB;
    case '[': return 0xDB;
    case ']': return 0xDD;
    case '\\': return 0xDC;
    case ';': return 0xBA;
    case '\'': return 0xDE;
    case ',': return 0xBC;
    case '.': return 0xBE;
    case '/': return 0xBF;
    case 0xFF13: return 0x13;
    case 0xFF14: return 0x91;
    case 0xFF50: return 0x24;
    case 0xFF55: return 0x21;
    case 0xFF56: return 0x22;
    case 0xFF57: return 0x23;
    case 0xFF61: return 0x2C;
    case 0xFF63: return 0x2D;
    case 0xFF67: return 0x5D;
    case 0xFF7F: return 0x90;
    case 0xFFFF: return 0x2E;
    case 0xFFE5: return 0x14;
    case 0xFFEB: return 0x5B;
    case 0xFFEC: return 0x5C;
    /* the keypad, as its keys are named with Num Lock off and on */
    case 0xFF9E: return 0x60;
    case 0xFF9C: return 0x61;
    case 0xFF99: return 0x62;
    case 0xFF9B: return 0x63;
    case 0xFF96: return 0x64;
    case 0xFF9D: return 0x65;
    case 0xFF98: return 0x66;
    case 0xFF95: return 0x67;
    case 0xFF97: return 0x68;
    case 0xFF9A: return 0x69;
    case 0xFF9F: case 0xFFAE: return 0x6E;
    case 0xFFAA: return 0x6A;
    case 0xFFAB: return 0x6B;
    case 0xFFAD: return 0x6D;
    case 0xFFAF: return 0x6F;
    case 0xFF8D: return 0x0D;
    case 0xFF08: return 0x08;
    case 0xFF09: return 0x09;
    case 0xFF0D: return 0x0D;
    case 0xFF1B: return 0x1B;
    case 0xFF51: return 0x25;
    case 0xFF52: return 0x26;
    case 0xFF53: return 0x27;
    case 0xFF54: return 0x28;
    case 0xFFE1: case 0xFFE2: return 0x10;
    case 0xFFE3: case 0xFFE4: return 0x11;
    case 0xFFE9: case 0xFFEA: return 0x12;
    }
    if (k >= 0xFFB0 && k <= 0xFFB9)
        return (int)(k - 0xFFB0 + 0x60);
    if (k >= 0xFFBE && k <= 0xFFD5)              /* F1-F24 */
        return (int)(k - 0xFFBE + 0x70);
    return 0;
}

static int last_x, last_y, buttons;
static uint8_t key_held[256];        /* by X key code: a repeat is not a new press */
static uint8_t key_switch[256];      /* by X key code: down as the full-screen switch, its release the game's no more */

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    if (!dpy) {
        if (wait_ms && wait_ms != 0xFFFFFFFFu)
            host_sleep_ms(wait_ms);
        return 0;
    }
    if (!pXPending(dpy)) {
        if (wait_ms && wait_ms != 0xFFFFFFFFu)
            host_sleep_ms(wait_ms);
        if (!pXPending(dpy))
            return 0;
    }
    memset(ev, 0, sizeof *ev);
    {
        XEvent e;
        pXNextEvent(dpy, &e);
        switch (e.type) {
        case X_KeyPress:
        case X_KeyRelease: {
            unsigned c = e.key.keycode & 0xFF;
            int down = e.type == X_KeyPress;
            KeySym k;
            if (key_held[c] == down)
                return 0;
            key_held[c] = (uint8_t)down;
            k = pXLookupKeysym(&e, 0);
            /* F11, and Return or the keypad's Enter with Alt (Mod1) held: full screen or a window */
            if (down && (k == 0xFFC8 || ((k == 0xFF0D || k == 0xFF8D) && (e.key.state & 8)))) {
                key_switch[c] = 1;
                host_window_set_full_screen(!atomic_load(&full_now));
                return 0;
            }
            if (!down && key_switch[c]) {
                key_switch[c] = 0;
                return 0;
            }
            ev->type = HOST_EV_KEY;
            ev->vk = vk_of(k);
            ev->down = down;
            return ev->vk != 0;
        }
        case X_ButtonPress:
        case X_ButtonRelease: {
            int b;
            if (e.button.button >= 4 && e.button.button <= 7) {
                /* the wheel: a press a notch, up, down, left, right */
                if (e.type != X_ButtonPress)
                    return 0;
                ev->type = HOST_EV_WHEEL;
                ev->dy = e.button.button == 4 ? 1.0f : e.button.button == 5 ? -1.0f : 0.0f;
                ev->dx = e.button.button == 7 ? 1.0f : e.button.button == 6 ? -1.0f : 0.0f;
                return 1;
            }
            b = e.button.button == 1 ? 1 : e.button.button == 3 ? 2 : e.button.button == 2 ? 4 : 0;
            if (e.type == X_ButtonPress)
                buttons |= b;
            else
                buttons &= ~b;
            ev->type = HOST_EV_MOUSE;
            ev->x = (float)e.button.x;
            ev->y = (float)e.button.y;
            ev->buttons = buttons;
            return 1;
        }
        case X_MotionNotify:
            ev->type = HOST_EV_MOUSE;
            ev->x = (float)e.motion.x;
            ev->y = (float)e.motion.y;
            ev->dx = (float)(e.motion.x - last_x);
            ev->dy = (float)(e.motion.y - last_y);
            last_x = e.motion.x;
            last_y = e.motion.y;
            ev->buttons = buttons;
            return 1;
        case X_FocusIn:
        case X_FocusOut:
            if (e.type == X_FocusOut) {
                memset(key_held, 0, sizeof key_held);
                memset(key_switch, 0, sizeof key_switch);
            }
            ev->type = HOST_EV_FOCUS;
            ev->down = e.type == X_FocusIn;
            return 1;
        case X_ConfigureNotify:
            win_w = e.configure.width;
            win_h = e.configure.height;
            atomic_store(&win_px, (uint64_t)(uint32_t)win_w << 32 | (uint32_t)win_h);
            if (watch)
                watch();
            return 0;
        case X_MapNotify:
            /* managed now: a switch asked before goes to the manager */
            if (e.configure.w == win && !mapped) {
                mapped = 1;
                if (full_asked >= 0)
                    full_screen_ask(full_asked);
            }
            return 0;
        case X_PropertyNotify:
            if (e.property.w == root && e.property.atom == wm_resources)
                scale_read();
            /* the manager's record of the window: full screen or not, by the player or the game */
            if (e.property.w == win && e.property.atom == wm_state) {
                int full = full_screen_state();
                if (full_asked < 0)
                    atomic_store(&full_now, full);
                else if (full == full_asked)
                    full_asked = -1;
            }
            return 0;
        case X_ClientMessage:
            if ((Atom)e.client.data.l[0] == wm_delete) {
                ev->type = HOST_EV_CLOSE;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

/* ---- controllers: pad_evdev.c ---- */

/* ---- ALSA, loaded at run time ---- */
static int (*p_open)(void **, const char *, int, int);
static int (*p_set)(void *, int, int, unsigned, unsigned, int, unsigned);
static long (*p_writei)(void *, const void *, unsigned long);
static int (*p_recover)(void *, int, int);
static int (*p_close)(void *);
static void *pcm;
static host_audio_fn afn;
static void *auser;
static volatile int astop;
static pthread_t athread;

static void *audio_main(void *arg)
{
    float buf[512 * 2];
    (void)arg;
    while (!astop) {
        long r;
        afn(buf, 512, auser);
        r = p_writei(pcm, buf, 512);
        if (r < 0)
            p_recover(pcm, (int)r, 1);
    }
    return NULL;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h)
        return 0;
    p_open = dlsym(h, "snd_pcm_open");
    p_set = dlsym(h, "snd_pcm_set_params");
    p_writei = dlsym(h, "snd_pcm_writei");
    p_recover = dlsym(h, "snd_pcm_recover");
    p_close = dlsym(h, "snd_pcm_close");
    if (!p_open || !p_set || !p_writei || !p_recover || !p_close)
        return 0;
    if (p_open(&pcm, "default", 0, 0) < 0)
        return 0;
    /* FLOAT_LE 14, RW_INTERLEAVED 3, stereo, 40 ms */
    if (p_set(pcm, 14, 3, 2, (unsigned)rate, 1, 40000) < 0)
        return 0;
    afn = fn;
    auser = user;
    return pthread_create(&athread, NULL, audio_main, NULL) == 0;
}

void host_audio_close(void)
{
    if (!pcm)
        return;
    astop = 1;
    pthread_join(athread, NULL);
    p_close(pcm);
    pcm = NULL;
}

void *host_vulkan_loader(void)
{
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    return h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
}
