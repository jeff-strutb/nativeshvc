/* m7check.c: how far the engine's anti-aliased Mode 7 is from a reference worked out from the
 * console's documented lookup. Numbers only: it sets no pass mark.
 *
 *   m7check --dir DIR      DIR holding the scenes scenes.py made (DIR/NAME/NAME.scene); draws
 *                          mode7-rot, mode7-clear and mode7-tile0 through the engine headless at
 *                          the console's own size (256 x 224) and at three times it, and prints for
 *                          each the largest and the mean difference a colour channel (0-255)
 *   m7check --scene FILE --size WxH --headless --updates 1
 *                          one scene at one size (what --dir starts, one process each)
 *   m7check --scene FILE --proof ORACLE.ppm
 *                          the reference proved first: it takes the console's own one point a pixel
 *                          and is compared byte for byte with the oracle's picture of the same scene
 *                          (DIR/NAME/frame_6.ppm, 256 x 224); no engine is started. The area-mean
 *                          numbers mean something only if this prints 0 pixels differ.
 *
 * The reference. The console shows pixel (x, y) of line y + 1 the plane's texel at the point its
 * fixed-point sums give: per line, the origin
 *     ox = (a * X & ~63) + (b * Y & ~63) + (b * line & ~63) + (cx << 8), the same with c, d and cy,
 *     X = clip(scroll_x - cx), Y = clip(scroll_y - cy), clip n = n & 0x2000 ? n | ~1023 : n & 1023,
 *     line the line (255 less it with the vertical flip),
 * and the texel ((ox + a * x) >> 8, (oy + c * x) >> 8), x the column (255 less it with the
 * horizontal flip); outside the 1024 x 1024 plane it repeats, or is transparent, or is tile 0's
 * texel, as the scene says; a texel 0 shows the backdrop. The reference takes that same lookup
 * over the whole area of each picture pixel (a window pixel is 1/k of one), at 8 x 8 points a
 * pixel, the point's line the row it lies in, and averages the colours (the backdrop where a
 * point is transparent). So it is the picture the console's lookup makes, seen through a
 * window pixel's area.
 *
 * What it does not imitate, on purpose: the engine's own filter. The engine reads the plane in
 * colour through the GPU's anisotropic filter along each pixel's footprint, from a plane with
 * smaller levels each the box mean of the level before, in the palette most rows use (others
 * kept at a quarter's side as a difference), the footprint's down-step estimated from the rows'
 * own matrices; where a pixel spans a texel or less it mixes only the two texels an edge falls
 * between by how much of the pixel each covers; the outside of the plane mixed by the footprint's
 * share; edges of the geometry by 8 or 4 samples. The reference is the exact area mean, so its
 * difference from the engine is what those approximations amount to, not an error of either. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

static const char *scene_path;
static int want_w = 256, want_h = 224;

static uint16_t pal[256];
static uint8_t chars[256 * 64];
static uint8_t plane[128 * 128];
static int m7[12];                      /* a b c d cx cy sx sy outside fx fy ext */
static nshvc_tiles tiles;
static nshvc_map map;

static uint8_t *hex(const char *s, size_t *n)
{
    size_t l = strlen(s), i;
    uint8_t *o = (uint8_t *)malloc(l / 2 + 1);
    static const char h[] = "0123456789abcdef";
    for (i = 0; i + 1 < l; i += 2)
        o[i / 2] = (uint8_t)((strchr(h, s[i]) - h) << 4 | (strchr(h, s[i + 1]) - h));
    *n = l / 2;
    return o;
}

/* the scene file's palette, chars, plane and Mode 7 settings */
static void load(void)
{
    FILE *f = fopen(scene_path, "r");
    size_t cap = 1 << 20;
    char *buf = (char *)malloc(cap);
    if (!f || !buf)
        exit(2);
    while (fgets(buf, (int)cap, f)) {
        char *c = strtok(buf, " \n");
        size_t n, i;
        if (!c)
            continue;
        if (!strcmp(c, "pal")) {
            uint8_t *d = hex(strtok(NULL, " \n"), &n);
            for (i = 0; i < 256 && i * 2 + 1 < n; i++)
                pal[i] = (uint16_t)(d[i * 2] | d[i * 2 + 1] << 8);
            free(d);
        } else if (!strcmp(c, "tiles7")) {
            uint8_t *d;
            strtok(NULL, " \n"); strtok(NULL, " \n"); strtok(NULL, " \n");
            d = hex(strtok(NULL, " \n"), &n);
            memcpy(chars, d, n < sizeof chars ? n : sizeof chars);
            free(d);
        } else if (!strcmp(c, "map")) {
            uint8_t *d;
            strtok(NULL, " \n"); strtok(NULL, " \n"); strtok(NULL, " \n");
            d = hex(strtok(NULL, " \n"), &n);
            for (i = 0; i < 128 * 128 && i * 2 < n; i++)
                plane[i] = d[i * 2];
            free(d);
        } else if (!strcmp(c, "m7")) {
            int k;
            for (k = 0; k < 12; k++)
                m7[k] = atoi(strtok(NULL, " \n"));
        }
    }
    fclose(f);
    free(buf);
}

static void init(void)
{
    uint16_t words[128 * 128];
    int i;
    load();
    nshvc_palette_write(0, 256, pal);
    tiles = nshvc_tiles_create(8, 256, 1);
    nshvc_tiles_write(tiles, 0, chars, 256, NSHVC_TILES_MODE7);
    map = nshvc_map_create(128, 128);
    for (i = 0; i < 128 * 128; i++)
        words[i] = plane[i];
    nshvc_map_write_snes(map, 0, 0, 128, 128, words);
}

static void update(const nshvc_input *in) { (void)in; }

static void draw(float between)
{
    nshvc_layer a;
    nshvc_mode7 m;
    nshvc_screen s;
    (void)between;
    /* the picture's settings are set in draw, each picture: the screen fills the window, a pixel for a pixel */
    memset(&s, 0, sizeof s);
    s.brightness = 15;
    s.aspect = NSHVC_ASPECT_FILL;
    nshvc_screen_set(&s);
    nshvc_mode_set(7, 0, 0);
    memset(&a, 0, sizeof a);
    a.enabled_main = 1;
    a.tile_size = 8;
    a.mosaic = 1;
    nshvc_layer_set(0, &a);
    memset(&m, 0, sizeof m);
    m.a = (int16_t)m7[0]; m.b = (int16_t)m7[1]; m.c = (int16_t)m7[2]; m.d = (int16_t)m7[3];
    m.centre_x = (int16_t)m7[4]; m.centre_y = (int16_t)m7[5]; m.scroll_x = (int16_t)m7[6]; m.scroll_y = (int16_t)m7[7];
    m.outside = (uint8_t)m7[8]; m.flip_x = (uint8_t)m7[9]; m.flip_y = (uint8_t)m7[10]; m.extbg = (uint8_t)m7[11];
    m.tiles = tiles;
    m.map = map;
    nshvc_mode7_set(&m);
}

static int clip13(int n) { return n & 0x2000 ? n | ~1023 : n & 1023; }

/* the texel at plane point (px, py) in texels, whole: 0 where transparent */
static int texel(int px, int py)
{
    if ((px | py) & ~1023) {
        if (m7[8] == 1)
            return 0;
        if (m7[8] == 2)
            return chars[(py & 7) * 8 + (px & 7)];
        px &= 1023;
        py &= 1023;
    }
    return chars[plane[(py >> 3) * 128 + (px >> 3)] * 64 + (py & 7) * 8 + (px & 7)];
}

static void rgb_of(int t, int out[3])
{
    unsigned c = pal[t], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    out[0] = (int)(r << 3 | r >> 2);
    out[1] = (int)(g << 3 | g >> 2);
    out[2] = (int)(b << 3 | b >> 2);
}

#define N 8             /* points an axis in a window pixel */

/* each line's origin, by the console's sums; line is the row + 1, 255 less it flipped */
static void origins(long long *ox, long long *oy)
{
    int k, x0 = clip13(m7[6] - m7[4]), y0 = clip13(m7[7] - m7[5]);
    for (k = 0; k < 224; k++) {
        int line = m7[10] ? 255 - (k + 1) : k + 1;
        ox[k] = (long long)((m7[0] * x0) & ~63) + ((m7[1] * line) & ~63) + ((m7[1] * y0) & ~63) + ((long long)m7[4] << 8);
        oy[k] = (long long)((m7[2] * x0) & ~63) + ((m7[3] * line) & ~63) + ((m7[3] * y0) & ~63) + ((long long)m7[5] << 8);
    }
}

/* floor(a / 256) as the console's shift: arithmetic, so down for a negative */
static int sh8(long long a) { return (int)(a >= 0 ? a / 256 : -((-a + 255) / 256)); }

/* the proof: the reference at one point a pixel, the console's own (the integer column, the line's origin), against
   the oracle's picture (a binary PPM, 256 x 224), byte for byte. Only if they agree are the area-mean numbers meaningful. */
static int proof(const char *ppm)
{
    FILE *f = fopen(ppm, "rb");
    long long ox[224], oy[224];
    int w = 0, h = 0, mx = 0, c, x, y, differ = 0, fx = -1, fy = -1, fr[3] = { 0 }, fo[3] = { 0 };
    unsigned char *px;
    if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != 256 || h != 224 || mx != 255) {
        printf("m7check proof: %s: not a 256 x 224 binary PPM\n", ppm);
        return 2;
    }
    fgetc(f);                           /* the one white space after the header */
    px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px || fread(px, 3, (size_t)w * h, f) != (size_t)w * h) {
        printf("m7check proof: %s: short picture\n", ppm);
        return 2;
    }
    fclose(f);
    origins(ox, oy);
    for (y = 0; y < 224; y++)
        for (x = 0; x < 256; x++) {
            int xp = m7[9] ? 255 - x : x, rgb[3];
            int t = texel(sh8(ox[y] + (long long)m7[0] * xp), sh8(oy[y] + (long long)m7[2] * xp));
            rgb_of(t, rgb);
            for (c = 0; c < 3; c++)
                if (rgb[c] != px[(y * 256 + x) * 3 + c]) {
                    if (!differ) {
                        fx = x; fy = y;
                        memcpy(fr, rgb, sizeof fr);
                        for (c = 0; c < 3; c++)
                            fo[c] = px[(y * 256 + x) * 3 + c];
                    }
                    differ++;
                    break;
                }
        }
    printf("m7check proof: %s: %d of %d pixels differ from the oracle's picture\n", scene_path, differ, 256 * 224);
    if (differ)
        printf("  first at %d, %d: reference %02x%02x%02x, oracle %02x%02x%02x\n", fx, fy, fr[0], fr[1], fr[2], fo[0], fo[1], fo[2]);
    free(px);
    return differ ? 1 : 0;
}

static void shutdown(void)
{
    int w = 0, h = 0, X, Y, k, i, j;
    const uint32_t *p = nshvc_picture_read(&w, &h);
    double sum = 0.0;
    int most = 0;
    long long ox[224], oy[224];
    int scale;
    if (!p) {
        printf("m7check: %s: no picture\n", scene_path);
        return;
    }
    if (w % 256 || h % 224 || w / 256 != h / 224) {
        printf("m7check: %s: a picture of %d x %d is no whole multiple of 256 x 224\n", scene_path, w, h);
        return;
    }
    scale = w / 256;
    origins(ox, oy);
    for (Y = 0; Y < h; Y++)
        for (X = 0; X < w; X++) {
            double acc[3] = { 0, 0, 0 };
            uint32_t e = p[Y * w + X];
            for (j = 0; j < N; j++)
                for (i = 0; i < N; i++) {
                    double xs = ((double)X + (i + 0.5) / N) / scale, ys = ((double)Y + (j + 0.5) / N) / scale;
                    int row = (int)floor(ys), c[3], t;
                    double xp = m7[9] ? 256.0 - xs : xs, u, v;
                    u = ((double)ox[row] + (double)m7[0] * xp) / 256.0;
                    v = ((double)oy[row] + (double)m7[2] * xp) / 256.0;
                    t = texel((int)floor(u), (int)floor(v));
                    rgb_of(t, c);
                    acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
                }
            for (k = 0; k < 3; k++) {
                int want = (int)floor(acc[k] / (N * N) + 0.5), got = (int)(e >> (16 - 8 * k) & 255), d = abs(want - got);
                if (d > most)
                    most = d;
                sum += d;
            }
        }
    printf("m7check: %s at %d x %d: largest difference %d, mean %.3f a channel\n", scene_path, w, h, most, sum / ((double)w * h * 3));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    const char *dir = NULL, *proof_ppm = NULL;
    int i;
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--scene"))
            scene_path = argv[i + 1];
        else if (!strcmp(argv[i], "--dir"))
            dir = argv[i + 1];
        else if (!strcmp(argv[i], "--proof"))
            proof_ppm = argv[i + 1];
        else if (!strcmp(argv[i], "--size"))
            sscanf(argv[i + 1], "%dx%d", &want_w, &want_h);
    }
    if (dir) {
        static const char *names[3] = { "mode7-rot", "mode7-clear", "mode7-tile0" };
        int n, s;
        for (n = 0; n < 3; n++)
            for (s = 1; s <= 3; s += 2) {
                char cmd[2048], line[512];
                FILE *p;
                snprintf(cmd, sizeof cmd, "'%s' --headless --updates 1 --scene '%s/%s/%s.scene' --size %dx%d", argv[0], dir, names[n], names[n], 256 * s,
                         224 * s);
                if (!(p = popen(cmd, "r"))) {
                    fprintf(stderr, "m7check: cannot start %s\n", cmd);
                    return 2;
                }
                while (fgets(line, sizeof line, p))
                    fputs(line, stdout);
                pclose(p);
            }
        return 0;
    }
    if (!scene_path) {
        fprintf(stderr, "usage: m7check --dir DIR | --scene FILE --proof ORACLE.ppm | --scene FILE --size WxH --headless --updates 1\n");
        return 2;
    }
    if (proof_ppm) {
        load();
        return proof(proof_ppm);
    }
    {
        char size[64];
        snprintf(size, sizeof size, "%dx%d", want_w, want_h);
        setenv("RR_SIZE", size, 1);
    }
    g.title = "Mode 7 check";
    g.id = "m7check";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = shutdown;
    return nshvc_run(&g, argc, argv);
}
