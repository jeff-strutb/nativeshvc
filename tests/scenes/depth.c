/* depth.c: a draw list's depth state (nshvc_draw_state_set, nshvc_draw_depth_own, nshvc_draw_depth_order), checked
 * without a reference emulator. Headless, the picture is 256 x 256, a pixel for a pixel; one scenario a picture:
 *
 *   depth --headless --updates 34
 *
 * Prints FAIL and exits 1 on any difference. The cases marked "shows" print what they find and assert nothing about it.
 * Two orders: NEAR_0 (z = 0 nearest, z = depth_a - depth_b / Z with a = 0.998, b = 255.488 for far views and a = 1, b = 2
 * for near ones) and NEAR_1 (z = near / Z: a = 0, b = -near, with near = 256 for far views and 4 for near ones, z up to
 * 1). What each expects, worked out:
 *
 *  0-3  cull (the list's default depth). A triangle with (x0, y0), (x0 + 16, y0), (x0, y0 + 16) goes right, then down and
 *       back: clockwise on a screen with y down (RED); (x0, y0), (x0, y0 + 16), (x0 + 16, y0), down, then up and right:
 *       counter-clockwise (GREEN). Three regions, cull NONE, BACK, FRONT, each both triangles, with the front
 *       counter-clockwise (front_ccw 1): NONE: red and green; BACK: green only (the clockwise is the back); FRONT: red
 *       only; and with the front clockwise (front_ccw 0): NONE: both; BACK: red only; FRONT: green only. Cases 0 and 1 draw
 *       them with nshvc_draw_tris, 2 and 3 as one mesh of both triangles with nshvc_draw_mesh (the same pixels, 8 pixels to a
 *       world unit at Z = 16).
 *  4    a closed box seen from the middle of its front, three of them: cull NONE, BACK, FRONT (front_ccw 1, each face wound
 *       to be counter-clockwise seen from outside). The pixel at each box's centre shows its near face (RED) with NONE and
 *       BACK, and with FRONT, which leaves the near face out and shows the far face's inside, the far face (GREEN).
 *
 *  5-14 (NEAR_0), 15-24 (NEAR_1), each ten cases, own depth, 1 sample a pixel unless said:
 *   +0  bias. A floor (Y = 100 below an eye at the origin, Z from 400 to 1200, f = 512) in RED and on its plane a smaller one
 *       in GREEN (X +-100, Z 500 to 900) drawn after it with depth_bias 1 and depth_slope 1: green over its whole footprint.
 *       A floor is a grazing view: the list's z changes 0.005 a row (NEAR_0: z = 0.998 - 255.488 / Z = 0.998 - 0.00499 (row -
 *       128); NEAR_1: z = 256 / Z = 0.005 (row - 128), as Z = 51200 / (row - 128)), and the second floor's depth differs from the
 *       first's by rounding, a few steps of about 3e-8: the bias of a step and one slope (0.005) is more than that by far.
 *   +1  a floor 5 world units lower (Y = 105, X 120 to 220, Z 500 to 900) in BLUE, drawn after the red one with the same bias:
 *       it is farther at every pixel they share by b / Z * (1 - 1 / 1.05) = 12.2 / Z (NEAR_1 the same, 256 / Z * 0.0476),
 *       0.0254 at the nearest row they share (Z = 478) and 0.0143 at the farthest (853), against a bias of 0.0048 (its own
 *       slope) and a step: it stays hidden, RED over its footprint.
 *   +2  depth_keep. A see-through quad (GREEN, alpha 0.5, which at 4 samples a pixel covers 2 of the 4) at Z = 8 drawn over a
 *       solid BLUE one at Z = 16, then a RED quad at Z = 12 (behind the see-through, in front of the solid), in two regions:
 *       the see-through quad with depth_keep (left) and without (right). Kept: the red one passes the depth test
 *       everywhere, so red covers all 4 samples: RED. Written: the see-through quad's 2 samples hold its depth, the red one
 *       is behind it there and fails: half green, half red, about 0x808000 (asserted: red below 0xF0, green above 0x70).
 *   +3  equal depths, the later wins: two quads at Z = 16 with the same vertices, RED then GREEN on the left (GREEN),
 *       GREEN then RED on the right (RED): the two give the same depth to the last bit.
 *   +4  the near plane: a ramp (Z from 8 to 24 across it, a square whose model z is its x, at Z = 16, size 8) with near = 12 is
 *       cut at Z = 12, screen x = 256 - 2048 / 12 = 85.33: black at x = 80 and 83, red at 88 and 95.
 *   +5  behind the eye: a floor (y = 4 below the eye, Z from -8 to 8, size 8 at the origin) with near = 6: only Z from 6 to
 *       8 is drawn, the rows 128 + 512 / Z from 192 to 213.3, and not what a division by Z would put above the horizon
 *       from the part behind the eye: red at (128, 200), black at (128, 150), (128, 225), and in rows 0 to 189.
 *   +6  the precision pair (mesh): two quads at Z = 17000 and 17000.01, the nearer (RED) drawn first, the farther (GREEN)
 *       after. NEAR_1 with z = 256 / Z = 0.01506 (float step 2^-30 = 9.3e-10): the two differ by z * dZ / Z = 8.7e-9, 9 steps:
 *       the nearer wins at every pixel. NEAR_0 with z = 0.998 - 255.488 / Z = 0.98297 (step 2^-24 = 6e-8): they differ by
 *       the same 8.7e-9, a seventh of a step: they cannot be told apart, and the later (the farther) is likely to win:
 *       shows, no assertion.
 *   +7  the same, the farther drawn first (NEAR_1: the nearer wins at every pixel; NEAR_0 shows).
 *   +8  a mesh against a wall of tris in one list: a ramp (Z = 16 + 4 x, x from -1 to 1 at Z = 16) and a wall of triangles at
 *       Z = 16 whose vertices carry z = 1 - 2 / 16 = 0.875 (NEAR_0) or 4 / 16 = 0.25 (NEAR_1), w = 16: the ramp where it is
 *       nearer (x < 0, screen x < 128: RED at 118), the wall where it is not (BLUE at 138), the wall drawn first.
 *   +9  the same, the mesh drawn first.
 *  25   shows, NEAR_0: the coplanar floor without bias: the fraction of green (no assertion).
 *  26   shows, NEAR_1: the same.
 *  27   NEAR_1 through nshvc_draw_tris: the pair of 5+6 as triangles with z = 256 / Z computed here in float and w = Z, the nearer
 *       (RED) drawn first: the nearer wins at every pixel, as in the mesh.
 *  28   the same, the farther drawn first.
 *  29   shows: the pair of 6, NEAR_0, in the shared depth (no depth of its own): one sixteenth of 24 bits is a step in z of
 *       9.5e-7, a hundred times the difference: the later (the farther) wins, GREEN (not asserted).
 *  30   composed. Mode 1: BG1 (front) red tiles on the left 64 pixels only, BG3 (back) blue everywhere, and between them, on
 *       BG2's layer, a list with a depth of its own with a GREEN quad at x 20 to 220, y 20 to 100, a window on that layer from
 *       x 100 to 160: left of 64 RED (BG1 in front), 64 to 99 and 161 to 219 GREEN, 100 to 160 BLUE (the window cuts the
 *       list), and BLUE outside the quad (BG3).
 *  31-33 an image over a quad that wrote depth: a RED quad at Z = 16 (x and y 96 to 160), then nshvc_draw_image of a 4 x 4 GREEN
 *       image over (100, 100) to (120, 120): an image is at the list's nearest end of z in its order, so it shows over the quad
 *       whichever end that is: GREEN at (110, 110), RED at (140, 140) and black outside the quad at (80, 80). 31: a depth of its
 *       own, NEAR_0 (the image's z is 0); 32: own, NEAR_1 (1); 33: no depth of its own, NEAR_1, in the picture's slice (reversed
 *       onto it, so the image's z 1 is the slice's nearest end).
 *  A list with none of this set draws as before this was added: mesh.c and feature.c (every draw there is of a list with the
 *  default state and order) cover that. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SIZE 256
#define SCENARIOS 34
static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define AT(x, y) (p[(y) * w + (x)] & 0xFFFFFFu)

#define RED 0xFFFF0000u
#define GREEN 0xFF00FF00u
#define BLUE 0xFF0000FFu
#define SEE 0x8000FF00u       /* green, alpha 0.5 */

/* the scenarios from 5 on in tens: the order (NEAR_0 first), then the case */
enum { C_BIAS, C_FARTHER, C_KEEP, C_EQUAL, C_NEARCUT, C_BEHIND, C_PAIR, C_PAIR_FAR_FIRST, C_WALL, C_WALL_MESH_FIRST };

static nshvc_draw_list list;
static nshvc_mesh tri2, box, floor_a, floor_b, floor_c, floor_unit, red16, green16, flat_blue, flat_red, see, near_a, near_b, bg3_quad, ramp;
static nshvc_tiles t4, t2;
static nshvc_image img;
static nshvc_map map1, map3;
static int pics, checked;

static nshvc_mesh_vertex mv(float x, float y, float z, uint32_t c)
{
    nshvc_mesh_vertex v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.z = z; v.colour = c;
    return v;
}

/* a quad of two triangles on the plane Z = z from (x0, y0) to (x1, y1), in the colour */
static void quad_z(nshvc_mesh m, float x0, float y0, float x1, float y1, float z, uint32_t c)
{
    nshvc_mesh_vertex v[6];
    v[0] = mv(x0, y0, z, c); v[1] = mv(x1, y0, z, c); v[2] = mv(x0, y1, z, c);
    v[3] = mv(x1, y0, z, c); v[4] = mv(x1, y1, z, c); v[5] = mv(x0, y1, z, c);
    nshvc_mesh_write(m, v, 6);
}

/* a quad of two triangles on the plane Y = y from (x0, z0) to (x1, z1) */
static void quad_y(nshvc_mesh m, float y, float x0, float z0, float x1, float z1, uint32_t c)
{
    nshvc_mesh_vertex v[6];
    v[0] = mv(x0, y, z0, c); v[1] = mv(x1, y, z0, c); v[2] = mv(x0, y, z1, c);
    v[3] = mv(x1, y, z0, c); v[4] = mv(x1, y, z1, c); v[5] = mv(x0, y, z1, c);
    nshvc_mesh_write(m, v, 6);
}

/* a triangle of the box, wound so that (b - a) x (c - a) . n > 0: counter-clockwise on the screen when n faces the eye */
static void box_tri(nshvc_mesh_vertex *out, const float *a, const float *b, const float *c, const float *n, uint32_t col)
{
    float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    int ok = cx * n[0] + cy * n[1] + cz * n[2] > 0.0f;
    out[0] = mv(a[0], a[1], a[2], col);
    out[1] = ok ? mv(b[0], b[1], b[2], col) : mv(c[0], c[1], c[2], col);
    out[2] = ok ? mv(c[0], c[1], c[2], col) : mv(b[0], b[1], b[2], col);
}

static void init(void)
{
    nshvc_mesh_vertex m[36];
    uint16_t pal[256];
    uint16_t words[32 * 32];
    int i, f;
    memset(pal, 0, sizeof pal);
    pal[1] = 0x7C00;                    /* BG3's blue */
    pal[17] = 0x001F;                   /* BG1's red, palette 1 */
    nshvc_palette_write(0, 256, pal);
    list = nshvc_draw_list_create();
    {
        static uint32_t tex[16];
        int k;
        img = nshvc_image_create(4, 4, NSHVC_IMAGE_RGBA);
        for (k = 0; k < 16; k++)
            tex[k] = GREEN;
        nshvc_image_write(img, tex);
    }
    /* two triangles, the clockwise at the model's origin and the counter-clockwise 3 units to the right: 2 units a leg */
    tri2 = nshvc_mesh_create(6);
    m[0] = mv(0, 0, 0, RED);   m[1] = mv(2, 0, 0, RED);   m[2] = mv(0, 2, 0, RED);
    m[3] = mv(3, 0, 0, GREEN); m[4] = mv(3, 2, 0, GREEN); m[5] = mv(5, 0, 0, GREEN);
    nshvc_mesh_write(tri2, m, 6);
    /* a closed box of half-size 2, faces wound outward, the near face (Z = -2) RED, the far (Z = +2) GREEN */
    box = nshvc_mesh_create(36);
    {
        static const float cs[8][3] = { { -2, -2, -2 }, { 2, -2, -2 }, { 2, 2, -2 }, { -2, 2, -2 }, { -2, -2, 2 }, { 2, -2, 2 }, { 2, 2, 2 }, { -2, 2, 2 } };
        static const int fc[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 4, 7, 3 }, { 1, 5, 6, 2 }, { 0, 1, 5, 4 }, { 3, 2, 6, 7 } };
        static const float nm[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
        static const uint32_t col[6] = { RED, GREEN, BLUE, 0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu };
        for (f = 0; f < 6; f++) {
            box_tri(m + f * 6, cs[fc[f][0]], cs[fc[f][1]], cs[fc[f][2]], nm[f], col[f]);
            box_tri(m + f * 6 + 3, cs[fc[f][0]], cs[fc[f][2]], cs[fc[f][3]], nm[f], col[f]);
        }
        nshvc_mesh_write(box, m, 36);
    }
    floor_a = nshvc_mesh_create(6);
    floor_b = nshvc_mesh_create(6);
    floor_c = nshvc_mesh_create(6);
    floor_unit = nshvc_mesh_create(6);
    quad_y(floor_a, 100.0f, -300.0f, 400.0f, 300.0f, 1200.0f, RED);
    quad_y(floor_b, 100.0f, -100.0f, 500.0f, 100.0f, 900.0f, GREEN);
    quad_y(floor_c, 105.0f, 120.0f, 500.0f, 220.0f, 900.0f, BLUE);
    quad_y(floor_unit, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, RED);
    red16 = nshvc_mesh_create(6);
    green16 = nshvc_mesh_create(6);
    flat_blue = nshvc_mesh_create(6);
    flat_red = nshvc_mesh_create(6);
    see = nshvc_mesh_create(6);
    near_a = nshvc_mesh_create(6);
    near_b = nshvc_mesh_create(6);
    bg3_quad = nshvc_mesh_create(6);
    ramp = nshvc_mesh_create(6);
    /* flat quads (f = 128 at Z = 16: 8 pixels to a unit) */
    quad_z(red16, -4.0f, -4.0f, 4.0f, 4.0f, 16.0f, RED);
    quad_z(green16, -4.0f, -4.0f, 4.0f, 4.0f, 16.0f, GREEN);
    quad_z(flat_blue, -4.0f, -4.0f, 4.0f, 4.0f, 16.0f, BLUE);
    quad_z(flat_red, -4.0f, -4.0f, 4.0f, 4.0f, 12.0f, RED);
    quad_z(see, -4.0f, -4.0f, 4.0f, 4.0f, 8.0f, SEE);
    quad_z(near_a, -2000.0f, -2000.0f, 2000.0f, 2000.0f, 17000.0f, RED);
    quad_z(near_b, -2000.0f, -2000.0f, 2000.0f, 2000.0f, 17000.01f, GREEN);
    quad_z(bg3_quad, 20.0f, 20.0f, 220.0f, 100.0f, 0.0f, GREEN);
    /* a square whose model z is its x, so that its Z runs across it */
    m[0] = mv(-1, -1, -1, RED); m[1] = mv(1, -1, 1, RED); m[2] = mv(-1, 1, -1, RED);
    m[3] = mv(1, -1, 1, RED);   m[4] = mv(1, 1, 1, RED);  m[5] = mv(-1, 1, -1, RED);
    nshvc_mesh_write(ramp, m, 6);
    /* tiles: 0 blank, 1 solid colour 1 */
    t4 = nshvc_tiles_create(4, 2, 1);
    {
        uint8_t two[64];
        memset(two, 0, sizeof two);
        for (i = 0; i < 8; i++)
            two[32 + 2 * i] = 0xFF;     /* tile 1 of 4-bit tiles: 32 bytes, plane 0 of every row all set: colour 1 */
        nshvc_tiles_write(t4, 0, two, 2, NSHVC_TILES_PLANAR);
    }
    t2 = nshvc_tiles_create(2, 2, 1);
    {
        uint8_t two[32];
        memset(two, 0, sizeof two);
        for (i = 0; i < 8; i++)
            two[16 + 2 * i] = 0xFF;     /* tile 1 of 2-bit tiles: 16 bytes */
        nshvc_tiles_write(t2, 0, two, 2, NSHVC_TILES_PLANAR);
    }
    map1 = nshvc_map_create(32, 32);
    for (i = 0; i < 32 * 32; i++)
        words[i] = (i % 32) < 8 ? (uint16_t)(1 | 1 << 10) : 0;      /* tile 1, palette 1, the left 8 columns */
    nshvc_map_write_snes(map1, 0, 0, 32, 32, words);
    map3 = nshvc_map_create(32, 32);
    for (i = 0; i < 32 * 32; i++)
        words[i] = 1;                                                /* tile 1, palette 0, everywhere */
    nshvc_map_write_snes(map3, 0, 0, 32, 32, words);
}

static nshvc_view view_of(float f, float near, float a, float b, float dmax)
{
    nshvc_view v;
    memset(&v, 0, sizeof v);
    v.rot[0] = v.rot[4] = v.rot[8] = 1.0f;
    v.f = f; v.cx = 128.0f; v.cy = 128.0f;
    v.near = near;
    v.depth_a = a; v.depth_b = b; v.depth_max = dmax;
    return v;
}

/* the views of each order: far (floors and the pair) and near (small quads at Z 8 to 24) */
static nshvc_view far_view(float f, int order, float near)
{
    return order ? view_of(f, near > 256.0f ? near : 256.0f, 0.0f, -256.0f, 1.0f) : view_of(f, near, 0.998f, 255.488f, 0.998f);
}

static nshvc_view near_view(int order, float near)
{
    return order ? view_of(128.0f, near, 0.0f, -4.0f, 1.0f) : view_of(128.0f, near, 1.0f, 2.0f, 0.99f);
}

static void tri_at(nshvc_vertex *v, float x0, float y0, int ccw, uint32_t c)
{
    int i;
    memset(v, 0, 3 * sizeof *v);
    v[0].x = x0; v[0].y = y0;
    v[1].x = ccw ? x0 : x0 + 16; v[1].y = ccw ? y0 + 16 : y0;
    v[2].x = ccw ? x0 + 16 : x0; v[2].y = ccw ? y0 : y0 + 16;
    for (i = 0; i < 3; i++)
        v[i].colour = c;
}

/* a quad of two triangles at Z, X and Y +-2000 (as near_a / near_b), z = 256 / Z computed here, w = Z */
static void tris_pair_quad(float z, uint32_t col)
{
    nshvc_vertex v[6];
    float half = 128.0f * 2000.0f / z, zz = 256.0f / z;
    int i;
    memset(v, 0, sizeof v);
    v[0].x = 128.0f - half; v[0].y = 128.0f - half;
    v[1].x = 128.0f + half; v[1].y = 128.0f - half;
    v[2].x = 128.0f - half; v[2].y = 128.0f + half;
    v[3] = v[1];
    v[4].x = 128.0f + half; v[4].y = 128.0f + half;
    v[5] = v[2];
    for (i = 0; i < 6; i++) {
        v[i].z = zz;
        v[i].w = z;
        v[i].colour = col;
    }
    nshvc_draw_tris(list, v, 6, NULL);
}

/* a wall of two triangles over the whole list at Z = 16 whose z is `zz` */
static void wall(float zz)
{
    nshvc_vertex v[6];
    int i;
    memset(v, 0, sizeof v);
    for (i = 0; i < 6; i++) {
        v[i].z = zz;
        v[i].w = 16.0f;
        v[i].colour = BLUE;
    }
    v[1].x = SIZE; v[2].y = SIZE; v[3].x = SIZE; v[3].y = 0; v[4].x = SIZE; v[4].y = SIZE; v[5].y = SIZE;
    nshvc_draw_tris(list, v, 6, NULL);
}

static void draw(float between)
{
    nshvc_view view = near_view(0, 2.5f);
    nshvc_draw_state st;
    nshvc_layer a;
    nshvc_screen s;
    nshvc_instance inst;
    nshvc_windows win;
    int sc = pics++, r, order = 0, cs = -1, own = 0;
    uint32_t samples = 1;
    (void)between;
    if (sc >= SCENARIOS)
        return;
    if (sc >= 5 && sc < 25) {
        order = (sc - 5) / 10;
        cs = (sc - 5) % 10;
        own = 1;
    } else if (sc == 25 || sc == 26) {
        order = sc - 25;
        own = 1;
    } else if (sc == 27 || sc == 28) {
        order = 1;
        own = 1;
    } else if (sc == 30) {
        own = 1;
    } else if (sc >= 31) {
        own = sc != 33;
        order = sc != 31;
    }
    memset(&st, 0, sizeof st);
    nshvc_draw_list_clear(list, SIZE, SIZE);
    nshvc_draw_depth_own(list, (uint32_t)own);
    nshvc_draw_depth_order(list, order ? NSHVC_DEPTH_NEAR_1 : NSHVC_DEPTH_NEAR_0);
    if (sc <= 3) {
        int ccw = sc == 0 || sc == 2;
        for (r = 0; r < 3; r++) {
            float x0 = 20.0f + 80.0f * (float)r, y0 = 40.0f;
            st.cull = (uint8_t)r;           /* NONE, BACK, FRONT */
            st.front_ccw = (uint8_t)ccw;
            nshvc_draw_state_set(list, &st);
            if (sc < 2) {
                nshvc_vertex v[6];
                tri_at(v, x0, y0, 0, RED);
                tri_at(v + 3, x0 + 24.0f, y0, 1, GREEN);
                nshvc_draw_tris(list, v, 6, NULL);
            } else {
                inst.x = (x0 - 128.0f) / 8.0f;
                inst.y = (y0 - 128.0f) / 8.0f;
                inst.z = 16.0f;
                inst.size = 1.0f;
                nshvc_draw_mesh(list, tri2, &view, &inst, 1, NULL);
            }
        }
    } else if (sc == 4) {
        for (r = 0; r < 3; r++) {
            st.cull = (uint8_t)r;
            st.front_ccw = 1;
            nshvc_draw_state_set(list, &st);
            inst.x = ((float)r - 1.0f) * 12.0f;
            inst.y = 0.0f;
            inst.z = 16.0f;
            inst.size = 1.0f;
            nshvc_draw_mesh(list, box, &view, &inst, 1, NULL);
        }
    } else if (cs == C_BIAS || cs == C_FARTHER || sc == 25 || sc == 26) {
        nshvc_instance zero = { 0, 0, 0, 1 };
        view = far_view(512.0f, order, 100.0f);
        nshvc_draw_mesh(list, floor_a, &view, &zero, 1, NULL);
        if (sc < 25) {
            st.depth_bias = 1.0f;
            st.depth_slope = 1.0f;
            nshvc_draw_state_set(list, &st);
        }
        nshvc_draw_mesh(list, cs == C_FARTHER ? floor_c : floor_b, &view, &zero, 1, NULL);
    } else if (cs == C_KEEP) {
        samples = 4;
        view = near_view(order, order ? 4.0f : 2.5f);
        for (r = 0; r < 2; r++) {
            inst.x = r ? 8.0f : -8.0f; inst.y = 0.0f; inst.z = 0.0f; inst.size = 1.0f;     /* two columns, at X = -8 and +8 */
            memset(&st, 0, sizeof st);
            nshvc_draw_state_set(list, &st);
            nshvc_draw_mesh(list, flat_blue, &view, &inst, 1, NULL);
            st.depth_keep = r == 0;
            nshvc_draw_state_set(list, &st);
            nshvc_draw_mesh(list, see, &view, &inst, 1, NULL);
            st.depth_keep = 0;
            nshvc_draw_state_set(list, &st);
            nshvc_draw_mesh(list, flat_red, &view, &inst, 1, NULL);
        }
    } else if (cs == C_EQUAL) {
        view = near_view(order, order ? 4.0f : 2.5f);
        inst.y = 0.0f; inst.z = 0.0f; inst.size = 1.0f;
        inst.x = -8.0f;                      /* screen x 64: red, then green */
        nshvc_draw_mesh(list, red16, &view, &inst, 1, NULL);
        nshvc_draw_mesh(list, green16, &view, &inst, 1, NULL);
        inst.x = 8.0f;                       /* screen x 192: green, then red */
        nshvc_draw_mesh(list, green16, &view, &inst, 1, NULL);
        nshvc_draw_mesh(list, red16, &view, &inst, 1, NULL);
    } else if (cs == C_NEARCUT) {
        nshvc_instance r8 = { 0, 0, 16, 8 };
        view = order ? view_of(128.0f, 12.0f, 0.0f, -8.0f, 1.0f) : near_view(0, 12.0f);
        nshvc_draw_mesh(list, ramp, &view, &r8, 1, NULL);
    } else if (cs == C_BEHIND) {
        nshvc_instance fl = { 0, -4, 0, 8 };
        view = near_view(order, 6.0f);
        nshvc_draw_mesh(list, floor_unit, &view, &fl, 1, NULL);
    } else if (cs == C_PAIR || cs == C_PAIR_FAR_FIRST || sc == 29) {
        nshvc_instance zero = { 0, 0, 0, 1 };
        view = far_view(128.0f, order, 1.0f);
        if (sc == 29)
            view = far_view(128.0f, 0, 1.0f);
        if (cs == C_PAIR_FAR_FIRST) {
            nshvc_draw_mesh(list, near_b, &view, &zero, 1, NULL);
            nshvc_draw_mesh(list, near_a, &view, &zero, 1, NULL);
        } else {
            nshvc_draw_mesh(list, near_a, &view, &zero, 1, NULL);
            nshvc_draw_mesh(list, near_b, &view, &zero, 1, NULL);
        }
    } else if (cs == C_WALL || cs == C_WALL_MESH_FIRST) {
        nshvc_instance rm = { 0, 0, 16, 4 };
        view = near_view(order, order ? 4.0f : 2.5f);
        if (cs == C_WALL)
            wall(order ? 4.0f / 16.0f : 1.0f - 2.0f / 16.0f);
        nshvc_draw_mesh(list, ramp, &view, &rm, 1, NULL);
        if (cs == C_WALL_MESH_FIRST)
            wall(order ? 4.0f / 16.0f : 1.0f - 2.0f / 16.0f);
    } else if (sc == 27 || sc == 28) {
        if (sc == 27) {
            tris_pair_quad(17000.0f, RED);
            tris_pair_quad(17000.01f, GREEN);
        } else {
            tris_pair_quad(17000.01f, GREEN);
            tris_pair_quad(17000.0f, RED);
        }
    } else if (sc >= 31) {
        nshvc_instance zero = { 0, 0, 0, 1 };
        nshvc_rect rc = { 100.0f, 100.0f, 120.0f, 120.0f };
        view = near_view(order, order ? 4.0f : 2.5f);
        nshvc_draw_mesh(list, red16, &view, &zero, 1, NULL);
        nshvc_draw_image(list, img, &rc, NULL);
    } else {
        nshvc_instance zero = { 0, 0, 1, 1 };
        /* f = 1 at Z = 1 with the centre at the corner: a view unit is a list pixel, so the quad's coordinates are pixels */
        view = view_of(1.0f, 0.5f, 1.0f, 0.5f, 0.99f);
        view.cx = 0.0f;
        view.cy = 0.0f;
        nshvc_draw_mesh(list, bg3_quad, &view, &zero, 1, NULL);
    }
    nshvc_draw_samples(list, samples);
    memset(&a, 0, sizeof a);
    if (sc == 30) {
        nshvc_layer l;
        nshvc_mode_set(1, 0, 0);
        memset(&l, 0, sizeof l);
        l.enabled_main = 1; l.tiles = t4; l.map = map1; l.tile_size = 8; l.mosaic = 1;
        nshvc_layer_set(0, &l);
        a.enabled_main = 1;
        a.draw_list = list;
        a.mosaic = 1;
        nshvc_layer_set(1, &a);
        memset(&l, 0, sizeof l);
        l.enabled_main = 1; l.tiles = t2; l.map = map3; l.tile_size = 8; l.mosaic = 1;
        nshvc_layer_set(2, &l);
        memset(&win, 0, sizeof win);
        win.w[0].left = 100;
        win.w[0].right = 160;
        win.layer_use[1] = NSHVC_WIN1;
        win.mask_main[1] = 1;
        nshvc_windows_set(&win);
    } else {
        nshvc_mode_set(1, 0, 0);
        a.enabled_main = 1;
        a.draw_list = list;
        nshvc_layer_set(0, &a);
    }
    memset(&s, 0, sizeof s);
    s.brightness = 15;
    s.width = s.height = SIZE;
    nshvc_screen_set(&s);
}

/* the fraction of the second floor's footprint (rows 187 to 228, half-width the row less 128) that shows `col` */
static void floor_b_count(const uint32_t *p, int w, uint32_t col, int *n, int *total)
{
    int x, y;
    *n = *total = 0;
    for (y = 187; y <= 228; y += 2)
        for (x = 128 - (y - 128) + 3; x <= 128 + (y - 128) - 3; x += 2) {
            (*total)++;
            *n += AT(x, y) == col;
        }
}

static void check(int sc, const uint32_t *p, int w, int h)
{
    int x, y, n, total, order = 0, cs = -1;
    if (!p || w != SIZE || h != SIZE) {
        CHECK(0, "scenario %d: no picture of %d x %d (%d x %d)", sc, SIZE, SIZE, w, h);
        return;
    }
    if (sc >= 5 && sc < 25) {
        order = (sc - 5) / 10;
        cs = (sc - 5) % 10;
    }
    if (sc <= 3) {
        /* per region (NONE, BACK, FRONT): the red at (x0 + 3, 43), the green at (x0 + 24 + 3, 43) */
        static const int red_shown[2][3] = { { 1, 0, 1 }, { 1, 1, 0 } };       /* by front_ccw 1, 0: NONE, BACK, FRONT */
        static const int green_shown[2][3] = { { 1, 1, 0 }, { 1, 0, 1 } };
        int row = sc == 0 || sc == 2 ? 0 : 1, r;
        for (r = 0; r < 3; r++) {
            uint32_t rd = AT(20 + 80 * r + 3, 43), gr = AT(20 + 80 * r + 24 + 3, 43);
            CHECK(rd == (red_shown[row][r] ? 0xFF0000u : 0u), "scenario %d region %d: the clockwise triangle: %06x", sc, r, rd);
            CHECK(gr == (green_shown[row][r] ? 0x00FF00u : 0u), "scenario %d region %d: the counter-clockwise triangle: %06x", sc, r, gr);
        }
    } else if (sc == 4) {
        CHECK(AT(32, 128) == 0xFF0000u, "box, no cull: %06x", AT(32, 128));
        CHECK(AT(128, 128) == 0xFF0000u, "box, cull back: %06x", AT(128, 128));
        CHECK(AT(224, 128) == 0x00FF00u, "box, cull front: %06x", AT(224, 128));
    } else if (cs == C_BIAS) {
        floor_b_count(p, w, 0x00FF00u, &n, &total);
        CHECK(n == total, "order %d bias: green on %d of %d points of the second floor", order, n, total);
    } else if (cs == C_FARTHER) {
        n = total = 0;
        for (y = 190; y <= 233; y += 2)
            for (x = 128 + (int)((float)(y - 128) * 120.0f / 105.0f) + 3; x <= 128 + (int)((float)(y - 128) * 220.0f / 105.0f) - 3; x += 2) {
                total++;
                n += AT(x, y) == 0xFF0000u;
            }
        CHECK(total > 100 && n == total, "order %d bias: the floor behind stays hidden at %d of %d points", order, n, total);
    } else if (cs == C_KEEP) {
        uint32_t kept = AT(50, 128), written = AT(206, 128);
        CHECK(kept == 0xFF0000u, "order %d depth_keep: the red one behind the see-through: %06x", order, kept);
        CHECK(((written >> 16) & 255u) < 0xF0u && ((written >> 8) & 255u) > 0x70u, "order %d written depth: expected about half green: %06x", order, written);
    } else if (cs == C_EQUAL) {
        CHECK(AT(64, 128) == 0x00FF00u, "order %d equal depths, green after red: %06x", order, AT(64, 128));
        CHECK(AT(192, 128) == 0xFF0000u, "order %d equal depths, red after green: %06x", order, AT(192, 128));
    } else if (cs == C_NEARCUT) {
        CHECK(AT(80, 128) == 0 && AT(83, 128) == 0, "order %d near: before the cut: %06x %06x", order, AT(80, 128), AT(83, 128));
        CHECK(AT(88, 128) == 0xFF0000u && AT(95, 128) == 0xFF0000u, "order %d near: after the cut: %06x %06x", order, AT(88, 128), AT(95, 128));
    } else if (cs == C_BEHIND) {
        CHECK(AT(128, 200) == 0xFF0000u, "order %d floor: the part ahead: %06x", order, AT(128, 200));
        CHECK(AT(128, 150) == 0 && AT(128, 225) == 0, "order %d floor: outside the part ahead: %06x %06x", order, AT(128, 150), AT(128, 225));
        for (y = 0, n = 0; y < 190; y++)
            for (x = 0; x < SIZE; x++)
                n += AT(x, y) != 0;
        CHECK(n == 0, "order %d floor: %d pixels of what lies behind the eye or before the near plane", order, n);
    } else if (cs == C_PAIR || cs == C_PAIR_FAR_FIRST) {
        n = total = 0;
        for (y = 130; y <= 141; y++)
            for (x = 118; x <= 138; x++) {
                total++;
                n += AT(x, y) == 0xFF0000u;
            }
        if (order)
            CHECK(n == total, "NEAR_1 own depth, %s first: the nearer (red) at %d of %d points", cs == C_PAIR ? "nearer" : "farther", n, total);
        else
            printf("shows: NEAR_0 own depth, %s first: the nearer at %d of %d points\n", cs == C_PAIR ? "nearer" : "farther", n, total);
    } else if (cs == C_WALL || cs == C_WALL_MESH_FIRST) {
        CHECK(AT(118, 128) == 0xFF0000u, "order %d wall (%s first): where the mesh is nearer: %06x", order, cs == C_WALL ? "wall" : "mesh", AT(118, 128));
        CHECK(AT(138, 128) == 0x0000FFu, "order %d wall (%s first): where it is not: %06x", order, cs == C_WALL ? "wall" : "mesh", AT(138, 128));
        CHECK(AT(10, 10) == 0x0000FFu, "order %d wall: the wall alone: %06x", order, AT(10, 10));
    } else if (sc == 25 || sc == 26) {
        floor_b_count(p, w, 0x00FF00u, &n, &total);
        printf("shows: coplanar without bias, NEAR_%d: green at %d of %d points\n", sc - 25, n, total);
    } else if (sc == 27 || sc == 28) {
        n = total = 0;
        for (y = 130; y <= 141; y++)
            for (x = 118; x <= 138; x++) {
                total++;
                n += AT(x, y) == 0xFF0000u;
            }
        CHECK(n == total, "tris NEAR_1 own depth, scenario %d: the nearer (red) at %d of %d points", sc, n, total);
    } else if (sc == 29) {
        n = total = 0;
        for (y = 130; y <= 141; y++)
            for (x = 118; x <= 138; x++) {
                total++;
                n += AT(x, y) == 0xFF0000u;
            }
        printf("shows: shared depth: the nearer at %d of %d points\n", n, total);
    } else if (sc >= 31) {
        CHECK(AT(110, 110) == 0x00FF00u, "scenario %d: the image over the quad that wrote depth: %06x", sc, AT(110, 110));
        CHECK(AT(140, 140) == 0xFF0000u, "scenario %d: the quad beside the image: %06x", sc, AT(140, 140));
        CHECK(AT(80, 80) == 0u, "scenario %d: outside the quad: %06x", sc, AT(80, 80));
    } else {
        CHECK(AT(30, 60) == 0xFF0000u, "composed: BG1 over the list: %06x", AT(30, 60));
        CHECK(AT(80, 60) == 0x00FF00u, "composed: the list over BG3: %06x", AT(80, 60));
        CHECK(AT(130, 60) == 0x0000FFu, "composed: the window cuts the list: %06x", AT(130, 60));
        CHECK(AT(190, 60) == 0x00FF00u, "composed: the list again: %06x", AT(190, 60));
        CHECK(AT(130, 150) == 0x0000FFu, "composed: outside the quad: %06x", AT(130, 150));
    }
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
    printf(failed ? "depth: FAILED\n" : "depth: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "depth";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
