/*
 * script.c: a headless run's script (script.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "snes_internal.h"
#include "script.h"

typedef struct {
    int kind;               /* 0 buttons for frames; 1 shot; 2 dump */
    uint16_t buttons[2];
    long frames;
    char name[128];
} Step;

static Step *steps;
static int nsteps, step_at;
static long step_left;
static const char *out = ".";

int script_steps(void) { return nsteps; }

static void add_step(int kind, uint16_t b0, uint16_t b1, long frames, const char *name)
{
    Step *s;
    steps = (Step *)realloc(steps, sizeof *steps * (size_t)(nsteps + 1));
    if (!steps)
        nshvc_fatal("no memory for the script");
    s = &steps[nsteps++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->buttons[0] = b0;
    s->buttons[1] = b1;
    s->frames = frames;
    if (name)
        snprintf(s->name, sizeof s->name, "%s", name);
}

static int parse_buttons(const char *t, uint16_t *b0, uint16_t *b1)
{
    static const struct { const char *n; uint16_t m; } k[] = {
        { "B", NSHVC_B }, { "Y", NSHVC_Y }, { "SELECT", NSHVC_SELECT }, { "START", NSHVC_START },
        { "UP", NSHVC_UP }, { "DOWN", NSHVC_DOWN }, { "LEFT", NSHVC_LEFT }, { "RIGHT", NSHVC_RIGHT },
        { "A", NSHVC_A }, { "X", NSHVC_X }, { "L", NSHVC_L }, { "R", NSHVC_R },
    };
    char buf[256], *p, *next;
    *b0 = *b1 = 0;
    snprintf(buf, sizeof buf, "%s", t);
    for (p = buf; p; p = next) {
        int pad = 0;
        size_t i;
        next = strchr(p, '+');
        if (next)
            *next++ = 0;
        if (!strncmp(p, "2.", 2)) {
            pad = 1;
            p += 2;
        }
        for (i = 0; i < sizeof k / sizeof k[0]; i++)
            if (!strcmp(p, k[i].n))
                break;
        if (i == sizeof k / sizeof k[0])
            return 0;
        if (pad)
            *b1 |= k[i].m;
        else
            *b0 |= k[i].m;
    }
    return 1;
}

/* lines [lo, hi) of the script, expanded */
static void script_range(char **lines, int lo, int hi, const char *path)
{
    int i;
    for (i = lo; i < hi; i++) {
        char cmd[64] = "", a[256] = "", b[64] = "";
        uint16_t b0, b1;
        if (sscanf(lines[i], "%63s %255s %63s", cmd, a, b) < 1)
            continue;
        if (!strcmp(cmd, "wait")) {
            add_step(0, 0, 0, atol(a), NULL);
        } else if (!strcmp(cmd, "hold") || !strcmp(cmd, "press")) {
            if (!parse_buttons(a, &b0, &b1))
                nshvc_fatal("%s:%d: buttons '%s'", path, i + 1, a);
            add_step(0, b0, b1, b[0] ? atol(b) : (cmd[0] == 'p' ? 2 : 1), NULL);
            if (cmd[0] == 'p')
                add_step(0, 0, 0, 2, NULL);
        } else if (!strcmp(cmd, "shot")) {
            add_step(1, 0, 0, 0, a);
        } else if (!strcmp(cmd, "dump")) {
            add_step(2, 0, 0, 0, a);
        } else if (!strcmp(cmd, "repeat")) {
            int depth = 0, j, n = atoi(a);
            for (j = i + 1; j < hi; j++) {
                char c2[64] = "";
                sscanf(lines[j], "%63s", c2);
                if (!strcmp(c2, "repeat"))
                    depth++;
                else if (!strcmp(c2, "end") && depth-- == 0)
                    break;
            }
            while (n-- > 0)
                script_range(lines, i + 1, j, path);
            i = j;
        } else {
            nshvc_fatal("%s:%d: '%s' is not a step nativeshvc's script runner takes", path, i + 1, cmd);
        }
    }
}

void script_load(const char *path, const char *out_dir)
{
    FILE *f = fopen(path, "r");
    char line[512], **lines = NULL;
    int n = 0;
    out = out_dir;
    if (!f)
        nshvc_fatal("cannot read %s", path);
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        lines = (char **)realloc(lines, sizeof *lines * (size_t)(n + 1));
        lines[n++] = strdup(line);
    }
    fclose(f);
    script_range(lines, 0, n, path);
}

static void write_file(const char *name, const char *ext, const void *p, size_t n)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s%s", out, name, ext);
    f = fopen(path, "wb");
    if (!f)
        nshvc_fatal("cannot write %s", path);
    if (n)
        fwrite(p, 1, n, f);
    fclose(f);
}

static void shot(const char *name)
{
    int w, h, i;
    const uint32_t *px = nshvc_picture(&w, &h);
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", out, name);
    f = fopen(path, "wb");
    if (!f)
        nshvc_fatal("cannot write %s", path);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

void script_takes(void)
{
    while (step_at < nsteps && steps[step_at].kind) {
        Step *s = &steps[step_at];
        if (s->kind == 1) {
            shot(s->name);
        } else {
            uint32_t n = snes_cart.map == NSHVC_SUPERFX ? gsu_ram_size : snes_cart.sram_size;
            write_file(s->name, ".wram", snes_wram(), 0x20000);
            write_file(s->name, ".sram", snes_sram(), snes_sram() ? n : 0);
            write_file(s->name, ".vram", nshvc_vram(), 0x10000 * PPU_VRAM_PAGES);
            write_file(s->name, ".cgram", nshvc_cgram(), 0x200);
            write_file(s->name, ".oam", nshvc_oam(), 544);
        }
        step_at++;
    }
}

int script_frame(uint16_t *b0, uint16_t *b1)
{
    while (step_at < nsteps) {
        Step *s = &steps[step_at];
        if (s->kind) {
            script_takes();
            continue;
        }
        if (!step_left)
            step_left = s->frames;
        if (step_left <= 0) {
            step_at++;
            step_left = 0;
            continue;
        }
        *b0 = s->buttons[0];
        *b1 = s->buttons[1];
        if (--step_left == 0)
            step_at++;
        return 1;
    }
    return 0;
}
