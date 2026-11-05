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
#include "rr_mode7.h"
#include "resample.h"
#include "script.h"
#include "input.h"
#include "front_check.h"

/* ---- the public calls ---- */
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
void nshvc_ext_screen(int width, int height)
{
    int mw, mh;
    rr_screen_max(&mw, &mh);
    if (width > mw || height > mh)
        nshvc_fatal("nshvc_ext_screen(%d, %d): this platform's screen is at most %d x %d", width, height, mw, mh);
    rr_canvas_max(&mw, &mh);
    if (mw > 0 && ppu_canvas_k() && (width * ppu_canvas_k() > mw || height * ppu_canvas_k() > mh))
        nshvc_fatal("nshvc_ext_screen(%d, %d): the canvas on, at k %d, past this platform's %d x %d texels", width, height,
                    ppu_canvas_k(), mw, mh);
    snes_ppu_catch_up();
    ppu_ext_screen(width, height);
}
void nshvc_ext_screen_origin(int x, int y) { snes_ppu_catch_up(); ppu_ext_screen_origin(x, y); }
void nshvc_ext_sprite_x(int sprite, int on) { snes_ppu_catch_up(); ppu_ext_sprite_x(sprite, on); }
void nshvc_ext_sprite_y(int sprite, int on) { snes_ppu_catch_up(); ppu_ext_sprite_y(sprite, on); }
void nshvc_ext_sprite_page(int sprite, int x_page, int y_page) { snes_ppu_catch_up(); ppu_ext_sprite_page(sprite, x_page, y_page); }
void nshvc_ext_sprite_limits(int on) { snes_ppu_catch_up(); ppu_ext_sprite_limits(on); }
void nshvc_ext_sprite_wrap(int on) { snes_ppu_catch_up(); ppu_ext_sprite_wrap(on); }
void nshvc_ext_vram(int kb) { snes_ppu_catch_up(); ppu_ext_vram(kb); }
void nshvc_ext_vram_page(int page) { snes_ppu_catch_up(); ppu_ext_vram_page(page); }
void nshvc_ext_layer_page(int layer, int page) { snes_ppu_catch_up(); ppu_ext_layer_page(layer, page); }
void nshvc_ext_layer_clip(int layer, int on) { snes_ppu_catch_up(); ppu_ext_layer_clip(layer, on); }
void nshvc_ext_window_edges(int on) { snes_ppu_catch_up(); ppu_ext_window_edges(on); }
void nshvc_ext_window_map(int on, int32_t scale_q16, int offset) { snes_ppu_catch_up(); ppu_ext_window_map(on, scale_q16, offset); }
void nshvc_ext_window_columns(int window, int left, int right) { snes_ppu_catch_up(); ppu_ext_window_columns(window, left, right); }
void nshvc_ext_layer_outside(int layer, const uint16_t *map, int size) { snes_ppu_catch_up(); ppu_ext_layer_outside(layer, map, size); }
void nshvc_ext_vram_copy(int to_page, int from_page) { snes_ppu_catch_up(); ppu_ext_vram_copy(to_page, from_page); }
void nshvc_ext_vram_direct(int page) { snes_ppu_catch_up(); ppu_ext_vram_direct(page); }
int nshvc_ext_canvas_scale(void) { return ppu_canvas_scale(); }
/* the window's pixels the console's screen was last presented over
   (nshvc_ext_canvas_presented; 0: not yet) */
static int presented_w, presented_h;

static void shot_scale(double *x, double *y);
void nshvc_ext_canvas_presented(int *w, int *h)
{
    const PpuFrame *f = ppu_frame();
    double sx, sy;
    int pw = presented_w, ph = presented_h;
    if (!pw) {
        /* none yet: headless, NSHVC_SCALE's window; else the screen's own */
        shot_scale(&sx, &sy);
        pw = sx ? (int)(f->width * sx + 0.5) : f->width;
        ph = sy ? (int)(f->screen_height * sy + 0.5) : f->screen_height;
    }
    if (w)
        *w = pw < 1 ? 1 : pw;
    if (h)
        *h = ph < 1 ? 1 : ph;
}
/* the host's VRAM free while the game runs (host_psp.c); none elsewhere */
__attribute__((weak)) void *host_ge_vram(size_t *bytes)
{
    *bytes = 0;
    return NULL;
}
void *nshvc_ext_ge_vram(size_t *bytes)
{
    size_t n = 0;
    void *p = host_ge_vram(&n);
    if (bytes)
        *bytes = p ? n : 0;
    return p;
}
/* a canvas past the platform's (rr_canvas_max): fatal. w x h its texels
   (a character canvas's: none, 0), k a whole scale's (0: none) */
static void canvas_fits(const char *what, int w, int h, int k)
{
    const PpuFrame *f = ppu_frame();
    int mw, mh;
    rr_canvas_max(&mw, &mh);
    if (mw > 0 && (w > mw || h > mh || (k > 0 && (f->width * k > mw || f->screen_height * k > mh))))
        nshvc_fatal("%s: %d x %d texels at k %d on a %d x %d screen: this platform's canvas is at most %d x %d texels "
                    "(the screen times k too)",
                    what, w ? w : f->width * k, h ? h : f->screen_height * k, k, f->width, f->screen_height, mw, mh);
}
void nshvc_ext_canvas_chars(int layer, unsigned word_addr, int count, const uint8_t *texels, int k)
{
    snes_ppu_catch_up();
    if (texels && count > 0)
        canvas_fits("nshvc_ext_canvas_chars", 0, 0, k);
    ppu_ext_canvas_chars(layer, word_addr, count, texels, k);
}
void nshvc_ext_canvas_screen(int layer, const uint8_t *texels, int w, int h, int k, int priority, int palette)
{
    snes_ppu_catch_up();
    if (texels)
        canvas_fits("nshvc_ext_canvas_screen", w, h, k);
    ppu_ext_canvas_screen(layer, texels, w, h, k, priority, palette);
}
void nshvc_ext_canvas_screen32(int layer, const uint32_t *texels, int w, int h, int k, int priority, int palette)
{
    snes_ppu_catch_up();
    if (texels)
        canvas_fits("nshvc_ext_canvas_screen32", w, h, k);
    ppu_ext_canvas_screen32(layer, texels, w, h, k, priority, palette);
}

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

/* ---- the watchdog ----
   A loop in the game's C that waits for the console's time, with its accesses
   taking none (the costs off, interrupts held), never ends: no frame comes.
   NSHVC_WATCHDOG=N: no frame for N seconds ends the run, naming the last
   access made through the console (a loop reading memory alone: the last
   before it). A game's default 30 s; headless, none (0 too: none). */
static volatile uint32_t loop_passes;        /* the front end's loop, paused too */
static long watch_secs;

static void watchdog(void)
{
    static uint64_t last = ~(uint64_t)0;
    static long idle;
    uint64_t now = (uint64_t)ppu_frames() + loop_passes;
    uint32_t a;
    if (now != last) {
        last = now;
        idle = 0;
        return;
    }
    if (++idle < watch_secs)
        return;
    a = snes_last_access();
    nshvc_fatal("no frame in %ld seconds: a loop that waits for the console's time, which its accesses do not take "
                "(wait with nshvc_wait_until or spc_wait_until); the last access through the console: a %s of $%06X",
                watch_secs, (a >> 24) ? "write" : "read", (unsigned)(a & 0xFFFFFFu));
}

static void watchdog_start(int is_headless)
{
    const char *e = getenv("NSHVC_WATCHDOG");
    watch_secs = e ? atol(e) : is_headless ? 0 : 30;
    if (watch_secs > 0)
        host_every(1000, watchdog);
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
void nshvc_ext_text_input(int on) { host_text_input(on); }
int nshvc_keyboard_attached(void) { return host_keyboard_attached(); }

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
    uint32_t *argb;
    int w, h;
    float cx, cy, scale, angle, alpha;
    int visible;
    /* an image the GPU holds (nshvc_ext_overlay_gpu): no argb, its handle
       (0: none now) and fence; shown: a frame has drawn this handle */
    int gpu, gpu_shown;
    uint64_t image, view, event, value;
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
    if (!o->argb)
        nshvc_fatal("no memory for an overlay");
    memcpy(o->argb, argb, (size_t)w * (size_t)h * 4);
    o->w = w;
    o->h = h;
    o->scale = 1;
    o->alpha = 1;
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

void nshvc_overlay_update(int id, const uint32_t *argb)
{
    Overlay *o;
    if (id < 0 || id >= noverlays)
        return;
    o = &overlays[id];
    if (o->gpu)
        nshvc_fatal("nshvc_overlay_update(%d): this overlay's image is the GPU's (nshvc_ext_overlay_gpu)", id);
    memcpy(o->argb, argb, (size_t)o->w * (size_t)o->h * 4);
}

/* ---- the game's pause (nshvc_ext_pause) ----
   While paused the machine does not run (the loop in nshvc_main), the
   sound is silence (audio_cb), and each display frame shows the last
   frame presented before the pause, blurred and tinted (the backdrop),
   under the game's overlays. The backdrop is made once by the renderer,
   from that frame, before anything else is drawn (rr_backdrop_make), and
   drawn back over the target each frame (rr_backdrop_draw). */
static volatile int paused;
static float pause_blur;
static uint32_t pause_tint;
static int backdrop, backdrop_due;

void nshvc_ext_pause(int on, float blur, uint32_t tint)
{
    if (on && !paused)
        backdrop_due = 1;
    pause_blur = blur < 0 ? 0 : blur;
    pause_tint = tint;
    paused = on != 0;
}

int nshvc_ext_paused(void) { return paused; }

/* the backdrop from the last presented frame, at scale target pixels a point */
static void make_backdrop(float scale)
{
    backdrop_due = 0;
    backdrop = rr_backdrop_make(pause_blur * scale, pause_tint);
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
        if (!o->visible || o->alpha <= 0 || (o->gpu && !o->image))
            continue;
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
        if (o->gpu) {
            rr_overlay_gpu(o->image, o->view, o->w, o->h, q, o->alpha, o->event, o->value);
            o->event = o->value = 0;             /* (a fence waited on once) */
            o->gpu_shown = 1;
        } else {
            rr_overlay(o->argb, o->w, o->h, q, o->alpha);
        }
    }
}

/* ---- the application ---- */
static int headless;

/* --counts FILE (headless): each frame's S-CPU accesses by kind, for
   tools/nshvc-profile's step 2: "F frame" and the reads of each kind
   (nshvc.h NSHVC_KIND_*), then the writes. A stretch of more than 64 reads
   with no write (a poll loop, a wait's reads) is left out, as the
   profile's measure of the original leaves it out. */
static FILE *counts_f;
static uint64_t counts[2][NSHVC_KINDS];
static uint8_t pending_kind[65];
static unsigned pending_n;

static void count_read(uint32_t addr, uint8_t value)
{
    (void)value;
    if (pending_n < 65)
        pending_kind[pending_n] = (uint8_t)nshvc_access_kind(addr);
    pending_n++;
}

static void count_write(uint32_t addr, uint8_t value)
{
    unsigned k;
    (void)value;
    if (pending_n <= 64)
        for (k = 0; k < pending_n; k++)
            counts[0][pending_kind[k]]++;
    pending_n = 0;
    counts[1][nshvc_access_kind(addr)]++;
}

static void counts_frame(long n)
{
    int w, k;
    fprintf(counts_f, "F %ld", n);
    for (w = 0; w < 2; w++)
        for (k = 0; k < NSHVC_KINDS; k++)
            fprintf(counts_f, " %llu", (unsigned long long)counts[w][k]);
    fprintf(counts_f, "\n");
    memset(counts, 0, sizeof counts);
}
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
    if (paused) {                       /* the machine makes no sound while paused */
        memset(lr, 0, (size_t)frames * 2 * sizeof *lr);
        return;
    }
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

/* the canvas's texels a console pixel along each axis for a picture shown
   w x h: the window's pixels a console pixel, rounded up (so a canvas at
   that scale has at least a texel a window pixel) */
/* k at most the platform's canvas allows (rr_canvas_max) */
static int canvas_k_max(int k)
{
    const PpuFrame *f = ppu_frame();
    int mw, mh, kw, kh;
    rr_canvas_max(&mw, &mh);
    if (mw <= 0)
        return k;
    kw = mw / f->width;
    kh = mh / f->screen_height;
    if (k > kw)
        k = kw;
    if (k > kh)
        k = kh;
    return k < 1 ? 1 : k;
}

static void canvas_scale(int w, int h)
{
    const PpuFrame *f = ppu_frame();
    presented_w = w;
    presented_h = h;
    int kx = (w + f->width - 1) / f->width, ky = (h + f->screen_height - 1) / f->screen_height;
    ppu_set_canvas_scale(canvas_k_max(kx > ky ? kx : ky));
}

/* the checks' (tests/ref, front_check.h): after each headless frame */
void (*front_frame_hook)(void);

/* The finished frame's description for the compositor: its picture w x h;
   wp an interlaced frame's weave (rr_compose_woven's ww, base0, base1: its
   own and, for the rows that are the frame before's, that one's), else
   zeros. 0 when there is no picture to draw: a frame never made (no VRAM
   taken: before the first). */
static int frame_desc(const uint32_t **desc, size_t *words, int *w, int *h, uint32_t wp[3])
{
    const PpuFrame *f = ppu_frame();
    wp[0] = wp[1] = wp[2] = 0;
    if (f->versions == 0)
        return 0;
    if (f->interlace && f->cv_on && f->cv_gpu)
        nshvc_fatal("a canvas the GPU holds (nshvc_ext_canvas_screen_gpu, _ge) on an interlaced frame: no weave of its texels");
    if (!f->interlace) {
        *desc = ppu_compose(f, words);
        *w = ppu_frame_width(f);
        *h = f->screen_height;
        return 1;
    } else {
        static uint32_t *both, *kept;
        static size_t cap, kept_n, kept_cap;
        /* the description last composed as the finished frame's: the next
           frame's frame before (the same record, whole until then) */
        static const PpuFrame *kept_f;
        static long kept_no = -2;
        int ww, wh, from[2];
        size_t na, nb = 0;
        const uint32_t *a;
        if (ppu_woven(&ww, &wh, from) && (!(from[0] | from[1]) || ppu_frame_before()->versions > 0)) {
            a = ppu_compose(f, &na);
            if (na > cap) {
                free(both);
                both = (uint32_t *)malloc(na * 2 * sizeof *both);
                cap = both ? na * 2 : 0;
            }
            if (both) {
                memcpy(both, a, na * sizeof *both);
                if (from[0] | from[1]) {
                    const uint32_t *b;
                    if (kept && kept_f == ppu_frame_before() && kept_no + 1 == frame_no) {
                        b = kept;
                        nb = kept_n;
                    } else {
                        b = ppu_compose(ppu_frame_before(), &nb);
                    }
                    if (na + nb > cap) {
                        uint32_t *g = (uint32_t *)realloc(both, (na + nb) * sizeof *both);
                        if (g) {
                            both = g;
                            cap = na + nb;
                        }
                    }
                    if (na + nb <= cap)
                        memcpy(both + na, b, nb * sizeof *both);
                    else
                        nb = (size_t)-1;
                }
                if (na > kept_cap) {
                    free(kept);
                    kept = (uint32_t *)malloc(na * sizeof *kept);
                    kept_cap = kept ? na : 0;
                }
                kept_f = NULL;
                if (kept) {
                    memcpy(kept, both, na * sizeof *kept);
                    kept_n = na;
                    kept_f = f;
                    kept_no = frame_no;
                }
                if (nb != (size_t)-1) {
                    *desc = both;
                    *words = na + nb;
                    *w = ww;
                    *h = wh;
                    wp[0] = (uint32_t)ww;
                    wp[1] = from[0] ? (uint32_t)na : 0;
                    wp[2] = from[1] ? (uint32_t)na : 0;
                    return 1;
                }
            }
        }
        nshvc_fatal("no memory for the interlaced frame's descriptions");
    }
    return 0;
}

/* The last frame's picture over target rectangle d, drawn by the
   renderer's compositor from the frame's description (render/compose_body.h),
   an interlaced frame's woven; a frame never made none, the target's
   black. The picture is the GPU's: a renderer that cannot draw it is
   fatal. */
static void show_picture(const int d[4])
{
    const uint32_t *desc;
    size_t nw;
    int w, h, ok;
    uint32_t wp[3];
    if (!frame_desc(&desc, &nw, &w, &h, wp))
        return;
    ok = wp[0] ? rr_compose_woven(desc, nw, w, h, wp[1], wp[2], d) : rr_compose(desc, nw, w, h, d);
    if (!ok)
        nshvc_fatal("the renderer (%s) cannot draw the frame's picture", rr_name());
}

/* the renderer, open when a picture is asked for with none presented yet */
static int rr_opened;

static void rr_need(void)
{
    if (!rr_opened) {
        if (!rr_open())
            nshvc_fatal("the renderer (%s) cannot open", rr_name());
        rr_opened = 1;
    }
}

/* ---- pictures the GPU holds (contract 2.22) ---- */
__attribute__((weak)) int rr_gpu_device(struct NshvcGpuLoan *loan)
{
    (void)loan;
    return 0;
}
__attribute__((weak)) int rr_ge_picture(const char *what, const void *vram, int stride, int w, int h, int psm)
{
    (void)what; (void)vram; (void)stride; (void)w; (void)h; (void)psm;
    return 0;
}
__attribute__((weak)) void rr_overlay_gpu(uint64_t image, uint64_t view, int w, int h, const float q[6], float alpha,
                                          uint64_t event, uint64_t value)
{
    (void)image; (void)view; (void)w; (void)h; (void)q; (void)alpha; (void)event; (void)value;
    nshvc_fatal("the renderer (%s) draws no overlay from the GPU", rr_name());
}
/* the VRAM the frame about to be drawn reads (host_psp.c: not the buffer it
   is drawn into); none elsewhere */
__attribute__((weak)) void host_ge_hold(const void *a, const void *b, const void *c)
{
    (void)a; (void)b; (void)c;
}

/* the depth buffer lent with the host's VRAM, its row in pixels (host_psp.c);
   none elsewhere */
__attribute__((weak)) void *host_ge_depth(int *stride)
{
    *stride = 0;
    return NULL;
}

void *nshvc_ext_ge_vram2(size_t *bytes, void **depth, int *depth_stride)
{
    int stride = 0;
    void *p = nshvc_ext_ge_vram(bytes);
    /* (while paused that VRAM holds the backdrop: rr_backdrop_make) */
    void *d = p && !paused ? host_ge_depth(&stride) : NULL;
    if (depth)
        *depth = d;
    if (depth_stride)
        *depth_stride = d ? stride : 0;
    return p;
}

/* the host's GPU, the renderer opened to ask it (0 where neither opens) */
static int gpu_api(NshvcGpuLoan *loan)
{
    NshvcGpuLoan l;
    int api;
    memset(&l, 0, sizeof l);
    if (!rr_opened) {
        if (!rr_open())
            return 0;
        rr_opened = 1;
    }
    api = rr_gpu_device(&l);
    if (loan)
        *loan = l;
    return api;
}

int nshvc_ext_gpu_device(NshvcGpuLoan *loan) { return gpu_api(loan); }

void nshvc_ext_canvas_screen_gpu(int layer, NshvcGpuHandle handle, int w, int h, uint32_t format, int priority,
                                 NshvcGpuFence fence)
{
    static const char name[] = "nshvc_ext_canvas_screen_gpu";
    snes_ppu_catch_up();
    if (handle.image) {
        int api = gpu_api(NULL);
        if (!api)
            nshvc_fatal("%s: this host lends no GPU (nshvc_ext_gpu_device 0)", name);
        if (format != (api == NSHVC_GPU_METAL ? NSHVC_GPU_FORMAT_METAL : NSHVC_GPU_FORMAT_VULKAN))
            nshvc_fatal("%s: format %u: 32-bit unsigned texels (MTLPixelFormatR32Uint, VK_FORMAT_R32_UINT)", name, (unsigned)format);
        canvas_fits(name, w, h, 0);
    }
    ppu_ext_canvas_screen_gpu(name, 1, layer, handle.image, w, h, priority, fence.event, fence.value);
}

void nshvc_ext_canvas_screen_ge(int layer, void *vram_address, int stride, int w, int h, int psm, int priority)
{
    static const char name[] = "nshvc_ext_canvas_screen_ge";
    snes_ppu_catch_up();
    if (vram_address) {
        if (!rr_opened && !rr_open())
            nshvc_fatal("%s: no renderer", name);
        rr_opened = 1;
        if (!rr_ge_picture(name, vram_address, stride, w, h, psm))
            nshvc_fatal("%s: this host has no GE (nshvc_ext_ge_vram2 NULL)", name);
        if (psm != RR_GE_PSM_8888)
            nshvc_fatal("%s: psm %d: GU_PSM_8888 alone", name, psm);
        canvas_fits(name, w, h, 0);
    }
    ppu_ext_canvas_screen_gpu(name, 2, layer, (uint64_t)(uintptr_t)vram_address, w, h, priority, 0,
                              (uint64_t)(uint32_t)stride);
}

void nshvc_ext_canvas_chars_gpu(int layer, unsigned word_addr, int count, NshvcGpuHandle handle, int per_row, int k,
                                NshvcGpuFence fence)
{
    static const char name[] = "nshvc_ext_canvas_chars_gpu";
    snes_ppu_catch_up();
    if (handle.image && count > 0) {
        if (!gpu_api(NULL))
            nshvc_fatal("%s: this host lends no GPU (nshvc_ext_gpu_device 0)", name);
        canvas_fits(name, 0, 0, k);
    }
    ppu_ext_canvas_chars_gpu(name, 1, layer, word_addr, count, handle.image, per_row, k, fence.event, fence.value);
}

void nshvc_ext_canvas_chars_ge(int layer, unsigned word_addr, int count, void *vram_address, int stride, int per_column,
                               int psm, int k)
{
    static const char name[] = "nshvc_ext_canvas_chars_ge";
    snes_ppu_catch_up();
    if (vram_address && count > 0) {
        int k8 = 8 * k, across, cols, bands;
        if (k < 1 || k > 64 || stride < k8 || per_column < 1)
            nshvc_fatal("%s: k %d, a row of %d pixels, %d characters a column: k 1 to 64, a row and a column of at least "
                        "one character", name, k, stride, per_column);
        /* the columns a band holds: the row's, at most the GE's 512 texels */
        across = (stride < 512 ? stride : 512) / k8;
        cols = (count + per_column - 1) / per_column;
        bands = (cols + across - 1) / across;
        if (!rr_opened && !rr_open())
            nshvc_fatal("%s: no renderer", name);
        rr_opened = 1;
        if (!rr_ge_picture(name, vram_address, stride, (cols < across ? cols : across) * k8, bands * per_column * k8, psm))
            nshvc_fatal("%s: this host has no GE (nshvc_ext_ge_vram2 NULL)", name);
        canvas_fits(name, 0, 0, k);
    }
    /* (the fence's event, unused on the GE: the texels' psm) */
    ppu_ext_canvas_chars_gpu(name, 2, layer, word_addr, count, (uint64_t)(uintptr_t)vram_address, per_column, k,
                             (uint64_t)(uint32_t)psm, (uint64_t)(uint32_t)stride);
}

/* the one overlay whose image the GPU holds */
static int overlay_gpu_id = -1;

int nshvc_ext_overlay_gpu(NshvcGpuHandle handle, int w, int h, NshvcGpuFence fence)
{
    static const char name[] = "nshvc_ext_overlay_gpu";
    Overlay *o;
    if (handle.image) {
        if (w <= 0 || h <= 0)
            nshvc_fatal("%s: %d x %d", name, w, h);
        if (!gpu_api(NULL) && !rr_ge_picture(name, (const void *)(uintptr_t)handle.image, (int)handle.view, w + 1, h + 1,
                                             RR_GE_PSM_8888))
            nshvc_fatal("%s: this host lends no GPU (nshvc_ext_gpu_device 0, nshvc_ext_ge_vram2 NULL)", name);
    }
    if (overlay_gpu_id < 0) {
        if (noverlays == NSHVC_OVERLAYS)
            return -1;
        overlay_gpu_id = noverlays++;
        o = &overlays[overlay_gpu_id];
        memset(o, 0, sizeof *o);
        o->gpu = 1;
        o->scale = 1;
        o->alpha = 1;
    }
    o = &overlays[overlay_gpu_id];
    o->image = handle.image;
    o->view = handle.view;
    o->event = fence.event;
    o->value = fence.value;
    o->gpu_shown = 0;
    if (handle.image) {
        o->w = w;
        o->h = h;
    }
    return overlay_gpu_id;
}

/* the game's frame hook, and the handles the GPU holds it let go by not
   handing again: those a frame has shown. While the machine stands no frame
   is made: a texture canvas stays for the next, handed or not. VRAM is let go
   then, its buffer wanted: with the frame on screen and the frame being drawn,
   the third of the screen's buffers is the pause's overlay's */
static void frame_hook(void)
{
    int stood = paused;
    if (hooks.frame)
        hooks.frame();
    if (paused)
        ppu_canvas_ge_off();
    else if (!stood)
        ppu_canvas_gpu_end();
    if (overlay_gpu_id >= 0 && overlays[overlay_gpu_id].gpu_shown) {
        overlays[overlay_gpu_id].image = 0;
        overlays[overlay_gpu_id].gpu_shown = 0;
    }
}

/* the VRAM the frame to be drawn reads, for the host (host_ge_hold) */
static void ge_holds(void)
{
    const PpuFrame *f = ppu_frame();
    uint64_t a = !paused && f->cv_on && f->cv_gpu == 2 ? f->cv_image : 0, c = ppu_canvas_ge_asked();
    uint64_t b = overlay_gpu_id >= 0 ? overlays[overlay_gpu_id].image : 0;
    host_ge_hold((const void *)(uintptr_t)a, (const void *)(uintptr_t)b, (const void *)(uintptr_t)c);
}

/* the finished frame's picture: the GPU's (rr_picture), read back */
const uint32_t *nshvc_picture(int *width, int *height)
{
    static uint32_t *px;
    static size_t cap;
    const PpuFrame *f = ppu_frame();
    const uint32_t *desc;
    size_t nw, n;
    int w = ppu_frame_width(f), h = f->screen_height, have;
    uint32_t wp[3];
    have = frame_desc(&desc, &nw, &w, &h, wp);
    n = (size_t)w * (size_t)h;
    if (n > cap) {
        free(px);
        px = (uint32_t *)malloc(n * 4);
        cap = px ? n : 0;
        if (!px)
            nshvc_fatal("no memory for the picture of %d x %d", w, h);
    }
    if (have) {
        rr_need();
        if (!rr_picture(desc, nw, w, h, wp, px))
            nshvc_fatal("the renderer (%s) cannot read the frame's picture back", rr_name());
    } else {
        size_t i;
        for (i = 0; i < n; i++)
            px[i] = 0xFF000000u;
    }
    if (width)
        *width = w;
    if (height)
        *height = h;
    return px;
}

/* ---- a frame with Mode 7 or a canvas presented (render/rr_mode7.h) ---- */
static int32_t *m7_cols, *m7_rows;
static int m7_cols_cap, m7_rows_cap;

/* the last frame's record and where it samples drawn at ow x oh; 0 when it
   is drawn as its picture scaled (no Mode 7 or canvas in it, or an
   interlaced weave) */
/* (rr.h: a backend that presents Mode 7 as any frame says so) */
__attribute__((weak)) void rr_screen_max(int *w, int *h)
{
    *w = PPU_EXT_WIDTH;
    *h = PPU_EXT_HEIGHT;
}

__attribute__((weak)) int rr_mode7_plain(void) { return 0; }

__attribute__((weak)) int rr_compose_canvas(void) { return 1; }

__attribute__((weak)) void rr_canvas_max(int *w, int *h)
{
    *w = 0;
    *h = 0;
}

__attribute__((weak)) int rr_mode7_window(int what)
{
    (void)what;
    return 1;
}

/* The last frame, presenting Mode 7 or a canvas, drawn at ow x oh over d
   by the renderer's compositor (its picture and records the GPU's); 0
   when it is not drawn so (an interlaced weave, or no Mode 7 or canvas
   presented: drawn as its picture); a renderer that cannot draw it is
   fatal */
static int show_mode7(int ow, int oh, const int d[4])
{
    static RrMode7 m;
    static RrM7Samples smp;
    const PpuFrame *f = ppu_frame();
    size_t nw;
    const uint32_t *desc;
    static int key[6];
    int now[6];
    if (ow <= 0 || oh <= 0 || f->versions == 0 || f->interlace || !ppu_frame_mode7(f))
        return 0;
    desc = ppu_compose_m7(f, &nw, &m);
    now[0] = ow;
    now[1] = oh;
    now[2] = m.pw;
    now[3] = m.ph;
    now[4] = m.cpp;
    now[5] = m.left;
    if (memcmp(key, now, sizeof key)) {
        if (ow > m7_cols_cap) {
            free(m7_cols);
            m7_cols = (int32_t *)malloc((size_t)ow * 3 * 4);
            m7_cols_cap = m7_cols ? ow : 0;
        }
        if (oh > m7_rows_cap) {
            free(m7_rows);
            m7_rows = (int32_t *)malloc((size_t)oh * 4 * 4);
            m7_rows_cap = m7_rows ? oh : 0;
        }
        if (!m7_cols || !m7_rows)
            nshvc_fatal("no memory for the picture at %dx%d", ow, oh);
        rr_m7_tables(&m, ow, oh, &smp, m7_cols, m7_rows);
        memcpy(key, now, sizeof key);
    } else {
        rr_m7_tables_flags(&m, oh, m7_rows);
    }
    if (!rr_compose_mode7(desc, nw, &m, d, &smp))
        nshvc_fatal("the renderer (%s) cannot draw the frame's Mode 7 or canvas at the window's resolution", rr_name());
    return 1;
}

/* a shot's picture (script.h): NSHVC_SCALE=S or SXxSY (headless), window
   pixels a console pixel: the frame presented as a window that size shows
   it (without it, the console's picture) */
/* NSHVC_SCALE (headless): window pixels a console pixel across and down,
   0 when not set */
static void shot_scale(double *x, double *y)
{
    static int parsed;
    static double sx, sy;
    if (!parsed) {
        const char *e = getenv("NSHVC_SCALE");
        parsed = 1;
        if (e && sscanf(e, "%lfx%lf", &sx, &sy) < 2)
            sy = sx;
        if (!(sx > 0 && sy > 0 && sx <= 16 && sy <= 16))
            sx = sy = 0;
    }
    *x = sx;
    *y = sy;
}

static int show_mode7(int ow, int oh, const int d[4]);
static void show_picture(const int d[4]);

/* a shot's picture: the console's (nshvc_picture), or at NSHVC_SCALE the
   frame presented by the renderer on a target that size, read back */
const uint32_t *front_shot_picture(int *w, int *h, int *presented)
{
    double sx, sy;
    static uint32_t *buf;
    static size_t cap;
    int pw, ph, d[4];
    const uint32_t *px = nshvc_picture(&pw, &ph);
    size_t len;
    shot_scale(&sx, &sy);
    *w = sx ? (int)(pw * sx + 0.5) : pw;
    *h = sy ? (int)(ph * sy + 0.5) : ph;
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
    *presented = 0;
    if (!sx)
        return px;
    *presented = 1;
    len = (size_t)*w * (size_t)*h;
    if (len > cap) {
        free(buf);
        buf = (uint32_t *)malloc(len * 4);
        cap = buf ? len : 0;
        if (!buf)
            nshvc_fatal("no memory for the picture at %dx%d", *w, *h);
    }
    d[0] = d[1] = 0;
    d[2] = *w;
    d[3] = *h;
    rr_need();
    rr_size(*w, *h);
    ge_holds();
    rr_begin(0xFF000000u);
    if (!(rr_mode7_window(ppu_frame_presents(ppu_frame())) && show_mode7(*w, *h, d)))
        show_picture(d);
    rr_present();
    if (!rr_read(buf))
        nshvc_fatal("the renderer (%s) cannot read the frame back", rr_name());
    return buf;
}

/* (front_check.h: what the renderer's check drives) */
int front_show_mode7(int ow, int oh, const int d[4]) { return show_mode7(ow, oh, d); }
void front_show_picture(const int d[4]) { show_picture(d); }
void front_rr_need(void) { rr_need(); }
long front_frame_no(void) { return frame_no; }
void front_canvas_scale(int w, int h) { canvas_scale(w, h); }

/* ---- the picture on the window ---- */
/* where the last frame was drawn on the renderer's target (pixels), and
   that target's width */
static int shown_at[4], shown_tw, shown_th;

int console_picture_at(float x, float y, int *dot, int *line)
{
    const PpuFrame *f = ppu_frame();
    float ww, wh;
    nshvc_window_size(&ww, &wh);
    if (!shown_tw || ww <= 0)
        return 0;
    return input_point(shown_at, (float)shown_tw / ww, f->width, f->screen_height, f->left, f->top, x, y, dot, line);
}

/* the frame over the whole window, each axis at its own scale, sharp
   bilinear (no bars: the screen's shape the window's, as nshvc_ext_screen
   sizes it to suit); a frame with Mode 7 or a canvas presented by its record
   (rr_mode7: the plane at the window's pixels, anti-aliased by each
   pixel's footprint on it; the canvas's texels) */
/* ---- the frame-rate meter (NSHVC_FPS=1, NSHVC_FPS_LOG=FILE) ----
   Over the last whole second of the window's run: frames presented and
   console frames run a second, the longest time between two presents,
   and the longest each frame spent running the console, presenting and
   waiting for its time. NSHVC_FPS=1 draws it at the top left;
   NSHVC_FPS_LOG=FILE (a window's run, or headless with NSHVC_PRESENT=1,
   its wait 0) writes a line a presented frame: its number, its
   present's time from the first (ms), the time since the last present,
   and the time it spent running, presenting and waiting. */
static int fps_on = -1;
static FILE *fps_log;
static uint64_t fps_first, fps_last_shown, fps_sec, fps_frame_start;
static long fps_shown, fps_run;                    /* this second's */
static double fps_worst_gap, fps_worst_run, fps_worst_present, fps_worst_wait;
static char fps_text[96] = "";                     /* the last second's, as shown */
static uint64_t fps_ran_ns, fps_present_ns;        /* this frame's */

/* a 3 x 5 font for the meter: a row a 3-bit number, top row first */
static const struct {
    char c;
    uint8_t row[5];
} fps_font[] = {
    { '0', { 7, 5, 5, 5, 7 } }, { '1', { 2, 6, 2, 2, 7 } }, { '2', { 7, 1, 7, 4, 7 } }, { '3', { 7, 1, 3, 1, 7 } },
    { '4', { 5, 5, 7, 1, 1 } }, { '5', { 7, 4, 7, 1, 7 } }, { '6', { 7, 4, 7, 5, 7 } }, { '7', { 7, 1, 1, 1, 1 } },
    { '8', { 7, 5, 7, 5, 7 } }, { '9', { 7, 5, 7, 1, 7 } }, { '.', { 0, 0, 0, 0, 2 } }, { ' ', { 0, 0, 0, 0, 0 } },
    { 'a', { 0, 7, 5, 7, 5 } }, { 'e', { 0, 7, 7, 4, 7 } }, { 'f', { 3, 4, 7, 4, 4 } }, { 'g', { 0, 7, 5, 7, 1 } },
    { 'i', { 2, 0, 2, 2, 2 } }, { 'm', { 0, 7, 7, 5, 5 } }, { 'n', { 0, 6, 5, 5, 5 } }, { 'o', { 0, 7, 5, 5, 7 } },
    { 'p', { 0, 7, 5, 7, 4 } }, { 'r', { 0, 7, 4, 4, 4 } }, { 's', { 0, 7, 6, 3, 7 } }, { 't', { 2, 7, 2, 2, 3 } },
    { 'u', { 0, 5, 5, 5, 7 } }, { 'w', { 0, 5, 5, 7, 7 } }, { 'x', { 0, 5, 2, 2, 5 } }, { '/', { 1, 1, 2, 4, 4 } },
};

/* The meter drawn by the renderer: its font an image made once from the
   table (each glyph's 3 x 5 pixels in a cell of 5 x 7, clear round it,
   the panel's dark colour the last cell's), then each frame a quad for the
   panel and one for each character, 3 target pixels a font pixel, at the
   top left (rr_overlay_quads) */
#define FPS_GLYPHS ((int)(sizeof fps_font / sizeof fps_font[0]))
static void fps_draw(void)
{
    static uint32_t font[7 * 5 * (FPS_GLYPHS + 1)];
    static float uv[4 * 97], q[6 * 97];
    static int made;
    int fw = 5 * (FPS_GLYPHS + 1), n = 0, x, y, g;
    size_t len = strlen(fps_text), i, w = 2 + 4 * len;
    if (!len)
        return;
    if (!made) {
        for (g = 0; g < FPS_GLYPHS; g++)
            for (y = 0; y < 5; y++)
                for (x = 0; x < 3; x++)
                    if (fps_font[g].row[y] >> (2 - x) & 1)
                        font[(y + 1) * fw + 5 * g + 1 + x] = 0xFFFFFF40u;
        for (y = 0; y < 7; y++)
            for (x = 0; x < 5; x++)
                font[y * fw + 5 * FPS_GLYPHS + x] = 0xB0000000u;   /* a dark panel behind the text */
        made = 1;
    }
    /* the panel: the dark cell's middle texels over the text's box */
    uv[0] = 5.0f * FPS_GLYPHS + 2;
    uv[1] = 3;
    uv[2] = 5.0f * FPS_GLYPHS + 3;
    uv[3] = 4;
    q[0] = 8;
    q[1] = 8;
    q[2] = 8 + 3.0f * (float)w;
    q[3] = 8;
    q[4] = 8;
    q[5] = 8 + 3.0f * 7;
    n = 1;
    for (i = 0; i < len && n < 97; i++) {
        float x0 = 8 + 3.0f * (float)(1 + 4 * i), y0 = 8 + 3.0f;
        for (g = 0; g < FPS_GLYPHS && fps_font[g].c != fps_text[i]; g++)
            ;
        if (g == FPS_GLYPHS || fps_text[i] == ' ')
            continue;
        /* the glyph's cell, its clear border's half texel with it (the
           filter's edge) */
        uv[4 * n + 0] = 5.0f * g + 0.5f;
        uv[4 * n + 1] = 0.5f;
        uv[4 * n + 2] = 5.0f * g + 4.5f;
        uv[4 * n + 3] = 6.5f;
        q[6 * n + 0] = x0 - 1.5f;
        q[6 * n + 1] = y0 - 1.5f;
        q[6 * n + 2] = x0 + 3.0f * 3 + 1.5f;
        q[6 * n + 3] = y0 - 1.5f;
        q[6 * n + 4] = x0 - 1.5f;
        q[6 * n + 5] = y0 + 3.0f * 5 + 1.5f;
        n++;
    }
    rr_overlay_quads(font, fw, 7, n, uv, q, 1);
}

/* a presented frame's times, ns: the meter's second and the log's line */
static void fps_frame(uint64_t shown, uint64_t ran, uint64_t presented, uint64_t waited)
{
    double gap = fps_last_shown ? (double)(shown - fps_last_shown) / 1e6 : 0;
    if (!fps_first)
        fps_first = fps_sec = shown;
    fps_last_shown = shown;
    fps_shown++;
    fps_run++;
    if (gap > fps_worst_gap) fps_worst_gap = gap;
    if ((double)ran / 1e6 > fps_worst_run) fps_worst_run = (double)ran / 1e6;
    if ((double)presented / 1e6 > fps_worst_present) fps_worst_present = (double)presented / 1e6;
    if ((double)waited / 1e6 > fps_worst_wait) fps_worst_wait = (double)waited / 1e6;
    if (fps_log)
        fprintf(fps_log, "%ld %.3f %.3f %.3f %.3f %.3f\n", frame_no, (double)(shown - fps_first) / 1e6, gap,
                (double)ran / 1e6, (double)presented / 1e6, (double)waited / 1e6);
    if (shown - fps_sec >= 1000000000ull) {
        double secs = (double)(shown - fps_sec) / 1e9;
        snprintf(fps_text, sizeof fps_text, "%.1f fps  %.1f run  gap %.1f  ran %.1f  present %.1f  wait %.1f ms",
                 fps_shown / secs, fps_run / secs, fps_worst_gap, fps_worst_run, fps_worst_present, fps_worst_wait);
        fps_sec = shown;
        fps_shown = fps_run = 0;
        fps_worst_gap = fps_worst_run = fps_worst_present = fps_worst_wait = 0;
    }
}

static void show(void)
{
    int tw, th, w, h, d[4];
    if (paused && backdrop_due && shown_tw) {
        /* read back before rr_target, which may size the target afresh */
        float pw, ph;
        nshvc_window_size(&pw, &ph);
        make_backdrop(pw > 0 ? (float)shown_tw / pw : 1);
    }
    if (!paused)
        backdrop = 0;
    rr_target(&tw, &th);
    /* the whole window, each axis at its own scale */
    w = tw;
    h = th;
    d[0] = d[1] = 0;
    d[2] = w;
    d[3] = h;
    memcpy(shown_at, d, sizeof shown_at);
    shown_tw = tw;
    shown_th = th;
    canvas_scale(w, h);
    ge_holds();
    rr_begin(0xFF000000u);
    if (paused && backdrop) {
        float q[6] = { 0, 0, (float)tw, 0, 0, (float)th };
        rr_backdrop_draw(q);
    } else if (paused)
        ;                                /* no backdrop could be made: black */
    else if (rr_mode7_window(ppu_frame_presents(ppu_frame())) && show_mode7(w, h, d))
        ;
    else
        show_picture(d);
    draw_overlays(tw, th);
    if (fps_on > 0)
        fps_draw();
    rr_present();
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
/* NSHVC_PRESENT=1 (headless): each frame presented by the renderer, opened
   at the start (nshvc_main) */
static int present_headless;

static void frame(void)
{
    uint16_t b0 = 0, b1 = 0;
    frame_no++;
    if (headless && paused) {
        /* the machine is still: no checks, script steps or sound; the
           game's hook goes on */
        frame_hook();
        if (max_frames >= 0 && frame_no >= max_frames)
            exit(0);
        return;
    }
    if (headless) {
        int more;
        if (counts_f)
            counts_frame(frame_no);
        if (front_frame_hook)
            front_frame_hook();
        if (present_headless) {
            /* NSHVC_PRESENT=1: the frame presented as a window presents it
               (no pacing), its run and present timed for NSHVC_FPS_LOG */
            uint64_t before = host_ticks_ns();
            fps_ran_ns = fps_frame_start ? before - fps_frame_start : 0;
            show();
            fps_present_ns = host_ticks_ns() - before;
            if (fps_log)
                fps_frame(fps_frame_start ? before + fps_present_ns : before, fps_ran_ns, fps_present_ns, 0);
            fps_frame_start = host_ticks_ns();
        }
        if (sound_f)
            sound_drain();
        more = script_frame(&b0, &b1);
        nshvc_set_joypad(0, (uint16_t)(b0 | game_buttons[0]));
        nshvc_set_joypad(1, (uint16_t)(b1 | game_buttons[1]));
        input_script();
        input_report(hooks.input);
        frame_hook();
        if ((max_frames >= 0 && frame_no >= max_frames) || (!more && script_steps() && max_frames < 0))
            exit(0);
        return;
    }
    if (fps_on < 0) {
        const char *lf = getenv("NSHVC_FPS_LOG");
        fps_on = getenv("NSHVC_FPS") != NULL;
        if (lf && !(fps_log = fopen(lf, "w")))
            nshvc_fatal("NSHVC_FPS_LOG: cannot write %s", lf);
        if (fps_log)
            setvbuf(fps_log, NULL, _IOLBF, 0);
    }
    {
        uint64_t before = host_ticks_ns();
        fps_ran_ns = fps_frame_start ? before - fps_frame_start : 0;
        show();
        fps_present_ns = host_ticks_ns() - before;
    }
    {
        host_event e;
        while (host_poll_event(&e, 0)) {
            if (e.type == HOST_EV_KEY) {
                if (!(hooks.key && hooks.key(e.vk, e.down)))
                    input_key(e.vk, e.down);
            } else if (e.type == HOST_EV_MOUSE) {
                if (!(hooks.pointer && hooks.pointer(e.x, e.y, e.buttons)))
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
    frame_hook();
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
        if (fps_on > 0 || fps_log) {
            uint64_t shown = fps_frame_start + fps_ran_ns + fps_present_ns;
            fps_frame(fps_frame_start ? shown : now, fps_ran_ns, fps_present_ns,
                      fps_frame_start ? now - shown : 0);
        }
        fps_frame_start = now;
    }
}

int nshvc_main(const nshvc_cart *cart, int argc, char **argv)
{
    const char *script = NULL, *out_dir = ".";
    int i;
    ppu_set_mode7_plain(rr_mode7_plain());
    ppu_set_compose_canvas(rr_compose_canvas());
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--counts") && i + 1 < argc) {
            if (!(counts_f = fopen(argv[++i], "w"))) {
                fprintf(stderr, "nativeshvc: cannot write %s\n", argv[i]);
                return 2;
            }
            setvbuf(counts_f, NULL, _IOFBF, 1 << 16);
        } else if (!strcmp(argv[i], "--sound") && i + 1 < argc) {
            /* a new copy of the program going on from a replay adds to it */
            if (!(sound_f = fopen(argv[++i], getenv("NSHVC_SCRIPT_PLAN") ? "ab" : "wb"))) {
                fprintf(stderr, "nativeshvc: cannot write %s\n", argv[i]);
                return 2;
            }
        }
    }
    host_set_app_name(cart->save_name ? cart->save_name : cart->title, cart->title ? cart->title : "nativeshvc");
    host_init(argc, argv);
    watchdog_start(headless);
    script_args(argc, argv);
    if (script)
        script_load(script, out_dir);
    nshvc_power(cart);
    if (headless && getenv("NSHVC_PRESENT")) {
        /* each frame presented, the renderer open from the start (its
           GPU memory lent from the first frame: nshvc_ext_ge_vram) */
        const char *lf = getenv("NSHVC_FPS_LOG");
        if (!rr_opened && !rr_open())
            nshvc_fatal("NSHVC_PRESENT: the renderer cannot open");
        rr_opened = 1;
        if (lf && !(fps_log = fopen(lf, "w")))
            nshvc_fatal("NSHVC_FPS_LOG: cannot write %s", lf);
        if (fps_log)
            setvbuf(fps_log, NULL, _IOLBF, 0);
        present_headless = 1;
    }
    if (headless) {
        /* shots as a window NSHVC_SCALE pixels a console pixel shows them:
           the canvas's scale that window's (nshvc_ext_canvas_scale) */
        double sx, sy;
        shot_scale(&sx, &sy);
        if (sx > 0)
            ppu_set_canvas_scale(canvas_k_max((int)ceil(sx > sy ? sx : sy)));
    }
    if (counts_f) {
        nshvc_set_read_hook(count_read);
        nshvc_set_write_hook(count_write);
    }
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
        if (!rr_opened && !rr_open()) {
            host_message_box("The renderer could not start on this computer.", snes_cart.title);
            return 1;
        }
        rr_opened = 1;
        s_rs = resample_new(NSHVC_DSP_RATE, OUT_RATE);
        host_audio_open(OUT_RATE, audio_cb, NULL);
    }
    for (;;) {
        loop_passes++;
        if (!paused)
            nshvc_run_frame();
        frame();
    }
}
