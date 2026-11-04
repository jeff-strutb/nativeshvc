/*
 * host_check.c: the PSP's host (src/host/psp), read back on the system:
 *
 *   time     the clock moves forward; a 50 ms sleep takes 50 to 60 ms
 *   files    the save folder made beside the program; a folder made, made
 *            again (there already), and one in a folder that is not made
 *            exactly when the system says so
 *   buttons  each button alone its bit of host_pad's layout, the stick at
 *            its ends and centre
 *   screen   frames presented: each in VRAM pixel for pixel (0xAARRGGBB
 *            made 0xAABBGGRR), set to show next, and never written into
 *            the buffer on screen, two presented within one vertical blank
 *            included
 *   sound    the channel taking 48 kHz: a second's frames asked for, within
 *            two blocks
 *
 *   host_check        the checks; "N check(s) failed" at the end
 */
#include <stdio.h>
#include <string.h>

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include "host.h"

void host_psp_pad(unsigned buttons, int lx, int ly, host_pad *p);

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static int is_dir(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0 && FIO_S_ISDIR(st.st_mode);
}

static volatile long s_frames;
static void audio_cb(float *lr, int frames, void *user)
{
    (void)user;
    memset(lr, 0, (size_t)frames * 2 * sizeof *lr);
    s_frames += frames;
}

/* the colour of pixel (x, y) in frame f, and as the screen holds it */
static uint32_t pattern(int f, int x, int y) { return 0xFF000000u | (uint32_t)(x & 0xFF) << 16 | (uint32_t)(y & 0xFF) << 8 | (uint32_t)(f * 37 & 0xFF); }
static uint32_t screen_of(uint32_t c) { return (c & 0xFF00FF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16; }

static void *pending(void)
{
    void *top;
    int w, fmt;
    sceDisplayGetFrameBuf(&top, &w, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
    return top;
}

static void *on_screen(void)
{
    void *top;
    int w, fmt;
    sceDisplayGetFrameBuf(&top, &w, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
    return top;
}

static int frame_held(const void *vram, int f)
{
    const uint32_t *fb = (const uint32_t *)((uintptr_t)vram | 0x40000000u);
    int x, y;
    for (y = 0; y < 272; y++)
        for (x = 0; x < 480; x++)
            if (fb[(size_t)y * 512 + (size_t)x] != screen_of(pattern(f, x, y)))
                return 0;
    return 1;
}

int main(int argc, char **argv)
{
    static uint32_t pic[480 * 272];
    char path[700], buf[160];
    uint64_t t0, t1;
    host_pad p;
    int i, f, x, y, w, h;

    host_set_app_name("host_check", "host_check");
    host_init(argc, argv);
    printf("program %s; saves in %s\n", argc > 0 ? argv[0] : "?", host_save_dir());

    /* time */
    t0 = host_ticks_ns();
    host_sleep_ms(50);
    t1 = host_ticks_ns();
    snprintf(buf, sizeof buf, "a 50 ms sleep: %.2f ms", (double)(t1 - t0) / 1e6);
    check(t1 - t0 >= 50000000u && t1 - t0 < 60000000u, buf);

    /* files */
    check(is_dir(host_save_dir()), "the save folder is made beside the program");
    snprintf(path, sizeof path, "%s/made", host_save_dir());
    check(host_mkdir(path) && is_dir(path), "a folder made");
    check(host_mkdir(path), "and made again: there already");
    snprintf(path, sizeof path, "%s/none/made", host_save_dir());
    check(!is_dir(path) && host_mkdir(path) == is_dir(path),
          "a folder in one that is not: made exactly when the system says so (the PSP refuses it, PPSSPP makes both)");

    /* buttons */
    {
        static const unsigned btn[12] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE,
                                          PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_SELECT, PSP_CTRL_START,
                                          PSP_CTRL_UP, PSP_CTRL_RIGHT, PSP_CTRL_DOWN, PSP_CTRL_LEFT };
        static const int bit[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15 };
        int ok = 1;
        for (i = 0; i < 12; i++) {
            host_psp_pad(btn[i], 128, 128, &p);
            ok &= p.buttons == 1u << bit[i];
        }
        check(ok, "each button alone: its bit (Cross 0, Circle 1, Square 2, Triangle 3, L 4, R 5, Select 6, Start 7, the d-pad 12-15)");
        host_psp_pad(0, 0, 255, &p);
        check(p.x == -1.0f && p.y == 1.0f, "the stick left and down: x -1, y 1");
        host_psp_pad(0, 255, 0, &p);
        check(p.x == 1.0f && p.y == -1.0f, "right and up: x 1, y -1");
        host_psp_pad(0, 128, 128, &p);
        check(p.x > 0 && p.x < 0.01f && p.y > 0 && p.y < 0.01f, "at rest (128): within 0.01 of the centre");
        check(p.style == HOST_PAD_PLAYSTATION && !strcmp(p.label[0], "Cross") && !strcmp(p.label[3], "Triangle"),
              "a PlayStation pad, its buttons' names");
        check(host_pad_read(0, &p) == 1 && host_pad_read(1, &p) == 0, "controller 0 there, 1 not");
    }

    /* screen */
    check(host_window_open(256, 224, "host_check"), "the screen opens");
    host_window_pixels(&w, &h);
    check(w == 480 && h == 272, "480 x 272 pixels");
    {
        int ok_px = 1, ok_set = 1, ok_free = 1;
        for (f = 0; f < 12; f++) {
            void *shown;
            for (y = 0; y < 272; y++)
                for (x = 0; x < 480; x++)
                    pic[y * 480 + x] = pattern(f, x, y);
            if (f % 3 == 2) {
                /* the second of two within one vertical blank: the one set
                   before it still waiting */
                host_present(pic, 480, 272, 4.0f / 3.0f);
            } else {
                sceDisplayWaitVblankStart();
                shown = on_screen();
                host_present(pic, 480, 272, 4.0f / 3.0f);
                ok_free &= pending() != shown;
            }
            ok_px &= frame_held(pending(), f);
            ok_set &= pending() != NULL;
        }
        check(ok_px, "12 frames: each in VRAM, pixel for pixel, its channels as the screen's");
        check(ok_set, "each set to show next");
        check(ok_free, "none written into the buffer on screen");
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart();
        check(frame_held(on_screen(), 11), "the last on screen after a vertical blank");
    }

    /* sound */
    check(host_audio_open(48000, audio_cb, NULL), "the sound channel opens at 48 kHz");
    host_sleep_ms(200);
    {
        long n0 = s_frames, n1;
        t0 = host_ticks_ns();
        host_sleep_ms(1000);
        n1 = s_frames;
        t1 = host_ticks_ns();
        x = (int)((double)(n1 - n0) * 1e9 / (double)(t1 - t0));
        snprintf(buf, sizeof buf, "frames asked for a second: %d (48,000 within two blocks of 1,024)", x);
        check(x > 48000 - 2048 && x < 48000 + 2048, buf);
    }
    host_audio_close();
    check(host_audio_open(48000, audio_cb, NULL), "closes, the channel let go: it opens again");
    host_audio_close();
    {
        host_event e;
        check(!host_poll_event(&e, 0), "no event pending");
    }
    printf("%d check(s) failed\n", fails);
    host_shutdown();
    return fails != 0;
}
