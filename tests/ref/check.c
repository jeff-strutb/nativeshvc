/*
 * check.c: the checks the environment asks for, in any test program linked
 * with the reference, on the hooks the console offers them (ppu.h, front_check.h):
 *
 *   NSHVC_PPUCHECK=1      each finished frame drawn again from its record and
 *                         compared with the picture drawn a line at a time
 *   NSHVC_COMPOSECHECK=1  each finished frame's picture from its description
 *                         (ppu_compose, render/rr_compose.h) against
 *                         ppu_draw_frame's, with its records and its weave
 *   NSHVC_RRCHECK=1       headless, any renderer: each finished frame shown by
 *                         the renderer one to one and scaled to the target
 *                         with sharp bilinear, read back, against the
 *                         reference's drawing (rr_ref_image); a frame with
 *                         Mode 7 or a canvas presented by its record against
 *                         rr_ref_mode7 (as any frame where the renderer
 *                         presents it so: rr_mode7_window); at exit, the
 *                         frames and pixels that differ by more than 1 in a
 *                         channel (a GPU's filtering rounds)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "ppu.h"
#include "ppu_ref.h"
#include "rr.h"
#include "rr_compose.h"
#include "rr_ref_mode7.h"
#include "front_check.h"

/* *p at least n elements of `size` bytes (*cap of them now), its contents kept */
static void *grow(void *p, size_t *cap, size_t n, size_t size)
{
    void *q;
    if (n <= *cap)
        return p;
    q = realloc(p, n * size);
    if (!q)
        nshvc_fatal("no memory for a picture of %lu pixels", (unsigned long)n);
    *cap = n;
    return q;
}

/* ---- NSHVC_PPUCHECK, NSHVC_COMPOSECHECK ---- */
static int check_on, compose_on;
static unsigned long check_frames, check_bad, compose_frames, compose_bad;
static uint32_t *check_pic;
static size_t check_cap;

static void line_check(const PpuFrame *f, unsigned line, const uint16_t *live_vram, const uint8_t *live_table)
{
    ppu_ref_line(f, line, live_vram, live_table);
}

static void frame_check(const PpuFrame *finished)
{
    unsigned long frames_done = ppu_frames();
    const PpuFrame *before = ppu_frame_before();
    if (check_on) {
        const uint32_t *picture = ppu_ref_lines();
        int w = ppu_frame_width(finished), y, pw = 2 * finished->width;
        int top = finished->top;
        check_pic = grow(check_pic, &check_cap, (size_t)w * (size_t)finished->screen_height, sizeof *check_pic);
        ppu_draw_frame(finished, check_pic);
        check_frames++;
        /* the lines were drawn 512 wide: a 256-wide frame is their even pixels */
        for (y = 0; y < finished->height; y++) {
            int x, bad = 0;
            for (x = 0; x < w && !bad; x++)
                bad = check_pic[(size_t)(y + top) * (size_t)w + (size_t)x] !=
                      picture[(size_t)y * (size_t)pw + (size_t)(w == finished->width ? 2 * x : x)];
            if (bad) {
                check_bad++;
                break;
            }
        }
    }
    if (compose_on) {
        int w = ppu_frame_width(finished), h = finished->screen_height, x, y;
        long bad = 0;
        size_t nw;
        check_pic = grow(check_pic, &check_cap, (size_t)w * (size_t)h, sizeof *check_pic);
        ppu_draw_frame(finished, check_pic);
        ppu_set_compose_canvas(1);           /* (the description's own texels, as the C build reads them) */
        rr_cp_fd = ppu_compose(finished, &nw);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t got = rr_cp_pixel((uint32_t)x, (uint32_t)y), want = check_pic[(size_t)y * (size_t)w + (size_t)x];
                if (got != want && !bad && compose_bad < 3)
                    fprintf(stderr, "composecheck: frame %lu at (%d, %d): got %08x want %08x\n", frames_done, x, y,
                            (unsigned)got, (unsigned)want);
                bad += got != want;
            }
        if (ppu_frame_mode7(finished)) {
            /* the records: ppu_mode7's, and the compositor's (their
               segments compared by what they hold) */
            static RrMode7 mc, mg;
            long rbad = 0;
            ppu_mode7(finished, check_pic, &mc);
            rr_cp_fd = ppu_compose_m7(finished, &nw, &mg);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const RrM7Px *q = &mc.px[(size_t)y * (size_t)w + (size_t)x];
                    int same;
                    cp_record((uint32_t)x, (uint32_t)y);
                    same = cp_r0 == q->w0 && cp_r1 == q->w1 && cp_r3 == q->w3 && (cp_r2 & 0xFFFF) == (q->w2 & 0xFFFF);
                    if (same && !(q->w2 & RR_M7_NOT)) {
                        const RrM7Seg *a = &mc.seg[q->w2 >> 16], *b = &mg.seg[cp_r2 >> 16];
                        same = !memcmp(a->s, b->s, 6 * sizeof a->s[0]) && a->s[7] == b->s[7] &&
                               !memcmp(mc.pal + (size_t)a->s[6] * 256, mg.pal + (size_t)b->s[6] * 256, 512);
                    }
                    if (!same && !rbad && compose_bad < 3)
                        fprintf(stderr, "composecheck: frame %lu record (%d, %d): got %08x %08x %08x %08x want %08x %08x %08x %08x\n",
                                frames_done, x, y, (unsigned)cp_r0, (unsigned)cp_r1, (unsigned)cp_r2, (unsigned)cp_r3,
                                (unsigned)q->w0, (unsigned)q->w1, (unsigned)q->w2, (unsigned)q->w3);
                    rbad += !same;
                }
            /* the rows marked: every row ppu_mode7 marks, at least */
            for (y = 0; y < h; y++)
                if (mc.row_m7[y] && !mg.row_m7[y]) {
                    if (!rbad && compose_bad < 3)
                        fprintf(stderr, "composecheck: frame %lu row %d not marked\n", frames_done, y);
                    rbad++;
                }
            bad += rbad;
        }
        {
            /* an interlaced frame's woven picture: from both descriptions */
            int ww, wh, from[2], wpw, wph;
            if (ppu_woven(&ww, &wh, from)) {
                static uint32_t *both;
                static size_t both_cap;
                size_t na, nb = 0;
                const uint32_t *pic = ppu_picture(&wpw, &wph);
                const uint32_t *da = ppu_compose(finished, &na);
                both = grow(both, &both_cap, na, sizeof *both);
                memcpy(both, da, na * sizeof *both);
                if (from[0] | from[1]) {
                    const uint32_t *db = ppu_compose(before, &nb);
                    both = grow(both, &both_cap, na + nb, sizeof *both);
                    memcpy(both + na, db, nb * sizeof *both);
                }
                rr_cp_fd = both;
                for (y = 0; y < wh; y++)
                    for (x = 0; x < ww; x++) {
                        uint32_t got = cp_woven((uint32_t)x, (uint32_t)y, (uint32_t)ww, from[0] ? (uint32_t)na : 0,
                                                from[1] ? (uint32_t)na : 0);
                        uint32_t want = pic[(size_t)y * (size_t)ww + (size_t)x];
                        if (got != want && !bad && compose_bad < 3)
                            fprintf(stderr, "composecheck: frame %lu woven (%d, %d): got %08x want %08x\n", frames_done,
                                    x, y, (unsigned)got, (unsigned)want);
                        bad += got != want;
                    }
                cp_fdb = 0;
            }
        }
        compose_frames++;
        compose_bad += bad != 0;
    }
}

static void frames_report(void)
{
    if (check_on)
        fprintf(stderr, "ppucheck: %lu frames, %lu differ; %lu needed more copies than kept\n", check_frames, check_bad,
                ppu_version_overflow);
    if (compose_on)
        fprintf(stderr, "composecheck: %lu frames, %lu differ\n", compose_frames, compose_bad);
}

/* ---- NSHVC_RRCHECK ---- */
static long rr_frames = -1, rr_bad, rr_pixels, rr_first = -1, rr_m7_frames, rr_pic_bad;
static int32_t *m7_cols, *m7_rows;
static int m7_cols_cap, m7_rows_cap;

static void rr_report(void)
{
    fprintf(stderr, "rrcheck: %s, %ld frames (%ld with Mode 7 or a canvas presented), %ld differ (first at frame %ld), %ld pixels; "
                    "%ld pictures read back differ\n",
            rr_name(), rr_frames, rr_m7_frames, rr_bad, rr_first, rr_pixels, rr_pic_bad);
}

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

static void rr_check(void)
{
    static uint32_t *got, *want;
    int tw, th, pw, ph, i, d[4];
    const uint32_t *px = ppu_picture(&pw, &ph);
    long bad = 0, frame_no = front_frame_no();
    RrMode7 m;
    RrM7Samples smp;
    int recorded = 0;
    if (rr_frames < 0) {
        front_rr_need();
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
    front_canvas_scale(tw, th);
    rr_begin(0xFF000000u);
    for (i = 0; i < tw * th; i++)
        want[i] = 0xFF000000u;
    if (rr_mode7_window(ppu_frame_presents(ppu_frame())) && ppu_frame_mode7(ppu_frame()) && ph == ppu_frame()->screen_height) {
        int gpu;
        recorded = 1;
        gpu = front_show_mode7(tw, th, d);
        if (!mode7_prepare(tw, th, &m, &smp))
            nshvc_fatal("rrcheck: no record for the frame");
        if (!gpu)
            nshvc_fatal("rrcheck: the frame's Mode 7 or canvas not drawn by the renderer");
        rr_ref_mode7(want, tw, th, &m, d, &smp);
        rr_m7_frames++;
    } else {
        front_show_picture(d);
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
                fprintf(stderr, "rrcheck: frame %ld at (%d, %d): got %08x want %08x", frame_no, X, Y, (unsigned)got[i], (unsigned)want[i]);
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
                            fprintf(stderr, "\n  (%d, %d): %08x %08x %08x %08x", ii, rr, (unsigned)q->w0, (unsigned)q->w1, (unsigned)q->w2,
                                    (unsigned)q->w3);
                            if (!(q->w2 & (RR_M7_NOT | RR_M7_BLACK))) {
                                const RrM7Seg *g = &m.seg[q->w2 >> 16];
                                fprintf(stderr, " seg %u: %d %d %d %d %08x %08x %d %08x", (unsigned)(q->w2 >> 16), (int)g->s[0], (int)g->s[1],
                                        (int)g->s[2], (int)g->s[3], (unsigned)g->s[4], (unsigned)g->s[5], (int)g->s[6], (unsigned)g->s[7]);
                            }
                        }
                }
                fprintf(stderr, "\n");
            }
            bad++;
        }
    }
    /* the picture read back from the GPU (nshvc_picture), the reference's
       exactly (the PSP's: none for a frame with a canvas shown, drawn only
       at the target's resolution) */
    if (!(ppu_frame_presents(ppu_frame()) & 6) || strcmp(rr_name(), "psp")) {
        int gw, gh;
        const uint32_t *g = nshvc_picture(&gw, &gh);
        long n = 0;
        if (gw != pw || gh != ph) {
            n = 1;
        } else {
            for (i = 0; i < pw * ph; i++)
                n += g[i] != px[i];
        }
        if (n && !rr_pic_bad)
            fprintf(stderr, "rrcheck: frame %ld: the picture read back differs in %ld pixels (%d x %d, %d x %d)\n", frame_no, n,
                    gw, gh, pw, ph);
        rr_pic_bad += n != 0;
    }
    rr_frames++;
    if (bad) {
        rr_bad++;
        rr_pixels += bad;
        if (rr_first < 0)
            rr_first = frame_no;
    }
}

/* The hooks set from the environment: as the program starts, and on the PSP
   again once its main has set the environment from args.txt (psp/args.c). A
   hook off leaves the console as a game's run has it. */
void nshvc_check_env(void)
{
    static int reporting;
    check_on = getenv("NSHVC_PPUCHECK") != NULL;
    compose_on = getenv("NSHVC_COMPOSECHECK") != NULL;
    ppu_line_hook = check_on ? line_check : NULL;
    ppu_frame_hook = check_on || compose_on ? frame_check : NULL;
    front_frame_hook = getenv("NSHVC_RRCHECK") != NULL ? rr_check : NULL;
    if ((check_on || compose_on) && !reporting) {
        reporting = 1;
        atexit(frames_report);
    }
}

__attribute__((constructor)) static void check_start(void) { nshvc_check_env(); }
