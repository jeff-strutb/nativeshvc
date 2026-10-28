/*
 * oracle.c: the reference nativeshvc's tests are held to: a ROM run on a
 * libretro core (bsnes, its speed shortcuts off), headless, with no input.
 * None of the core's code or logic is in nativeshvc; this program only
 * runs it and records what it shows and plays.
 *
 *   oracle --core DYLIB --rom ROM --frames N --out DIR [--shot F,F...]
 *          [--sound FILE] [--wram FILE] [--overscan]
 *   oracle --core DYLIB --rom ROM --script FILE --out DIR
 *
 * Shots are DIR/frame_F.ppm (F counted from 1, the frames the core has
 * run), each pixel's 15-bit colour widened as nativeshvc widens it (c << 3
 * | c >> 2), so the two compare byte for byte. The core draws every frame
 * 512 wide and with its lines doubled; a shot is the picture as the PPU
 * made it: its lines once, and 256 wide unless a line is hires (its pixel
 * pairs differ); an interlaced picture (its two fields' lines differ) keeps
 * its 448 (or 478) lines. --overscan: the core shows 239 lines (else 224). --sound: every sample the
 * DSP makes, raw 16-bit stereo, left first, at the DSP's rate (the pinned core's
 * DSP hook; a core without it, what libretro is given). --spclog: each
 * SPC700 write, and each read of its registers ($F0-$FF), 8 bytes: its
 * time in half cycles from power-on (32 bits), 1 for a write or 0 for a
 * read (2: an S-CPU write to a port, $F4-$F7 its address, at the SPC700's
 * time when the core reports it, which may be behind the S-CPU's), the
 * address and the value, little-endian (the DSP's clock is the
 * SPC700's cycle: tests/dsp/mix_replay; and to pair with nativeshvc's
 * NSHVC_SPCLOG); --spcreads: every data read too. --cpulog: each S-CPU
 * read and write (an interrupt's pushes too), and each byte a DMA or HDMA
 * writes, 16 bytes: the core's CPU clock as it is made (master clocks from
 * power-on, 64 bits; a read's 4 clocks before its cycle ends, a write's at
 * its end), the address (32 bits), 0 for a read of data, 1 for a write, 2
 * for a fetch of code (the open bus holds what it fetched), 3 for a DMA's
 * or HDMA's write, the value, 2 bytes of 0; little-endian
 * (tests/rom/replay.c plays it back on nativeshvc). --cpustats FILE
 * [--cpustats-after SHOT]: the CPU's own time around its accesses, the
 * work the game's C does not reproduce: for each S-CPU read of data and
 * each write, the clocks its CPU spent since the one before on code
 * fetches and internal cycles (not WAI's, not DMA's or refresh's), a
 * histogram (GAP COUNT lines, then totals), from the script's shot SHOT
 * on when given; a stretch of more than 64 reads with no write is a poll
 * loop (the console's waits stand for those) and is left out, as is a gap
 * past 1,024 clocks (a branch to itself). And a line a frame: "F frame",
 * then for reads and for writes of work RAM, ROM, cartridge RAM and I/O
 * (nshvc.h NSHVC_KIND_*: the same addresses), each the accesses and their
 * gaps' clocks, then the accesses' own bus clocks. --voices: at
 * each sample, each voice's envelope level, its stage, pitch counter, BRR
 * block and byte, as the core's DSP holds them (40 32-bit values).
 *
 * --script: a play as nativeshvc's front end runs one (src/front/script.c:
 * wait, hold, press, repeat ... end, shot, dump, save, load, plug, pad,
 * mouse, gun, load replaying the play in NSHVC_STATES that saves the
 * state, as there), the buttons of each frame
 * set before the core runs it, a step's shots and dumps (DIR/NAME.ppm,
 * .wram) taken at the end of the frame before it, the first frame's
 * buttons taken before power-on (as there, they reach no joypad read). --watch
 * F-T:A,A...: each write to work RAM offsets A (hex) in frames F to T
 * (the frame being run counted from 1), to stderr with the S-CPU's
 * instruction that made it (a DMA's: the last instruction before it).
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* the parts of the libretro interface used here */
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable { const char *key; const char *value; };
enum { ENV_GET_CAN_DUPE = 3, ENV_GET_SYSTEM_DIRECTORY = 9, ENV_SET_PIXEL_FORMAT = 10, ENV_GET_VARIABLE = 15,
       ENV_GET_SAVE_DIRECTORY = 31 };
enum { PIX_0RGB1555 = 0, PIX_XRGB8888 = 1, PIX_RGB565 = 2 };

static void (*p_init)(void);
static void (*p_set_environment)(bool (*)(unsigned, void *));
static void (*p_set_video_refresh)(void (*)(const void *, unsigned, unsigned, size_t));
static void (*p_set_audio_sample)(void (*)(int16_t, int16_t));
static void (*p_set_audio_sample_batch)(size_t (*)(const int16_t *, size_t));
static void (*p_set_input_poll)(void (*)(void));
static void (*p_set_input_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
static bool (*p_load_game)(const struct retro_game_info *);
static void (*p_run)(void);
static void (*p_set_controller_port_device)(unsigned, unsigned);
static void *(*p_get_memory_data)(unsigned);
static size_t (*p_get_memory_size)(unsigned);

static unsigned pixfmt = PIX_0RGB1555;
static int overscan;
static char sysdir[4096] = ".";
static FILE *sound_f, *voices_f;
static uint32_t frame[512 * 512];
static unsigned fw, fh;

static void die(const char *m, const char *a)
{
    fprintf(stderr, "oracle: %s%s\n", m, a ? a : "");
    exit(1);
}

static bool env(unsigned cmd, void *data)
{
    switch (cmd) {
    case ENV_GET_SYSTEM_DIRECTORY:
    case ENV_GET_SAVE_DIRECTORY:
        *(const char **)data = sysdir;
        return true;
    case ENV_SET_PIXEL_FORMAT:
        pixfmt = *(const unsigned *)data;
        return true;
    case ENV_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case ENV_GET_VARIABLE: {
        /* accuracy first: the scanline PPU, fast DSP and delayed coprocessor
           sync off; no overclock */
        struct retro_variable *v = (struct retro_variable *)data;
        static const char *fixed[][2] = {
            { "bsnes_ppu_fast", "OFF" }, { "bsnes_dsp_fast", "OFF" }, { "bsnes_coprocessor_delayed_sync", "OFF" },
            { "bsnes_cpu_overclock", "100" }, { "bsnes_cpu_sfx_overclock", "100" },
            { "bsnes_cpu_sa1_overclock", "100" }, { "bsnes_sfx_overclock", "100" }, { "bsnes_sa1_overclock", "100" },
            { "bsnes_ppu_show_overscan", NULL }, { "bsnes_blur_emulation", "OFF" }, { "bsnes_entropy", "None" },
            { "bsnes_hotfixes", "OFF" }, { "bsnes_cpu_fastmath", "OFF" }, { "bsnes_ppu_deinterlace", "OFF" },
            { "bsnes_ppu_no_sprite_limit", "OFF" }, { "bsnes_ppu_no_vram_blocking", "OFF" },
            { "bsnes_mode7_scale", "1x" }, { "bsnes_mode7_perspective", "OFF" }, { "bsnes_mode7_supersample", "OFF" },
            { "bsnes_mode7_mosaic", "OFF" }, { "bsnes_dsp_cubic", "OFF" }, { "bsnes_dsp_echo_shadow", "OFF" },
            { "bsnes_coprocessor_prefer_hle", "OFF" }, { "bsnes_video_filter", "None" }, { "bsnes_run_ahead_frames", "OFF" },
            { "bsnes_aspect_ratio", "8:7" },
        };
        unsigned i;
        for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
            if (!strcmp(v->key, fixed[i][0])) {
                v->value = fixed[i][1] ? fixed[i][1] : overscan ? "ON" : "OFF";
                return true;
            }
        return false;
    }
    }
    return false;
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    unsigned x, y;
    if (!data)
        return;
    if (w > 512) w = 512;
    if (h > 512) h = 512;
    fw = w;
    fh = h;
    if (getenv("ORACLE_DEBUG")) {
        const uint8_t *r0 = (const uint8_t *)data + 100 * pitch;
        fprintf(stderr, "fmt %u %ux%u pitch %zu: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", pixfmt, w, h, pitch,
                r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7], r0[8], r0[9], r0[10], r0[11], r0[12], r0[13], r0[14], r0[15]);
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        for (x = 0; x < w; x++) {
            unsigned r, g, b;
            if (pixfmt == PIX_XRGB8888) {
                uint32_t c = ((const uint32_t *)row)[x];
                r = (c >> 19) & 31; g = (c >> 11) & 31; b = (c >> 3) & 31;
            } else {
                uint16_t p = ((const uint16_t *)row)[x];
                if (pixfmt == PIX_RGB565) {
                    r = p >> 11 & 31; g = p >> 6 & 31; b = p & 31;
                } else {
                    r = p >> 10 & 31; g = p >> 5 & 31; b = p & 31;
                }
            }
            frame[y * 512 + x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
        }
    }
}

static int dsp_hooked;

static void sample(int16_t l, int16_t r)
{
    if (sound_f && !dsp_hooked) {
        int16_t b[2] = { l, r };
        fwrite(b, 2, 2, sound_f);
    }
}

static size_t samples(const int16_t *d, size_t n)
{
    if (sound_f && !dsp_hooked)
        fwrite(d, 4, n, sound_f);
    return n;
}

/* the pinned core's DSP and SPC700 write hooks: each sample as the DSP
   makes it, and each write with the DSP's time */
static void dsp_sample(int16_t l, int16_t r)
{
    if (sound_f) {
        int16_t b[2] = { l, r };
        fwrite(b, 2, 2, sound_f);
    }
}

struct smp_callbacks {
    void (*insn)(uint16_t pc, uint16_t prev_end, uint8_t opcode);
    void (*mem)(int write, uint16_t addr, uint8_t data);
    void (*step)(uint32_t clocks);
};
static FILE *spclog_f;
static int spclog_reads;
static uint32_t smp_halves;

static void smp_step(uint32_t clocks) { smp_halves += clocks & 0x7FFFFFFFu; }

static void smp_mem(int write, uint16_t addr, uint8_t v)
{
    if (write || spclog_reads || (addr >= 0xF0 && addr <= 0xFF)) {
        uint8_t rec[8] = { (uint8_t)smp_halves, (uint8_t)(smp_halves >> 8), (uint8_t)(smp_halves >> 16),
                           (uint8_t)(smp_halves >> 24), (uint8_t)write, (uint8_t)addr, (uint8_t)(addr >> 8), v };
        fwrite(rec, 1, 8, spclog_f);
    }
}

static void dsp_voices(const int32_t *st) { fwrite(st, 4, 40, voices_f); }


/* the buttons held: libretro's joypad ids as bits (B, Y, SELECT, START,
   UP, DOWN, LEFT, RIGHT, A, X, L, R: the joypad's own order) */
static uint16_t pad[2];

static int words(const char *line, char w[][1024], int max);

/* the script's plug, pad, mouse and gun steps: joypads 2-7 (their own
   bits, B 0x8000), each port's mouse motion for the next frame and its
   buttons, and where each light gun points; the core's devices take the
   guns' aim as motion, from where each starts */
static uint16_t padx[8];
static int mouse_m[2][3];
static int gun_at[2][2], gun_b[2], gun_cur[3][2][2];
static int16_t gun_input(unsigned kind, unsigned id)
{
    unsigned g = kind ? id / 4 : 0, k = kind ? id % 4 : id, b = gun_b[g];
    int d;
    if (k <= 1) {
        d = gun_at[g][k] - gun_cur[kind][g][k];
        gun_cur[kind][g][k] = gun_at[g][k];
        return (int16_t)d;
    }
    if (kind)
        return k == 2 ? (b & 1) != 0 : (b & 16) != 0;            /* trigger, start */
    return (b >> (k - 2) & 1) != 0;                            /* trigger, cursor, turbo, pause */
}

static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (dev == 2)                                               /* RETRO_DEVICE_MOUSE */
        return port < 2 && id < 4 ? (int16_t)(id < 2 ? mouse_m[port][id] : mouse_m[port][2] >> (id - 2) & 1) : 0;
    if (dev == 4)                                               /* RETRO_DEVICE_LIGHTGUN */
        return gun_input(idx, id);
    if (port > 7)
        return 0;
    if (port > 1)
        return id < 12 ? (padx[port] & 0x8000 >> id) != 0 : 0;
    if (id == 256)
        return (int16_t)pad[port];
    return id < 12 ? (pad[port] >> id) & 1 : 0;
}

/* plug PORT DEVICE; pad N BITS; mouse PORT DX DY BUTTONS; gun G X Y BUTTONS */
static void input_step(const char *line)
{
    static const char *const dev[] = { "joypad", "none", "mouse", "multitap", "superscope", "justifier",
                                       "justifiers" };
    static const unsigned id[] = { 1, 0, 2, 0x101, 0x104, 0x204, 0x304 };
    char w[8][1024];
    int n = words(line, w, 8), i;
    if (!strcmp(w[0], "plug") && n == 3) {
        for (i = 0; i < 7; i++)
            if (!strcmp(w[2], dev[i])) {
                p_set_controller_port_device((unsigned)atoi(w[1]), id[i]);
                /* where the core's guns start: the screen's middle, two
                   chained Justifiers 16 dots either side */
                memset(gun_cur, 0, sizeof gun_cur);
                gun_cur[0][0][0] = gun_cur[1][0][0] = 128;
                gun_cur[2][0][0] = 112;
                gun_cur[2][1][0] = 144;
                gun_cur[0][0][1] = gun_cur[1][0][1] = gun_cur[2][0][1] = gun_cur[2][1][1] = 120;
                /* aimed there until a gun step says otherwise */
                if (i >= 4)
                    memcpy(gun_at, gun_cur[i - 4], sizeof gun_at);
                return;
            }
    } else if (!strcmp(w[0], "pad") && n == 3 && atoi(w[1]) >= 0 && atoi(w[1]) < 8) {
        padx[atoi(w[1])] = (uint16_t)strtoul(w[2], NULL, 16);
        return;
    } else if (!strcmp(w[0], "mouse") && n == 5 && (atoi(w[1]) & ~1) == 0) {
        mouse_m[atoi(w[1])][0] = atoi(w[2]);
        mouse_m[atoi(w[1])][1] = atoi(w[3]);
        mouse_m[atoi(w[1])][2] = atoi(w[4]);
        return;
    } else if (!strcmp(w[0], "gun") && n == 5 && (atoi(w[1]) & ~1) == 0) {
        gun_at[atoi(w[1])][0] = atoi(w[2]);
        gun_at[atoi(w[1])][1] = atoi(w[3]);
        gun_b[atoi(w[1])] = atoi(w[4]);
        return;
    }
    die("a controller step: ", line);
}

static void shot_to(const char *path)
{
    FILE *f;
    unsigned x, y;
    f = fopen(path, "wb");
    if (!f)
        die("cannot write ", path);
    {
        /* the lines once (the core doubles them); 256 wide unless a pixel
           pair differs somewhere */
        unsigned ys = 1, xs = 1, oh, ow, skip = 0;
        if (fh >= 448) {
            ys = 2;
            for (y = 0; y + 1 < fh && ys == 2; y += 2)
                if (memcmp(&frame[y * 512], &frame[(y + 1) * 512], fw * 4))
                    ys = 1;
        }
        if (fw == 512) {
            xs = 2;
            for (y = 0; y < fh && xs == 2; y++)
                for (x = 0; x < fw; x += 2)
                    if (frame[y * 512 + x] != frame[y * 512 + x + 1]) {
                        xs = 1;
                        break;
                    }
        }
        ow = fw / xs;
        oh = fh / ys;
        /* with overscan shown the core's picture starts at line 0, which is
           never drawn: the PPU's picture is lines 1-239 */
        if (overscan && oh == 240) {
            skip = 1;
            oh = 239;
        } else if (overscan && oh == 480) {
            /* interlaced: line 0 in both fields */
            skip = 2;
            oh = 478;
        }
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++) {
                uint32_t c = frame[(y + skip) * ys * 512 + x * xs];
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
                fwrite(rgb, 1, 3, f);
            }
    }
    fclose(f);
}

static void shot(const char *dir, long n)
{
    char path[4200];
    snprintf(path, sizeof path, "%s/frame_%ld.ppm", dir, n);
    shot_to(path);
}

/* ---- --script ---- */
static void *core;
static long runs;                      /* frames the core has run */
/* --cpustats: counting from the script's shot named this (NULL: from power on), and
   whether it is now */
static const char *cpustats_after;
static int cpustats_on;

static uint8_t *memory(const char *name, size_t *size)
{
    uint8_t *(*mem)(const char *, size_t *) = (uint8_t *(*)(const char *, size_t *))dlsym(core, "sfxbox_memory");
    *size = 0;
    return mem ? mem(name, size) : NULL;
}

static void write_mem(const char *dir, const char *name, const char *ext, const char *which)
{
    char path[4200];
    size_t n;
    uint8_t *m = memory(which, &n);
    FILE *f;
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    if (!(f = fopen(path, "wb")))
        die("cannot write ", path);
    if (m && n)
        fwrite(m, 1, n, f);
    fclose(f);
}

/* The script's steps (src/front/script.c's, which says what each does),
   run on the core. Where the machine is put back (a load) the run forks as
   nativeshvc's does, so the two make the same frames in the same order. */
enum {
    S_FRAMES, S_SHOT, S_DUMP, S_SAVE, S_LOAD, S_INPUT,
};

typedef struct {
    int kind;
    uint16_t pad[2];
    long frames;
    char name[128];
    char *text;
} Step;

typedef struct {
    Step *s;
    int n;
} List;

static List main_list, replay_list, *cur = &main_list;
static int step_at;
static long step_left;

#define EXIT_WANT 75

typedef struct {
    char name[128];
    int resume;
} Shared;
static Shared *shared;

static Step *add_step(List *l, int kind, uint16_t b0, uint16_t b1, long frames, const char *name)
{
    Step *st;
    if (!(l->s = (Step *)realloc(l->s, sizeof *l->s * (size_t)(l->n + 1))))
        die("no memory for the script", NULL);
    st = &l->s[l->n++];
    memset(st, 0, sizeof *st);
    st->kind = kind;
    st->pad[0] = b0;
    st->pad[1] = b1;
    st->frames = frames;
    if (name)
        snprintf(st->name, sizeof st->name, "%s", name);
    return st;
}

static void add_text(List *l, int kind, const char *text)
{
    if (!(add_step(l, kind, 0, 0, 0, NULL)->text = strdup(text)))
        die("no memory for the script", NULL);
}

static int buttons(const char *t, uint16_t *b0, uint16_t *b1)
{
    static const char *k[12] = { "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R" };
    char buf[256], *p, *next;
    *b0 = *b1 = 0;
    snprintf(buf, sizeof buf, "%s", t);
    for (p = buf; p; p = next) {
        int two = 0, i;
        if ((next = strchr(p, '+')))
            *next++ = 0;
        if (!strncmp(p, "2.", 2)) {
            two = 1;
            p += 2;
        }
        for (i = 0; i < 12 && strcmp(p, k[i]); i++)
            ;
        if (i == 12)
            return 0;
        *(two ? b1 : b0) |= (uint16_t)(1u << i);
    }
    return 1;
}

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

static void script_range(List *l, char **lines, int lo, int hi)
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
            if (!buttons(a, &b0, &b1))
                die("the script's buttons: ", a);
            add_step(l, S_FRAMES, b0, b1, b[0] ? atol(b) : (cmd[0] == 'p' ? 2 : 1), NULL);
            if (cmd[0] == 'p')
                add_step(l, S_FRAMES, 0, 0, 2, NULL);
        } else if (!strcmp(cmd, "shot") || !strcmp(cmd, "dump") || !strcmp(cmd, "save") || !strcmp(cmd, "load")) {
            if (n < 2)
                die("a step with no name: ", cmd);
            add_step(l, cmd[0] == 's' ? (cmd[1] == 'h' ? S_SHOT : S_SAVE) : cmd[0] == 'd' ? S_DUMP : S_LOAD, 0, 0, 0, a);
        } else if (!strcmp(cmd, "plug") || !strcmp(cmd, "pad") || !strcmp(cmd, "mouse") || !strcmp(cmd, "gun")) {
            add_text(l, S_INPUT, lines[i]);
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
                die("repeat without end", NULL);
            while (k-- > 0)
                script_range(l, lines, i + 1, j);
            i = j;
        } else {
            die("a step the oracle's script runner does not take: ", cmd);
        }
    }
}

static void list_load(List *l, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096], **lines = NULL;
    int n = 0;
    if (!f)
        die("cannot read ", path);
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        lines = (char **)realloc(lines, sizeof *lines * (size_t)(n + 1));
        if (!lines || !(lines[n++] = strdup(line)))
            die("no memory for the script", NULL);
    }
    fclose(f);
    script_range(l, lines, 0, n);
}

static void script_load(const char *path) { list_load(&main_list, path); }

/* ---- the processes (src/front/script.c's) ---- */
static pid_t fork_run(void)
{
    pid_t p;
    fflush(NULL);
    if ((p = fork()) < 0)
        die("fork failed", NULL);
    return p;
}

static int child_status(pid_t p)
{
    int st;
    while (waitpid(p, &st, 0) < 0)
        if (errno != EINTR)
            die("waitpid failed", NULL);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static char replay_name[128];
static int replay_resume;

static void begin_replay(const char *name, int resume)
{
    const char *dir = getenv("NSHVC_STATES");
    char found[2048] = "", path[2048], line[512];
    DIR *d;
    struct dirent *e;
    int i;
    for (i = 0; i < main_list.n && main_list.s[i].kind != S_LOAD; i++)
        if (main_list.s[i].kind == S_SAVE && !strcmp(main_list.s[i].name, name)) {
            replay_list = main_list;
            snprintf(replay_name, sizeof replay_name, "%s", name);
            replay_resume = resume;
            cur = &replay_list;
            step_at = 0;
            step_left = 0;
            return;
        }
    if (!dir)
        die("load: set NSHVC_STATES to the folder of the plays that save states: ", name);
    if (!(d = opendir(dir)))
        die("NSHVC_STATES: cannot read ", dir);
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
        die("load: no play in NSHVC_STATES saves ", name);
    memset(&replay_list, 0, sizeof replay_list);
    list_load(&replay_list, found);
    snprintf(replay_name, sizeof replay_name, "%s", name);
    replay_resume = resume;
    cur = &replay_list;
    step_at = 0;
    step_left = 0;
}

static void hold(const char *have, int at)
{
    char replay[128] = "";
    for (;;) {
        int st;
        pid_t p = fork_run();
        if (p == 0) {
            if (replay[0]) {
                begin_replay(replay, at);
            } else {
                cur = &main_list;
                step_at = at;
                step_left = 0;
            }
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
}

static void takes(const char *dir)
{
    static int started;
    if (!started) {
        int i, needed = 0;
        started = 1;
        for (i = 0; i < main_list.n; i++)
            needed |= main_list.s[i].kind == S_SAVE || main_list.s[i].kind == S_LOAD;
        if (needed) {
            shared = (Shared *)mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
            if (shared == MAP_FAILED)
                die("mmap failed", NULL);
            hold("", 0);
        }
    }
    while (step_at < cur->n) {
        Step *st = &cur->s[step_at];
        char path[4200];
        if (st->kind == S_FRAMES)
            return;
        if (cur == &replay_list) {
            if (st->kind == S_SAVE && !strcmp(st->name, replay_name)) {
                hold(replay_name, replay_resume);
                continue;
            }
            if (st->kind == S_LOAD)
                die("a load in a play replayed for a state", NULL);
            if (st->kind == S_SHOT || st->kind == S_DUMP || st->kind == S_SAVE) {
                step_at++;
                continue;
            }
        }
        switch (st->kind) {
        case S_SHOT:
            snprintf(path, sizeof path, "%s/%s.ppm", dir, st->name);
            shot_to(path);
            if (cpustats_after && !strcmp(st->name, cpustats_after))
                cpustats_on = 1;
            break;
        case S_DUMP:
            write_mem(dir, st->name, ".wram", "wram");
            break;
        case S_SAVE:
            hold(st->name, step_at + 1);
            continue;
        case S_LOAD: {
            const char *b = strrchr(st->name, '/');
            size_t k;
            snprintf(shared->name, sizeof shared->name, "%s", b ? b + 1 : st->name);
            k = strlen(shared->name);
            if (k > 6 && !strcmp(shared->name + k - 6, ".state"))
                shared->name[k - 6] = 0;
            shared->resume = step_at + 1;
            fflush(NULL);
            _exit(EXIT_WANT);
        }
        case S_INPUT: input_step(st->text); break;
        }
        step_at++;
    }
}

/* the steps due now, then the next frame's buttons; 0 when the script is
   done */
static int script_frame(const char *dir)
{
    for (;;) {
        Step *st;
        takes(dir);
        if (step_at >= cur->n) {
            if (cur == &replay_list)
                die("the play ends before it saves ", replay_name);
            return 0;
        }
        st = &cur->s[step_at];
        if (!step_left)
            step_left = st->frames;
        if (step_left <= 0) {
            step_at++;
            step_left = 0;
            continue;
        }
        pad[0] = st->pad[0];
        pad[1] = st->pad[1];
        if (--step_left == 0)
            step_at++;
        return 1;
    }
}

static long watch_from = -1, watch_to;
static uint8_t *watch_set;             /* work RAM offsets watched, a byte each */
static uint32_t last_pc;

static void watch_insn(uint32_t pc) { last_pc = pc; }

/* the work RAM offset an S-CPU or DMA address reaches, or -1 */
static long wram_offset(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    if (bank == 0x7E || bank == 0x7F)
        return (long)(bank & 1) << 16 | off;
    if ((bank & 0x7F) < 0x40 && off < 0x2000)
        return off;
    return -1;
}

/* --spclog: an S-CPU write to a port ($2140-$2143), kind 2, at the
   SPC700's time when the core reports it */
static void spclog_cpu_write(uint32_t addr, uint8_t value)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    if (spclog_f && (addr & 0xFFFC) == 0x2140 && (bank & 0x7F) < 0x40) {
        uint8_t rec[8] = { (uint8_t)smp_halves, (uint8_t)(smp_halves >> 8), (uint8_t)(smp_halves >> 16),
                           (uint8_t)(smp_halves >> 24), 2, (uint8_t)(0xF4 + (addr & 3)), 0, value };
        fwrite(rec, 1, 8, spclog_f);
    }
}

/* --cpulog: the core's CPU clock, widened to 64 bits as it runs, and
   what its reads are for (SFXBOX_WHO_DATA 0: data; 1, 2 an opcode's, an
   operand's fetch) */
static FILE *cpulog_f;
static uint32_t (*core_clock)(void);
static int *core_who;
static uint64_t cpu_clock64;
static uint32_t cpu_clock_last;

static void cpulog_put(uint32_t addr, int write, uint8_t value)
{
    uint32_t c = core_clock();
    uint8_t rec[16];
    int k;
    cpu_clock64 += (uint32_t)(c - cpu_clock_last);
    cpu_clock_last = c;
    for (k = 0; k < 8; k++)
        rec[k] = (uint8_t)(cpu_clock64 >> (8 * k));
    for (k = 0; k < 4; k++)
        rec[8 + k] = (uint8_t)(addr >> (8 * k));
    rec[12] = (uint8_t)write;
    rec[13] = value;
    rec[14] = rec[15] = 0;
    fwrite(rec, 1, 16, cpulog_f);
}

/* --cpustats */
static FILE *cpustats_f;
static uint8_t *core_opcode;
#define GAP_MAX 1024
static uint64_t gap_hist[GAP_MAX + 1], gap_reads[65], gap_fetches, gap_idles;
static uint32_t gap_now, gap_fetch_now, gap_idle_now;
static uint32_t pending[65], pending_n;      /* the reads since the last write */
static uint8_t pending_kind[65];
static uint32_t pending_bus[65];
/* this frame's: by kind (read or write x work RAM, ROM, I/O) the accesses
   and their gaps, and the bus clocks */
static uint64_t frame_n[8], frame_gap[8], frame_bus;
static uint32_t access_kind, access_bus;     /* the access being counted */

/* the access kinds of nshvc.h (NSHVC_KIND_*): work RAM 0, ROM 1,
   cartridge RAM 2, I/O 3 */
static int region(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16), b = bank & 0x7F;
    uint16_t off = (uint16_t)addr;
    if ((b >= 0x70 && b <= 0x7D && off < 0x8000) || (bank >= 0xF0 && off < 0x8000) ||
        (b >= 0x20 && b <= 0x3F && off >= 0x6000 && off < 0x8000))
        return 2;
    if (bank == 0x7E || bank == 0x7F || (!(bank & 0x40) && off < 0x2000))
        return 0;
    if (!(bank & 0x40) && off < 0x6000)
        return 3;
    return 1;
}
static uint64_t poll_reads, long_gaps;

static void gap_count_kind(uint32_t g, unsigned kind, uint32_t bus)
{
    frame_n[kind]++;
    frame_gap[kind] += g;
    frame_bus += bus;
}

static void gap_count(uint32_t g)
{
    if (g > GAP_MAX) {
        long_gaps++;
        return;
    }
    gap_hist[g]++;
}

/* an access of data: its gap, held with the reads before it until a write
   shows they were not a poll loop */
static void gap_access(int write)
{
    uint32_t k;
    if (!cpustats_on)
        goto done;
    if (!write) {
        if (pending_n < 65) {
            pending[pending_n] = gap_now;
            pending_kind[pending_n] = (uint8_t)access_kind;
            pending_bus[pending_n] = access_bus;
        }
        pending_n++;
        goto done;
    }
    if (pending_n <= 64)
        for (k = 0; k < pending_n; k++) {
            gap_count(pending[k]);
            if (pending[k] <= GAP_MAX)
                gap_count_kind(pending[k], pending_kind[k], pending_bus[k]);
        }
    else
        poll_reads += pending_n;
    pending_n = 0;
    gap_count(gap_now);
    if (gap_now <= GAP_MAX)
        gap_count_kind(gap_now, access_kind, access_bus);
done:
    gap_now = 0;
}

static void core_timing(int kind, int clocks, uint32_t addr)
{
    if (kind == 1 || kind == 2) {
        access_kind = (kind == 2 ? 4u : 0u) + (unsigned)region(addr);
        access_bus = (uint32_t)clocks;
    }
    if (kind == 1 && *core_who == 0)
        gap_access(0);
    else if (kind == 1 && (*core_who == 1 || *core_who == 2))
        gap_now += (uint32_t)clocks, gap_fetches += cpustats_on;
    else if (kind == 2)
        gap_access(1);
    else if (kind == 3 && *core_opcode != 0xCB && *core_opcode != 0xDB)
        gap_now += (uint32_t)clocks, gap_idles += cpustats_on;
}

/* the frame's line (--cpustats), at its end */
static void cpustats_frame(long frame)
{
    int k;
    if (!cpustats_f || !cpustats_on)
        return;
    fprintf(cpustats_f, "F %ld", frame);
    for (k = 0; k < 8; k++)
        fprintf(cpustats_f, " %llu %llu", (unsigned long long)frame_n[k], (unsigned long long)frame_gap[k]);
    fprintf(cpustats_f, " %llu\n", (unsigned long long)frame_bus);
    memset(frame_n, 0, sizeof frame_n);
    memset(frame_gap, 0, sizeof frame_gap);
    frame_bus = 0;
}

static void cpustats_write(void)
{
    uint64_t n = 0, sum = 0;
    int g;
    for (g = 0; g <= GAP_MAX; g++) {
        if (gap_hist[g])
            fprintf(cpustats_f, "%d %llu\n", g, (unsigned long long)gap_hist[g]);
        n += gap_hist[g];
        sum += gap_hist[g] * (uint64_t)g;
    }
    fprintf(cpustats_f, "accesses %llu clocks %llu fetches %llu idles %llu poll_reads %llu long_gaps %llu\n",
            (unsigned long long)n, (unsigned long long)sum, (unsigned long long)gap_fetches,
            (unsigned long long)gap_idles, (unsigned long long)poll_reads, (unsigned long long)long_gaps);
    fclose(cpustats_f);
}

static void core_read(uint32_t addr, uint8_t value)
{
    if (cpulog_f && *core_who <= 2)
        cpulog_put(addr, *core_who ? 2 : 0, value);
}

/* the core's write hook (its harness's, before the write lands) */
static void core_write(uint32_t addr, uint8_t value, int kind)
{
    if (kind == 0)
        spclog_cpu_write(addr, value);
    if (cpulog_f)
        cpulog_put(addr, kind == 0 ? 1 : 3, value);
    if (watch_set && runs + 1 >= watch_from && runs + 1 <= watch_to) {
        long o = wram_offset(addr);
        if (o >= 0 && watch_set[o])
            fprintf(stderr, "frame %ld pc %06X %s $%05lX <- %02X\n", runs + 1, last_pc, kind ? "dma" : "cpu", o, value);
    }
}

struct sfxbox_callbacks {
    void (*insn)(uint32_t pc);
    void (*write)(uint32_t addr, uint8_t data, int kind);
    void (*read)(uint32_t addr, uint8_t data);
    void (*interrupt)(uint32_t resume_pc, uint16_t s);
    void (*timing)(int kind, int clocks, uint32_t addr);
};

#define LOAD(p, name) do { *(void **)&p = dlsym(core, name); if (!p) die("the core has no ", name); } while (0)

int main(int argc, char **argv)
{
    const char *corepath = NULL, *rom = NULL, *out = ".", *shots = "", *wram_path = NULL, *script = NULL;
    long frames = 0, n;
    FILE *f;
    static uint8_t romdata[16 << 20];
    struct retro_game_info gi;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) corepath = argv[++i];
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--overscan")) overscan = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            char *p = argv[++i], *e;
            watch_from = strtol(p, &e, 10);
            watch_to = *e == '-' ? strtol(e + 1, &e, 10) : watch_from;
            if (*e != ':')
                die("--watch F-T:A,A...", NULL);
            watch_set = (uint8_t *)calloc(0x20000, 1);
            for (p = e + 1; *p; p = *e ? e + 1 : e) {
                unsigned long a = strtoul(p, &e, 16);
                if (a < 0x20000)
                    watch_set[a] = 1;
            }
        }
        else if (!strcmp(argv[i], "--wram") && i + 1 < argc) wram_path = argv[++i];
        else if (!strcmp(argv[i], "--cpustats") && i + 1 < argc) {
            if (!(cpustats_f = fopen(argv[++i], "w")))
                die("cannot write ", argv[i]);
        } else if (!strcmp(argv[i], "--cpustats-after") && i + 1 < argc)
            cpustats_after = argv[++i];
        else if (!strcmp(argv[i], "--cpulog") && i + 1 < argc) {
            if (!(cpulog_f = fopen(argv[++i], "wb")))
                die("cannot write ", argv[i]);
        }
        else if (!strcmp(argv[i], "--sound") && i + 1 < argc) {
            if (!(sound_f = fopen(argv[++i], "wb")))
                die("cannot write ", argv[i]);
        } else if (!strcmp(argv[i], "--spclog") && i + 1 < argc) {
            if (!(spclog_f = fopen(argv[++i], "wb")))
                die("cannot write ", argv[i]);
        } else if (!strcmp(argv[i], "--spcreads")) {
            spclog_reads = 1;
        } else if (!strcmp(argv[i], "--voices") && i + 1 < argc) {
            if (!(voices_f = fopen(argv[++i], "wb")))
                die("cannot write ", argv[i]);
        } else
            die("usage: oracle --core DYLIB --rom ROM --frames N --out DIR [--shot F,F...] [--sound FILE] [--spclog FILE] [--voices FILE]",
                NULL);
    }
    if (!corepath || !rom)
        die("--core and --rom are needed", NULL);
    snprintf(sysdir, sizeof sysdir, "%s", out);
    core = dlopen(corepath, RTLD_NOW | RTLD_LOCAL);
    if (!core)
        die("cannot load the core: ", dlerror());
    LOAD(p_init, "retro_init");
    LOAD(p_set_environment, "retro_set_environment");
    LOAD(p_set_video_refresh, "retro_set_video_refresh");
    LOAD(p_set_audio_sample, "retro_set_audio_sample");
    LOAD(p_set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOAD(p_set_input_poll, "retro_set_input_poll");
    LOAD(p_set_input_state, "retro_set_input_state");
    LOAD(p_load_game, "retro_load_game");
    LOAD(p_run, "retro_run");
    LOAD(p_set_controller_port_device, "retro_set_controller_port_device");
    LOAD(p_get_memory_data, "retro_get_memory_data");
    LOAD(p_get_memory_size, "retro_get_memory_size");
    p_set_environment(env);
    p_init();
    p_set_video_refresh(video);
    p_set_audio_sample(sample);
    p_set_audio_sample_batch(samples);
    p_set_input_poll(poll);
    p_set_input_state(input);
    {
        void (**ds)(int16_t, int16_t) = (void (**)(int16_t, int16_t))dlsym(core, "sfxbox_dsp_sample");
        if (ds) {
            *ds = dsp_sample;
            dsp_hooked = 1;
        }
        if (spclog_f) {
            struct smp_callbacks *cb = (struct smp_callbacks *)dlsym(core, "sfxbox_smp_cb");
            if (!cb)
                die("the core has no SPC700 hooks for --spclog", NULL);
            cb->mem = smp_mem;
            cb->step = smp_step;
        }
        if (voices_f) {
            void (**dv)(const int32_t *) = (void (**)(const int32_t *))dlsym(core, "sfxbox_dsp_voices");
            if (!dv)
                die("the core has no DSP voice hook for --voices", NULL);
            *dv = dsp_voices;
        }
    }
    f = fopen(rom, "rb");
    if (!f)
        die("cannot read ", rom);
    memset(&gi, 0, sizeof gi);
    gi.path = rom;
    gi.data = romdata;
    gi.size = fread(romdata, 1, sizeof romdata, f);
    fclose(f);
    if (!p_load_game(&gi))
        die("the core did not load ", rom);
    if (watch_set || spclog_f || cpulog_f || cpustats_f) {
        static struct sfxbox_callbacks cb;
        void (*set)(const struct sfxbox_callbacks *) =
            (void (*)(const struct sfxbox_callbacks *))dlsym(core, "sfxbox_set_callbacks");
        if (!set)
            die("the core has no write hook", NULL);
        cb.write = core_write;
        if (cpustats_f) {
            core_who = (int *)dlsym(core, "sfxbox_cpu_who");
            core_opcode = (uint8_t *)dlsym(core, "sfxbox_cpu_opcode");
            if (!core_who || !core_opcode)
                die("the core has no CPU hooks", NULL);
            cpustats_on = !cpustats_after;
            cb.timing = core_timing;
        }
        if (cpulog_f) {
            *(void **)&core_clock = dlsym(core, "sfxbox_core_clock");
            core_who = (int *)dlsym(core, "sfxbox_cpu_who");
            if (!core_clock || !core_who)
                die("the core has no CPU clock", NULL);
            cpu_clock_last = core_clock();
            cpu_clock64 = cpu_clock_last;
            cb.read = core_read;
        }
        if (watch_set)
            cb.insn = watch_insn;
        set(&cb);
    }
    if (script) {
        script_load(script);
        /* the first frame's buttons reach no joypad read: taken now */
        if (script_frame(out))
            for (;;) {
                p_run();
                runs++;
                cpustats_frame(runs);
                /* a mouse's motion is the frame's */
                mouse_m[0][0] = mouse_m[0][1] = mouse_m[1][0] = mouse_m[1][1] = 0;
                if (!script_frame(out))
                    break;
            }
        if (cpustats_f)
            cpustats_write();
        return 0;
    }
    for (n = 1; n <= frames; n++) {
        char key[32];
        const char *p;
        size_t k;
        p_run();
        cpustats_frame(n);
        snprintf(key, sizeof key, ",%ld,", n);
        k = strlen(shots);
        {
            char buf[4096];
            snprintf(buf, sizeof buf, ",%s,", shots);
            p = strstr(buf, key);
        }
        (void)k;
        if (p)
            shot(out, n);
    }
    if (sound_f)
        fclose(sound_f);
    if (voices_f)
        fclose(voices_f);
    if (spclog_f)
        fclose(spclog_f);
    if (cpulog_f)
        fclose(cpulog_f);
    if (cpustats_f)
        cpustats_write();
    if (wram_path) {
        /* work RAM after the last frame (libretro's system RAM, id 2) */
        uint8_t *m = (uint8_t *)p_get_memory_data(2);
        size_t sz = p_get_memory_size(2);
        if (!m || !sz) {
            /* the pinned core's harness hook, where libretro's id has none */
            uint8_t *(*mem)(const char *, size_t *) = (uint8_t *(*)(const char *, size_t *))dlsym(core, "sfxbox_memory");
            if (mem)
                m = mem("wram", &sz);
        }
        if (!m || !sz)
            die("the core gives no work RAM", NULL);
        if (!(f = fopen(wram_path, "wb")))
            die("cannot write ", wram_path);
        fwrite(m, 1, sz, f);
        fclose(f);
    }
    return 0;
}
