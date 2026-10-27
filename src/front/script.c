/*
 * script.c: a headless run's script (script.h).
 *
 * A machine is never copied or put back: the C under way on the S-CPU's
 * and the SPC700's coroutines is part of it. Where the script needs the
 * machine as it was (a load; a call, after which the game goes on as if it
 * had not been made), the run forks: the process that holds the machine as
 * it was waits while a child runs on, and goes on itself when the child is
 * done with it.
 *
 *   root      the machine at power-on: a child runs the script; a child
 *             that loads a state no process holds comes back here, and a
 *             new child replays the steps that save it to that save: the
 *             script's own up to its `save NAME` when one comes before any
 *             load, else the play in NSHVC_STATES (*.sfx) with `save NAME`
 *   save      the process becomes the holder of NAME: a child goes on with
 *             the script; one that loads NAME ends, and a new child goes on
 *             from the step after that load
 *   call      (without keep) a child runs the call; when it returns the
 *             child ends at that frame's end and the parent goes on
 *
 * A child's end that is not one of these (the script's end, an error) ends
 * every process above it with the same status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "snes_internal.h"
#include "script.h"
#include "ppu.h"

enum {
    S_FRAMES,               /* buttons for frames */
    S_CALL,                 /* a unit call, over the frames it runs */
    S_SHOT, S_DUMP, S_SAVE, S_LOAD, S_POKE, S_SET, S_SPCSET, S_SPCCALL,
    S_INPUT,                /* plug, pad, mouse, gun: the controller ports from now */
};

typedef struct {
    int kind;
    uint16_t buttons[2];
    long frames;
    char name[128];
    char *text;             /* the line (poke, set, call, spcset, spccall) */
    int line;
} Step;

typedef struct {
    Step *s;
    int n;
    const char *path;
} List;

static List main_list, replay_list, *cur = &main_list;
static int step_at;
static long step_left;
static const char *out = ".";

/* the exit statuses between the processes */
#define EXIT_WANT 75        /* a load of a state this process does not hold */
#define EXIT_CALLED 76      /* a call without keep returned */

/* what a child that ends with EXIT_WANT wants: the state, and the step of
   the script after its load */
typedef struct {
    char name[128];
    int resume;
    uint32_t frames;        /* the script's frames run, every process's in turn */
} Shared;
static Shared *shared;

int script_steps(void) { return main_list.n; }

static Step *add_step(List *l, int kind, uint16_t b0, uint16_t b1, long frames, const char *name)
{
    Step *s;
    l->s = (Step *)realloc(l->s, sizeof *l->s * (size_t)(l->n + 1));
    if (!l->s)
        nshvc_fatal("no memory for the script");
    s = &l->s[l->n++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->buttons[0] = b0;
    s->buttons[1] = b1;
    s->frames = frames;
    if (name)
        snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

static Step *add_text(List *l, int kind, const char *text, int line)
{
    Step *s = add_step(l, kind, 0, 0, 0, NULL);
    s->text = strdup(text);
    s->line = line;
    if (!s->text)
        nshvc_fatal("no memory for the script");
    return s;
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

/* the words of a line (at most `max`, each up to 1023 characters) */
static int words(const char *line, char w[][1024], int max)
{
    int n = 0;
    while (n < max) {
        int k = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
            line++;
        if (!*line)
            break;
        while (*line && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n' && k < 1023)
            w[n][k++] = *line++;
        w[n++][k] = 0;
    }
    return n;
}

/* an address as the scripts write one: hex, '$' and ':' allowed */
static uint32_t parse_addr(const char *t)
{
    char ab[64];
    int j = 0;
    for (; *t && j < 63; t++)
        if (*t != '$' && *t != ':')
            ab[j++] = *t;
    ab[j] = 0;
    return (uint32_t)strtoul(ab, NULL, 16);
}

/* lines [lo, hi) of the script, expanded */
static void script_range(List *l, char **lines, int lo, int hi, const char *path)
{
    int i;
    for (i = lo; i < hi; i++) {
        char cmd[64] = "", a[256] = "", b[64] = "";
        uint16_t b0, b1;
        int n = sscanf(lines[i], "%63s %255s %63s", cmd, a, b);
        if (n < 1)
            continue;
        if (!strcmp(cmd, "wait")) {
            add_step(l, S_FRAMES, 0, 0, atol(a), NULL);
        } else if (!strcmp(cmd, "hold") || !strcmp(cmd, "press")) {
            if (!parse_buttons(a, &b0, &b1))
                nshvc_fatal("%s:%d: buttons '%s'", path, i + 1, a);
            add_step(l, S_FRAMES, b0, b1, b[0] ? atol(b) : (cmd[0] == 'p' ? 2 : 1), NULL);
            if (cmd[0] == 'p')
                add_step(l, S_FRAMES, 0, 0, 2, NULL);
        } else if (!strcmp(cmd, "shot") || !strcmp(cmd, "dump") || !strcmp(cmd, "save") || !strcmp(cmd, "load")) {
            if (n < 2)
                nshvc_fatal("%s:%d: %s NAME", path, i + 1, cmd);
            add_step(l, cmd[0] == 's' ? (cmd[1] == 'h' ? S_SHOT : S_SAVE) : cmd[0] == 'd' ? S_DUMP : S_LOAD, 0, 0, 0,
                     a)->line = i + 1;
        } else if (!strcmp(cmd, "poke") || !strcmp(cmd, "set") || !strcmp(cmd, "spcset")) {
            if (n < 3)
                nshvc_fatal("%s:%d: %s ADDR BYTE[,BYTE...]", path, i + 1, cmd);
            add_text(l, cmd[0] == 'p' ? S_POKE : cmd[1] == 'e' ? S_SET : S_SPCSET, lines[i], i + 1);
        } else if (!strcmp(cmd, "plug") || !strcmp(cmd, "pad") || !strcmp(cmd, "mouse") || !strcmp(cmd, "gun")) {
            add_text(l, S_INPUT, lines[i], i + 1);
        } else if (!strcmp(cmd, "call") || !strcmp(cmd, "spccall")) {
            if (n < 2)
                nshvc_fatal("%s:%d: %s ADDR [options]", path, i + 1, cmd);
            add_text(l, cmd[0] == 'c' ? S_CALL : S_SPCCALL, lines[i], i + 1);
        } else if (!strcmp(cmd, "repeat")) {
            int depth = 0, j, k = atoi(a);
            for (j = i + 1; j < hi; j++) {
                char c2[64] = "";
                sscanf(lines[j], "%63s", c2);
                if (!strcmp(c2, "repeat"))
                    depth++;
                else if (!strcmp(c2, "end") && depth-- == 0)
                    break;
            }
            if (j == hi)
                nshvc_fatal("%s:%d: repeat without end", path, i + 1);
            while (k-- > 0)
                script_range(l, lines, i + 1, j, path);
            i = j;
        } else if (!strcmp(cmd, "end")) {
            nshvc_fatal("%s:%d: end without repeat", path, i + 1);
        } else {
            nshvc_fatal("%s:%d: '%s' is not a step nativeshvc's script runner takes", path, i + 1, cmd);
        }
    }
}

static void list_load(List *l, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096], **lines = NULL;
    int n = 0;
    if (!f)
        nshvc_fatal("cannot read %s", path);
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        lines = (char **)realloc(lines, sizeof *lines * (size_t)(n + 1));
        if (!lines || !(lines[n++] = strdup(line)))
            nshvc_fatal("no memory for the script");
    }
    fclose(f);
    l->path = strdup(path);
    script_range(l, lines, 0, n, path);
}

void script_load(const char *path, const char *out_dir)
{
    out = out_dir;
    list_load(&main_list, path);
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

/* A shot is the picture made the way the reference's are (tests/oracle):
   an interlaced picture whose two fields' rows are the same is its lines
   once, a hires one of the console's width whose pixel pairs are all the
   same 256 wide. */
static void shot(const char *name)
{
    int w, h, x, y, xs = 1, ys = 1;
    const uint32_t *px = nshvc_picture(&w, &h);
    const PpuFrame *fr = ppu_frame();
    char path[1024];
    FILE *f;
    if (fr->interlace && h == 2 * fr->screen_height) {
        ys = 2;
        for (y = 0; y + 1 < h && ys == 2; y += 2)
            if (memcmp(px + (size_t)y * (size_t)w, px + (size_t)(y + 1) * (size_t)w, (size_t)w * sizeof *px))
                ys = 1;
    }
    if (fr->width == PPU_WIDTH && w == PPU_HIRES_WIDTH) {
        xs = 2;
        for (y = 0; y < h && xs == 2; y++)
            for (x = 0; x < w; x += 2)
                if (px[y * w + x] != px[y * w + x + 1]) {
                    xs = 1;
                    break;
                }
    }
    snprintf(path, sizeof path, "%s/%s.ppm", out, name);
    f = fopen(path, "wb");
    if (!f)
        nshvc_fatal("cannot write %s", path);
    fprintf(f, "P6\n%d %d\n255\n", w / xs, h / ys);
    for (y = 0; y < h / ys; y++)
        for (x = 0; x < w / xs; x++) {
            uint32_t c = px[(y * ys) * w + x * xs];
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

/* the cartridge's chip's own RAM (nshvc_chip.ram), or NULL */
static uint8_t *chip_ram(uint32_t *n)
{
    *n = 0;
    if (!snes_cart.chip || !snes_cart.chip->ram)
        return NULL;
    return snes_cart.chip->ram(n);
}

static void dump(const char *name)
{
    uint32_t n = snes_sram_size(), g;
    uint8_t *gr = chip_ram(&g);
    write_file(name, ".wram", snes_wram(), 0x20000);
    write_file(name, ".sram", nshvc_sram(), nshvc_sram() ? n : 0);
    if (gr)
        write_file(name, ".chipram", gr, g);
    write_file(name, ".vram", nshvc_vram(), 0x10000 * PPU_VRAM_PAGES);
    write_file(name, ".cgram", nshvc_cgram(), 0x200);
    write_file(name, ".oam", nshvc_oam(), 544);
}

/* poke/set ADDR BYTE[,BYTE...] (the bytes split by commas or spaces): from
   S-CPU address ADDR into work RAM ($7E-$7F, or the low 8 KB mirror in
   banks $00-$3F), as the reference's harness writes them */
/* plug PORT DEVICE (joypad, none, mouse, multitap, superscope, justifier,
   justifiers); pad N BITS (joypad N's buttons from now, hex; 0 and 1 are
   hold's); mouse PORT DX DY BUTTONS (motion for its next report); gun G X
   Y BUTTONS (from now) */
static void input_step(const Step *s)
{
    static const char *const dev[] = { "joypad", "none", "mouse", "multitap", "superscope", "justifier",
                                       "justifiers" };
    char w[8][1024];
    int n = words(s->text, w, 8), i;
    if (!strcmp(w[0], "plug") && n == 3) {
        for (i = 0; i < 7; i++)
            if (!strcmp(w[2], dev[i])) {
                nshvc_plug(atoi(w[1]), i);
                return;
            }
    } else if (!strcmp(w[0], "pad") && n == 3) {
        nshvc_set_joypad(atoi(w[1]), (uint16_t)strtoul(w[2], NULL, 16));
        return;
    } else if (!strcmp(w[0], "mouse") && n == 5) {
        nshvc_set_mouse(atoi(w[1]), atoi(w[2]), atoi(w[3]), atoi(w[4]));
        return;
    } else if (!strcmp(w[0], "gun") && n == 5) {
        nshvc_set_gun(atoi(w[1]), atoi(w[2]), atoi(w[3]), (unsigned)atoi(w[4]));
        return;
    }
    nshvc_fatal("%s:%d: '%s'", cur->path, s->line, s->text);
}

static void poke(const Step *s)
{
    char w[64][1024];
    int n = words(s->text, w, 64), i;
    uint32_t a = parse_addr(w[1]), bank = a >> 16 & 0xFF, off = a & 0xFFFF, size, at;
    uint8_t *p;
    const char *path = cur->path;
    if (bank <= 0x3F && off < 0x2000) {
        p = snes_wram_rw();
        size = 0x20000;
        at = off;
    } else if (bank == 0x7E || bank == 0x7F) {
        p = snes_wram_rw();
        size = 0x20000;
        at = (bank - 0x7E) << 16 | off;
    } else {
        nshvc_fatal("%s:%d: %s: $%06X is not work RAM", path, s->line, w[0], (unsigned)a);
        return;
    }
    for (i = 2; i < n; i++) {
        char *t = w[i], *e;
        while (*t) {
            unsigned long v = strtoul(t, &e, 16);
            if (e == t || v > 0xFF || (*e && *e != ','))
                nshvc_fatal("%s:%d: %s: bad byte list", path, s->line, w[0]);
            if (at >= size)
                nshvc_fatal("%s:%d: %s: past the end of the RAM", path, s->line, w[0]);
            p[at++] = (uint8_t)v;
            t = *e ? e + 1 : e;
        }
    }
}

/* set: staged for the next call, written as it starts */
static const Step *staged[64];
static int nstaged;

static void apply_staged(void)
{
    int i;
    for (i = 0; i < nstaged; i++)
        poke(staged[i]);
}

static void spcset(const Step *s)
{
    char w[64][1024];
    int n = words(s->text, w, 64), i;
    unsigned long a = strtoul(w[1][0] == '$' ? w[1] + 1 : w[1], NULL, 16);
    for (i = 2; i < n; i++) {
        char *t = w[i], *e;
        while (*t) {
            unsigned long v = strtoul(t, &e, 16);
            if (e == t || v > 0xFF || (*e && *e != ',') || a > 0xFFFF)
                nshvc_fatal("%s:%d: spcset: bad byte list", cur->path, s->line);
            snes_spc_unit_set((uint16_t)a++, (uint8_t)v);
            t = *e ? e + 1 : e;
        }
    }
}

/* spccall APU [a= x= y= psw= at=] [noreturn] */
static void spccall(const Step *s)
{
    char w[16][1024];
    int n = words(s->text, w, 16), i, has[4] = { 0, 0, 0, 0 };
    uint8_t reg[4] = { 0, 0, 0, 0 };
    unsigned long target = strtoul(w[1][0] == '$' ? w[1] + 1 : w[1], NULL, 16), at = 0x0897;
    if (snes_spc_unit_busy())
        nshvc_fatal("%s:%d: spccall: the previous one has not run yet", cur->path, s->line);
    for (i = 2; i < n; i++) {
        char *eq = strchr(w[i], '=');
        unsigned long v = eq ? strtoul(eq + 1, NULL, 16) : 0;
        if (!strcmp(w[i], "noreturn"))
            continue;
        if (!eq)
            nshvc_fatal("%s:%d: spccall: bad option %s", cur->path, s->line, w[i]);
        if (!strncmp(w[i], "a=", 2)) { has[0] = 1; reg[0] = (uint8_t)v; }
        else if (!strncmp(w[i], "x=", 2)) { has[1] = 1; reg[1] = (uint8_t)v; }
        else if (!strncmp(w[i], "y=", 2)) { has[2] = 1; reg[2] = (uint8_t)v; }
        else if (!strncmp(w[i], "psw=", 4)) { has[3] = 1; reg[3] = (uint8_t)v; }
        else if (!strncmp(w[i], "at=", 3)) at = v;
        else nshvc_fatal("%s:%d: spccall: bad option %s", cur->path, s->line, w[i]);
    }
    snes_spc_unit_call((uint16_t)target, (uint16_t)at, has, reg);
}

/* ---- the processes ---- */
#ifndef _WIN32
static pid_t fork_run(void)
{
    pid_t p;
    fflush(NULL);
    p = fork();
    if (p < 0)
        nshvc_fatal("fork: %s", strerror(errno));
    return p;
}

static int child_status(pid_t p)
{
    int st;
    while (waitpid(p, &st, 0) < 0)
        if (errno != EINTR)
            nshvc_fatal("waitpid: %s", strerror(errno));
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}
#endif

/* the main script from step `at` */
static void begin_main(int at)
{
    snes_cyclog_quiet = 0;
    cur = &main_list;
    step_at = at;
    step_left = 0;
}

static char replay_name[128];
static int replay_resume;

/* the play that saves state `name` (a .sfx in NSHVC_STATES with `save
   NAME`), from power-on to that save, then the main script from `resume` */
static void begin_replay(const char *name, int resume)
{
#ifndef _WIN32
    const char *dir = getenv("NSHVC_STATES");
    char found[2048] = "", path[2048], line[512];
    DIR *d;
    struct dirent *e;
    int i;
    /* a state the script saves itself, before any load: its own steps to
       that save */
    for (i = 0; i < main_list.n && main_list.s[i].kind != S_LOAD; i++)
        if (main_list.s[i].kind == S_SAVE && !strcmp(main_list.s[i].name, name)) {
            replay_list = main_list;
            snprintf(replay_name, sizeof replay_name, "%s", name);
            replay_resume = resume;
            cur = &replay_list;
            step_at = 0;
            step_left = 0;
            snes_cyclog_quiet = 1;
            return;
        }
    if (!dir)
        nshvc_fatal("load %s: set NSHVC_STATES to the folder of the plays that save states", name);
    if (!(d = opendir(dir)))
        nshvc_fatal("NSHVC_STATES: cannot read %s", dir);
    while ((e = readdir(d)) && !found[0]) {
        size_t k = strlen(e->d_name);
        FILE *f;
        if (k < 4 || strcmp(e->d_name + k - 4, ".sfx"))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!(f = fopen(path, "r")))
            continue;
        while (fgets(line, sizeof line, f)) {
            char c[64] = "", a[256] = "";
            if (sscanf(line, "%63s %255s", c, a) == 2 && !strcmp(c, "save") && !strcmp(a, name)) {
                snprintf(found, sizeof found, "%s", path);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    if (!found[0])
        nshvc_fatal("load %s: no play in %s saves it", name, dir);
    memset(&replay_list, 0, sizeof replay_list);
    list_load(&replay_list, found);
    snprintf(replay_name, sizeof replay_name, "%s", name);
    replay_resume = resume;
    cur = &replay_list;
    step_at = 0;
    step_left = 0;
    snes_cyclog_quiet = 1;
#else
    (void)name;
    (void)resume;
#endif
}

/* This process holds the machine as state `have` ("" power-on): children
   run on from it, from step `at` of the main script (or, at power-on, the
   play that saves a state wanted); it returns in each child, and itself
   only ends. */
static void hold(const char *have, int at)
{
#ifndef _WIN32
    char replay[128] = "";
    for (;;) {
        int st;
        pid_t p = fork_run();
        if (p == 0) {
            if (replay[0])
                begin_replay(replay, at);
            else
                begin_main(at);
            return;
        }
        st = child_status(p);
        if (st != EXIT_WANT)
            _exit(st);
        if (!strcmp(shared->name, have)) {
            replay[0] = 0;
            at = shared->resume;
        } else if (have[0]) {
            _exit(EXIT_WANT);
        } else {
            snprintf(replay, sizeof replay, "%s", shared->name);
            at = shared->resume;
        }
    }
#else
    (void)have;
    (void)at;
    nshvc_fatal("save and load need fork, which this system has not");
#endif
}

static int started;
static int counting;        /* the frame just run is one of the script's own */

/* NSHVC_CYCLOG's frame: the script's frames counted as the reference's
   harness counts them, on across loads and calls (a replay's not counted) */
static uint32_t cyclog_frame(void) { return shared->frames + 1; }

static void start(void)
{
    if (started)
        return;
    started = 1;
#ifndef _WIN32
    {
        int i, need = 0;
        shared = (Shared *)mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (shared == MAP_FAILED)
            nshvc_fatal("mmap: %s", strerror(errno));
        snes_cyclog_frame = cyclog_frame;
        for (i = 0; i < main_list.n; i++)
            need |= main_list.s[i].kind == S_SAVE || main_list.s[i].kind == S_LOAD;
        if (need)
            hold("", 0);
    }
#endif
}

/* an spccall that has not run when the machine it was for is left: the
   driver's C never marked its instruction (spc.h spc_at) */
static void spc_unrun(void)
{
    if (snes_spc_unit_busy() == 1)
        nshvc_fatal("spccall: the SPC700 never reached the instruction it waits for (its C marks none with spc_at)");
}

static void load(const char *path)
{
    const char *b = strrchr(path, '/');
    char name[128];
    size_t k;
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    k = strlen(name);
    if (k > 6 && !strcmp(name + k - 6, ".state"))
        name[k - 6] = 0;
    spc_unrun();
#ifndef _WIN32
    snprintf(shared->name, sizeof shared->name, "%s", name);
    shared->resume = step_at + 1;
    fflush(NULL);
    _exit(EXIT_WANT);
#endif
}

/* the steps that are not frames' due now */
static void takes(void)
{
    start();
    while (step_at < cur->n) {
        Step *s = &cur->s[step_at];
        if (s->kind == S_FRAMES || s->kind == S_CALL)
            return;
        if (cur == &replay_list) {
            if (s->kind == S_SAVE && !strcmp(s->name, replay_name)) {
                hold(replay_name, replay_resume);
                continue;
            }
            if (s->kind == S_LOAD)
                nshvc_fatal("%s:%d: a load in a play replayed for a state", cur->path, s->line);
            if (s->kind == S_SHOT || s->kind == S_DUMP || s->kind == S_SAVE) {
                step_at++;
                continue;
            }
        }
        switch (s->kind) {
        case S_SHOT: shot(s->name); break;
        case S_DUMP: dump(s->name); break;
        case S_SAVE:
            hold(s->name, step_at + 1);
            continue;
        case S_LOAD: load(s->name); break;
        case S_POKE: poke(s); break;
        case S_SET:
            if (nstaged == 64)
                nshvc_fatal("%s:%d: set: more than 64 for one call", cur->path, s->line);
            staged[nstaged++] = s;
            break;
        case S_SPCSET: spcset(s); break;
        case S_SPCCALL: spccall(s); break;
        case S_INPUT: input_step(s); break;
        }
        step_at++;
    }
}

void script_takes(void) { takes(); }

/* ---- the call under way ---- */
static long call_f = -1;            /* the frames it has run; -1 none */
static long call_max;
static int call_noreturn, call_keep, call_npress;
static long call_press_at[8];
static uint16_t call_press[8][2];
static uint32_t call_addr;

/* call ADDR [long] [keep] [noreturn] [a=] [x=] [y=] [p=] [db=] [d=]
   [frames=N] [press=B@F ...] [frame=PPDD] [php] [phb] [rti=PP] [vblank] */
static void call_parse(const Step *s, SnesUnit *u)
{
    char w[24][1024];
    int n = words(s->text, w, 24), i;
    memset(u, 0, sizeof *u);
    u->frame = u->rti = -1;
    u->addr = call_addr = parse_addr(w[1]);
    call_max = 600;
    call_noreturn = call_npress = 0;
    for (i = 2; i < n; i++) {
        const char *eq = strchr(w[i], '=');
        unsigned long v = strtoul(eq ? eq + 1 : "0", NULL, 16);
        if (!strcmp(w[i], "long")) u->lng = 1;
        else if (!strcmp(w[i], "noreturn")) call_noreturn = 1;
        else if (!strcmp(w[i], "keep")) u->keep = 1;
        else if (!strncmp(w[i], "a=", 2)) { u->has_a = 1; u->a = (uint16_t)v; }
        else if (!strncmp(w[i], "x=", 2)) { u->has_x = 1; u->x = (uint16_t)v; }
        else if (!strncmp(w[i], "y=", 2)) { u->has_y = 1; u->y = (uint16_t)v; }
        else if (!strncmp(w[i], "p=", 2)) { u->has_p = 1; u->p = (uint8_t)v; }
        else if (!strncmp(w[i], "db=", 3)) { u->has_db = 1; u->db = (uint8_t)v; }
        else if (!strncmp(w[i], "d=", 2)) { u->has_d = 1; u->d = (uint16_t)v; }
        else if (!strncmp(w[i], "frames=", 7)) call_max = strtol(w[i] + 7, NULL, 10);
        else if (!strncmp(w[i], "frame=", 6)) u->frame = (long)(v & 0xFFFF);
        else if (!strncmp(w[i], "rti=", 4)) u->rti = (long)(v & 0xFF);
        else if (!strcmp(w[i], "php")) u->php = 1;
        else if (!strcmp(w[i], "phb")) u->phb = 1;
        else if (!strcmp(w[i], "vblank")) u->vblank = 1;
        else if (!strncmp(w[i], "press=", 6) && strchr(w[i], '@') && call_npress < 8) {
            char pb[64];
            snprintf(pb, sizeof pb, "%s", w[i] + 6);
            *strchr(pb, '@') = 0;
            if (!parse_buttons(pb, &call_press[call_npress][0], &call_press[call_npress][1]))
                nshvc_fatal("%s:%d: buttons '%s'", cur->path, s->line, pb);
            call_press_at[call_npress++] = strtol(strchr(w[i], '@') + 1, NULL, 10);
        } else
            nshvc_fatal("%s:%d: call: bad option %s", cur->path, s->line, w[i]);
    }
    call_keep = u->keep;
    u->apply = apply_staged;
}

static void call_end(void)
{
    call_f = -1;
    nstaged = 0;
    step_at++;
    step_left = 0;
}

/* the call step, at a frame's end: 1 and the next frame's buttons while
   it runs, 0 when it is over */
static int call_frame(Step *s, uint16_t *b0, uint16_t *b1)
{
    int i;
    if (call_f < 0) {
        SnesUnit u;
        call_parse(s, &u);
        snes_unit_call(&u);
#ifndef _WIN32
        if (!u.keep) {
            pid_t p = fork_run();
            if (p) {
                int st = child_status(p);
                if (st != EXIT_CALLED)
                    _exit(st);
                snes_unit_cancel();
                call_end();
                return 0;
            }
        }
#else
        if (!u.keep)
            nshvc_fatal("a call without keep needs fork, which this system has not");
#endif
        call_f = 0;
    } else {
        int st = snes_unit_state();
        if (st == 3) {
            if (!call_keep) {
                spc_unrun();
                fflush(NULL);
                _exit(EXIT_CALLED);
            }
            call_end();
            return 0;
        }
        if (call_f >= call_max) {
            snes_unit_cancel();
            if (!call_noreturn)
                nshvc_fatal("%s:%d: call %06X did not return in %ld frames", cur->path, s->line,
                            (unsigned)call_addr, call_max);
            call_end();
            return 0;
        }
    }
    *b0 = *b1 = 0;
    for (i = 0; i < call_npress; i++)
        if (call_f >= call_press_at[i] && call_f < call_press_at[i] + 2) {
            *b0 |= call_press[i][0];
            *b1 |= call_press[i][1];
        }
    call_f++;
    return 1;
}

/* the next frame's buttons, from the main script: counted when it has run */
static int frame_out(void)
{
    counting = cur == &main_list;
    return 1;
}

int script_frame(uint16_t *b0, uint16_t *b1)
{
    start();
    if (counting && shared) {
        shared->frames++;
        counting = 0;
    }
    for (;;) {
        Step *s;
        takes();
        if (step_at >= cur->n) {
            if (cur == &replay_list)
                nshvc_fatal("%s: the play ends before it saves %s", cur->path, replay_name);
            spc_unrun();
            return 0;
        }
        s = &cur->s[step_at];
        if (s->kind == S_CALL) {
            if (call_frame(s, b0, b1))
                return frame_out();
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
        return frame_out();
    }
}
