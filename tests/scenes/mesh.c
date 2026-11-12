/* mesh.c: instanced meshes (nshvc_draw_mesh), checked without a reference emulator. Headless, the picture is
 * 256 x 256, a pixel for a pixel; one scenario a picture:
 *
 *   mesh --headless --updates 12
 *
 * Prints FAIL and exits 1 on any difference.
 *
 *  a1  the same triangles two ways, into lists of the same kind (one sample a pixel): nshvc_draw_mesh with 24
 *      instances, and the triangles projected here and given to nshvc_draw_tris with z = depth_a - depth_b / Z and
 *      w = Z. The camera looks along Z from the origin and every Z is a power of two, so every projection is exact in
 *      float whichever way it is worked: expected difference: none, 0 pixels.
 *  a2  the same with a turned camera and an eye off the origin (projections no longer exact: the two differ in the
 *      last bits of a vertex's position): a pixel differs only where an edge lies within about 1e-4 of its centre and the
 *      list has one sample, so a whole pixel flips: at most 0.5% of the picture's pixels (327).
 *  b1, b2  depth against nshvc_draw_tris: a ramp instance runs from nearer to farther than a wall of triangles (at
 *      Z = 16): where nearer it shows, where farther the wall does; the wall drawn before the mesh, and after.
 *  c1  the near plane: the same ramp reaches Z = 8 with near = 12: it is cut at 12, not dropped, at x = 85.33.
 *  c2  a floor at y = 4 from Z = -8 to Z = 8 (the eye over it), near = 6: a point of it at Z is on row 128 + 512 / Z,
 *      so only the part with Z from 6 to 8 is drawn, rows 192 to 213.3; what is nearer than 6 would be the rows below
 *      and is cut, and what is behind the eye, which a division by Z would put in the rows above 128, is not drawn.
 *  d1, d2, d3  a mesh written, the same drawn after another written (the first unwritten), and again with neither
 *      written: the new colours show, the unwritten mesh is unchanged.
 *  e   4,096 instances of one triangle in one call. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SIZE 256
#define SCENARIOS 12
static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed = 1; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { A1_MESH, A1_TRIS, A2_MESH, A2_TRIS, B1, B2, C1, C2, D1, D2, D3, E };
static nshvc_draw_list list;
static nshvc_mesh flat, pyramid, ramp, floor_, tri, pm, qm;
static uint32_t kept[SIZE * SIZE];         /* the picture of the scenario before, for the pairs */
static int pics, checked;

#define RED 0xFFFF0000u
#define GREEN 0xFF00FF00u
#define BLUE 0xFF0000FFu
#define YELLOW 0xFFFFFF00u

static nshvc_mesh_vertex mv(float x, float y, float z, uint32_t c)
{
    nshvc_mesh_vertex v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.z = z; v.colour = c;
    return v;
}

static nshvc_view view_of(float near)
{
    nshvc_view v;
    memset(&v, 0, sizeof v);
    v.rot[0] = v.rot[4] = v.rot[8] = 1.0f;
    v.f = 128.0f; v.cx = 128.0f; v.cy = 128.0f;
    v.near = near;
    v.depth_a = 1.0f; v.depth_b = 2.0f; v.depth_max = 0.99f;
    return v;
}

static void init(void)
{
    nshvc_mesh_vertex m[12];
    uint16_t pal[256];
    memset(pal, 0, sizeof pal);
    nshvc_palette_write(0, 256, pal);
    list = nshvc_draw_list_create();
    /* flat: a square of four triangles round its centre, in the plane of its model z = 0 */
    flat = nshvc_mesh_create(12);
    m[0] = mv(-1, -1, 0, RED);    m[1] = mv(1, -1, 0, RED);     m[2] = mv(0, 0, 0, RED);
    m[3] = mv(1, -1, 0, GREEN);   m[4] = mv(1, 1, 0, GREEN);    m[5] = mv(0, 0, 0, GREEN);
    m[6] = mv(1, 1, 0, BLUE);     m[7] = mv(-1, 1, 0, BLUE);    m[8] = mv(0, 0, 0, BLUE);
    m[9] = mv(-1, 1, 0, YELLOW);  m[10] = mv(-1, -1, 0, YELLOW); m[11] = mv(0, 0, 0, YELLOW);
    nshvc_mesh_write(flat, m, 12);
    /* pyramid: four faces, a point in front of its base */
    pyramid = nshvc_mesh_create(12);
    m[0] = mv(-1, -1, 0, RED);    m[1] = mv(1, -1, 0, RED);     m[2] = mv(0, 0, 1, RED);
    m[3] = mv(1, -1, 0, GREEN);   m[4] = mv(1, 1, 0, GREEN);    m[5] = mv(0, 0, 1, GREEN);
    m[6] = mv(1, 1, 0, BLUE);     m[7] = mv(-1, 1, 0, BLUE);    m[8] = mv(0, 0, 1, BLUE);
    m[9] = mv(-1, 1, 0, YELLOW);  m[10] = mv(-1, -1, 0, YELLOW); m[11] = mv(0, 0, 1, YELLOW);
    nshvc_mesh_write(pyramid, m, 12);
    /* ramp: a square whose model z is its x, so that its Z runs across it */
    ramp = nshvc_mesh_create(6);
    m[0] = mv(-1, -1, -1, RED);   m[1] = mv(1, -1, 1, RED);     m[2] = mv(-1, 1, -1, RED);
    m[3] = mv(1, -1, 1, RED);     m[4] = mv(1, 1, 1, RED);      m[5] = mv(-1, 1, -1, RED);
    nshvc_mesh_write(ramp, m, 6);
    /* floor: a square at y = 1 from z = -1 to 1 */
    floor_ = nshvc_mesh_create(6);
    m[0] = mv(-1, 1, -1, RED);    m[1] = mv(1, 1, -1, RED);     m[2] = mv(-1, 1, 1, RED);
    m[3] = mv(1, 1, -1, RED);     m[4] = mv(1, 1, 1, RED);      m[5] = mv(-1, 1, 1, RED);
    nshvc_mesh_write(floor_, m, 6);
    /* one triangle */
    tri = nshvc_mesh_create(3);
    m[0] = mv(0, 0, 0, RED);      m[1] = mv(2, 0, 0, RED);      m[2] = mv(0, 2, 0, RED);
    nshvc_mesh_write(tri, m, 3);
    /* two squares, side by side, written separately: P at the left, Q at the right */
    pm = nshvc_mesh_create(6);
    qm = nshvc_mesh_create(6);
    m[0] = mv(-1, -1, 0, RED);    m[1] = mv(1, -1, 0, RED);     m[2] = mv(-1, 1, 0, RED);
    m[3] = mv(1, -1, 0, RED);     m[4] = mv(1, 1, 0, RED);      m[5] = mv(-1, 1, 0, RED);
    nshvc_mesh_write(pm, m, 6);
    {
        int i;
        for (i = 0; i < 6; i++)
            m[i].colour = BLUE;
    }
    nshvc_mesh_write(qm, m, 6);
}

static void update(const nshvc_input *in);

/* the world's triangles of an instance of a mesh given here, projected as the view says, to nshvc_draw_tris */
typedef struct { double rot[9], eye[3]; } Cam;
static void project(const nshvc_mesh_vertex *mesh, int n, const nshvc_instance *inst, int count, const Cam *cam, nshvc_draw_list d)
{
    nshvc_vertex *out = (nshvc_vertex *)malloc((size_t)n * (size_t)count * sizeof *out);
    int i, k, o = 0;
    for (i = 0; i < count; i++)
        for (k = 0; k < n; k++) {
            double w[3] = { mesh[k].x * inst[i].size + inst[i].x - cam->eye[0], mesh[k].y * inst[i].size + inst[i].y - cam->eye[1],
                            mesh[k].z * inst[i].size + inst[i].z - cam->eye[2] };
            double X = cam->rot[0] * w[0] + cam->rot[1] * w[1] + cam->rot[2] * w[2], Y = cam->rot[3] * w[0] + cam->rot[4] * w[1] + cam->rot[5] * w[2],
                   Z = cam->rot[6] * w[0] + cam->rot[7] * w[1] + cam->rot[8] * w[2];
            nshvc_vertex *v = &out[o++];
            memset(v, 0, sizeof *v);
            v->x = (float)(128.0 + 128.0 * X / Z);
            v->y = (float)(128.0 + 128.0 * Y / Z);
            v->z = (float)(1.0 - 2.0 / Z);
            v->w = (float)Z;
            v->colour = mesh[k].colour;
        }
    nshvc_draw_tris(d, out, (uint32_t)o, NULL);
    free(out);
}

/* the 12 vertices of flat or pyramid, as the test wrote them (the mesh's own are on the GPU) */
static void model(int which, nshvc_mesh_vertex *m)
{
    uint32_t c[4] = { RED, GREEN, BLUE, YELLOW };
    float z = which ? 1.0f : 0.0f;
    int i;
    for (i = 0; i < 4; i++) {
        float x0 = i == 0 ? -1 : i == 1 ? 1 : i == 2 ? 1 : -1, y0 = i == 0 ? -1 : i == 1 ? -1 : i == 2 ? 1 : 1;
        float x1 = i == 0 ? 1 : i == 1 ? 1 : i == 2 ? -1 : -1, y1 = i == 0 ? -1 : i == 1 ? 1 : i == 2 ? 1 : -1;
        m[i * 3] = mv(x0, y0, 0, c[i]);
        m[i * 3 + 1] = mv(x1, y1, 0, c[i]);
        m[i * 3 + 2] = mv(0, 0, z, c[i]);
    }
}

static void wall(void)
{
    nshvc_vertex v[6];
    int i;
    memset(v, 0, sizeof v);
    for (i = 0; i < 6; i++) {
        v[i].z = 1.0f - 2.0f / 16.0f;
        v[i].w = 16.0f;
        v[i].colour = BLUE;
    }
    v[1].x = SIZE; v[2].y = SIZE; v[3].x = SIZE; v[3].y = 0; v[4].x = SIZE; v[4].y = SIZE; v[5].y = SIZE;
    nshvc_draw_tris(list, v, 6, NULL);
}

static void draw(float between)
{
    static nshvc_instance many[4096];
    nshvc_instance inst[24];
    nshvc_mesh_vertex m[12];
    nshvc_view view;
    nshvc_layer a;
    nshvc_screen s;
    int sc = pics++, i;
    (void)between;
    if (sc >= SCENARIOS)
        return;
    nshvc_draw_list_clear(list, SIZE, SIZE);
    nshvc_draw_samples(list, 1);
    view = view_of(2.5f);
    /* a1/a2's instances: places at half units, sizes powers of two, Z a power of two in a1 */
    for (i = 0; i < 24; i++) {
        inst[i].x = ((i % 6) - 2.5f) * 2.0f;
        inst[i].y = (((i / 6) % 4) - 1.5f) * 2.0f;
        inst[i].z = 8.0f * (float)(1 << (i % 3));
        inst[i].size = i % 2 ? 1.0f : 0.5f;
    }
    if (sc == A1_MESH) {
        nshvc_draw_mesh(list, flat, &view, inst, 24, NULL);
    } else if (sc == A1_TRIS) {
        Cam cam = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 0, 0 } };
        model(0, m);
        project(m, 12, inst, 24, &cam, list);
    } else if (sc == A2_MESH || sc == A2_TRIS) {
        double ca = cos(0.3), sa = sin(0.3), cb = cos(0.2), sb = sin(0.2);
        /* world to view: turned about Y by 0.3, then about X by 0.2 */
        Cam cam = { { ca, 0, sa, sa * sb, cb, -ca * sb, -sa * cb, sb, ca * cb }, { 0.3, -0.2, -1.7 } };
        for (i = 0; i < 9; i++)
            view.rot[i] = (float)cam.rot[i];
        for (i = 0; i < 3; i++)
            view.eye[i] = (float)cam.eye[i];
        if (sc == A2_MESH) {
            nshvc_draw_mesh(list, pyramid, &view, inst, 24, NULL);
        } else {
            /* the instances' floats, as the mesh path has them, and the model as written */
            model(1, m);
            project(m, 12, inst, 24, &cam, list);
        }
    } else if (sc == B1 || sc == B2) {
        nshvc_instance r = { 0, 0, 16, 4 };
        if (sc == B1)
            wall();
        nshvc_draw_mesh(list, ramp, &view, &r, 1, NULL);
        if (sc == B2)
            wall();
    } else if (sc == C1) {
        nshvc_instance r = { 0, 0, 16, 8 };
        view = view_of(12.0f);
        nshvc_draw_mesh(list, ramp, &view, &r, 1, NULL);
    } else if (sc == C2) {
        nshvc_instance r = { 0, -4, 0, 8 };
        view = view_of(6.0f);
        nshvc_draw_mesh(list, floor_, &view, &r, 1, NULL);
    } else if (sc == D1 || sc == D2 || sc == D3) {
        nshvc_instance l = { -4, 0, 16, 2 }, r = { 4, 0, 16, 2 };      /* screen x 96 and 160, 16 pixels each way */
        if (sc == D2) {
            for (i = 0; i < 6; i++) {
                m[i] = mv(0, 0, 0, GREEN);
            }
            m[0] = mv(-1, -1, 0, GREEN);  m[1] = mv(1, -1, 0, GREEN);  m[2] = mv(-1, 1, 0, GREEN);
            m[3] = mv(1, -1, 0, GREEN);   m[4] = mv(1, 1, 0, GREEN);   m[5] = mv(-1, 1, 0, GREEN);
            nshvc_mesh_write(pm, m, 6);          /* only P is written */
        }
        nshvc_draw_mesh(list, pm, &view, &l, 1, NULL);
        nshvc_draw_mesh(list, qm, &view, &r, 1, NULL);
    } else {
        for (i = 0; i < 4096; i++) {
            many[i].x = ((i % 64) - 32) * 0.5f;
            many[i].y = ((i / 64) - 32) * 0.5f;
            many[i].z = 16.0f;
            many[i].size = 0.25f;
        }
        nshvc_draw_mesh(list, tri, &view, many, 4096, NULL);
    }
    memset(&a, 0, sizeof a);
    a.enabled_main = 1;
    a.draw_list = list;
    nshvc_mode_set(1, 0, 0);
    nshvc_layer_set(0, &a);
    memset(&s, 0, sizeof s);
    s.brightness = 15;
    s.width = s.height = SIZE;
    nshvc_screen_set(&s);
}

#define AT(x, y) (p[(y) * w + (x)] & 0xFFFFFFu)

/* the picture of scenario sc */
static void check(int sc, const uint32_t *p, int w, int h)
{
    int x, y, n = 0;
    if (!p || w != SIZE || h != SIZE) {
        CHECK(0, "scenario %d: no picture of %d x %d (%d x %d)", sc, SIZE, SIZE, w, h);
        return;
    }
    if (sc == A1_MESH || sc == A2_MESH) {
        memcpy(kept, p, sizeof kept);
        for (x = 0; x < SIZE * SIZE; x++)
            n += (kept[x] & 0xFFFFFFu) != 0;
        CHECK(n > 1000, "scenario %d: the mesh drew only %d pixels", sc, n);
    } else if (sc == A1_TRIS || sc == A2_TRIS) {
        int limit = sc == A1_TRIS ? 0 : SIZE * SIZE * 5 / 1000;
        for (x = 0; x < SIZE * SIZE; x++)
            n += (kept[x] & 0xFFFFFFu) != (p[x] & 0xFFFFFFu);
        CHECK(n <= limit, "scenario %d: %d pixels differ between the mesh and the triangles (at most %d)", sc, n, limit);
        printf("a%d: %d pixels differ (at most %d)\n", sc == A1_TRIS ? 1 : 2, n, limit);
    } else if (sc == B1 || sc == B2) {
        CHECK(AT(118, 128) == 0xFF0000u, "scenario %d: nearer than the wall: %06x", sc, AT(118, 128));
        CHECK(AT(138, 128) == 0x0000FFu, "scenario %d: farther than the wall: %06x", sc, AT(138, 128));
        CHECK(AT(10, 10) == 0x0000FFu, "scenario %d: the wall alone: %06x", sc, AT(10, 10));
    } else if (sc == C1) {
        CHECK(AT(80, 128) == 0x000000u, "near: before the cut: %06x", AT(80, 128));
        CHECK(AT(83, 128) == 0x000000u, "near: before the cut: %06x", AT(83, 128));
        CHECK(AT(88, 128) == 0xFF0000u, "near: after the cut: %06x", AT(88, 128));
        CHECK(AT(95, 128) == 0xFF0000u, "near: after the cut: %06x", AT(95, 128));
    } else if (sc == C2) {
        CHECK(AT(128, 194) == 0xFF0000u && AT(128, 210) == 0xFF0000u, "floor: the part ahead: %06x %06x", AT(128, 194), AT(128, 210));
        CHECK(AT(128, 216) == 0 && AT(128, 250) == 0, "floor: before the near plane: %06x %06x", AT(128, 216), AT(128, 250));
        for (y = 0; y < SIZE; y++)
            for (x = 0; x < SIZE; x++)
                if (y < 191 || y > 214)     /* a row either side of the part drawn left to rounding */
                    n += AT(x, y) != 0;
        CHECK(n == 0, "floor: %d pixels of what lies behind the eye, past the floor's end or before the near plane", n);
    } else if (sc == D1) {
        CHECK(AT(96, 128) == 0xFF0000u && AT(160, 128) == 0x0000FFu, "written: %06x %06x", AT(96, 128), AT(160, 128));
    } else if (sc == D2) {
        CHECK(AT(96, 128) == 0x00FF00u, "rewritten: %06x", AT(96, 128));
        CHECK(AT(160, 128) == 0x0000FFu, "unwritten: %06x", AT(160, 128));
    } else if (sc == D3) {
        CHECK(AT(96, 128) == 0x00FF00u && AT(160, 128) == 0x0000FFu, "kept: %06x %06x", AT(96, 128), AT(160, 128));
    } else {
        int i;
        for (i = 0; i < 4096; i++) {
            int ox = (i % 64) * 4, oy = (i / 64) * 4;
            n += AT(ox + 1, oy + 1) == 0xFF0000u && AT(ox + 3, oy + 3) == 0;
        }
        CHECK(n == 4096, "many: %d of 4096 instances drawn right", n);
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
    if (pics > checked && checked < SCENARIOS) {
        int w = 0, h = 0;
        const uint32_t *p = nshvc_picture_read(&w, &h);
        check(checked++, p, w, h);
    }
    CHECK(checked == SCENARIOS, "%d of %d scenarios checked: run with --updates %d", checked, SCENARIOS, SCENARIOS);
    printf(failed ? "mesh: FAILED\n" : "mesh: ok\n");
    fflush(stdout);
    if (failed)
        exit(1);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "mesh";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
