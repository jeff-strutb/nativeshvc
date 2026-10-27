/*
 * script.c: a headless run's script (script.h).
 *
 * A machine is never copied or put back: the C under way on the S-CPU's
 * and the SPC700's coroutines is part of it. Where the script needs the
 * machine as it was (a load), the run forks: the process that holds the machine as
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
 *
 * A child's end that is not one of these (the script's end, an error) ends
 * every process above it with the same status.
 *
 * Without fork (Windows; NSHVC_SCRIPT_NOFORK=1 elsewhere, to check it), no
 * process holds a machine: where the script needs the machine as it was,
 * the run starts a new copy of the program and ends with its status. The
 * new one rebuilds that machine by replaying, from power-on, the steps that
 * made it (the run is the same each time), and goes on from there:
 *
 *   history   what made the machine: the steps of the play that saves a
 *             state (P:NAME, as above) and runs of the script's own steps
 *             (M:A-B), each without its shots, dumps and saves
 *   save      NAME's history noted (passed on to each new copy)
 *   load      a new copy rebuilds NAME, and goes on from the step after
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#else
#include <io.h>
#include <windows.h>
#endif

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "snes_internal.h"
#include "script.h"
#include "ppu.h"

enum {
    S_FRAMES,               /* buttons for frames */
    S_SHOT, S_DUMP, S_SAVE, S_LOAD,
    S_INPUT,                /* plug, pad, mouse, gun: the controller ports from now */
};

typedef struct {
    int kind;
    uint16_t buttons[2];
    long frames;
    char name[128];
    char *text;             /* the line (plug, pad, mouse, gun) */
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
        } else if (!strcmp(cmd, "plug") || !strcmp(cmd, "pad") || !strcmp(cmd, "mouse") || !strcmp(cmd, "gun")) {
            add_text(l, S_INPUT, lines[i], i + 1);
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

/* the play in NSHVC_STATES (*.sfx) with `save NAME`: its path in found */
static void find_play(const char *name, char *found, size_t size)
{
    const char *dir = getenv("NSHVC_STATES");
    char path[2048], line[512];
    found[0] = 0;
    if (!dir)
        nshvc_fatal("load %s: set NSHVC_STATES to the folder of the plays that save states", name);
#ifndef _WIN32
    {
        DIR *d;
        struct dirent *e;
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
                    snprintf(found, size, "%s", path);
                    break;
                }
            }
            fclose(f);
        }
        closedir(d);
    }
#else
    {
        struct _finddata_t fd;
        intptr_t h;
        snprintf(path, sizeof path, "%s/*.sfx", dir);
        if ((h = _findfirst(path, &fd)) != -1) {
            do {
                FILE *f;
                snprintf(path, sizeof path, "%s/%s", dir, fd.name);
                if (!(f = fopen(path, "r")))
                    continue;
                while (fgets(line, sizeof line, f)) {
                    char c[64] = "", a[256] = "";
                    if (sscanf(line, "%63s %255s", c, a) == 2 && !strcmp(c, "save") && !strcmp(a, name)) {
                        snprintf(found, size, "%s", path);
                        break;
                    }
                }
                fclose(f);
            } while (!found[0] && _findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#endif
    if (!found[0])
        nshvc_fatal("load %s: no play in %s saves it", name, dir);
}

/* the steps that save state `name`: the script's own up to its save, when
   one comes before any load (1, `l` the main script), else the play that
   saves it, loaded into `l` (0) */
static int play_for(const char *name, List *l)
{
    char found[2048];
    int i;
    for (i = 0; i < main_list.n && main_list.s[i].kind != S_LOAD; i++)
        if (main_list.s[i].kind == S_SAVE && !strcmp(main_list.s[i].name, name)) {
            *l = main_list;
            return 1;
        }
    find_play(name, found, sizeof found);
    memset(l, 0, sizeof *l);
    list_load(l, found);
    return 0;
}

#ifndef _WIN32
/* the play that saves state `name` (a .sfx in NSHVC_STATES with `save
   NAME`), from power-on to that save, then the main script from `resume`
   (with fork) */
static void begin_replay(const char *name, int resume)
{
    play_for(name, &replay_list);
    snprintf(replay_name, sizeof replay_name, "%s", name);
    replay_resume = resume;
    cur = &replay_list;
    step_at = 0;
    step_left = 0;
    snes_cyclog_quiet = 1;
}
#endif

/* ---- without fork: the machine rebuilt by replay ---- */
static int nofork = -1;
static int plan_on, plan_resume;            /* replaying the plan, then the main script from plan_resume */
static int main_from;                       /* the main script's step this process went on from */
static char *history;                       /* what made the machine as this process began its own steps */
static char *saved;                         /* NAME=HISTORY;... the states saved */
static int self_argc;
static char **self_argv;
static Shared own_shared;

static int no_fork(void)
{
    if (nofork < 0) {
#ifdef _WIN32
        nofork = 1;
#else
        nofork = getenv("NSHVC_SCRIPT_NOFORK") != NULL;
#endif
    }
    return nofork;
}

void script_args(int argc, char **argv)
{
    self_argc = argc;
    self_argv = argv;
}

int script_replaying(void) { return cur == &replay_list; }

static char *str_cat(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 1;
    char *r = (char *)malloc(n);
    if (!r)
        nshvc_fatal("no memory for the script");
    snprintf(r, n, "%s%s", a, b);
    return r;
}

/* the machine's history now, the main script's steps up to `upto` */
static char *history_to(int upto)
{
    char seg[64];
    snprintf(seg, sizeof seg, "%sM:%d-%d", history[0] ? "," : "", main_from, upto);
    return str_cat(history, seg);
}

/* steps [a, b) of `src` onto `dst`, as a history replays them */
static void plan_add(List *dst, const List *src, int a, int b)
{
    int i;
    for (i = a; i < b; i++) {
        const Step *t = &src->s[i];
        if (t->kind == S_SHOT || t->kind == S_DUMP || t->kind == S_SAVE)
            continue;
        if (t->kind == S_LOAD)
            nshvc_fatal("%s:%d: a load in steps replayed for a state", src->path, t->line);
        dst->s = (Step *)realloc(dst->s, sizeof *dst->s * (size_t)(dst->n + 1));
        if (!dst->s)
            nshvc_fatal("no memory for the script");
        dst->s[dst->n++] = *t;
    }
}

/* the replay of a history, then the main script from `resume` */
static void plan_begin(const char *plan, int resume)
{
    char *copy = str_cat(plan, ""), *seg, *next;
    memset(&replay_list, 0, sizeof replay_list);
    replay_list.path = main_list.path;
    for (seg = copy; seg && *seg; seg = next) {
        next = strchr(seg, ',');
        if (next)
            *next++ = 0;
        if (!strncmp(seg, "P:", 2)) {
            List l;
            int i;
            play_for(seg + 2, &l);
            for (i = 0; i < l.n; i++)
                if (l.s[i].kind == S_SAVE && !strcmp(l.s[i].name, seg + 2))
                    break;
            if (i == l.n)
                nshvc_fatal("%s: the play ends before it saves %s", l.path, seg + 2);
            plan_add(&replay_list, &l, 0, i);
        } else if (!strncmp(seg, "M:", 2)) {
            int a = 0, b = 0;
            sscanf(seg + 2, "%d-%d", &a, &b);
            plan_add(&replay_list, &main_list, a, b);
        } else {
            nshvc_fatal("NSHVC_SCRIPT_PLAN: '%s'", seg);
        }
    }
    free(copy);
    history = str_cat(plan, "");
    plan_on = 1;
    plan_resume = resume;
    cur = &replay_list;
    step_at = 0;
    step_left = 0;
    snes_cyclog_quiet = 1;
}

/* the replay done: the main script from where it goes on */
static void plan_end(void)
{
    plan_on = 0;
    main_from = plan_resume;
    begin_main(plan_resume);
}

static void env_set(const char *k, const char *v)
{
#ifdef _WIN32
    if (_putenv_s(k, v))
#else
    if (setenv(k, v, 1))
#endif
        nshvc_fatal("cannot set %s", k);
}

/* a new copy of the program, rebuilding the machine `plan` made and going
   on from step `resume`; this process ends with its status */
static void go_on_in_new_copy(const char *plan, int resume)
{
    char n[32];
    int st;
    if (!self_argv)
        nshvc_fatal("save and load need the program's arguments (script_args)");
    env_set("NSHVC_SCRIPT_PLAN", plan[0] ? plan : "M:0-0");
    snprintf(n, sizeof n, "%d", resume);
    env_set("NSHVC_SCRIPT_RESUME", n);
    env_set("NSHVC_SCRIPT_SAVED", saved ? saved : "");
    snprintf(n, sizeof n, "%u", (unsigned)shared->frames);
    env_set("NSHVC_SCRIPT_FRAMES", n);
    fflush(NULL);
#ifndef _WIN32
    {
        pid_t p;
        int err = posix_spawnp(&p, self_argv[0], NULL, NULL, self_argv, environ);
        if (err)
            nshvc_fatal("cannot start %s: %s", self_argv[0], strerror(err));
        st = child_status(p);
    }
#else
    {
        char exe[MAX_PATH];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (!GetModuleFileNameA(NULL, exe, sizeof exe) ||
            !CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
            nshvc_fatal("cannot start a new copy of the program (%lu)", (unsigned long)GetLastError());
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        st = (int)code;
    }
#endif
    fflush(NULL);
    _exit(st);
}

/* save NAME: its history noted */
static void plan_save(const char *name)
{
    char *h = history_to(step_at), *e = str_cat(name, "="), *entry = str_cat(e, h), *all;
    all = str_cat(saved ? saved : "", saved && saved[0] ? ";" : "");
    free(saved);
    saved = str_cat(all, entry);
    free(all);
    free(entry);
    free(e);
    free(h);
}

/* load NAME: rebuilt in a new copy */
static void plan_load(const char *name)
{
    const char *p = saved;
    size_t k = strlen(name);
    while (p && *p) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > k && !strncmp(p, name, k) && p[k] == '=') {
            char *plan = (char *)malloc(len - k);
            if (!plan)
                nshvc_fatal("no memory for the script");
            memcpy(plan, p + k + 1, len - k - 1);
            plan[len - k - 1] = 0;
            go_on_in_new_copy(plan, step_at + 1);
        }
        p = end ? end + 1 : NULL;
    }
    {
        char seg[160];
        snprintf(seg, sizeof seg, "P:%s", name);
        go_on_in_new_copy(seg, step_at + 1);
    }
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
   harness counts them, on across loads (a replay's not counted) */
static uint32_t cyclog_frame(void) { return shared->frames + 1; }

static void start(void)
{
    if (started)
        return;
    started = 1;
    if (no_fork()) {
        const char *plan = getenv("NSHVC_SCRIPT_PLAN"), *f = getenv("NSHVC_SCRIPT_FRAMES"),
                   *sv = getenv("NSHVC_SCRIPT_SAVED"), *r = getenv("NSHVC_SCRIPT_RESUME");
        shared = &own_shared;
        shared->frames = f ? (uint32_t)strtoul(f, NULL, 10) : 0;
        snes_cyclog_frame = cyclog_frame;
        saved = str_cat(sv ? sv : "", "");
        history = str_cat("", "");
        if (plan)
            plan_begin(plan, r ? atoi(r) : 0);
        return;
    }
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

static void load(const char *path)
{
    const char *b = strrchr(path, '/');
    char name[128];
    size_t k;
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    k = strlen(name);
    if (k > 6 && !strcmp(name + k - 6, ".state"))
        name[k - 6] = 0;
    if (no_fork())
        plan_load(name);
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
        if (s->kind == S_FRAMES)
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
            if (no_fork()) {
                plan_save(s->name);
                break;
            }
            hold(s->name, step_at + 1);
            continue;
        case S_LOAD: load(s->name); break;
        case S_INPUT: input_step(s); break;
        }
        step_at++;
    }
}

void script_takes(void) { takes(); }

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
        if (step_at >= cur->n && plan_on) {
            plan_end();
            continue;
        }
        if (step_at >= cur->n) {
            if (cur == &replay_list)
                nshvc_fatal("%s: the play ends before it saves %s", cur->path, replay_name);
            return 0;
        }
        s = &cur->s[step_at];
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
