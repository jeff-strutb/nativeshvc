/*
 * console.c: the console as an application (nshvc_main): the window, the
 * controls, the sound, the save and the pace, over the machine (src/console)
 * and the game's C; and the console's public calls (nshvc.h).
 *
 * Time is the console's: a frame each 1/60.0988 s (NTSC; 1/50.007 s PAL)
 * against the wall clock, the sound drained at the DSP's rate (32,040 Hz)
 * and resampled to the device's (windowed sinc), the rate trimmed by up to
 * 0.5% to keep 50 ms buffered.
 *
 * The controls: the keyboard, game controllers, the mouse and touches,
 * onto whatever is plugged into the controller ports (input.c).
 *
 * The save is the cartridge's battery RAM, kept in the host's save folder
 * as SAVE_NAME.srm and written when it changes. A headless run (a test)
 * neither reads nor writes it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nshvc/nshvc.h"
#include "snes_internal.h"
#include "host.h"
#include "rr.h"
#include "rr_size.h"
#include "resample.h"
#include "script.h"
#include "input.h"

/* ---- the public calls ---- */
const uint32_t *nshvc_picture(int *width, int *height) { return ppu_picture(width, height); }
uint32_t nshvc_frames(void) { return (uint32_t)ppu_frames(); }
int nshvc_audio_take(int16_t *lr, int max) { return snes_audio_take(lr, max); }
int nshvc_audio_queued(void) { return snes_audio_queued(); }
uint8_t *nshvc_wram(void) { return snes_wram_rw(); }
/* the cartridge's RAM as it stands (a chip that owns it brought up to now) */
uint8_t *nshvc_sram(void) { return snes_sram(); }
const uint8_t *nshvc_aram(void) { return snes_aram(); }
const uint16_t *nshvc_vram(void) { return ppu_vram(); }
const uint16_t *nshvc_cgram(void) { return ppu_cgram(); }
const uint8_t *nshvc_oam(void) { return ppu_oam(); }
uint8_t nshvc_peek(uint32_t addr) { return snes_peek(addr); }
void nshvc_ext_screen(int width, int height) { ppu_ext_screen(width, height); }
void nshvc_ext_sprite_x(int sprite, int on) { ppu_ext_sprite_x(sprite, on); }
void nshvc_ext_sprite_y(int sprite, int on) { ppu_ext_sprite_y(sprite, on); }
void nshvc_ext_sprite_limits(int on) { ppu_ext_sprite_limits(on); }
void nshvc_ext_vram(int kb) { ppu_ext_vram(kb); }
void nshvc_ext_vram_page(int page) { ppu_ext_vram_page(page); }
void nshvc_ext_layer_page(int layer, int page) { ppu_ext_layer_page(layer, page); }

void nshvc_fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "nativeshvc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(70);
}

/* ---- the game's hooks ---- */
static nshvc_hooks hooks;
void nshvc_set_hooks(const nshvc_hooks *h)
{
    if (h)
        hooks = *h;
    else
        memset(&hooks, 0, sizeof hooks);
}
const char *nshvc_save_dir(void) { return host_save_dir(); }
void nshvc_safe_area(float *l, float *t, float *r, float *b) { host_safe_area(l, t, r, b); }

void nshvc_window_size(float *w, float *h)
{
    host_window_points(w, h);
    if (*w <= 0 || *h <= 0) {
        int tw = 0, th = 0;
        rr_headless_size(&tw, &th);
        *w = (float)tw;
        *h = (float)th;
    }
}

/* ---- the game's buttons and overlays ---- */
static uint16_t game_buttons[8];
void nshvc_game_buttons(int player, uint16_t buttons)
{
    if (player >= 0 && player < 8)
        game_buttons[player] = buttons;
}

typedef struct {
    uint32_t *argb, *shown;      /* the image; it at its alpha */
    int w, h;
    float cx, cy, scale, angle, alpha, shown_alpha;
    int visible;
} Overlay;
static Overlay overlays[NSHVC_OVERLAYS];
static int noverlays;

int nshvc_overlay_image(const uint32_t *argb, int w, int h)
{
    Overlay *o;
    if (noverlays == NSHVC_OVERLAYS || w <= 0 || h <= 0)
        return -1;
    o = &overlays[noverlays];
    o->argb = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    o->shown = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!o->argb || !o->shown)
        nshvc_fatal("no memory for an overlay");
    memcpy(o->argb, argb, (size_t)w * (size_t)h * 4);
    o->w = w;
    o->h = h;
    o->scale = 1;
    o->alpha = 1;
    o->shown_alpha = -1;
    o->visible = 0;
    return noverlays++;
}

void nshvc_overlay_place(int id, float cx, float cy, float scale, float angle_rad, float alpha, int visible)
{
    Overlay *o;
    if (id < 0 || id >= noverlays)
        return;
    o = &overlays[id];
    o->cx = cx;
    o->cy = cy;
    o->scale = scale;
    o->angle = angle_rad;
    o->alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    o->visible = visible != 0;
}

/* the overlays over the target (tw x th pixels for pw x ph points) */
static void draw_overlays(int tw, int th)
{
    float pw, ph, sx, sy;
    int i;
    if (!noverlays)
        return;
    nshvc_window_size(&pw, &ph);
    sx = pw > 0 ? (float)tw / pw : 1;
    sy = ph > 0 ? (float)th / ph : 1;
    for (i = 0; i < noverlays; i++) {
        Overlay *o = &overlays[i];
        float hw, hh, c, sn, q[6];
        if (!o->visible || o->alpha <= 0)
            continue;
        if (o->shown_alpha != o->alpha) {
            size_t k, n = (size_t)o->w * (size_t)o->h;
            for (k = 0; k < n; k++) {
                uint32_t a = (uint32_t)((float)(o->argb[k] >> 24) * o->alpha + 0.5f);
                o->shown[k] = (o->argb[k] & 0xFFFFFFu) | a << 24;
            }
            o->shown_alpha = o->alpha;
        }
        hw = (float)o->w * o->scale / 2;
        hh = (float)o->h * o->scale / 2;
        c = cosf(o->angle);
        sn = sinf(o->angle);
        /* the corners (top left, top right, bottom left) turned about the
           centre, in points, then in target pixels */
        q[0] = (o->cx + (-hw * c - -hh * sn)) * sx;
        q[1] = (o->cy + (-hw * sn + -hh * c)) * sy;
        q[2] = (o->cx + (hw * c - -hh * sn)) * sx;
        q[3] = (o->cy + (hw * sn + -hh * c)) * sy;
        q[4] = (o->cx + (-hw * c - hh * sn)) * sx;
        q[5] = (o->cy + (-hw * sn + hh * c)) * sy;
        rr_overlay(o->shown, o->w, o->h, q);
    }
}

/* ---- the application ---- */
static int headless;
static long max_frames = -1;
static long frame_no;
static FILE *sound_f;

/* ---- the save ---- */
static uint8_t *saved;
static uint32_t save_from, save_size;
static char save_path[1024];

static uint32_t sram_bytes(void)
{
    return snes_sram_size();
}

static void save_load(void)
{
    FILE *f;
    uint32_t n = sram_bytes();
    if (headless || !snes_cart.battery || !n || !snes_sram())
        return;
    save_from = snes_cart.save_size ? snes_cart.save_from : 0;
    save_size = snes_cart.save_size ? snes_cart.save_size : n;
    if (save_from + save_size > n)
        nshvc_fatal("the cartridge's save area ($%X + $%X) is past its RAM ($%X)", save_from, save_size, n);
    saved = (uint8_t *)malloc(n);
    if (!saved)
        nshvc_fatal("no memory for the save");
    memcpy(saved, snes_sram(), n);
    snprintf(save_path, sizeof save_path, "%s/%s.srm", host_save_dir(), snes_cart.save_name);
    f = fopen(save_path, "rb");
    if (!f)
        return;
    if (fread(snes_sram(), 1, n, f) == n)
        memcpy(saved, snes_sram(), n);
    fclose(f);
}

/* written (to a new file, then renamed over the old) when the save area
   changed */
static void save_write(void)
{
    char tmp[1100];
    FILE *f;
    uint32_t n = sram_bytes();
    if (!saved)
        return;
    if (!memcmp(saved + save_from, snes_sram() + save_from, save_size))
        return;
    memcpy(saved, snes_sram(), n);
    snprintf(tmp, sizeof tmp, "%s.new", save_path);
    f = fopen(tmp, "wb");
    if (!f)
        return;
    fwrite(saved, 1, n, f);
    fclose(f);
    remove(save_path);
    rename(tmp, save_path);
}

/* ---- sound: the DSP's 32,040 Hz to the device's rate ---- */
#define OUT_RATE 48000
static Resampler *s_rs;

static void audio_cb(float *lr, int frames, void *user)
{
    int16_t in[2048 * 2];
    int got, made, q;
    double target = NSHVC_DSP_RATE * 0.05, d;
    (void)user;
    while (resample_queued(s_rs) < 12000 && (got = snes_audio_take(in, 2048)) > 0)
        resample_push(s_rs, in, got);
    q = resample_queued(s_rs) + snes_audio_queued();
    d = (q - target) / target;
    resample_adjust(s_rs, 0.005 * (d > 1 ? 1 : d < -1 ? -1 : d));
    made = resample_pull(s_rs, lr, frames);
    if (made < frames)
        memset(lr + 2 * made, 0, (size_t)(frames - made) * 2 * sizeof *lr);
}

static void sound_drain(void)
{
    int16_t b[2048 * 2];
    int n;
    while ((n = snes_audio_take(b, 2048)) > 0)
        if (!script_replaying())
            fwrite(b, 4, (size_t)n, sound_f);
}

/* ---- the picture on the window ---- */
/* where the last frame was drawn on the renderer's target (pixels), and
   that target's width */
static int shown_at[4], shown_tw;

int console_picture_at(float x, float y, int *dot, int *line)
{
    const PpuFrame *f = ppu_frame();
    float ww, wh;
    nshvc_window_size(&ww, &wh);
    if (!shown_tw || ww <= 0)
        return 0;
    return input_point(shown_at, (float)shown_tw / ww, f->width, f->screen_height, f->height, x, y, dot, line);
}

/* the frame as large as the window holds, centred, sharp bilinear: the
   console's 256 x 224 at a television's 4:3, a larger screen at the same
   shape of pixel */
static void show(void)
{
    int tw, th, w, h, d[4];
    double aspect;
    rr_target(&tw, &th);
    {
        const PpuFrame *f = ppu_frame();
        aspect = 4.0 / 3.0 * ((double)f->width / PPU_WIDTH) * (224.0 / (double)f->screen_height);
    }
    w = (int)(th * aspect + 0.5);
    h = th;
    if (w > tw) {
        w = tw;
        h = (int)(tw / aspect + 0.5);
    }
    d[0] = (tw - w) / 2;
    d[1] = (th - h) / 2;
    d[2] = d[0] + w;
    d[3] = d[1] + h;
    memcpy(shown_at, d, sizeof shown_at);
    shown_tw = tw;
    rr_begin(0xFF000000u);
    {
        int pw, ph;
        const uint32_t *px = ppu_picture(&pw, &ph);
        rr_image(px, pw, ph, d);
    }
    draw_overlays(tw, th);
    rr_present();
}

/* NSHVC_RRCHECK=1 (headless, any RENDER): each finished frame shown by the
   renderer one to one and scaled to the target with sharp bilinear, read
   back, against the PPU's picture drawn the same way on the CPU (rr_size.h
   rr_ref_image); at exit, the frames and pixels that differ by more than 1
   in a channel (a GPU's filtering rounds) */
static long rr_frames = -1, rr_bad, rr_pixels, rr_first = -1;

static void rr_report(void)
{
    fprintf(stderr, "rrcheck: %s, %ld frames, %ld differ (first at frame %ld), %ld pixels\n", rr_name(), rr_frames,
            rr_bad, rr_first, rr_pixels);
}

static void rr_check(void)
{
    static uint32_t *got, *want;
    int tw, th, pw, ph, i, d[4];
    const uint32_t *px = ppu_picture(&pw, &ph);
    long bad = 0;
    if (rr_frames < 0) {
        if (!rr_open()) {
            fprintf(stderr, "rrcheck: the renderer cannot open\n");
            exit(2);
        }
        atexit(rr_report);
        rr_frames = 0;
    }
    rr_target(&tw, &th);
    if (!got) {
        got = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
        want = (uint32_t *)malloc((size_t)tw * (size_t)th * 4);
    }
    /* one to one at the top left, the rest of the target the picture
       scaled up (whole and part pixels) */
    d[0] = 0;
    d[1] = 0;
    d[2] = tw;
    d[3] = th;
    rr_begin(0xFF000000u);
    rr_image(px, pw, ph, d);
    rr_present();
    rr_read(got);
    for (i = 0; i < tw * th; i++)
        want[i] = 0xFF000000u;
    rr_ref_image(want, tw, th, px, pw, ph, d);
    for (i = 0; i < tw * th; i++) {
        int sh, worst = 0;
        for (sh = 0; sh < 24; sh += 8) {
            int a = (int)((got[i] >> sh) & 0xFF) - (int)((want[i] >> sh) & 0xFF);
            if (a < 0)
                a = -a;
            if (a > worst)
                worst = a;
        }
        if (worst > 1)
            bad++;
    }
    rr_frames++;
    if (bad) {
        rr_bad++;
        rr_pixels += bad;
        if (rr_first < 0)
            rr_first = frame_no;
    }
}

/* ---- each frame ---- */
static uint64_t next_due;

static void quit(void)
{
    save_write();
    host_shutdown();
    exit(0);
}

/* After a frame: in a headless run the script's shots, dumps and buttons;
   else the window, input, save and the pace. */
static void frame(void)
{
    uint16_t b0 = 0, b1 = 0;
    frame_no++;
    if (headless) {
        static int rc = -1;
        int more;
        if (rc < 0)
            rc = getenv("NSHVC_RRCHECK") != NULL;
        if (rc)
            rr_check();
        if (sound_f)
            sound_drain();
        more = script_frame(&b0, &b1);
        nshvc_set_joypad(0, (uint16_t)(b0 | game_buttons[0]));
        nshvc_set_joypad(1, (uint16_t)(b1 | game_buttons[1]));
        input_script();
        input_report(hooks.input);
        if (hooks.frame)
            hooks.frame();
        if ((max_frames >= 0 && frame_no >= max_frames) || (!more && script_steps() && max_frames < 0))
            exit(0);
        return;
    }
    show();
    {
        host_event e;
        while (host_poll_event(&e, 0)) {
            if (e.type == HOST_EV_KEY) {
                if (!(hooks.key && hooks.key(e.vk, e.down)))
                    input_key(e.vk, e.down);
            } else if (e.type == HOST_EV_MOUSE) {
                input_mouse(e.x, e.y, e.dx, e.dy, e.buttons);
            } else if (e.type == HOST_EV_TOUCH) {
                input_touch(e.id, e.phase, e.x, e.y);
                if (hooks.touch) {
                    nshvc_touch t;
                    t.id = e.id;
                    t.phase = e.phase;
                    t.x = e.x;
                    t.y = e.y;
                    hooks.touch(&t, 1);
                }
            } else if (e.type == HOST_EV_CLOSE)
                quit();
            else if (e.type == HOST_EV_FOCUS && !e.down)
                input_focus_lost();
        }
    }
    input_frame(game_buttons);
    input_report(hooks.input);
    if (frame_no % 120 == 0)
        save_write();
    if (hooks.frame)
        hooks.frame();
    /* the pace: each frame due a frame's time after the last */
    {
        uint64_t frame_ns = snes_pal() ? 19997194ull : 16639267ull;
        uint64_t now = host_ticks_ns();
        if (!next_due || now > next_due + 100000000ull)
            next_due = now;
        next_due += frame_ns;
        while ((now = host_ticks_ns()) < next_due) {
            uint64_t left = next_due - now;
            if (left > 2000000ull)
                host_sleep_ms((uint32_t)(left / 1000000ull) - 1);
        }
    }
}

/* NSHVC_PPUCHECK=1: the frames drawn from their records against the
   picture drawn a line at a time (ppu.c), at exit */
static void ppucheck_report(void)
{
    if (getenv("NSHVC_PPUCHECK"))
        fprintf(stderr, "ppucheck: %lu frames, %lu differ; %lu needed more copies than kept\n", ppu_check_frames,
                ppu_check_bad, ppu_version_overflow);
}

int nshvc_main(const nshvc_cart *cart, int argc, char **argv)
{
    const char *script = NULL, *out_dir = ".";
    int i;
    atexit(ppucheck_report);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--sound") && i + 1 < argc) {
            /* a new copy of the program going on from a replay adds to it */
            if (!(sound_f = fopen(argv[++i], getenv("NSHVC_SCRIPT_PLAN") ? "ab" : "wb"))) {
                fprintf(stderr, "nativeshvc: cannot write %s\n", argv[i]);
                return 2;
            }
        }
    }
    host_set_app_name(cart->save_name ? cart->save_name : cart->title, cart->title ? cart->title : "nativeshvc");
    host_init(argc, argv);
    script_args(argc, argv);
    if (script)
        script_load(script, out_dir);
    nshvc_power(cart);
    save_load();
    if (headless && script) {
        /* A step's first frame has its buttons before that frame runs: set
           at the end of the frame before. The first frame has none before
           it: it is taken now (its buttons reach no joypad read), with the
           shots and dumps before it. */
        uint16_t b0, b1;
        script_takes();
        script_frame(&b0, &b1);
    }
    if (!headless) {
        host_thread_interactive();
        host_window_open(PPU_WIDTH, 224, snes_cart.title);
        if (!rr_open()) {
            host_message_box("The renderer could not start on this computer.", snes_cart.title);
            return 1;
        }
        s_rs = resample_new(NSHVC_DSP_RATE, OUT_RATE);
        host_audio_open(OUT_RATE, audio_cb, NULL);
    }
    for (;;) {
        nshvc_run_frame();
        frame();
    }
}
