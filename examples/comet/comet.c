/*
 * comet.c: Comet Catch, a complete small game on the engine, using each of its
 * parts: tiles, maps, palette, layers, Mode 7 with a per-line perspective, a
 * mid-picture mode change, sprites, windows, colour blending, mosaic, a draw
 * list with a game shader, an overlay, input and control names, typed text,
 * saves, packaged data, jobs, profiling, music code with voices and echo.
 *
 * Catch the falling comets with the ship; three missed ends the run. Start
 * begins and Select pauses. A new best score takes three typed initials.
 *
 *   comet                          real time, until the window closes
 *   comet --headless --updates N   N updates as fast as they run
 *
 * At exit it prints a hash of its state: the same input gives the same state
 * however the threads ran.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define SCREEN_W 256
#define SKY_LINES 64
#define LINES 224
#define COMETS 16
#define SHIP_Y 184
#define MISSES 3

enum { TITLE, PLAY, OVER };
enum { SND_CATCH = 1, SND_MISS = 2, SND_OVER = 3 };

/* ---- the game's state: changed only in update ------------------------------ */

typedef struct { int32_t x, y, vy, live; } Comet;

typedef struct { uint32_t best; char initials[4]; } Save;

static struct {
    int mode, paused, misses, entering, typed;
    uint32_t score, seed, ticks, spawn_in, wave;
    int32_t ship_x, last_ship_x;
    uint16_t last_pad;
    Comet comet[COMETS];
    Save save;
} g;

static struct { uint32_t every, speed; } waves[16];
static uint32_t nwaves;

/* ---- what the picture is made from ----------------------------------------- */

static nshvc_tiles hud_tiles, star_tiles, sprite_tiles, ground_tiles;
static nshvc_map hud_map, star_map, ground_map;
static nshvc_image moon_image, panel_image;
static nshvc_shader moon_shader;
static nshvc_draw_list sky;
static uint64_t pictures;

/* a 3 x 5 font: digits then letters, each row 3 bits, top row first */
static const uint16_t font[36] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF,
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A,
    0x5BAD, 0x4927, 0x5FED, 0x6B6D, 0x2B6A, 0x6BA4, 0x2B73, 0x6BAD, 0x388E, 0x7492,
    0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7
};

static uint32_t rnd(uint32_t n)
{
    g.seed = g.seed * 1664525u + 1013904223u;
    return (g.seed >> 8) % n;
}

/* ---- the music: the audio thread's own state, reached only by messages ------ */

static const uint8_t melody[32] = {
    0, 4, 7, 12, 7, 4, 0, 4,  2, 5, 9, 14, 9, 5, 2, 5,
    4, 7, 11, 16, 11, 7, 4, 7,  0, 4, 7, 12, 16, 12, 7, 4
};
static const uint16_t note_pitch[17] = {
    0x0800, 0x087A, 0x08FB, 0x0983, 0x0A14, 0x0AAD, 0x0B50, 0x0BFC, 0x0CB3,
    0x0D74, 0x0E41, 0x0F1A, 0x1000, 0x10F4, 0x11F6, 0x1307, 0x1428
};
static nshvc_sample tone, hiss;
static uint32_t music_tick_count, music_step;

static void music_start(void)
{
    static const int8_t fir[8] = { 40, 20, 10, 5, 0, 0, 0, 0 };
    nshvc_sound_volume(100, 100);
    nshvc_echo(2, 40, 30, 30, fir);
    nshvc_voice_adsr(0, 12, 3, 4, 14);
    nshvc_voice_volume(0, 50, 50);
    nshvc_voice_options(0, 0, 1, 0);
    nshvc_voice_adsr(1, 15, 7, 0, 20);
    nshvc_voice_volume(1, 70, 70);
    nshvc_voice_options(1, 1, 0, 0);
    nshvc_sound_noise_rate(24);
}

static void effect(uint16_t pitch)
{
    nshvc_voice_pitch(1, pitch);
    nshvc_voice_key_on(1, hiss);
}

static void music_tick(void)
{
    uint8_t m[8];
    if (music_tick_count == 0)
        music_start();
    while (nshvc_music_receive(m, sizeof m)) {
        if (m[0] == SND_CATCH)
            effect(0x1800);
        else if (m[0] == SND_MISS)
            effect(0x0600);
        else if (m[0] == SND_OVER)
            nshvc_sound_volume(50, 50);
    }
    if (music_tick_count % 12 == 0) {
        nshvc_voice_pitch(0, note_pitch[melody[music_step]]);
        nshvc_voice_key_on(0, tone);
        music_step = (music_step + 1) % 32;
    }
    music_tick_count++;
}

/* one looping block of BRR: a square wave */
static void make_samples(void)
{
    static const uint8_t square[9] = { 0x83, 0x77, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88, 0x88 };
    tone = nshvc_sample_load(square, 9, NSHVC_SAMPLE_BRR, 0);
    hiss = nshvc_sample_load(square, 9, NSHVC_SAMPLE_BRR, 0);
}

static void send_sound(uint8_t what) { nshvc_music_send(&what, 1); }

/* ---- saves and packaged data ------------------------------------------------ */

static void load_game_data(void)
{
    static char text[512];
    nshvc_asset a = nshvc_asset_open("waves.txt");
    uint32_t n, used = 0;
    char *p = text;
    if (a.id) {
        n = nshvc_asset_read(a, text, sizeof text - 1);
        text[n] = 0;
        nshvc_asset_close(a);
        while (nwaves < 16 && sscanf(p, "%u %u%n", &waves[nwaves].every, &waves[nwaves].speed, (int *)&used) == 2) {
            p += used;
            nwaves++;
        }
    }
    if (!nwaves) {
        waves[0].every = 45;
        waves[0].speed = 256;
        nwaves = 1;
    }
    if (nshvc_save_read("best.bin", &g.save, sizeof g.save) != (int)sizeof g.save) {
        memset(&g.save, 0, sizeof g.save);
        memcpy(g.save.initials, "---", 4);
    }
}

/* ---- the art, made here so the example needs no art files -------------------- */

static void make_font(void)
{
    static uint8_t tile[37][64];
    int c, x, y;
    for (c = 0; c < 36; c++)
        for (y = 0; y < 5; y++)
            for (x = 0; x < 3; x++)
                if (font[c] >> (14 - (y * 3 + x)) & 1) {
                    /* each dot two texels wide, one tall, a line lower */
                    tile[c + 1][(y + 1) * 8 + x * 2 + 1] = 1;
                    tile[c + 1][(y + 1) * 8 + x * 2 + 2] = 1;
                }
    hud_tiles = nshvc_tiles_create(2, 37, 1);
    nshvc_tiles_write(hud_tiles, 0, tile, 37, NSHVC_TILES_PACKED);
    hud_map = nshvc_map_create(32, 32);
}

static void make_stars(void)
{
    static uint8_t tile[4][64];
    static nshvc_map_entry map[32 * 32];
    int i;
    tile[1][3 * 8 + 3] = 1;
    tile[2][5 * 8 + 6] = 2;
    tile[3][2 * 8 + 1] = 3;
    star_tiles = nshvc_tiles_create(4, 4, 1);
    nshvc_tiles_write(star_tiles, 0, tile, 4, NSHVC_TILES_PACKED);
    star_map = nshvc_map_create(32, 32);
    for (i = 0; i < 32 * 32; i++) {
        map[i].tile = (uint16_t)((i * 7 + i / 3) % 4);
        map[i].palette = 1;
    }
    nshvc_map_write(star_map, 0, 0, 32, 32, map);
}

static void make_sprites(void)
{
    static uint8_t tile[5][64];
    int x, y;
    /* the ship: four tiles; a comet: the fifth */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int dx = x * 2 - 15, dy = y * 2 - 15, t = (y / 8) * 2 + x / 8;
            if ((dx < 0 ? -dx : dx) * 2 + (15 - dy) < 32 && dy > -14)
                tile[t][(y % 8) * 8 + x % 8] = (uint8_t)(dy > 8 ? 2 : 1);
        }
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int dx = x * 2 - 7, dy = y * 2 - 7;
            if (dx * dx + dy * dy < 50)
                tile[4][y * 8 + x] = (uint8_t)(dx * dx + dy * dy < 16 ? 3 : 4);
        }
    sprite_tiles = nshvc_tiles_create(4, 5, 1);
    nshvc_tiles_write(sprite_tiles, 0, tile, 5, NSHVC_TILES_PACKED);
}

static void make_ground(void)
{
    static uint8_t tile[2][64];
    static uint16_t map[128 * 128];
    int i;
    memset(tile[0], 32, 64);
    memset(tile[1], 33, 64);
    for (i = 0; i < 8; i++) {
        tile[1][i] = 34;
        tile[1][i * 8] = 34;
    }
    ground_tiles = nshvc_tiles_create(8, 2, 1);
    nshvc_tiles_write(ground_tiles, 0, tile, 2, NSHVC_TILES_MODE7);
    ground_map = nshvc_map_create(128, 128);
    for (i = 0; i < 128 * 128; i++)
        map[i] = (uint16_t)(((i & 1) ^ (i >> 7 & 1)));
    nshvc_map_write_snes(ground_map, 0, 0, 128, 128, map);
}

static void make_images(void)
{
    static uint32_t moon[32 * 32], panel[64 * 16];
    int x, y;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            int v = 150 + ((x * 13 ^ y * 7) & 63);
            moon[y * 32 + x] = 0xFF000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)(v / 2);
        }
    moon_image = nshvc_image_create(32, 32, NSHVC_IMAGE_RGBA);
    nshvc_image_write(moon_image, moon);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 64; x++)
            panel[y * 64 + x] = (y < 2 || y > 13 || x < 2 || x > 61) ? 0xFFFFFFFFu : 0xFF203060u;
    panel_image = nshvc_image_create(64, 16, NSHVC_IMAGE_RGBA);
    nshvc_image_write(panel_image, panel);
    moon_shader = nshvc_shader_load("moon");
    sky = nshvc_draw_list_create();
}

static void make_palette(void)
{
    static uint16_t p[256];
    p[0] = 0x0000;
    p[1] = 0x7FFF;                      /* the text */
    p[16 + 1] = 0x7FFF;
    p[16 + 2] = 0x5294;
    p[16 + 3] = 0x03FF;
    p[32] = 0x0200;
    p[33] = 0x0180;
    p[34] = 0x03E0;
    p[128 + 1] = 0x7E10;                /* the ship */
    p[128 + 2] = 0x001F;
    p[128 + 3] = 0x7FFF;                /* a comet */
    p[128 + 4] = 0x03FF;
    nshvc_palette_write(0, 256, p);
}

static void init(void)
{
    static const nshvc_device one = NSHVC_PAD;
    g.seed = 0x1234567u;
    g.ship_x = g.last_ship_x = (SCREEN_W / 2 - 8) << 8;
    nshvc_players(1, &one);
    load_game_data();
    make_font();
    make_stars();
    make_sprites();
    make_ground();
    make_images();
    make_palette();
    make_samples();
}

/* ---- update ------------------------------------------------------------------- */

static void move_comet(void *context, uint32_t i)
{
    (void)context;
    if (g.comet[i].live)
        g.comet[i].y += g.comet[i].vy;
}

static void start_run(void)
{
    g.mode = PLAY;
    g.score = 0;
    g.misses = 0;
    g.spawn_in = 30;
    g.wave = 0;
    memset(g.comet, 0, sizeof g.comet);
}

static void end_run(void)
{
    g.mode = OVER;
    send_sound(SND_OVER);
    if (g.score > g.save.best) {
        g.entering = 1;
        g.typed = 0;
        g.save.best = g.score;
        memcpy(g.save.initials, "---", 4);
        nshvc_text_input(1);
    }
}

static void enter_initials(void)
{
    char typed[8];
    uint32_t n = nshvc_text_read(typed, sizeof typed), i;
    for (i = 0; i < n && g.typed < 3; i++)
        if (typed[i] >= 'A' && typed[i] <= 'Z')
            g.save.initials[g.typed++] = typed[i];
    if (g.typed == 3) {
        g.entering = 0;
        nshvc_text_input(0);
        nshvc_save_write("best.bin", &g.save, sizeof g.save);
    }
}

static void spawn(void)
{
    uint32_t i;
    const uint32_t w = g.wave / 8 < nwaves ? g.wave / 8 : nwaves - 1;
    if (g.spawn_in-- > 0)
        return;
    g.spawn_in = waves[w].every;
    for (i = 0; i < COMETS; i++)
        if (!g.comet[i].live) {
            g.comet[i].live = 1;
            g.comet[i].x = (int32_t)(8 + rnd(SCREEN_W - 24)) << 8;
            g.comet[i].y = 0;
            g.comet[i].vy = (int32_t)waves[w].speed;
            g.wave++;
            return;
        }
}

static void play(uint16_t pad)
{
    int i;
    g.last_ship_x = g.ship_x;
    if (pad & NSHVC_LEFT)
        g.ship_x -= 3 << 8;
    if (pad & NSHVC_RIGHT)
        g.ship_x += 3 << 8;
    if (g.ship_x < 0)
        g.ship_x = 0;
    if (g.ship_x > (SCREEN_W - 16) << 8)
        g.ship_x = (SCREEN_W - 16) << 8;
    spawn();
    nshvc_jobs(move_comet, NULL, COMETS);
    for (i = 0; i < COMETS; i++) {
        Comet *c = &g.comet[i];
        int y = c->y >> 8, dx = (c->x >> 8) + 4 - ((g.ship_x >> 8) + 8);
        if (!c->live)
            continue;
        if (y >= SHIP_Y - 6 && y <= SHIP_Y + 8 && dx >= -11 && dx <= 11) {
            c->live = 0;
            g.score += 10;
            send_sound(SND_CATCH);
        } else if (y > LINES) {
            c->live = 0;
            send_sound(SND_MISS);
            if (++g.misses >= MISSES) {
                end_run();
                return;
            }
        }
    }
}

static void update(const nshvc_input *in)
{
    uint16_t pad = in->pad[0], pressed = (uint16_t)(pad & ~g.last_pad);
    nshvc_profile_begin("update");
    g.last_pad = pad;
    g.ticks++;
    if (g.mode == PLAY && (pressed & NSHVC_SELECT)) {
        /* the engine stops calling update while paused; draw shows the panel */
        g.paused = 1;
        nshvc_pause(1);
    }
    if (g.mode == TITLE) {
        if (pressed & NSHVC_START)
            start_run();
    } else if (g.mode == PLAY) {
        play(pad);
    } else if (g.entering) {
        enter_initials();
    } else if (pressed & NSHVC_START) {
        g.mode = TITLE;
    }
    nshvc_profile_end();
}

/* ---- draw --------------------------------------------------------------------- */

static void put_text(nshvc_map_entry *row, int x, const char *s)
{
    for (; *s && x < 32; s++, x++) {
        int c = *s;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        row[x].tile = (uint16_t)(c >= '0' && c <= '9' ? c - '0' + 1 : c >= 'A' && c <= 'Z' ? c - 'A' + 11 : 0);
    }
}

static void draw_hud(void)
{
    static nshvc_map_entry top[32], middle[32];
    char text[40];
    memset(top, 0, sizeof top);
    memset(middle, 0, sizeof middle);
    snprintf(text, sizeof text, "SCORE %u", g.score);
    put_text(top, 1, text);
    snprintf(text, sizeof text, "BEST %u %s", g.save.best, g.save.initials);
    put_text(top, 17, text);
    if (g.mode == TITLE) {
        snprintf(text, sizeof text, "COMET CATCH  %.10s TO START", nshvc_control_name(0, NSHVC_START));
        put_text(middle, 2, text);
    } else if (g.entering) {
        put_text(middle, 4, "NEW BEST  TYPE INITIALS");
    } else if (g.mode == OVER) {
        put_text(middle, 11, "GAME OVER");
    }
    nshvc_map_write(hud_map, 0, 1, 32, 1, top);
    nshvc_map_write(hud_map, 0, 4, 32, 1, middle);
}

static void draw_moon(float between)
{
    nshvc_vertex v[18];
    nshvc_material m;
    int i;
    const float cx = 200, cy = 32, r = 22, turn = ((float)g.ticks + between) * 0.01f;
    memset(v, 0, sizeof v);
    memset(&m, 0, sizeof m);
    nshvc_draw_list_clear(sky, SCREEN_W, LINES);
    nshvc_draw_rule(sky, NSHVC_FILL_SMOOTH);
    for (i = 0; i < 6; i++) {
        float a = turn + (float)i * 1.0471976f, b = a + 1.0471976f;
        nshvc_vertex *t = &v[i * 3];
        t[0].x = cx; t[0].y = cy; t[0].u = 16; t[0].v = 16;
        t[1].x = cx + cosf(a) * r; t[1].y = cy + sinf(a) * r;
        t[1].u = 16 + cosf(a) * 15; t[1].v = 16 + sinf(a) * 15;
        t[2].x = cx + cosf(b) * r; t[2].y = cy + sinf(b) * r;
        t[2].u = 16 + cosf(b) * 15; t[2].v = 16 + sinf(b) * 15;
    }
    m.image = moon_image;
    m.shader = moon_shader;
    m.inputs[0] = 0.75f + 0.25f * sinf(((float)g.ticks + between) * 0.05f);
    nshvc_draw_tris(sky, v, 18, &m);
}

/* the sky's gradient by line; the ground's perspective by line */
static void draw_lines(void)
{
    static int32_t fixed[SKY_LINES], scale[LINES - SKY_LINES];
    int i;
    for (i = 0; i < SKY_LINES; i++)
        fixed[i] = (i / 4) << 10 | (i / 8) << 5;
    nshvc_lines(0, SKY_LINES, NSHVC_LINE_FIXED, fixed);
    for (i = 0; i < LINES - SKY_LINES; i++)
        scale[i] = 256 * 48 / (i + 24);
    nshvc_lines(SKY_LINES, LINES - SKY_LINES, NSHVC_LINE_M7_A, scale);
    nshvc_lines(SKY_LINES, LINES - SKY_LINES, NSHVC_LINE_M7_D, scale);
    nshvc_line(SKY_LINES, NSHVC_LINE_MODE, 7);
}

/* a circle closing on the ship when the run has ended */
static void draw_iris(void)
{
    static int32_t left[LINES], right[LINES];
    const int cx = (g.ship_x >> 8) + 8, cy = SHIP_Y + 8;
    const int age = g.mode == OVER ? 200 : 400;
    nshvc_windows w;
    nshvc_blend b;
    int y;
    memset(&w, 0, sizeof w);
    memset(&b, 0, sizeof b);
    for (y = 0; y < LINES; y++) {
        int d = y - cy, sq = age * age - d * d, h = sq > 0 ? (int)sqrtf((float)sq) : -1;
        left[y] = h < 0 ? 1 : cx - h < 0 ? 0 : cx - h;
        right[y] = h < 0 ? 0 : cx + h > 255 ? 255 : cx + h;
    }
    nshvc_lines(0, LINES, NSHVC_LINE_WIN1_LEFT, left);
    nshvc_lines(0, LINES, NSHVC_LINE_WIN1_RIGHT, right);
    w.w[0].left = 0;
    w.w[0].right = 255;
    w.layer_use[5] = NSHVC_WIN1;
    nshvc_windows_set(&w);
    b.layers = 1 << 5;                  /* the backdrop takes the sky's gradient */
    b.use_fixed = 1;
    b.clip = 1;                         /* black outside the circle */
    nshvc_blend_set(&b);
}

static void draw_sprites(float between)
{
    static nshvc_sprite s[COMETS + 1];
    uint32_t n = 0;
    int i;
    memset(s, 0, sizeof s);
    if (g.mode == PLAY) {
        const float x = (float)g.last_ship_x + (float)(g.ship_x - g.last_ship_x) * between;
        s[n].x = (int32_t)x >> 8;
        s[n].y = SHIP_Y;
        s[n].width = s[n].height = 2;
        s[n].tiles = sprite_tiles;
        s[n].priority = 3;
        n++;
        for (i = 0; i < COMETS; i++)
            if (g.comet[i].live) {
                s[n].x = g.comet[i].x >> 8;
                s[n].y = g.comet[i].y >> 8;
                s[n].width = s[n].height = 1;
                s[n].tiles = sprite_tiles;
                s[n].tile = 4;
                s[n].priority = 3;
                n++;
            }
    }
    nshvc_sprites_set(s, n, 0);
}

static void draw_panel(void)
{
    nshvc_display d = nshvc_display_info();
    const float w = (float)d.width, h = (float)d.height;
    float corners[6];
    corners[0] = w * 0.3f; corners[1] = h * 0.45f;
    corners[2] = w * 0.7f; corners[3] = h * 0.45f;
    corners[4] = w * 0.3f; corners[5] = h * 0.55f;
    nshvc_overlay_set(0, panel_image, corners, g.paused ? 0.9f : 0.0f);
}

static void draw(float between)
{
    nshvc_screen scr;
    nshvc_mode7 m7;
    nshvc_layer l;

    nshvc_profile_begin("draw");
    pictures++;
    memset(&scr, 0, sizeof scr);
    scr.brightness = g.ticks < 15 ? (uint8_t)g.ticks : 15;
    nshvc_screen_set(&scr);
    nshvc_mode_set(1, 1, 0);

    draw_moon(between);
    memset(&l, 0, sizeof l);
    l.enabled_main = 1;
    l.mosaic = 1;
    l.draw_list = sky;
    nshvc_layer_set(0, &l);

    memset(&l, 0, sizeof l);
    l.enabled_main = 1;
    l.tiles = star_tiles;
    l.map = star_map;
    l.tile_size = 8;
    l.wrap = 1;
    l.mosaic = g.mode == OVER ? 4 : 1;
    l.scroll_x = (int32_t)g.ticks / 8;
    nshvc_layer_set(1, &l);

    draw_hud();
    memset(&l, 0, sizeof l);
    l.enabled_main = 1;
    l.tiles = hud_tiles;
    l.map = hud_map;
    l.tile_size = 8;
    l.wrap = 1;
    l.mosaic = 1;
    nshvc_layer_set(2, &l);

    memset(&l, 0, sizeof l);
    nshvc_layer_set(3, &l);

    memset(&m7, 0, sizeof m7);
    m7.a = m7.d = 256;
    m7.centre_x = 128;
    m7.centre_y = 0;
    m7.scroll_x = 0;
    m7.scroll_y = (int16_t)(g.mode == PLAY ? -(int32_t)g.ticks * 2 : 0);
    m7.tiles = ground_tiles;
    m7.map = ground_map;
    nshvc_mode7_set(&m7);

    draw_lines();
    draw_iris();
    draw_sprites(between);
    draw_panel();
    nshvc_profile_end();
}

/* ---- the end ------------------------------------------------------------------ */

static void shutdown(void)
{
    uint64_t hash = 14695981039346656037ull;
    const unsigned char *p = (const unsigned char *)&g;
    size_t i;
    nshvc_profile f = nshvc_profile_frame();
    for (i = 0; i < sizeof g; i++)
        hash = (hash ^ p[i]) * 1099511628211ull;
    printf("updates %" PRIu64 " pictures %" PRIu64 " score %u state %016" PRIx64 "\n",
           nshvc_updates(), pictures, g.score, hash);
    printf("engine %.4f ms, render %.4f ms, audio %.4f ms, %u cores\n",
           f.engine_ms, f.render_ms, f.audio_ms, nshvc_cores());
}

int main(int argc, char **argv)
{
    static nshvc_game game;
    game.title = "Comet Catch";
    game.id = "comet-catch";
    game.update_hz = 60;
    game.init = init;
    game.update = update;
    game.draw = draw;
    game.shutdown = shutdown;
    game.music.hz = 60;
    game.music.tick = music_tick;
    game.music.queue_bytes = 64;
    return nshvc_run(&game, argc, argv);
}
