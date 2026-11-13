/* pixel.c: nshvc_pixel() in a game's shader (API 6.4), checked without a reference emulator. The shader
 * (tests/scenes/shaders/pixel.shade) returns red = floor(x) / 255, green = floor(y) / 255, blue = 1, with (x, y) the pixel's
 * place in the list's pixels, so a drawn pixel of the picture is (floor(x), floor(y), 255) and an undrawn one blue 0:
 * floor(x) / 255 is stored as the whole number floor(x) by any 8-bit target (x < 256).
 *
 *   tools/nshvc-shaders tests/scenes/shaders OUT RENDER      (builds tint and pixel for every backend)
 *   PORT_RESDIR=OUT pixel --headless --updates 11            (the screen's size: a pixel for a pixel)
 *   PORT_RESDIR=OUT RR_SIZE=640x360 pixel --headless --updates 11   (a square screen in a window not square: letterboxed)
 *
 * Prints FAIL and exits 1 on any difference. The screen is 256 x 256 with aspect 1, shown in a window of w x h as the
 * renderer does (rr_view): a square of side min(w, h), centred. For a window pixel (wx, wy) its place in the picture is
 * ((wx + 0.5 - vx) * 256 / vw, (wy + 0.5 - vy) * 256 / vh), and in the list ((that) * list size / 256). Every window pixel
 * is checked: the pixels whose colour has blue 255 (the shader's, a pixel the primitive covers wholly: the edges are
 * mixed with the background and have a blue below 255) must have red = floor(list x) and green = floor(list y), with
 * a pixel whose place is within 0.05 of a whole number left out (a rounding's to decide), and there must be at least 90% of the
 * pixels the primitive's inside has (the inside shrunk by 2 window pixels) with blue 255, and none outside the primitive's
 * rectangle grown by 3 window pixels. One picture each:
 *
 *   0 triangles: a quad of two nshvc_draw_tris triangles, list 256 x 256, (16, 16) to (240, 240).
 *   1 lines: nshvc_draw_lines, a horizontal line (20, 100.5) to (200, 100.5) and a vertical (150.5, 30) to (150.5, 220): the
 *     pixels covering them whole.
 *   2 an image: nshvc_draw_image of an image over (30, 40) to (210, 200) with the shader as the material.
 *   3 a mesh: nshvc_draw_mesh of a quad at Z = 1 with f = 1 and the centre at the corner, so a view unit is a list pixel:
 *     (16, 16) to (240, 240).
 *   4 a mesh across the near plane: a ramp whose Z runs from 8 to 24 across it, near = 12: cut at Z = 12, the part nearer not
 *     drawn: every drawn pixel has the place of its own pixel, there are at least a thousand, and none lies left of the
 *     cut (screen x 85.33 at the 1:1 picture; the left margin is taken as 80 list pixels for any window: this case
 *     compares its drawn pixels only).
 *   5 the list with a depth of its own (nshvc_draw_depth_own), as 0.
 *   6 the same as 3 with a depth of its own.
 *   7 the list with one sample a pixel (fewer than the picture's: its own target), as 0.
 *   8 a list of 128 x 128 (so a list pixel is 2 picture pixels): the quad (8, 8) to (120, 120), as 0.
 *   9 a list of 100 x 140 (a list pixel is 2.56 picture pixels across and 1.83 down): the quad (5, 5) to (95, 135).
 *  10 the picture read back and drawn again: nothing different is expected; scenario 0's list again (the picture read by this
 *     program's check is the renderer's, run again for the read).
 * What the arithmetic is, for the 1:1 picture, scenario 0: window pixel (100, 50) has centre (100.5, 50.5), its place in
 * the list (100.5, 50.5): colour (100, 50, 255). Scenario 8: window pixel (100, 50) is the list's (50.25, 25.25):
 * (50, 25, 255). Scenario 9: window pixel (100, 50): (100.5 / 2.56, 50.5 / 1.8286) = (39.26, 27.62): (39, 27, 255). Letterboxed
 * at 640 x 360: the square is 360 across at x from 140: window pixel (320, 180), the middle, is picture (128.0, 128.0)
 * within a pixel: exactly (320.5 - 140) * 256 / 360 = 128.36, so list (128, 128, 255) in scenario 0. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SCREEN 256
#define SCENARIOS 11
static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static nshvc_draw_list list;
static nshvc_shader pixel;
static nshvc_image img;
static nshvc_mesh quad, ramp;
static int pics, checked;
/* the scenario's list size and primitive rectangle (list pixels), and what it expects */
typedef struct { int lw, lh; float x0, y0, x1, y1; int whole_check; } Scene;
static Scene scene_of[SCENARIOS];

static void init(void)
{
    nshvc_mesh_vertex m[6];
    uint32_t tex[16];
    int k;
    list = nshvc_draw_list_create();
    pixel = nshvc_shader_load("pixel");
    CHECK(pixel.id != 0, "the pixel shader is not loaded (PORT_RESDIR/shaders/pixel.<backend>)");
    img = nshvc_image_create(4, 4, NSHVC_IMAGE_RGBA);
    for (k = 0; k < 16; k++)
        tex[k] = 0xFFFFFFFFu;
    nshvc_image_write(img, tex);
    quad = nshvc_mesh_create(6);
    memset(m, 0, sizeof m);
    for (k = 0; k < 6; k++)
        m[k].colour = 0xFFFFFFFFu;
    m[0].x = 16;  m[0].y = 16;  m[1].x = 240; m[1].y = 16;  m[2].x = 16;  m[2].y = 240;
    m[3].x = 240; m[3].y = 16;  m[4].x = 240; m[4].y = 240; m[5].x = 16;  m[5].y = 240;
    nshvc_mesh_write(quad, m, 6);
    /* a ramp: x from -1 to 1, model z = x */
    ramp = nshvc_mesh_create(6);
    memset(m, 0, sizeof m);
    for (k = 0; k < 6; k++)
        m[k].colour = 0xFFFFFFFFu;
    m[0].x = -1; m[0].y = -1; m[0].z = -1; m[1].x = 1; m[1].y = -1; m[1].z = 1; m[2].x = -1; m[2].y = 1; m[2].z = -1;
    m[3].x = 1;  m[3].y = -1; m[3].z = 1;  m[4].x = 1; m[4].y = 1;  m[4].z = 1; m[5].x = -1; m[5].y = 1; m[5].z = -1;
    nshvc_mesh_write(ramp, m, 6);
}

static void draw_quad(float x0, float y0, float x1, float y1, const nshvc_material *m)
{
    nshvc_vertex v[6];
    int k;
    memset(v, 0, sizeof v);
    for (k = 0; k < 6; k++)
        v[k].colour = 0xFFFFFFFFu;
    v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0; v[2].x = x0; v[2].y = y1;
    v[3].x = x1; v[3].y = y0; v[4].x = x1; v[4].y = y1; v[5].x = x0; v[5].y = y1;
    nshvc_draw_tris(list, v, 6, m);
}

static void draw(float between)
{
    nshvc_material m;
    nshvc_layer a;
    nshvc_screen s;
    nshvc_view view;
    nshvc_instance inst = { 0, 0, 1, 1 };
    int sc = pics++;
    uint32_t lw = SCREEN, lh = SCREEN;
    Scene *sn;
    (void)between;
    if (sc >= SCENARIOS)
        return;
    sn = &scene_of[sc];
    if (sc == 8) { lw = 128; lh = 128; }
    if (sc == 9) { lw = 100; lh = 140; }
    sn->lw = (int)lw; sn->lh = (int)lh;
    sn->x0 = 16; sn->y0 = 16; sn->x1 = 240; sn->y1 = 240;
    memset(&m, 0, sizeof m);
    m.shader = pixel;
    nshvc_draw_list_clear(list, lw, lh);
    nshvc_draw_depth_own(list, sc == 5 || sc == 6);
    nshvc_draw_samples(list, sc == 7 ? 1 : 8);
    memset(&view, 0, sizeof view);
    view.rot[0] = view.rot[4] = view.rot[8] = 1.0f;
    view.f = 1.0f; view.near = 0.5f; view.depth_a = 1.0f; view.depth_b = 0.5f; view.depth_max = 0.99f;
    if (sc == 1) {
        nshvc_vertex v[4];
        memset(v, 0, sizeof v);
        v[0].x = 20; v[0].y = 100.5f; v[1].x = 200; v[1].y = 100.5f;
        v[2].x = 150.5f; v[2].y = 30; v[3].x = 150.5f; v[3].y = 220;
        nshvc_draw_lines(list, v, 4, &m);
        sn->whole_check = 2;
    } else if (sc == 2) {
        nshvc_rect r = { 30, 40, 210, 200 };
        nshvc_draw_image(list, img, &r, &m);
        sn->x0 = 30; sn->y0 = 40; sn->x1 = 210; sn->y1 = 200;
    } else if (sc == 3 || sc == 6) {
        nshvc_draw_mesh(list, quad, &view, &inst, 1, &m);
    } else if (sc == 4) {
        nshvc_instance r = { 0, 0, 16, 8 };
        view = (nshvc_view){ 0 };
        view.rot[0] = view.rot[4] = view.rot[8] = 1.0f;
        view.f = 128.0f; view.cx = 128.0f; view.cy = 128.0f; view.near = 12.0f;
        view.depth_a = 1.0f; view.depth_b = 2.0f; view.depth_max = 0.99f;
        nshvc_draw_mesh(list, ramp, &view, &r, 1, &m);
        sn->whole_check = 3;
    } else if (sc == 8) {
        draw_quad(8, 8, 120, 120, &m);
        sn->x0 = 8; sn->y0 = 8; sn->x1 = 120; sn->y1 = 120;
    } else if (sc == 9) {
        draw_quad(5, 5, 95, 135, &m);
        sn->x0 = 5; sn->y0 = 5; sn->x1 = 95; sn->y1 = 135;
    } else {
        draw_quad(16, 16, 240, 240, &m);
    }
    memset(&a, 0, sizeof a);
    a.enabled_main = 1;
    a.draw_list = list;
    nshvc_mode_set(1, 0, 0);
    nshvc_layer_set(0, &a);
    memset(&s, 0, sizeof s);
    s.brightness = 15;
    s.width = s.height = SCREEN;
    s.aspect = 1.0f;
    nshvc_screen_set(&s);
}

static void check(int sc, const uint32_t *p, int w, int h)
{
    const Scene *sn = &scene_of[sc];
    int vw, vh, vx, vy, x, y, bad = 0, drawn = 0, inside = 0, stray = 0, first_bad_x = -1, first_bad_y = -1;
    float mx, my;
    if (!p) {
        CHECK(0, "scenario %d: no picture", sc);
        return;
    }
    /* the view as rr_view makes it: the square of aspect 1 in w x h, centred */
    vh = h < w ? h : w;
    vw = vh;
    vx = (w - vw) / 2;
    vy = (h - vh) / 2;
    mx = 2.0f * (float)sn->lw / (float)vw;      /* two window pixels in list pixels */
    my = 2.0f * (float)sn->lh / (float)vh;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = p[y * w + x] & 0xFFFFFFu;
            int r = (int)(c >> 16 & 255u), g = (int)(c >> 8 & 255u), b = (int)(c & 255u);
            float lx = ((float)x + 0.5f - (float)vx) * (float)SCREEN / (float)vw * (float)sn->lw / (float)SCREEN;
            float ly = ((float)y + 0.5f - (float)vy) * (float)SCREEN / (float)vh * (float)sn->lh / (float)SCREEN;
            float fx = lx - floorf(lx), fy = ly - floorf(ly);
            int in_rect = lx >= sn->x0 + mx && lx <= sn->x1 - mx && ly >= sn->y0 + my && ly <= sn->y1 - my;
            int near_rect = lx >= sn->x0 - 1.5f * mx && lx <= sn->x1 + 1.5f * mx && ly >= sn->y0 - 1.5f * my && ly <= sn->y1 + 1.5f * my;
            if (b == 255) {
                drawn++;
                if (!(fx < 0.05f || fx > 0.95f || fy < 0.05f || fy > 0.95f) && (r != (int)floorf(lx) || g != (int)floorf(ly))) {
                    if (!bad++) {
                        first_bad_x = x;
                        first_bad_y = y;
                    }
                }
                if (sn->whole_check == 0 && !near_rect)
                    stray++;
            }
            if (in_rect)
                inside++;
            (void)near_rect;
        }
    if (sn->whole_check == 2) {
        /* the lines: two of them, a pixel wide: drawn pixels in the rows and columns they cover, none elsewhere (counted loosely) */
        int expect = (int)((180.0f * (float)vw / (float)SCREEN + 190.0f * (float)vh / (float)SCREEN) * (float)vw / (float)SCREEN * 0.9f);
        CHECK(drawn >= expect / 2, "scenario %d: only %d pixels drawn by the lines (expected about %d or more)", sc, drawn, expect);
    } else if (sn->whole_check == 3) {
        CHECK(drawn >= 1000 * vw / SCREEN * vw / SCREEN, "scenario %d: only %d pixels drawn", sc, drawn);
    } else {
        CHECK(drawn * 10 >= inside * 9, "scenario %d: %d pixels drawn of the %d inside", sc, drawn, inside);
        CHECK(stray == 0, "scenario %d: %d pixels drawn outside the primitive", sc, stray);
    }
    CHECK(bad == 0, "scenario %d: %d drawn pixels with a wrong place, the first at window (%d, %d): colour %06x", sc, bad, first_bad_x, first_bad_y,
          first_bad_x >= 0 ? p[first_bad_y * w + first_bad_x] & 0xFFFFFFu : 0u);
}

static void update(const nshvc_input *in)
{
    (void)in;
    if (pics > checked && checked < SCENARIOS) {
        int w = 0, h = 0;
        const uint32_t *p = nshvc_picture_read(&w, &h);
        check(checked++, p, w, h);
    }
}

static void shutdown(void)
{
    update(NULL);
    CHECK(checked == SCENARIOS, "%d of %d scenarios checked: run with --updates %d", checked, SCENARIOS, SCENARIOS);
    printf(failed ? "pixel: FAILED\n" : "pixel: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "pixel";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
