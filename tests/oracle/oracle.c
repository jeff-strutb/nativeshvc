/*
 * oracle.c: the reference nativeshvc's tests are held to: a ROM run on a
 * libretro core (bsnes, its speed shortcuts off), headless, with no input.
 * None of the core's code or logic is in nativeshvc; this program only
 * runs it and records what it shows and plays.
 *
 *   oracle --core DYLIB --rom ROM --frames N --out DIR [--shot F,F...]
 *          [--sound FILE] [--wram FILE] [--overscan]
 *   oracle --core DYLIB --rom ROM --script FILE --out DIR [--pacing FILE]
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
 * read, the address and the value, little-endian (the DSP's clock is the
 * SPC700's cycle: tests/dsp/mix_replay; and to pair with nativeshvc's
 * NSHVC_SPCLOG); --spcreads: every data read too. --voices: at
 * each sample, each voice's envelope level, its stage, pitch counter, BRR
 * block and byte, as the core's DSP holds them (40 32-bit values).
 *
 * --script: a play as nativeshvc's front end runs one (src/front/script.c:
 * wait, hold, press, repeat ... end, shot, dump; poke, set, call, fx, save,
 * load, spcset, spccall as Stunt Race FX's harness runs them on the
 * pinned core's hooks, load replaying the play in NSHVC_STATES that saves
 * the state, as there), the buttons of each frame
 * set before the core runs it, a step's shots and dumps (DIR/NAME.ppm,
 * .wram, and .sram the cartridge's RAM, the Super FX's) taken at the end of
 * the frame before it, the first frame's buttons taken before power-on (as
 * there, they reach no joypad read). --pacing: Stunt Race FX's pacing
 * record, as tests/srfx/srfx_cart.c writes it (the pinned core's write
 * hook). --hashes: at each frame's end, the frame and hashes of work RAM
 * (the stack, $0100-$02FF, left out) and the Super FX's RAM. --watch
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

static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)dev; (void)idx;
    if (port > 1)
        return 0;
    if (id == 256)
        return (int16_t)pad[port];
    return id < 12 ? (pad[port] >> id) & 1 : 0;
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

/* ---- --script and --pacing ---- */
static void *core;
static long runs;                      /* frames the core has run */

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
   run on the core the way the Stunt Race FX project's harness (sfxbox)
   runs them: poke and set into the core's work RAM and Super FX RAM; call
   and fx through its CPU hooks (a stub in work RAM, entered at the first
   instruction the Super FX is stopped and the vertical blank is not under
   way); spcset and spccall through its SPC700 hooks. Where the machine is
   put back (a load; a call without keep) the run forks as nativeshvc's
   does, so the two make the same frames in the same order. */
enum {
    S_FRAMES, S_CALL, S_SHOT, S_DUMP, S_SAVE, S_LOAD, S_POKE, S_SET, S_SPCSET, S_SPCCALL,
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
#define EXIT_CALLED 76

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

/* fx: the stub sfxbox's fx stages, as a set, and its call */
static void fx(List *l, const char *line)
{
    char w[24][1024], set[1024], call[256];
    uint8_t b[160];
    long reg[16], stop = -1;
    uint32_t addr;
    int n = words(line, w, 24), m = 0, i, o;
    if (n < 3)
        die("fx BANK:ADDR stop=ADDR [rN=VALUE...] [frames=N] [keep]: ", line);
    addr = parse_addr(w[1]);
    snprintf(call, sizeof call, "call 7F:F000 p=30 db=00 d=0000");
    for (i = 0; i < 16; i++)
        reg[i] = -1;
    for (i = 2; i < n; i++) {
        const char *eq = strchr(w[i], '=');
        unsigned long v = eq ? strtoul(eq + 1, NULL, 16) : 0;
        if (!strcmp(w[i], "keep") || !strncmp(w[i], "frames=", 7)) {
            strncat(call, " ", sizeof call - strlen(call) - 1);
            strncat(call, w[i], sizeof call - strlen(call) - 1);
        } else if (eq && !strncmp(w[i], "stop=", 5)) {
            stop = (long)(v & 0xFFFF);
        } else if (eq && w[i][0] == 'r' && atoi(w[i] + 1) >= 0 && atoi(w[i] + 1) <= 14) {
            reg[atoi(w[i] + 1)] = (long)(v & 0xFFFF);
        } else {
            die("fx: bad option ", w[i]);
        }
    }
    if (stop < 0)
        die("fx: stop=ADDR is needed: ", line);
    reg[11] = stop;
    b[m++] = 0xC2; b[m++] = 0x30;
    for (i = 0; i < 15; i++)
        if (reg[i] >= 0) {
            b[m++] = 0xA9; b[m++] = (uint8_t)reg[i]; b[m++] = (uint8_t)(reg[i] >> 8);
            b[m++] = 0x8D; b[m++] = (uint8_t)(2 * i); b[m++] = 0x30;
        }
    b[m++] = 0xE2; b[m++] = 0x20;
    b[m++] = 0xA9; b[m++] = (uint8_t)(addr >> 16);
    b[m++] = 0x8D; b[m++] = 0x34; b[m++] = 0x30;
    b[m++] = 0xAD; b[m++] = 0x74; b[m++] = 0x03;
    b[m++] = 0x09; b[m++] = 0x18;
    b[m++] = 0x8D; b[m++] = 0x3A; b[m++] = 0x30;
    b[m++] = 0x9C; b[m++] = 0x30; b[m++] = 0x30;
    b[m++] = 0xC2; b[m++] = 0x20;
    b[m++] = 0xA9; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8);
    b[m++] = 0x8D; b[m++] = 0x1E; b[m++] = 0x30;
    b[m++] = 0xE2; b[m++] = 0x20;
    b[m++] = 0xAD; b[m++] = 0x30; b[m++] = 0x30;
    b[m++] = 0x29; b[m++] = 0x20;
    b[m++] = 0xD0; b[m++] = 0xF9;
    b[m++] = 0xAD; b[m++] = 0x74; b[m++] = 0x03;
    b[m++] = 0x8D; b[m++] = 0x3A; b[m++] = 0x30;
    b[m++] = 0x60;
    o = snprintf(set, sizeof set, "set 7FF000 ");
    for (i = 0; i < m; i++)
        o += snprintf(set + o, sizeof set - (size_t)o, i ? ",%02X" : "%02X", b[i]);
    add_text(l, S_SET, set);
    add_text(l, S_CALL, call);
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
        } else if (!strcmp(cmd, "poke") || !strcmp(cmd, "set") || !strcmp(cmd, "spcset")) {
            if (n < 3)
                die("ADDR BYTE[,BYTE...]: ", lines[i]);
            add_text(l, cmd[0] == 'p' ? S_POKE : cmd[1] == 'e' ? S_SET : S_SPCSET, lines[i]);
        } else if (!strcmp(cmd, "call") || !strcmp(cmd, "spccall")) {
            add_text(l, cmd[0] == 'c' ? S_CALL : S_SPCCALL, lines[i]);
        } else if (!strcmp(cmd, "fx")) {
            fx(l, lines[i]);
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

/* ---- the core's harness hooks ---- */
struct SfxboxRegs { uint16_t a, x, y, s, d; uint8_t db, p, e; uint32_t pc; };
struct SfxboxSmpCallbacks {
    void (*insn)(uint16_t pc, uint16_t prev_end, uint8_t opcode);
    void (*mem)(int write, uint16_t addr, uint8_t data);
    void (*step)(uint32_t clocks);
};
struct SmpState { uint16_t pc; uint8_t a, x, y, s, p; };
static void (*core_regs)(struct SfxboxRegs *);
static void (*core_set_regs)(const struct SfxboxRegs *);
static uint8_t (*core_bus_read)(uint32_t);
static int (**cpu_exec_hook)(uint32_t);
static int (*exec_before)(uint32_t);
static void (*smp_get)(struct SmpState *);
static void (*smp_put)(const struct SmpState *);
static void (*smp_set_cb)(const struct SfxboxSmpCallbacks *, int (*)(uint16_t, uint16_t));

static void need(void **p, const char *name)
{
    if (!*p && !(*p = dlsym(core, name)))
        die("the core has no ", name);
}

/* poke ADDR BYTE[,BYTE...] into work RAM or Super FX RAM, as sfxbox */
static void poke(const char *line)
{
    char w[64][1024];
    int n = words(line, w, 64), i;
    uint32_t a = parse_addr(w[1]), bank = a >> 16 & 0xFF, off = a & 0xFFFF;
    const char *view;
    size_t base, size;
    uint8_t *p;
    if (bank == 0x7E || bank == 0x7F) { view = "wram"; base = (size_t)(bank - 0x7E) << 16 | off; }
    else if (bank <= 0x3F && off < 0x2000) { view = "wram"; base = off; }
    else if (bank == 0x70 || bank == 0x71) { view = "gsuram"; base = (size_t)(bank - 0x70) << 16 | off; }
    else die("poke: neither work RAM nor Super FX RAM: ", line);
    if (!(p = memory(view, &size)))
        die("the core has no memory ", view);
    for (i = 2; i < n; i++) {
        char *t = w[i], *e;
        while (*t) {
            unsigned long v = strtoul(t, &e, 16);
            if (e == t || v > 0xFF || (*e && *e != ','))
                die("poke: bad byte list: ", line);
            if (base >= size)
                die("poke: past the end of the RAM: ", line);
            p[base++] = (uint8_t)v;
            t = *e ? e + 1 : e;
        }
    }
}

static const char *staged[64];
static int nstaged;

/* ---- the call (sfxbox's call) ---- */
static struct SfxboxRegs call_from, call_regs;
static uint32_t call_stub, call_loop, call_addr;
static int call_state;                 /* 1 to start, 2 running, 3 returned */
static int call_keep, call_vblank, call_noreturn, call_npress;
static long call_f = -1, call_max, call_press_at[8];
static uint16_t call_press[8][2];

static int call_exec(uint32_t pc)
{
    if (call_state == 1 && !(core_bus_read(0x003030) & 0x20) && (call_vblank || !(core_bus_read(0x004212) & 0x80))) {
        struct SfxboxRegs r;
        int i;
        core_regs(&call_from);
        for (i = 0; i < nstaged; i++)
            poke(staged[i]);
        r = call_regs;
        r.s = call_from.s;
        r.e = 0;
        r.pc = call_stub;
        core_set_regs(&r);
        call_state = 2;
        return 1;
    }
    if (call_state == 2 && pc == call_loop) {
        call_state = 3;
        if (call_keep) {
            core_set_regs(&call_from);
            return 1;
        }
        return 0;
    }
    return exec_before ? exec_before(pc) : 0;
}

/* the call's stub written and its start armed (sfxbox's call up to its
   frames) */
static void call_arm(const char *line)
{
    char w[24][1024];
    int n = words(line, w, 24), i, lng = 0, php = 0, phb = 0, m = 0;
    long frame = -1, rti = -1;
    uint32_t addr, bank;
    size_t at = 0, sz;
    uint32_t sb = 0;
    uint8_t stub[32], *wram;
    need((void **)&core_regs, "sfxbox_core_regs");
    need((void **)&core_set_regs, "sfxbox_core_set_regs");
    need((void **)&core_bus_read, "sfxbox_core_bus_read");
    if (!cpu_exec_hook) {
        need((void **)&cpu_exec_hook, "sfxbox_cpu_exec");
        exec_before = *cpu_exec_hook;
        *cpu_exec_hook = call_exec;
    }
    addr = call_addr = parse_addr(w[1]);
    bank = addr >> 16;
    core_regs(&call_regs);
    call_keep = call_vblank = call_noreturn = call_npress = 0;
    call_max = 600;
    for (i = 2; i < n; i++) {
        const char *eq = strchr(w[i], '=');
        unsigned long v = strtoul(eq ? eq + 1 : "0", NULL, 16);
        if (!strcmp(w[i], "long")) lng = 1;
        else if (!strcmp(w[i], "noreturn")) call_noreturn = 1;
        else if (!strcmp(w[i], "keep")) call_keep = 1;
        else if (!strncmp(w[i], "a=", 2)) call_regs.a = (uint16_t)v;
        else if (!strncmp(w[i], "x=", 2)) call_regs.x = (uint16_t)v;
        else if (!strncmp(w[i], "y=", 2)) call_regs.y = (uint16_t)v;
        else if (!strncmp(w[i], "p=", 2)) call_regs.p = (uint8_t)v;
        else if (!strncmp(w[i], "db=", 3)) call_regs.db = (uint8_t)v;
        else if (!strncmp(w[i], "d=", 2)) call_regs.d = (uint16_t)v;
        else if (!strncmp(w[i], "frames=", 7)) call_max = strtol(w[i] + 7, NULL, 10);
        else if (!strncmp(w[i], "frame=", 6)) frame = (long)(v & 0xFFFF);
        else if (!strncmp(w[i], "rti=", 4)) rti = (long)(v & 0xFF);
        else if (!strcmp(w[i], "php")) php = 1;
        else if (!strcmp(w[i], "phb")) phb = 1;
        else if (!strcmp(w[i], "vblank")) call_vblank = 1;
        else if (!strncmp(w[i], "press=", 6) && strchr(w[i], '@') && call_npress < 8) {
            char pb[64];
            snprintf(pb, sizeof pb, "%s", w[i] + 6);
            *strchr(pb, '@') = 0;
            if (!buttons(pb, &call_press[call_npress][0], &call_press[call_npress][1]))
                die("the script's buttons: ", pb);
            call_press_at[call_npress++] = strtol(strchr(w[i], '@') + 1, NULL, 10);
        } else
            die("call: bad option ", w[i]);
    }
    if (call_regs.p & 0x10) {
        call_regs.x &= 0x00FF;
        call_regs.y &= 0x00FF;
    }
    if (lng || rti >= 0) { at = 0x1FE0; sb = 0x00; }
    else if (bank == 0x7F) { at = 0x1FFE0; sb = 0x7F; }
    else if (bank == 0x7E || bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) { at = 0x1FE0; sb = bank; }
    else die("call: a JSR cannot reach that bank from work RAM; use long: ", line);
    if (php && lng)
        die("call: php is for a JSR call: ", line);
    if (phb && !lng)
        die("call: phb is for a JSL call: ", line);
    if (rti >= 0) {
        uint16_t l = (uint16_t)(at + 10);
        stub[m++] = 0xF4; stub[m++] = (uint8_t)(l >> 8); stub[m++] = (uint8_t)sb;
        stub[m++] = 0xF4; stub[m++] = (uint8_t)rti; stub[m++] = (uint8_t)l;
        stub[m++] = 0x5C; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); stub[m++] = (uint8_t)bank;
    } else if ((frame >= 0 && lng) || phb) {
        uint16_t t = (uint16_t)(at + 6);
        stub[m++] = 0x22; stub[m++] = (uint8_t)t; stub[m++] = (uint8_t)(t >> 8); stub[m++] = (uint8_t)sb;
    } else if (frame >= 0 || php) {
        uint16_t t = (uint16_t)(at + 6);
        stub[m++] = 0x20; stub[m++] = (uint8_t)t; stub[m++] = (uint8_t)(t >> 8); stub[m++] = 0xEA;
    } else if (lng) {
        stub[m++] = 0x22; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); stub[m++] = (uint8_t)bank;
    } else {
        stub[m++] = 0x20; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); stub[m++] = 0xEA;
    }
    stub[m++] = 0x80; stub[m++] = 0xFE;
    call_stub = sb << 16 | (uint32_t)(at & 0xFFFF);
    call_loop = call_stub + (uint32_t)(m - 2);
    if (frame >= 0) {
        stub[m++] = 0xF4; stub[m++] = (uint8_t)frame; stub[m++] = (uint8_t)(frame >> 8);
        if (lng) { stub[m++] = 0x5C; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); stub[m++] = (uint8_t)bank; }
        else { stub[m++] = 0x4C; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); }
    }
    if (phb) {
        stub[m++] = 0x8B;
        stub[m++] = 0x5C; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8); stub[m++] = (uint8_t)bank;
    }
    if (php) {
        stub[m++] = 0x08;
        stub[m++] = 0x4C; stub[m++] = (uint8_t)addr; stub[m++] = (uint8_t)(addr >> 8);
    }
    if (!(wram = memory("wram", &sz)) || sz < 0x20000)
        die("the core has no work RAM", NULL);
    memcpy(wram + at, stub, (size_t)m);
    call_state = 1;
}

/* ---- spcset, spccall (sfxbox's spccall.c unit calls) ---- */
#define SPC_STUB 0x01F0
static struct {
    int state;
    uint16_t target, at;
    int has[4];
    uint8_t reg[4];
    struct SmpState saved;
    uint8_t stub_save[5];
    int nset;
    uint16_t set_at[256];
    uint8_t set_v[256];
} U;
static uint8_t *apuram;

static int spc_exec(uint16_t pc, uint16_t prev_end)
{
    (void)prev_end;
    if (U.state == 1 && pc == U.at) {
        struct SmpState r;
        int i;
        smp_get(&U.saved);
        memcpy(U.stub_save, apuram + SPC_STUB, 5);
        apuram[SPC_STUB] = 0x3F;
        apuram[SPC_STUB + 1] = (uint8_t)U.target;
        apuram[SPC_STUB + 2] = (uint8_t)(U.target >> 8);
        apuram[SPC_STUB + 3] = 0x2F;
        apuram[SPC_STUB + 4] = 0xFE;
        for (i = 0; i < U.nset; i++)
            apuram[U.set_at[i]] = U.set_v[i];
        U.nset = 0;
        r = U.saved;
        r.pc = SPC_STUB;
        if (U.has[0]) r.a = U.reg[0];
        if (U.has[1]) r.x = U.reg[1];
        if (U.has[2]) r.y = U.reg[2];
        if (U.has[3]) r.p = U.reg[3];
        smp_put(&r);
        U.state = 2;
        return 1;
    }
    if (U.state == 2 && pc == SPC_STUB + 3) {
        memcpy(apuram + SPC_STUB, U.stub_save, 5);
        smp_put(&U.saved);
        U.state = 0;
        return 1;
    }
    return 0;
}

static void spc_need(void)
{
    size_t n;
    if (apuram)
        return;
    need((void **)&smp_get, "sfxbox_smp_get");
    need((void **)&smp_put, "sfxbox_smp_put");
    need((void **)&smp_set_cb, "sfxbox_smp_set_callbacks");
    if (!(apuram = memory("apuram", &n)))
        die("the core has no APU RAM", NULL);
    {
        static struct SfxboxSmpCallbacks none = { NULL, NULL, NULL };
        smp_set_cb(&none, spc_exec);
    }
}

static void spcset(const char *line)
{
    char w[64][1024];
    int n = words(line, w, 64), i;
    unsigned long a = strtoul(w[1][0] == '$' ? w[1] + 1 : w[1], NULL, 16);
    spc_need();
    for (i = 2; i < n; i++) {
        char *t = w[i], *e;
        while (*t) {
            unsigned long v = strtoul(t, &e, 16);
            if (e == t || v > 0xFF || (*e && *e != ',') || a > 0xFFFF)
                die("spcset: bad byte list: ", line);
            if (U.nset == 256)
                die("spcset: more than 256 bytes for one spccall", NULL);
            U.set_at[U.nset] = (uint16_t)a++;
            U.set_v[U.nset++] = (uint8_t)v;
            t = *e ? e + 1 : e;
        }
    }
}

static void spccall(const char *line)
{
    char w[16][1024];
    int n = words(line, w, 16), i;
    spc_need();
    if (U.state)
        die("spccall: the previous one has not run yet", NULL);
    U.target = (uint16_t)strtoul(w[1][0] == '$' ? w[1] + 1 : w[1], NULL, 16);
    U.at = 0x0897;
    memset(U.has, 0, sizeof U.has);
    for (i = 2; i < n; i++) {
        char *eq = strchr(w[i], '=');
        unsigned long v = eq ? strtoul(eq + 1, NULL, 16) : 0;
        if (!strcmp(w[i], "noreturn"))
            continue;
        if (!eq)
            die("spccall: bad option ", w[i]);
        if (!strncmp(w[i], "a=", 2)) { U.has[0] = 1; U.reg[0] = (uint8_t)v; }
        else if (!strncmp(w[i], "x=", 2)) { U.has[1] = 1; U.reg[1] = (uint8_t)v; }
        else if (!strncmp(w[i], "y=", 2)) { U.has[2] = 1; U.reg[2] = (uint8_t)v; }
        else if (!strncmp(w[i], "psw=", 4)) { U.has[3] = 1; U.reg[3] = (uint8_t)v; }
        else if (!strncmp(w[i], "at=", 3)) U.at = (uint16_t)v;
        else die("spccall: bad option ", w[i]);
    }
    U.state = 1;
}

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

static void spc_unrun(void)
{
    if (U.state == 1)
        die("spccall: the SPC700 never reached the instruction it waits for", NULL);
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
        if (st->kind == S_FRAMES || st->kind == S_CALL)
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
            break;
        case S_DUMP:
            write_mem(dir, st->name, ".wram", "wram");
            write_mem(dir, st->name, ".sram", "gsuram");
            write_mem(dir, st->name, ".gsuram", "gsuram");
            break;
        case S_SAVE:
            hold(st->name, step_at + 1);
            continue;
        case S_LOAD: {
            const char *b = strrchr(st->name, '/');
            size_t k;
            spc_unrun();
            snprintf(shared->name, sizeof shared->name, "%s", b ? b + 1 : st->name);
            k = strlen(shared->name);
            if (k > 6 && !strcmp(shared->name + k - 6, ".state"))
                shared->name[k - 6] = 0;
            shared->resume = step_at + 1;
            fflush(NULL);
            _exit(EXIT_WANT);
        }
        case S_POKE: poke(st->text); break;
        case S_SET:
            if (nstaged == 64)
                die("set: more than 64 for one call", NULL);
            staged[nstaged++] = st->text;
            break;
        case S_SPCSET: spcset(st->text); break;
        case S_SPCCALL: spccall(st->text); break;
        }
        step_at++;
    }
}

static void call_end(void)
{
    call_f = -1;
    nstaged = 0;
    step_at++;
    step_left = 0;
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
            spc_unrun();
            return 0;
        }
        st = &cur->s[step_at];
        if (st->kind == S_CALL) {
            int i;
            if (call_f < 0) {
                call_arm(st->text);
                if (!call_keep) {
                    pid_t p = fork_run();
                    if (p) {
                        int s = child_status(p);
                        if (s != EXIT_CALLED)
                            _exit(s);
                        call_state = 0;
                        call_end();
                        continue;
                    }
                }
                call_f = 0;
            } else if (call_state == 3) {
                call_state = 0;
                if (!call_keep) {
                    spc_unrun();
                    fflush(NULL);
                    _exit(EXIT_CALLED);
                }
                call_end();
                continue;
            } else if (call_f >= call_max) {
                if (!call_noreturn) {
                    char m[64];
                    snprintf(m, sizeof m, "%06X", (unsigned)call_addr);
                    die("a call did not return in its frames: ", m);
                }
                call_state = 0;
                call_end();
                continue;
            }
            pad[0] = pad[1] = 0;
            for (i = 0; i < call_npress; i++)
                if (call_f >= call_press_at[i] && call_f < call_press_at[i] + 2) {
                    pad[0] |= call_press[i][0];
                    pad[1] |= call_press[i][1];
                }
            call_f++;
            return 1;
        }
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

static FILE *pacing_f, *hashes_f;
static uint8_t *pacing_wram, *pacing_gsuram;

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
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

/* the core's write hook (its harness's, before the write lands): an
   S-CPU write (kind 0) to $06D4 */
static void pacing_write(uint32_t addr, uint8_t value, int kind)
{
    if (watch_set && runs + 1 >= watch_from && runs + 1 <= watch_to) {
        long o = wram_offset(addr);
        if (o >= 0 && watch_set[o])
            fprintf(stderr, "frame %ld pc %06X %s $%05lX <- %02X\n", runs + 1, last_pc, kind ? "dma" : "cpu", o, value);
    }
    if (!pacing_f)
        return;
    uint8_t bank = (uint8_t)(addr >> 16);
    const uint8_t *w = pacing_wram;
    (void)value;
    if (kind != 0 || (addr & 0xFFFF) != 0x06D4 || !(bank == 0x7E || (bank & 0x7F) < 0x40))
        return;
    fprintf(pacing_f, "%ld %06X %u %08x %08x\n", runs, 0u, w[0x306],
            fnv(w + 0x300, 0x20000 - 0x300, fnv(w, 0x100, 2166136261u)), fnv(pacing_gsuram, 0x10000, 2166136261u));
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
    const char *corepath = NULL, *rom = NULL, *out = ".", *shots = "", *wram_path = NULL, *script = NULL,
               *pacing = NULL;
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
        else if (!strcmp(argv[i], "--pacing") && i + 1 < argc) pacing = argv[++i];
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
        else if (!strcmp(argv[i], "--hashes") && i + 1 < argc) {
            if (!(hashes_f = fopen(argv[++i], "w")))
                die("cannot write ", argv[i]);
        }
        else if (!strcmp(argv[i], "--wram") && i + 1 < argc) wram_path = argv[++i];
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
    if (hashes_f) {
        size_t n;
        if (!(pacing_wram = memory("wram", &n)) || n < 0x20000 || !(pacing_gsuram = memory("gsuram", &n)) ||
            n < 0x10000)
            die("the core has no memories for --hashes", NULL);
    }
    if (pacing || watch_set) {
        size_t n;
        static struct sfxbox_callbacks cb;
        void (*set)(const struct sfxbox_callbacks *) =
            (void (*)(const struct sfxbox_callbacks *))dlsym(core, "sfxbox_set_callbacks");
        if (!set || !(pacing_wram = memory("wram", &n)) || n < 0x20000 || !(pacing_gsuram = memory("gsuram", &n)) ||
            n < 0x10000)
            die("the core has no write hook or memories for --pacing", NULL);
        if (pacing && !(pacing_f = fopen(pacing, "w")))
            die("cannot write ", pacing);
        if (pacing_f)
            setvbuf(pacing_f, NULL, _IOLBF, 0);
        cb.write = pacing_write;
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
                if (hashes_f)
                    fprintf(hashes_f, "%ld %08x %08x\n", runs,
                            fnv(pacing_wram + 0x300, 0x20000 - 0x300, fnv(pacing_wram, 0x100, 2166136261u)),
                            fnv(pacing_gsuram, 0x10000, 2166136261u));
                if (!script_frame(out))
                    break;
            }
        if (pacing_f)
            fclose(pacing_f);
        if (hashes_f)
            fclose(hashes_f);
        return 0;
    }
    for (n = 1; n <= frames; n++) {
        char key[32];
        const char *p;
        size_t k;
        p_run();
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
