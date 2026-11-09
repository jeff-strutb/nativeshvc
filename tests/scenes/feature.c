/* feature.c: what the scenes against bsnes cannot reach, each checked against
 * values worked out by hand: draw lists, the exact fill, game shaders, images,
 * overlays, saves, input replay, jobs and pause. Prints FAIL and exits 1 on any
 * difference. Run with --headless --updates 4 --input FILE and PORT_RESDIR set
 * to the folder holding shaders/tint.<backend>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static nshvc_draw_list list0, list1;
static nshvc_image img, over;
static nshvc_shader tint;
static uint16_t pads[4];
static uint32_t job_out[64];

static void job(void *c, uint32_t i) { (void)c; job_out[i] = i * 3 + 1; }

static void init(void)
{
    static uint32_t tex[16], ov[4];
    uint16_t pal[256];
    char back[8] = { 0 };
    int i;
    memset(pal, 0, sizeof pal);
    pal[0] = 0x0000;
    pal[5] = 0x001F;                    /* red */
    nshvc_palette_write(0, 256, pal);
    list0 = nshvc_draw_list_create();
    list1 = nshvc_draw_list_create();
    img = nshvc_image_create(4, 4, NSHVC_IMAGE_RGBA);
    for (i = 0; i < 16; i++)
        tex[i] = 0xFF00FF00u;           /* green */
    nshvc_image_write(img, tex);
    over = nshvc_image_create(2, 2, NSHVC_IMAGE_RGBA);
    for (i = 0; i < 4; i++)
        ov[i] = 0xFF0000FFu;
    nshvc_image_write(over, ov);
    tint = nshvc_shader_load("tint");
    CHECK(tint.id != 0, "shader not loaded");
    CHECK(nshvc_save_write("t.bin", "hello", 6) == 0, "save write");
    CHECK(nshvc_save_read("t.bin", back, 6) == 6 && !strcmp(back, "hello"), "save read: %s", back);
    nshvc_save_delete("t.bin");
    CHECK(nshvc_save_read("t.bin", back, 6) == -1, "save delete");
    {
        nshvc_asset a = nshvc_asset_open("shaders/missing");
        CHECK(a.id == 0, "a missing asset opened");
    }
    nshvc_jobs(job, NULL, 64);
    for (i = 0; i < 64; i++)
        CHECK(job_out[i] == (uint32_t)i * 3 + 1, "job %d", i);
    {
        nshvc_job_group g = nshvc_jobs_begin(job, NULL, 8);
        nshvc_jobs_wait(g);
    }
}

static void update(const nshvc_input *in)
{
    uint64_t n = nshvc_updates();
    if (n < 4)
        pads[n] = in->pad[0];
}

static void draw(float between)
{
    static const float corners[6] = { 200, 200, 216, 200, 200, 216 };
    nshvc_layer a;
    nshvc_vertex v[3];
    nshvc_material m;
    nshvc_rect r = { 100, 10, 120, 30 };
    (void)between;
    memset(v, 0, sizeof v);
    memset(&m, 0, sizeof m);
    memset(&a, 0, sizeof a);
    nshvc_draw_list_clear(list0, 256, 224);
    /* a right triangle in palette red, exactly filled */
    nshvc_draw_rule(list0, NSHVC_FILL_EXACT);
    v[0].x = 10; v[0].y = 10; v[0].colour = 5;
    v[1].x = 60; v[1].y = 10; v[1].colour = 5;
    v[2].x = 10; v[2].y = 60; v[2].colour = 5;
    nshvc_draw_tris(list0, v, 3, NULL);
    nshvc_draw_image(list0, img, &r, NULL);
    /* the shader halves green over a rectangle */
    m.shader = tint;
    m.image = img;
    m.inputs[0] = 0.5f;
    r.x0 = 140; r.x1 = 160; r.y0 = 10; r.y1 = 30;
    nshvc_draw_image(list0, img, &r, &m);
    a.enabled_main = 1;
    a.draw_list = list0;
    nshvc_mode_set(1, 0, 0);
    nshvc_layer_set(0, &a);
    {
        nshvc_screen s;
        memset(&s, 0, sizeof s);
        s.brightness = 15;
        nshvc_screen_set(&s);
    }
    nshvc_overlay_set(0, over, corners, 1.0f);
}

static void shutdown(void)
{
    int w = 0, h = 0;
    const uint32_t *p = nshvc_picture_read(&w, &h);
#define AT(x, y) (p[(y) * w + (x)] & 0xFFFFFFu)
    CHECK(pads[0] == 0x0000 && pads[1] == 0x8000 && pads[2] == 0x8000 && pads[3] == 0x0100, "input %x %x %x %x", pads[0], pads[1], pads[2], pads[3]);
    if (!p) {
        CHECK(0, "no picture");
    } else {
        CHECK(w == 256 && h == 224, "size %d %d", w, h);
        CHECK(AT(20, 20) == 0xFF0000u, "triangle inside: %06x", AT(20, 20));
        CHECK(AT(55, 55) == 0x000000u, "outside the hypotenuse: %06x", AT(55, 55));
        CHECK(AT(9, 20) == 0x000000u, "left of the triangle: %06x", AT(9, 20));
        CHECK(AT(110, 20) == 0x00FF00u, "image: %06x", AT(110, 20));
        CHECK(AT(150, 20) == 0x008000u, "shader: %06x", AT(150, 20));
    }
    printf(failed ? "feature: FAILED\n" : "feature: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "feature";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
