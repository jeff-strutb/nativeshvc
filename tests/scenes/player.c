/* player.c: plays a scene file (scenes.py) through the engine's API.
 * Run headless with the picture at the console's own size so that a window
 * pixel is a picture pixel: RR_SIZE=WxH (the oracle's shot's size) and
 * SCENE_ASPECT=-1 (NSHVC_ASPECT_FILL: the screen fills the window). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define MAX 32
static nshvc_tiles tiles[MAX];
static nshvc_map maps[MAX];
static char *lines_of[4096];
static int nlines;
static const char *scene_path;

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

static void init(void)
{
    FILE *f = fopen(scene_path, "r");
    size_t cap = 1 << 26;
    char *buf = (char *)malloc(cap);
    if (!f || !buf)
        exit(2);
    while (fgets(buf, (int)cap, f)) {
        char *a[8], *c = strtok(buf, " \n");
        int k = 0;
        size_t n;
        if (!c)
            continue;
        if (!strcmp(c, "tiles") || !strcmp(c, "tiles7")) {
            while (k < 4)
                a[k++] = strtok(NULL, " \n");
            {
                int id = atoi(a[0]), count = atoi(a[2]);
                uint8_t *d = hex(a[3], &n);
                tiles[id] = nshvc_tiles_create((uint8_t)atoi(a[1]), (uint32_t)count, 1);
                nshvc_tiles_write(tiles[id], 0, d, (uint32_t)count, c[5] ? NSHVC_TILES_MODE7 : NSHVC_TILES_PLANAR);
                free(d);
            }
        } else if (!strcmp(c, "map")) {
            while (k < 4)
                a[k++] = strtok(NULL, " \n");
            {
                int id = atoi(a[0]);
                uint8_t *d = hex(a[3], &n);
                maps[id] = nshvc_map_create((uint32_t)atoi(a[1]), (uint32_t)atoi(a[2]));
                nshvc_map_write_snes(maps[id], 0, 0, (uint32_t)atoi(a[1]), (uint32_t)atoi(a[2]), (const uint16_t *)(void *)d);
                free(d);
            }
        } else if (!strcmp(c, "pal")) {
            uint8_t *d = hex(strtok(NULL, " \n"), &n);
            nshvc_palette_write(0, 256, (const uint16_t *)(void *)d);
            free(d);
        } else {
            size_t l = strlen(c);
            char *rest = strtok(NULL, "\n"), *line = (char *)malloc(l + 2 + (rest ? strlen(rest) : 0));
            sprintf(line, "%s %s", c, rest ? rest : "");
            lines_of[nlines++] = line;
        }
    }
    fclose(f);
    free(buf);
}

static double sum_engine, sum_render;
static int profiled;

static void update(const nshvc_input *in)
{
    (void)in;
    if (nshvc_updates()) {
        nshvc_profile r = nshvc_profile_frame();
        sum_engine += r.engine_ms;
        sum_render += r.render_ms;
        profiled++;
    }
}

static void finish(void)
{
    if (getenv("NSHVC_SHOW_PROFILE") && profiled)
        printf("engine %.1f us, render %.1f us a picture over %d pictures\n", sum_engine * 1000 / profiled, sum_render * 1000 / profiled, profiled);
}

static int ints(const char *s, int *v, int max)
{
    int n = 0;
    char *e;
    while (n < max) {
        long x = strtol(s, &e, 10);
        if (e == s)
            break;
        v[n++] = (int)x;
        s = e;
    }
    return n;
}

static void draw(float between)
{
    static nshvc_sprite spr[256];
    int nspr = 0, i;
    nshvc_windows win;
    (void)between;
    memset(&win, 0, sizeof win);
    for (i = 0; i < nlines; i++) {
        char *sp = strchr(lines_of[i], ' ');
        int v[512];
        char cmd[16];
        memset(v, 0, sizeof v);
        size_t l = (size_t)(sp - lines_of[i]);
        memcpy(cmd, lines_of[i], l);
        cmd[l] = 0;
        ints(sp + 1, v, 512);
        if (!strcmp(cmd, "screen")) {
            nshvc_screen s;
            memset(&s, 0, sizeof s);
            if (getenv("SCENE_ASPECT"))
                s.aspect = (float)atof(getenv("SCENE_ASPECT"));
            s.brightness = (uint8_t)v[0];
            s.interlace = (uint8_t)v[1];
            s.blank = (uint8_t)v[2];
            s.overscan = (uint8_t)v[3];
            if (s.overscan)
                s.height = 239;         /* the height is the game's to set: overscan lets the console's 239 lines be shown */
            nshvc_screen_set(&s);
        } else if (!strcmp(cmd, "mode")) {
            nshvc_mode_set((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
        } else if (!strcmp(cmd, "layer")) {
            nshvc_layer a;
            memset(&a, 0, sizeof a);
            a.tiles = tiles[v[1]];
            a.map = maps[v[2]];
            a.enabled_main = (uint8_t)v[3];
            a.enabled_sub = (uint8_t)v[4];
            a.tile_size = (uint8_t)v[5];
            a.scroll_x = v[6];
            a.scroll_y = v[7];
            a.mosaic = (uint8_t)v[8];
            a.wrap = (uint8_t)v[9];
            a.direct_colour = (uint8_t)v[10];
            if (v[11] >= 0)
                a.offset_per_tile = maps[v[11]];
            nshvc_layer_set((uint8_t)v[0], &a);
        } else if (!strcmp(cmd, "win")) {
            int k;
            win.w[0].left = v[0]; win.w[0].right = v[1]; win.w[1].left = v[2]; win.w[1].right = v[3];
            for (k = 0; k < 6; k++)
                win.layer_use[k] = (uint8_t)v[4 + k];
            for (k = 0; k < 5; k++) {
                win.mask_main[k] = (uint8_t)v[10 + k];
                win.mask_sub[k] = (uint8_t)v[15 + k];
            }
            nshvc_windows_set(&win);
        } else if (!strcmp(cmd, "blend")) {
            nshvc_blend b;
            b.subtract = (uint8_t)v[0];
            b.half = (uint8_t)v[1];
            b.use_fixed = (uint8_t)v[2];
            b.layers = (uint8_t)v[3];
            b.clip = (uint8_t)v[4];
            b.prevent = (uint8_t)v[5];
            b.fixed_bgr15 = (uint16_t)v[6];
            nshvc_blend_set(&b);
        } else if (!strcmp(cmd, "m7")) {
            nshvc_mode7 m;
            memset(&m, 0, sizeof m);
            m.a = (int16_t)v[0]; m.b = (int16_t)v[1]; m.c = (int16_t)v[2]; m.d = (int16_t)v[3];
            m.centre_x = (int16_t)v[4]; m.centre_y = (int16_t)v[5]; m.scroll_x = (int16_t)v[6]; m.scroll_y = (int16_t)v[7];
            m.outside = (uint8_t)v[8]; m.flip_x = (uint8_t)v[9]; m.flip_y = (uint8_t)v[10]; m.extbg = (uint8_t)v[11];
            m.tiles = tiles[v[12]];
            m.map = maps[v[13]];
            nshvc_mode7_set(&m);
        } else if (!strcmp(cmd, "sprite")) {
            nshvc_sprite *s = &spr[nspr++];
            memset(s, 0, sizeof *s);
            s->x = v[0]; s->y = v[1];
            s->width = (uint16_t)v[2]; s->height = (uint16_t)v[3];
            s->tiles = tiles[v[4]];
            s->tile = (uint16_t)v[5];
            s->palette = (uint8_t)v[6]; s->priority = (uint8_t)v[7]; s->hflip = (uint8_t)v[8]; s->vflip = (uint8_t)v[9];
        } else if (!strcmp(cmd, "spriteshow")) {
            nshvc_sprites_show((uint8_t)v[0], (uint8_t)v[1]);
        } else if (!strcmp(cmd, "limits")) {
            nshvc_limits lim;
            memset(&lim, 0, sizeof lim);
            lim.sprites_per_line = (uint16_t)v[0];
            lim.sprite_tiles_per_line = (uint16_t)v[1];
            lim.sprite_wrap = (uint8_t)v[2];
            memset(lim.layer_clip, 0, 5);
            nshvc_limits_set(&lim);
        } else if (!strcmp(cmd, "lines")) {
            nshvc_lines((uint32_t)v[1], (uint32_t)v[2], (nshvc_line_field)v[0], v + 3);
        }
    }
    nshvc_sprites_set(spr, (uint32_t)nspr, 0);
}

int main(int argc, char **argv)
{
    static nshvc_game game;
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--scene"))
            scene_path = argv[i + 1];
    if (!scene_path)
        return 2;
    game.title = "Scene";
    game.id = "scene";
    game.update_hz = 60;
    game.init = init;
    game.update = update;
    game.draw = draw;
    game.shutdown = finish;
    return nshvc_run(&game, argc, argv);
}
