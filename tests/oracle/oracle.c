/*
 * oracle.c: the reference nativeshvc's tests are held to: a ROM run on a
 * libretro core (bsnes, its speed shortcuts off), headless, with no input.
 * None of the core's code or logic is in nativeshvc; this program only
 * runs it and records what it shows and plays.
 *
 *   oracle --core DYLIB --rom ROM --frames N --out DIR [--shot F,F...]
 *          [--sound FILE] [--wram FILE] [--overscan]
 *
 * Shots are DIR/frame_F.ppm (F counted from 1, the frames the core has
 * run), each pixel's 15-bit colour widened as nativeshvc widens it (c << 3
 * | c >> 2), so the two compare byte for byte. The core draws every frame
 * 512 wide and with its lines doubled; a shot is the picture as the PPU
 * made it: its lines once, and 256 wide unless a line is hires (its pixel
 * pairs differ). --overscan: the core shows 239 lines (else 224). --sound: every sample the
 * core plays, raw 16-bit stereo, left first, at the DSP's rate.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static FILE *sound_f;
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

static void sample(int16_t l, int16_t r)
{
    if (sound_f) {
        int16_t b[2] = { l, r };
        fwrite(b, 2, 2, sound_f);
    }
}

static size_t samples(const int16_t *d, size_t n)
{
    if (sound_f)
        fwrite(d, 4, n, sound_f);
    return n;
}

static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)port; (void)dev; (void)idx; (void)id;
    return 0;
}

static void shot(const char *dir, long n)
{
    char path[4200];
    FILE *f;
    unsigned x, y;
    snprintf(path, sizeof path, "%s/frame_%ld.ppm", dir, n);
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

#define LOAD(p, name) do { *(void **)&p = dlsym(core, name); if (!p) die("the core has no ", name); } while (0)

int main(int argc, char **argv)
{
    const char *corepath = NULL, *rom = NULL, *out = ".", *shots = "", *wram_path = NULL;
    long frames = 0, n;
    void *core;
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
        else if (!strcmp(argv[i], "--wram") && i + 1 < argc) wram_path = argv[++i];
        else if (!strcmp(argv[i], "--sound") && i + 1 < argc) {
            if (!(sound_f = fopen(argv[++i], "wb")))
                die("cannot write ", argv[i]);
        } else
            die("usage: oracle --core DYLIB --rom ROM --frames N --out DIR [--shot F,F...] [--sound FILE]", NULL);
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
