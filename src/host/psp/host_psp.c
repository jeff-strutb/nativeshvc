/* host_psp.c: the PSP's screen, buttons and sound.
 *
 *   screen   480 x 272: three buffers in VRAM, each finished one set to
 *            show from the next vertical blank without waiting for it, the
 *            next frame drawn into one neither shown nor waiting to be; by
 *            a renderer that draws on them (host_psp.h), or the software
 *            renderer's picture (drawn at the screen's size) copied in
 *   buttons  controller 0, an XInput pad's layout by position (Cross the
 *            bottom button, A; Circle B; Square X; Triangle Y), the
 *            analogue stick its stick
 *   sound    a channel the system converts from the rate asked, fed from
 *            a thread of its own, below the program's in priority: it
 *            runs while the program waits for its frame's time (its
 *            block's 21 ms of sound give it room), never across that
 *            time
 *   HOME     the system's exit is HOST_EV_CLOSE; a suspend and a resume
 *            HOST_EV_FOCUS
 *   clock    the CPU at its full 333 MHz (a program starts at 222)
 *
 * Saves go in save/ beside the program, resources are the program's folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <psploadexec.h>
#include <psppower.h>
#include <pspthreadman.h>

#include "host.h"
#include "host_psp.h"

#define SCREEN_W 480
#define SCREEN_H 272
#define FB_STRIDE HOST_PSP_STRIDE
#define FB_BYTES (HOST_PSP_VRAM_USED / 3)

static char s_app_dir[64] = "game", s_title[128] = "nativeshvc";
static char s_res[512] = ".", s_save[600];

/* ---- the system's callbacks ---- */
static volatile int s_exit, s_suspended, s_focus_lost, s_focus_back;

static int exit_cb(int a, int b, void *c)
{
    (void)a;
    (void)b;
    (void)c;
    s_exit = 1;
    return 0;
}

static int power_cb(int a, int flags, void *c)
{
    (void)a;
    (void)c;
    if (flags & PSP_POWER_CB_SUSPENDING) {
        s_suspended = 1;
        s_focus_lost = 1;
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        s_suspended = 0;
        s_focus_back = 1;
    }
    return 0;
}

static int callbacks_main(SceSize n, void *p)
{
    (void)n;
    (void)p;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("nshvc exit", exit_cb, NULL));
    scePowerRegisterCallback(-1, sceKernelCreateCallback("nshvc power", power_cb, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

/* ---- process ---- */
void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        snprintf(s_app_dir, sizeof s_app_dir, "%s", dir);
    if (title)
        snprintf(s_title, sizeof s_title, "%s", title);
}

void host_init(int argc, char **argv)
{
    const char *e = getenv("PORT_RESDIR");
    SceUID t;
    if (e) {
        snprintf(s_res, sizeof s_res, "%s", e);
    } else if (argc > 0 && argv[0] && strrchr(argv[0], '/')) {
        snprintf(s_res, sizeof s_res, "%s", argv[0]);
        *strrchr(s_res, '/') = 0;
    }
    snprintf(s_save, sizeof s_save, "%s/save", s_res);
    host_mkdir(s_save);
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    t = sceKernelCreateThread("nshvc callbacks", callbacks_main, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
}

void host_shutdown(void) { host_audio_close(); }

const char *host_resource_dir(void) { return s_res; }
const char *host_save_dir(void) { return s_save; }

/* ---- the screen ---- */
static uint32_t *s_fb[3];                    /* VRAM, uncached */
static int s_shown = -1, s_next = -1;       /* the buffer on screen; the one set to show next */
static int s_target;                         /* the one the next frame is drawn into */
static unsigned s_next_vcount;              /* the vertical blanks counted when it was set */
static int s_last = 0;                       /* the one last set to show */

static void *fb_addr(int i) { return (char *)sceGeEdramGetAddr() + (size_t)i * FB_BYTES; }

static void *screen_target(void)
{
    int i;
    if (s_next >= 0 && sceDisplayGetVcount() != s_next_vcount) {
        s_shown = s_next;                    /* a vertical blank has passed: it is on screen */
        s_next = -1;
    }
    for (i = 0; i == s_shown || i == s_next; i++)
        ;
    s_target = i;
    return fb_addr(i);
}

static void screen_show(void)
{
    s_next_vcount = sceDisplayGetVcount();
    sceDisplaySetFrameBuf(fb_addr(s_target), FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    s_next = s_target;
    s_last = s_target;
}

static void *screen_shown(void) { return fb_addr(s_last); }

/* The buffer neither on screen nor waiting to be, lent to the game while it
   runs: the renderer draws into one (screen_target) only after the game's
   run of the frame has returned, the GE then idle with the game's lists,
   and a vertical blank in between only frees the one shown before. The
   GE's state as the renderer left it is saved at the first loan (the GE
   idle: the renderer syncs its lists before it returns) and put back
   before the renderer's next frame (ge_back), so the game's lists may set
   any of it. */
static PspGeContext s_ge_kept;
static int s_ge_lent;

void *host_ge_vram(size_t *bytes)
{
    if (s_shown < 0) {
        *bytes = 0;
        return NULL;
    }
    if (!s_ge_lent) {
        sceGeDrawSync(0);
        s_ge_lent = sceGeSaveContext(&s_ge_kept) >= 0;
    }
    *bytes = FB_BYTES;
    return screen_target();
}

static void ge_back(void)
{
    if (!s_ge_lent)
        return;
    sceGeDrawSync(0);
    sceGeRestoreContext(&s_ge_kept);
    s_ge_lent = 0;
}

static HostPspScreen s_screen = { screen_target, screen_show, screen_shown, ge_back };

int host_window_open(int width, int height, const char *title)
{
    int i;
    (void)width;
    (void)height;
    (void)title;
    if (s_shown >= 0)
        return 1;
    for (i = 0; i < 3; i++) {
        s_fb[i] = (uint32_t *)((uintptr_t)fb_addr(i) | 0x40000000u);
        memset(s_fb[i], 0, FB_BYTES);
    }
    sceDisplaySetMode(0, SCREEN_W, SCREEN_H);
    sceDisplaySetFrameBuf(fb_addr(0), FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    s_shown = 0;
    return 1;
}

void host_window_close(void) {}
int host_window_visible(void) { return !s_suspended; }
void *host_window_handle(void) { return s_shown >= 0 ? &s_screen : NULL; }

void host_window_pixels(int *w, int *h)
{
    *w = SCREEN_W;
    *h = SCREEN_H;
}

void host_window_points(float *w, float *h)
{
    *w = SCREEN_W;
    *h = SCREEN_H;
}

/* The picture is the screen's size (the renderer draws at
   host_window_pixels); one of another size is shown at the centre,
   unscaled. 0xAARRGGBB to the screen's 0xAABBGGRR. */
void host_present(const uint32_t *argb, int w, int h, float aspect)
{
    uint32_t *fb;
    int x, y, x0 = (SCREEN_W - w) / 2, y0 = (SCREEN_H - h) / 2;
    (void)aspect;
    if (s_shown < 0)
        return;
    screen_target();
    fb = s_fb[s_target];
    if (w != SCREEN_W || h != SCREEN_H)
        memset(fb, 0, FB_BYTES);
    for (y = 0; y < h; y++) {
        const uint32_t *src = argb + (size_t)y * (size_t)w;
        uint32_t *dst;
        int yy = y + y0;
        if (yy < 0 || yy >= SCREEN_H)
            continue;
        dst = fb + (size_t)yy * FB_STRIDE;
        for (x = 0; x < w; x++) {
            uint32_t c = src[x];
            int xx = x + x0;
            if (xx >= 0 && xx < SCREEN_W)
                dst[xx] = (c & 0xFF00FF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16;
        }
    }
    screen_show();
}

/* ---- events ---- */
int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    memset(ev, 0, sizeof *ev);
    if (s_exit == 1) {
        s_exit = 2;
        ev->type = HOST_EV_CLOSE;
        return 1;
    }
    if (s_focus_lost) {
        s_focus_lost = 0;
        ev->type = HOST_EV_FOCUS;
        ev->down = 0;
        return 1;
    }
    if (s_focus_back) {
        s_focus_back = 0;
        ev->type = HOST_EV_FOCUS;
        ev->down = 1;
        return 1;
    }
    if (wait_ms && wait_ms != 0xFFFFFFFFu)
        host_sleep_ms(wait_ms);
    return 0;
}

/* the system's debug screen, until a button is pressed or the system
   exits the program */
void host_message_box(const char *text, const char *caption)
{
    SceCtrlData c;
    fprintf(stderr, "%s: %s\n", caption ? caption : "message", text ? text : "");
    pspDebugScreenInit();
    pspDebugScreenPrintf("%s\n\n%s\n\nPress a button.\n", caption ? caption : "", text ? text : "");
    do
        sceCtrlPeekBufferPositive(&c, 1);
    while (c.Buttons && !s_exit);
    do {
        sceKernelDelayThread(10000);
        sceCtrlPeekBufferPositive(&c, 1);
    } while (!c.Buttons && !s_exit);
}

/* ---- sound ---- */
#define AUDIO_FRAMES 1024                    /* a block: 21 ms at 48 kHz */
static host_audio_fn s_afn;
static void *s_auser;
static SceUID s_athread = -1;
static volatile int s_arun;

static int audio_main(SceSize n, void *p)
{
    static float f[AUDIO_FRAMES * 2];
    static int16_t b[2][AUDIO_FRAMES * 2];
    int k = 0, i;
    (void)n;
    (void)p;
    while (s_arun) {
        s_afn(f, AUDIO_FRAMES, s_auser);
        for (i = 0; i < AUDIO_FRAMES * 2; i++) {
            float v = f[i] * 32768.0f;
            b[k][i] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, b[k]);
        k ^= 1;
    }
    return 0;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    if (s_athread >= 0)
        return 1;
    if (sceAudioSRCChReserve(AUDIO_FRAMES, rate, 2) < 0)
        return 0;
    s_afn = fn;
    s_auser = user;
    s_arun = 1;
    s_athread = sceKernelCreateThread("nshvc audio", audio_main, sceKernelGetThreadCurrentPriority() + 1, 0x10000,
                                      PSP_THREAD_ATTR_USER, NULL);
    if (s_athread < 0 || sceKernelStartThread(s_athread, 0, NULL) < 0) {
        s_arun = 0;
        sceAudioSRCChRelease();
        s_athread = -1;
        return 0;
    }
    return 1;
}

void host_audio_close(void)
{
    int i;
    if (s_athread < 0)
        return;
    s_arun = 0;
    sceKernelWaitThreadEnd(s_athread, NULL);
    sceKernelDeleteThread(s_athread);
    /* the channel is let go once its last block has played (up to two,
       43 ms): refused until then */
    for (i = 0; i < 100 && sceAudioSRCChRelease() < 0; i++)
        sceKernelDelayThread(1000);
    s_athread = -1;
}

/* ---- the buttons ---- */
/* The PSP's buttons and stick (0-255 each way, 128 the centre) as
   host_pad: the layout's bits, the stick -1..1, y positive downward */
void host_psp_pad(unsigned buttons, int lx, int ly, host_pad *p)
{
    static const unsigned from[16] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE,
                                       PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_SELECT, PSP_CTRL_START,
                                       0, 0, 0, 0, PSP_CTRL_UP, PSP_CTRL_RIGHT, PSP_CTRL_DOWN, PSP_CTRL_LEFT };
    static const char *const label[16] = { "Cross", "Circle", "Square", "Triangle", "L", "R", "Select", "Start",
                                           "", "", "", "", "Up", "Right", "Down", "Left" };
    int i;
    memset(p, 0, sizeof *p);
    for (i = 0; i < 16; i++) {
        if (from[i] && (buttons & from[i]))
            p->buttons |= 1u << i;
        snprintf(p->label[i], sizeof p->label[i], "%s", label[i]);
    }
    p->x = ((float)lx - 127.5f) / 127.5f;
    p->y = ((float)ly - 127.5f) / 127.5f;
    p->style = HOST_PAD_PLAYSTATION;
    snprintf(p->name, sizeof p->name, "PSP");
}

int host_pad_read(int index, host_pad *p)
{
    SceCtrlData c;
    memset(p, 0, sizeof *p);
    if (index != 0 || sceCtrlPeekBufferPositive(&c, 1) < 1)
        return 0;
    host_psp_pad(c.Buttons, c.Lx, c.Ly, p);
    return 1;
}

void host_safe_area(float *l, float *t, float *r, float *b) { *l = *t = *r = *b = 0; }
void host_text_input(int on) { (void)on; }
int host_keyboard_attached(void) { return 0; }
