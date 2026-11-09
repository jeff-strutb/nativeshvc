/* bridge.c: zelda3 (github.com/snesrev/zelda3) on nativeshvc. A test case for the
 * engine. The game's C is untouched except for zelda3.patch: once a frame, what
 * the game has set becomes engine calls, and line-by-line changes become
 * nshvc_line; its sound driver runs as the engine's music code.
 *
 * With ZELDA3_REFERENCE=DIR, the game's own line renderer draws instead and
 * every 50th picture is written to DIR, to compare with the engine's --shots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nshvc/nshvc.h"
#include "src/types.h"
#include "src/zelda_rtl.h"
#include "src/assets.h"
#include "snes/ppu.h"
#include <stdatomic.h>
static atomic_int busy = 1;
void ZeldaPushApuState(void);

const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

static nshvc_tiles t_bg[4], t_obj, t_m7;
static nshvc_map m_bg[4][4], m_m7;
static uint8_t m7_tex[256 * 64];
static uint16_t m7_map[128 * 128];
static uint16_t pad_now;
static int use_reference;
static uint8_t *ref_buffer;

/* the state the line diff compares */
typedef struct {
    uint8_t w1l, w1r, w2l, w2r, r, g, b, tm, ts, brightness, blank, mode;
    uint16_t hs[4], vs[4];
    int16_t m7[8];
    uint8_t sub, half, fixed_use, math, clip, prevent, tmw, tsw, mos, mossize;
    uint32_t winsel;
} Line;
static Line prev;

static void snap(const Ppu *p, Line *l)
{
    int i;
    memset(l, 0, sizeof *l);
    l->w1l = p->window1left; l->w1r = p->window1right; l->w2l = p->window2left; l->w2r = p->window2right;
    l->r = p->fixedColorR; l->g = p->fixedColorG; l->b = p->fixedColorB;
    l->tm = p->screenEnabled[0]; l->ts = p->screenEnabled[1];
    l->brightness = p->brightness; l->blank = p->forcedBlank; l->mode = p->mode;
    for (i = 0; i < 4; i++) { l->hs[i] = p->bgLayer[i].hScroll; l->vs[i] = p->bgLayer[i].vScroll; }
    memcpy(l->m7, p->m7matrix, sizeof l->m7);
    l->sub = p->subtractColor; l->half = p->halfColor; l->fixed_use = !p->addSubscreen; l->math = p->mathEnabled;
    l->clip = p->clipMode; l->prevent = p->preventMathMode; l->tmw = p->screenWindowed[0]; l->tsw = p->screenWindowed[1];
    l->mos = p->mosaicEnabled; l->mossize = p->mosaicSize; l->winsel = p->windowsel;
}

static int use_bits(unsigned n)
{
    /* the SNES's nibble: bit 0 W1 invert, 1 W1 on, 2 W2 invert, 3 W2 on */
    return ((n & 2) ? NSHVC_WIN1 : 0) | ((n & 1) ? NSHVC_WIN1_INVERT : 0) |
           ((n & 8) ? NSHVC_WIN2 : 0) | ((n & 4) ? NSHVC_WIN2_INVERT : 0);
}

static void set_tiles(const Ppu *p)
{
    int i;
    for (i = 0; i < 3; i++) {
        const int words = i == 2 ? 8 : 16;
        uint32_t adr = p->bgLayer[i].tileAdr, n = (0x8000 - adr) / words;
        if (n > 1024) n = 1024;
        nshvc_tiles_write(t_bg[i], 0, p->vram + adr, n, NSHVC_TILES_PLANAR);
    }
    nshvc_tiles_write(t_obj, 0, p->vram + (p->objTileAdr1 & 0x7fff), 256, NSHVC_TILES_PLANAR);
    nshvc_tiles_write(t_obj, 256, p->vram + (p->objTileAdr2 & 0x7fff), 256, NSHVC_TILES_PLANAR);
}

static nshvc_map set_map(const Ppu *p, int i)
{
    const BgLayer *b = &p->bgLayer[i];
    const int k = (b->tilemapWider ? 1 : 0) | (b->tilemapHigher ? 2 : 0);
    const nshvc_map m = m_bg[i][k];
    uint32_t a = b->tilemapAdr & 0x7fff;
    int y, x;
    for (y = 0; y < (b->tilemapHigher ? 2 : 1); y++)
        for (x = 0; x < (b->tilemapWider ? 2 : 1); x++) {
            nshvc_map_write_snes(m, (uint32_t)x * 32, (uint32_t)y * 32, 32, 32, p->vram + a);
            a = (a + 0x400) & 0x7fff;
        }
    return m;
}

static void set_sprites(const Ppu *p)
{
    static const uint8_t small_large[8][2] = { {1,2},{1,4},{1,8},{2,4},{2,8},{4,8},{2,4},{2,4} };
    static nshvc_sprite s[128];
    int i;
    for (i = 0; i < 128; i++) {
        const uint16_t w0 = p->oam[i * 2], w1 = p->oam[i * 2 + 1];
        const unsigned hi = (p->oam[0x100 + i / 8] >> ((i & 7) * 2)) & 3;
        int x = (w0 & 0xff) | ((hi & 1) << 8), y = w0 >> 8;
        const int size = small_large[p->objSize & 7][hi >> 1];
        if (x >= 256) x -= 512;
        if (y >= 224) y -= 256;
        s[i].x = x; s[i].y = y;
        s[i].width = s[i].height = (uint16_t)size;
        s[i].tiles = t_obj;
        s[i].tile = w1 & 0x1ff;
        s[i].palette = (w1 >> 9) & 7;
        s[i].priority = (w1 >> 12) & 3;
        s[i].hflip = (w1 >> 14) & 1;
        s[i].vflip = (w1 >> 15) & 1;
    }
    nshvc_sprites_set(s, 128, 0);
}

static void set_mode7(const Ppu *p)
{
    nshvc_mode7 m;
    int i;
    for (i = 0; i < 0x8000 && i < 256 * 64; i++)
        m7_tex[i] = (uint8_t)(p->vram[i] >> 8);
    for (i = 0; i < 128 * 128; i++)
        m7_map[i] = p->vram[i] & 0xff;
    nshvc_tiles_write(t_m7, 0, m7_tex, 256, NSHVC_TILES_MODE7);
    nshvc_map_write_snes(m_m7, 0, 0, 128, 128, m7_map);
    memset(&m, 0, sizeof m);
    m.a = p->m7matrix[0]; m.b = p->m7matrix[1]; m.c = p->m7matrix[2]; m.d = p->m7matrix[3];
    m.centre_x = p->m7matrix[4]; m.centre_y = p->m7matrix[5];
    m.scroll_x = p->m7matrix[6]; m.scroll_y = p->m7matrix[7];
    m.outside = p->m7largeField ? (p->m7charFill ? 2 : 1) : 0;
    m.flip_x = p->m7xFlip; m.flip_y = p->m7yFlip;
    m.tiles = t_m7; m.map = m_m7;
    nshvc_mode7_set(&m);
}

static void set_picture(const Ppu *p)
{
    nshvc_screen sc;
    nshvc_windows w;
    nshvc_blend b;
    int i;
    memset(&sc, 0, sizeof sc);
    sc.brightness = p->brightness;
    sc.blank = p->forcedBlank;
    nshvc_screen_set(&sc);
    nshvc_mode_set(p->mode, 1, 0);
    set_tiles(p);
    if (p->mode == 7)
        set_mode7(p);
    for (i = 0; i < 4; i++) {
        nshvc_layer l;
        memset(&l, 0, sizeof l);
        if (i < 3 || p->mode == 7) {
            l.enabled_main = (p->screenEnabled[0] >> i) & 1;
            l.enabled_sub = (p->screenEnabled[1] >> i) & 1;
            l.tiles = t_bg[i < 3 ? i : 0];
            if (i < 3 && p->mode != 7)
                l.map = set_map(p, i);
            l.tile_size = 8;
            l.scroll_x = p->bgLayer[i].hScroll;
            l.scroll_y = p->bgLayer[i].vScroll;
            l.mosaic = (p->mosaicEnabled >> i) & 1 ? p->mosaicSize : 1;
            l.wrap = 1;
        }
        nshvc_layer_set((uint8_t)i, &l);
    }
    nshvc_sprites_show((p->screenEnabled[0] >> 4) & 1, (p->screenEnabled[1] >> 4) & 1);
    set_sprites(p);
    memset(&w, 0, sizeof w);
    w.w[0].left = p->window1left; w.w[0].right = p->window1right;
    w.w[1].left = p->window2left; w.w[1].right = p->window2right;
    for (i = 0; i < 6; i++)
        w.layer_use[i] = (uint8_t)use_bits((p->windowsel >> (i * 4)) & 15);
    for (i = 0; i < 5; i++) {
        w.mask_main[i] = (p->screenWindowed[0] >> i) & 1;
        w.mask_sub[i] = (p->screenWindowed[1] >> i) & 1;
    }
    nshvc_windows_set(&w);
    memset(&b, 0, sizeof b);
    b.subtract = p->subtractColor; b.half = p->halfColor; b.use_fixed = !p->addSubscreen;
    b.layers = p->mathEnabled; b.clip = p->clipMode; b.prevent = p->preventMathMode;
    b.fixed_bgr15 = (uint16_t)(p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10);
    nshvc_blend_set(&b);
    nshvc_palette_write(0, 256, p->cgram);
}

#define DIFF(f, field, v) do { if (n.f != prev.f) nshvc_line(line, field, v); } while (0)

/* in place of the game's line renderer: i is 1 for the picture's first line */
void engine_line(Ppu *p, int i)
{
    Line n;
    uint32_t line;
    int k;
    if (i == 0 || i > 224)
        return;
    snap(p, &n);
    if (i == 1) {
        set_picture(p);
        prev = n;
        return;
    }
    line = (uint32_t)i - 1;
    DIFF(w1l, NSHVC_LINE_WIN1_LEFT, n.w1l);
    DIFF(w1r, NSHVC_LINE_WIN1_RIGHT, n.w1r);
    DIFF(w2l, NSHVC_LINE_WIN2_LEFT, n.w2l);
    DIFF(w2r, NSHVC_LINE_WIN2_RIGHT, n.w2r);
    if (n.r != prev.r || n.g != prev.g || n.b != prev.b)
        nshvc_line(line, NSHVC_LINE_FIXED, n.r | n.g << 5 | n.b << 10);
    DIFF(brightness, NSHVC_LINE_BRIGHTNESS, n.brightness);
    DIFF(blank, NSHVC_LINE_BLANK, n.blank);
    for (k = 0; k < 4; k++) {
        DIFF(hs[k], (nshvc_line_field)(NSHVC_LINE_SCROLL_X + k), n.hs[k]);
        DIFF(vs[k], (nshvc_line_field)(NSHVC_LINE_SCROLL_Y + k), n.vs[k]);
    }
    for (k = 0; k < 4; k++) {
        if (((n.tm ^ prev.tm) >> k) & 1)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_MAIN + k), (n.tm >> k) & 1);
        if (((n.ts ^ prev.ts) >> k) & 1)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_SUB + k), (n.ts >> k) & 1);
    }
    DIFF(sub, NSHVC_LINE_SUBTRACT, n.sub);
    DIFF(half, NSHVC_LINE_HALF, n.half);
    DIFF(fixed_use, NSHVC_LINE_USE_FIXED, n.fixed_use);
    DIFF(math, NSHVC_LINE_BLEND_LAYERS, n.math);
    DIFF(clip, NSHVC_LINE_CLIP, n.clip);
    DIFF(prevent, NSHVC_LINE_PREVENT, n.prevent);
    for (k = 0; k < 6; k++)
        if (((n.winsel ^ prev.winsel) >> (k * 4)) & 15)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_WIN_USE + k), use_bits((n.winsel >> (k * 4)) & 15));
    for (k = 0; k < 5; k++) {
        if (((n.tmw ^ prev.tmw) >> k) & 1)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_MASK_MAIN + k), (n.tmw >> k) & 1);
        if (((n.tsw ^ prev.tsw) >> k) & 1)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_MASK_SUB + k), (n.tsw >> k) & 1);
    }
    if (n.mos != prev.mos || n.mossize != prev.mossize)
        for (k = 0; k < 4; k++)
            nshvc_line(line, (nshvc_line_field)(NSHVC_LINE_MOSAIC + k), (n.mos >> k) & 1 ? n.mossize : 1);
    if (((n.tm ^ prev.tm) >> 4) & 1) nshvc_line(line, NSHVC_LINE_SPRITES_MAIN, (n.tm >> 4) & 1);
    if (((n.ts ^ prev.ts) >> 4) & 1) nshvc_line(line, NSHVC_LINE_SPRITES_SUB, (n.ts >> 4) & 1);
    DIFF(m7[0], NSHVC_LINE_M7_A, n.m7[0]);
    DIFF(m7[1], NSHVC_LINE_M7_B, n.m7[1]);
    DIFF(m7[2], NSHVC_LINE_M7_C, n.m7[2]);
    DIFF(m7[3], NSHVC_LINE_M7_D, n.m7[3]);
    DIFF(m7[4], NSHVC_LINE_M7_CENTRE_X, n.m7[4]);
    DIFF(m7[5], NSHVC_LINE_M7_CENTRE_Y, n.m7[5]);
    DIFF(m7[6], NSHVC_LINE_M7_SCROLL_X, n.m7[6]);
    DIFF(m7[7], NSHVC_LINE_M7_SCROLL_Y, n.m7[7]);
    prev = n;
}

/* ---- the game ---- */

static void load_assets(void)
{
    static const char sig[] = { kAssets_Sig };
    nshvc_asset a = nshvc_asset_open("zelda3_assets.dat");
    size_t n = 0, cap = 4u << 20;
    uint8_t *d = malloc(cap);
    uint32_t offset, i, got;
    if (!a.id || !d) {
        fprintf(stderr, "zelda3_assets.dat is not in the game's package\n");
        exit(2);
    }
    while ((got = nshvc_asset_read(a, d + n, (uint32_t)(cap - n))) != 0) {
        n += got;
        if (n == cap && !(d = realloc(d, cap *= 2)))
            exit(70);
    }
    nshvc_asset_close(a);
    if (n < 88 || memcmp(d, sig, 48) || *(uint32_t *)(d + 80) != kNumberOfAssets) {
        fprintf(stderr, "zelda3_assets.dat is not valid\n");
        exit(2);
    }
    offset = 88 + kNumberOfAssets * 4 + *(uint32_t *)(d + 84);
    for (i = 0; i < kNumberOfAssets; i++) {
        uint32_t size = *(uint32_t *)(d + 88 + i * 4);
        offset = (offset + 3) & ~3u;
        g_asset_sizes[i] = size;
        g_asset_ptrs[i] = d + offset;
        offset += size;
    }
}

static void init(void)
{
    int i, k;
    static const uint8_t w[4] = { 32, 64, 32, 64 }, h[4] = { 32, 32, 64, 64 };
    load_assets();
    ZeldaInitialize();
    ZeldaSetLanguage(NULL);
    { nshvc_sound_limits l; l.voices = 8; l.sample_bytes = 32u << 20; nshvc_sound_limits_set(&l); }
    for (i = 0; i < 4; i++) {
        t_bg[i] = nshvc_tiles_create(i == 2 ? 2 : 4, 1024, 1);
        for (k = 0; k < 4; k++)
            m_bg[i][k] = nshvc_map_create(w[k], h[k]);
    }
    t_obj = nshvc_tiles_create(4, 512, 1);
    t_m7 = nshvc_tiles_create(8, 256, 1);
    m_m7 = nshvc_map_create(128, 128);
    use_reference = getenv("ZELDA3_REFERENCE") != NULL;
    if (use_reference) {
        ref_buffer = malloc(512 * 260 * 4);
    }
}

/* the game's joypad word: bit 0 B, 1 Y, 2 Select, 3 Start, 4-7 directions, 8 A, 9 X, 10 L, 11 R */
static uint16_t to_game(uint16_t p)
{
    uint16_t r = 0;
    int b;
    for (b = 0; b < 12; b++)
        if (p & (0x8000 >> b))
            r |= (uint16_t)(1 << b);
    return r;
}

static void update(const nshvc_input *in)
{
    pad_now = to_game(in->pad[0]);
    ZeldaRunFrameInternal(pad_now, 3);
    ZeldaPushApuState();
    if (nshvc_updates() == 1 && atomic_load(&busy)) ZeldaApuUnlock();
}

static void write_ref(uint64_t n)
{
    int x, y;
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%06llu.ppm", getenv("ZELDA3_REFERENCE"), (unsigned long long)n);
    f = fopen(path, "wb");
    fprintf(f, "P6\n256 224\n255\n");
    for (y = 0; y < 224; y++)
        for (x = 0; x < 256; x++) {
            const uint8_t *q = ref_buffer + (size_t)y * 512 * 4 + (x + 96) * 4;
            putc(q[2], f); putc(q[1], f); putc(q[0], f);
        }
    fclose(f);
}

static void draw(float between)
{
    (void)between;
    if (use_reference) {
        ZeldaDrawPpuFrame(ref_buffer, 512 * 4, 0);
        if (nshvc_updates() % 50 == 0)
            write_ref(nshvc_updates());
    } else {
        ZeldaDrawPpuFrame(NULL, 0, 0);
    }
}

/* what the game's own front end supplied */
void Die(const char *e) { fprintf(stderr, "die: %s\n", e); exit(3); }
#include "src/util.h"
MemBlk FindInAssetArray(int asset, int idx) { return FindIndexInMemblk((MemBlk){ g_asset_ptrs[asset], g_asset_sizes[asset] }, idx); }
int SDL_GetKeyFromName(const char *n) { (void)n; return 0; }
const char *getProcessorStateCpu(void *s) { (void)s; return ""; }
void *opus_decoder_create(int a, int b, int *e) { (void)a; (void)b; if (e) *e = 1; return NULL; }
int opus_decode(void *d, const void *a, int n, void *o, int f, int g) { (void)d;(void)a;(void)n;(void)o;(void)f;(void)g; return -1; }
int opus_decoder_ctl(void *d, int r, ...) { (void)d;(void)r; return -1; }
void opus_decoder_destroy(void *d) { (void)d; }

/* ---- the game's music code on the engine's voices ---- */
#include <unistd.h>
#include "src/spc_player.h"
#include "snes/dsp.h"
void engine_spc_tick(SpcPlayer *p);
void engine_pop(void);
static uint8_t regs[128];
static atomic_int in_tick;
static atomic_int need_resync;
static int on_audio, ticks;
static nshvc_sample table[256];
#define HZ 500u

void ZeldaApuLock(void)
{
    atomic_store(&busy, 1);
    while (atomic_load(&in_tick))
        usleep(50);
}

static void load_samples(void)
{
    const uint8_t *ram = g_zenv.player->ram;
    const uint32_t dir = (uint32_t)regs[0x5D] << 8;
    int e;
    for (e = 0; e < 256; e++) {
        uint32_t st = ram[(dir + e * 4) & 0xffff] | ram[(dir + e * 4 + 1) & 0xffff] << 8;
        uint32_t lp = ram[(dir + e * 4 + 2) & 0xffff] | ram[(dir + e * 4 + 3) & 0xffff] << 8;
        uint32_t at = st, n = 0;
        table[e].id = 0;
        if (st < 0x200 || st > 0xfff0)
            continue;
        while (at + 9 <= 0x10000 && n < 7000) {
            n++;
            if (ram[at] & 1)
                break;
            at += 9;
        }
        if (lp < st || lp >= st + n * 9)
            lp = st;
        table[e] = nshvc_sample_load(ram + st, n * 9, NSHVC_SAMPLE_BRR, lp - st);
    }
}

static int uploaded = 1;
void engine_uploaded(void) { uploaded = 1; }

void ZeldaApuUnlock(void)
{
    if (uploaded) {
        load_samples();
        atomic_store(&need_resync, 1);
        uploaded = 0;
    }
    atomic_store(&busy, 0);
}

static void apply_voice(int v)
{
    const uint8_t *r = regs + v * 16;
    nshvc_voice_volume((uint32_t)v, (int8_t)r[0], (int8_t)r[1]);
    nshvc_voice_pitch((uint32_t)v, (uint16_t)((r[2] | r[3] << 8) & 0x3fff));
    if (r[5] & 0x80) {
        nshvc_voice_adsr((uint32_t)v, r[5] & 15, (r[5] >> 4) & 7, r[6] >> 5, r[6] & 31);
    } else if (r[7] & 0x80) {
        nshvc_voice_gain((uint32_t)v, (nshvc_gain_mode)((r[7] >> 5) & 7), r[7] & 31);
    } else {
        nshvc_voice_gain((uint32_t)v, NSHVC_GAIN_DIRECT, r[7] & 0x7f);
    }
    nshvc_voice_options((uint32_t)v, (regs[0x3D] >> v) & 1, (regs[0x4D] >> v) & 1, v ? (regs[0x2D] >> v) & 1 : 0);
}

static void apply_echo(void)
{
    int8_t f[8];
    int i;
    for (i = 0; i < 8; i++)
        f[i] = (int8_t)regs[0x0F + i * 16];
    nshvc_echo(regs[0x7D] & 15, (int8_t)regs[0x0D], (int8_t)regs[0x2C], (int8_t)regs[0x3C], f);
}

static void apply_flags(void)
{
    nshvc_sound_mute((regs[0x6C] & 0x40) != 0);
    nshvc_sound_noise_rate(regs[0x6C] & 31);
}

void engine_dsp(uint8_t reg, uint8_t value)
{
    const int low = reg & 15, v = reg >> 4;
    int i;
    if (reg >= 128)
        return;
    regs[reg] = value;
    if (!on_audio)
        return;
    if (low <= 7 && low != 4) {
        apply_voice(v);
        return;
    }
    switch (reg) {
    case 0x0C: case 0x1C: nshvc_sound_volume((int8_t)regs[0x0C], (int8_t)regs[0x1C]); break;
    case 0x4C:
        for (i = 0; i < 8; i++)
            if (value >> i & 1) {
                apply_voice(i);
                if (table[regs[i * 16 + 4]].id)
                    nshvc_voice_key_on((uint32_t)i, table[regs[i * 16 + 4]]);
            }
        break;
    case 0x5C:
        for (i = 0; i < 8; i++)
            if (value >> i & 1)
                nshvc_voice_key_off((uint32_t)i);
        break;
    case 0x6C: apply_flags(); break;
    case 0x2D: case 0x3D: case 0x4D:
        for (i = 0; i < 8; i++)
            apply_voice(i);
        break;
    default:
        if (low == 15 || reg == 0x0D || reg == 0x2C || reg == 0x3C || reg == 0x7D)
            apply_echo();
    }
}

static void music_tick(void)
{
    int i;
    if (atomic_load(&busy))
        return;
    atomic_store(&in_tick, 1);
    if (atomic_load(&busy)) {
        atomic_store(&in_tick, 0);
        return;
    }
    on_audio = 1;
    if (atomic_exchange(&need_resync, 0)) {
        for (i = 0; i < 8; i++) {
            nshvc_voice_key_off((uint32_t)i);
            apply_voice(i);
        }
        nshvc_sound_volume((int8_t)regs[0x0C], (int8_t)regs[0x1C]);
        apply_echo();
        apply_flags();
    }
    if (ticks % 8 == 0)
        engine_pop();
    engine_spc_tick(g_zenv.player);
    ticks++;
    on_audio = 0;
    atomic_store(&in_tick, 0);
}

/* whether the game's own line renderer draws: zelda3.patch asks */
int engine_ref(void) { return use_reference; }

int main(int argc, char **argv)
{
    static nshvc_game g;
    g.id = "z3";
    g.title = "zelda3 on nativeshvc";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.music.hz = HZ;
    g.music.tick = music_tick;
    return nshvc_run(&g, argc, argv);
}
