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
void nshvc_ext_screen_origin(int x, int y) { ppu_ext_screen_origin(x, y); }
void nshvc_ext_sprite_x(int sprite, int on) { ppu_ext_sprite_x(sprite, on); }
void nshvc_ext_sprite_y(int sprite, int on) { ppu_ext_sprite_y(sprite, on); }
void nshvc_ext_sprite_page(int sprite, int x_page, int y_page) { ppu_ext_sprite_page(sprite, x_page, y_page); }
void nshvc_ext_sprite_limits(int on) { ppu_ext_sprite_limits(on); }
void nshvc_ext_vram(int kb) { ppu_ext_vram(kb); }
void nshvc_ext_vram_page(int page) { ppu_ext_vram_page(page); }
void nshvc_ext_layer_page(int layer, int page) { ppu_ext_layer_page(layer, page); }
void nshvc_ext_layer_clip(int layer, int on) { ppu_ext_layer_clip(layer, on); }
void nshvc_ext_vram_copy(int to_page, int from_page) { ppu_ext_vram_copy(to_page, from_page); }
void nshvc_ext_vram_direct(int page) { ppu_ext_vram_direct(page); }
int nshvc_ext_canvas_scale(void) { return ppu_canvas_scale(); }
void nshvc_ext_canvas_chars(int layer, unsigned word_addr, int count, const uint8_t *texels, int k)
{
    ppu_ext_canvas_chars(layer, word_addr, count, texels, k);
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
static void canvas_scale(int w, int h)
{
    const PpuFrame *f = ppu_frame();
    int kx = (w + f->width - 1) / f->width, ky = (h + f->screen_height - 1) / f->screen_height;
    ppu_set_canvas_scale(kx > ky ? kx : ky);
}

/* The last frame's picture over target rectangle d: drawn by the
   renderer's compositor from the frame's description where it has one
   (render/compose_body.h; NSHVC_COMPOSE=0: never), else the PPU's picture
   drawn here. An interlaced frame's weave is the PPU's. */
static void show_picture(const int d[4])
{
    static int on = -1;
    const PpuFrame *f = ppu_frame();
    const uint32_t *px;
    int pw, ph;
    if (on < 0)
        on = !getenv("NSHVC_COMPOSE") || strcmp(getenv("NSHVC_COMPOSE"), "0");
    if (on && f->versions > 0 && !f->interlace) {
        size_t nw;
        const uint32_t *desc = ppu_compose(f, &nw);
        if (rr_compose(desc, nw, ppu_frame_width(f), f->screen_height, d))
            return;
    } else if (on && f->versions > 0) {
        /* an interlaced frame: woven from its description and, for the
           rows that are the frame before's, that one's */
        static uint32_t *both;
        static size_t cap;
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
                    const uint32_t *b = ppu_compose(ppu_frame_before(), &nb);
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
                if (nb != (size_t)-1 &&
                    rr_compose_woven(both, na + nb, ww, wh, from[0] ? (uint32_t)na : 0, from[1] ? (uint32_t)na : 0, d))
                    return;
            }
        }
    }
    px = ppu_picture(&pw, &ph);
    rr_image(px, pw, ph, d);
}

/* ---- a frame with Mode 7 or a canvas presented (render/rr_mode7.h) ---- */
static int32_t *m7_cols, *m7_rows;
static int m7_cols_cap, m7_rows_cap;

/* the last frame's record and where it samples drawn at ow x oh; 0 when it
   is drawn as its picture scaled (no Mode 7 or canvas in it, or an
   interlaced weave) */
static int mode7_prepare(int ow, int oh, RrMode7 *m, RrM7Samples *s)
{
    const PpuFrame *f = ppu_frame();
    int pw, ph;
    const uint32_t *px;
    if (ow <= 0 || oh <= 0 || !ppu_frame_mode7(f))
        return 0;
    px = ppu_picture(&pw, &ph);
    if (ph != f->screen_height || !ppu_mode7(f, px, m))
        return 0;
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
    rr_m7_tables(m, ow, oh, s, m7_cols, m7_rows);
    return 1;
}

/* The last frame, presenting Mode 7 or a canvas, drawn at ow x oh over d
   by the renderer's compositor (its picture and records the GPU's); 0
   when it is not drawn so (no compositor, NSHVC_COMPOSE=0, an interlaced
   weave, or no Mode 7 or canvas presented) */
static int show_mode7(int ow, int oh, const int d[4])
{
    static RrMode7 m;
    static RrM7Samples smp;
    const PpuFrame *f = ppu_frame();
    const char *e = getenv("NSHVC_COMPOSE");
    size_t nw;
    const uint32_t *desc;
    if ((e && !strcmp(e, "0")) || ow <= 0 || oh <= 0 || f->versions == 0 || f->interlace || !ppu_frame_mode7(f))
        return 0;
    desc = ppu_compose_m7(f, &nw, &m);
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
    return rr_compose_mode7(desc, nw, &m, d, &smp);
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

const uint32_t *front_shot_picture(int *w, int *h, int *presented)
{
    double sx, sy;
    static uint32_t *buf;
    static size_t cap;
    int pw, ph, d[4];
    const uint32_t *px = nshvc_picture(&pw, &ph);
    RrMode7 m;
    RrM7Samples smp;
    size_t i, len;
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
    if (mode7_prepare(*w, *h, &m, &smp)) {
        rr_ref_mode7(buf, *w, *h, &m, d, &smp);
    } else {
        for (i = 0; i < len; i++)
            buf[i] = 0xFF000000u;
        rr_ref_image(buf, *w, *h, px, pw, ph, d);
    }
    return buf;
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
    return input_point(shown_at, (float)shown_tw / ww, f->width, f->screen_height, f->left, f->top, x, y, dot, line);
}

/* the frame as large as the window holds, centred, sharp bilinear: the
   console's 256 x 224 at a television's 4:3, a larger screen at the same
   shape of pixel; a frame with Mode 7 or a canvas presented by its record
   (rr_mode7: the plane at the window's pixels, anti-aliased by each
   pixel's footprint on it; the canvas's texels) */
/* ---- the frame-rate meter (NSHVC_FPS=1, NSHVC_FPS_LOG=FILE) ----
   Over the last whole second of the window's run: frames presented and
   console frames run a second, the longest time between two presents,
   and the longest each frame spent running the console, presenting and
   waiting for its time. NSHVC_FPS=1 draws it at the top left;
   NSHVC_FPS_LOG=FILE writes a line a presented frame: its number, its
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

static void fps_draw(void)
{
    static uint32_t px[7 * (2 + 4 * 96)];
    size_t n = strlen(fps_text), i, w = 2 + 4 * n;
    int x, y;
    float q[6];
    if (!n)
        return;
    for (i = 0; i < 7 * w; i++)
        px[i] = 0xB0000000u;                       /* a dark panel behind the text */
    for (i = 0; i < n; i++) {
        size_t g;
        for (g = 0; g < sizeof fps_font / sizeof fps_font[0] && fps_font[g].c != fps_text[i]; g++)
            ;
        if (g == sizeof fps_font / sizeof fps_font[0])
            continue;
        for (y = 0; y < 5; y++)
            for (x = 0; x < 3; x++)
                if (fps_font[g].row[y] >> (2 - x) & 1)
                    px[(size_t)(y + 1) * w + 1 + 4 * i + (size_t)x] = 0xFFFFFF40u;
    }
    /* 3 target pixels a font pixel, at the top left */
    q[0] = 8;
    q[1] = 8;
    q[2] = 8 + 3.0f * (float)w;
    q[3] = 8;
    q[4] = 8;
    q[5] = 8 + 3.0f * 7;
    rr_overlay(px, (int)w, 7, q);
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
    RrMode7 m;
    RrM7Samples smp;
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
    canvas_scale(w, h);
    rr_begin(0xFF000000u);
    if (show_mode7(w, h, d))
        ;
    else if (mode7_prepare(w, h, &m, &smp))
        rr_mode7(&m, d, &smp);
    else
        show_picture(d);
    draw_overlays(tw, th);
    if (fps_on > 0)
        fps_draw();
    rr_present();
}

/* NSHVC_RRCHECK=1 (headless, any RENDER): each finished frame shown by the
   renderer one to one and scaled to the target with sharp bilinear, read
   back, against the PPU's picture drawn the same way on the CPU (rr_size.h
   rr_ref_image); a frame with Mode 7 or a canvas presented by its record (rr_mode7)
   against rr_ref_mode7; at exit, the frames and pixels
   that differ by more than 1 in a channel (a GPU's filtering rounds) */
static long rr_frames = -1, rr_bad, rr_pixels, rr_first = -1, rr_m7_frames;

static void rr_report(void)
{
    fprintf(stderr, "rrcheck: %s, %ld frames (%ld with Mode 7 or a canvas presented), %ld differ (first at frame %ld), %ld pixels\n",
            rr_name(), rr_frames, rr_m7_frames, rr_bad, rr_first, rr_pixels);
}

static void rr_check(void)
{
    static uint32_t *got, *want;
    int tw, th, pw, ph, i, d[4];
    const uint32_t *px = ppu_picture(&pw, &ph);
    long bad = 0;
    RrMode7 m;
    RrM7Samples smp;
    int recorded = 0;
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
    canvas_scale(tw, th);
    rr_begin(0xFF000000u);
    for (i = 0; i < tw * th; i++)
        want[i] = 0xFF000000u;
    if (ppu_frame_mode7(ppu_frame()) && ph == ppu_frame()->screen_height) {
        int gpu;
        recorded = 1;
        /* the compositor's records where the renderer has one (drawn
           first: the reference's are made over the same tables) */
        gpu = show_mode7(tw, th, d);
        if (!mode7_prepare(tw, th, &m, &smp))
            nshvc_fatal("rrcheck: no record for the frame");
        if (!gpu)
            rr_mode7(&m, d, &smp);
        rr_ref_mode7(want, tw, th, &m, d, &smp);
        rr_m7_frames++;
    } else {
        show_picture(d);
        rr_ref_image(want, tw, th, px, pw, ph, d);
    }
    rr_present();
    rr_read(got);
    for (i = 0; i < tw * th; i++) {
        int sh, worst = 0;
        for (sh = 0; sh < 24; sh += 8) {
            int a = (int)((got[i] >> sh) & 0xFF) - (int)((want[i] >> sh) & 0xFF);
            if (a < 0)
                a = -a;
            if (a > worst)
                worst = a;
        }
        if (worst > 1) {
            /* NSHVC_RRCHECK_WHY=1: the first frame that differs, its first
               pixels with the record of the picture pixel under each */
            if (rr_first < 0 && bad < 8 && getenv("NSHVC_RRCHECK_WHY")) {
                int X = i % tw, Y = i / tw;
                fprintf(stderr, "rrcheck: frame %ld at (%d, %d): got %08x want %08x", frame_no, X, Y, got[i], want[i]);
                if (recorded) {
                    int tx = smp.cols[3 * X], ty = smp.rows[4 * Y], u = smp.cols[3 * X + 2], v = smp.rows[4 * Y + 3];
                    float fx, fy;
                    memcpy(&fx, &smp.cols[3 * X + 1], 4);
                    memcpy(&fy, &smp.rows[4 * Y + 1], 4);
                    fprintf(stderr, "; picture (%d, %d) fx %.3f fy %.3f u %d v %d; k %d, %zu texels", tx, ty, fx, fy, u, v,
                            m.cv_k, m.cv_len);
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int ii = tx + dx < 0 ? 0 : tx + dx >= m.pw ? m.pw - 1 : tx + dx;
                            int rr = ty + dy < 0 ? 0 : ty + dy >= m.ph ? m.ph - 1 : ty + dy;
                            const RrM7Px *q = &m.px[(size_t)rr * (size_t)m.pw + (size_t)ii];
                            fprintf(stderr, "\n  (%d, %d): %08x %08x %08x %08x", ii, rr, q->w0, q->w1, q->w2, q->w3);
                            if (!(q->w2 & (RR_M7_NOT | RR_M7_BLACK))) {
                                const RrM7Seg *g = &m.seg[q->w2 >> 16];
                                fprintf(stderr, " seg %u: %d %d %d %d %08x %08x %d %08x", q->w2 >> 16, g->s[0], g->s[1], g->s[2],
                                        g->s[3], (unsigned)g->s[4], (unsigned)g->s[5], g->s[6], (unsigned)g->s[7]);
                            }
                        }
                }
                fprintf(stderr, "\n");
            }
            bad++;
        }
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
        if (counts_f)
            counts_frame(frame_no);
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
        if (fps_on > 0 || fps_log) {
            uint64_t shown = fps_frame_start + fps_ran_ns + fps_present_ns;
            fps_frame(fps_frame_start ? shown : now, fps_ran_ns, fps_present_ns,
                      fps_frame_start ? now - shown : 0);
        }
        fps_frame_start = now;
    }
}

/* NSHVC_PPUCHECK=1: the frames drawn from their records against the
   picture drawn a line at a time (ppu.c), at exit */
static void ppucheck_report(void)
{
    if (getenv("NSHVC_PPUCHECK"))
        fprintf(stderr, "ppucheck: %lu frames, %lu differ; %lu needed more copies than kept\n", ppu_check_frames,
                ppu_check_bad, ppu_version_overflow);
    if (getenv("NSHVC_COMPOSECHECK"))
        fprintf(stderr, "composecheck: %lu frames, %lu differ\n", ppu_compose_frames, ppu_compose_bad);
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
    script_args(argc, argv);
    if (script)
        script_load(script, out_dir);
    nshvc_power(cart);
    if (headless) {
        /* shots as a window NSHVC_SCALE pixels a console pixel shows them:
           the canvas's scale that window's (nshvc_ext_canvas_scale) */
        double sx, sy;
        shot_scale(&sx, &sy);
        if (sx > 0)
            ppu_set_canvas_scale((int)ceil(sx > sy ? sx : sy));
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
