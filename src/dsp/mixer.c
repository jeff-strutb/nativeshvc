/*
 * mixer.c: the sound, native (mixer.h).
 *
 * Written from Nocash's fullsnes ("SNES APU DSP" chapters) and checked
 * against the reference's sound chip by mix_replay (tests/dsp), which feeds
 * this mixer the chip's inputs at the chip's clocks and compares every
 * frame. Where fullsnes is silent the behaviour here is what that check
 * measured, and says so.
 *
 * A frame is 32 clocks, one an SPC700 cycle, and the chip does its work at
 * fixed clocks of it: each voice in nine steps (step_clock below), the
 * echo over clocks 22-30, the shared settings taken at 27-30. A voice
 * reads its settings, and the sound memory, at the clock of the step that
 * uses them, so a change the game makes between two clocks reaches the
 * steps after it and not those before, as on the console. Values are the
 * chip's: decoded samples 15 bits signed, envelope levels 11 bits, sums 16
 * bits.
 */
#include <string.h>

#include "mixer.h"

enum { RELEASE, ATTACK, DECAY, SUSTAIN };

typedef struct {
    /* what the game set (read at the clocks the steps use them) */
    int8_t vol_l, vol_r;
    uint16_t pitch;
    uint8_t entry;
    int adsr, attack, decay, sustain_level, sustain_rate;
    uint8_t gain;
    /* taken by the steps a few clocks before they are used */
    uint8_t srcn;            /* the directory entry (step 1) */
    uint16_t dir_addr;       /* its address (worked out at the next voice's step 1) */
    uint16_t next_block;     /* the entry's start or loop address (step 2) */
    int adsr_on, adsr_attack, adsr_decay;   /* ADSR's first byte (step 2) */
    int step_pitch;          /* the pitch, low byte at step 2, high at 3 */
    uint8_t header, data;    /* the block's header and the byte pair's first (step 3) */
    int looped;              /* the block just ended with a loop (step 4, for ENDX) */
    uint8_t level_out;       /* the level ENVX will be given */
    /* playing */
    int16_t hist[24];        /* the last 12 decoded values, twice (no wrap when read) */
    int hist_next;           /* where the next four go: 0, 4 or 8 */
    int pos;                 /* the pitch counter: bits 12-13 the value, 4-11 the Gaussian index */
    uint16_t block;          /* the BRR block being decoded */
    int byte;                /* the next pair of bytes in it: 1, 3, 5, 7 */
    int kon_delay;           /* the frames left of a key-on's start (5 to 0) */
    int stage, level;
    int bent;                /* the last level computed (bent increase) */
    int out;                 /* this frame's output, 15 bits (pitch modulation, OUTX) */
    /* what the game reads back */
    uint8_t envx, outx;
} Voice;

static uint8_t *mem;
static Voice voice[8];
static uint8_t dir_page, echo_page, echo_len_set;
static int8_t mvol_l, mvol_r, evol_l, evol_r, efb, fir[8];
static uint8_t pmon, non, eon, released;
static int reset_held, muted, echo_off, noise_rate;
static int counter, every_other;
static int noise;                         /* 15 bits */
static int phase;                         /* the clock to run next, 0-31 */
/* the shared settings as taken at clocks 27-30 */
static uint8_t pmon_now, non_now, eon_now, dir_now, koff_now, kon_now, kon_pending;
/* ENDX, and the values ENDX, OUTX and ENVX are next given (a write to one
   1-2 clocks before is overwritten) */
static uint8_t endx, endx_next, outx_next, envx_next;
static int main_sum[2], echo_sum[2], echo_in[2];
static int echo_hist[8][2], echo_at;
static int echo_index, echo_length, echo_esa, echo_ptr, echo_writes_off;
static int16_t made_l, made_r;
static int made;

/* The chip's Gaussian interpolation table, as fullsnes gives it: gauss[i]
   weighs a value (511 - i) / 256 values away; each position's four
   weights sum to 2047-2049. */
static const int16_t gauss[512] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10,
    11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 15, 16, 16, 17, 17,
    18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24, 24, 25, 26, 27, 27,
    28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 36, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
    58, 59, 60, 61, 62, 64, 65, 66, 67, 69, 70, 71, 73, 74, 76, 77,
    78, 80, 81, 83, 84, 86, 87, 89, 90, 92, 94, 95, 97, 99, 100, 102,
    104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
    134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
    171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
    212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
    260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
    314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
    374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
    439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
    508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
    582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
    659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
    737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
    816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
    894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
    969, 974, 978, 983, 988, 992, 997, 1001, 1005, 1010, 1014, 1019, 1023, 1027, 1032, 1036,
    1040, 1045, 1049, 1053, 1057, 1061, 1066, 1070, 1074, 1078, 1082, 1086, 1090, 1094, 1098, 1102,
    1106, 1109, 1113, 1117, 1121, 1125, 1128, 1132, 1136, 1139, 1143, 1146, 1150, 1153, 1157, 1160,
    1164, 1167, 1170, 1174, 1177, 1180, 1183, 1186, 1190, 1193, 1196, 1199, 1202, 1205, 1207, 1210,
    1213, 1216, 1219, 1221, 1224, 1227, 1229, 1232, 1234, 1237, 1239, 1241, 1244, 1246, 1248, 1251,
    1253, 1255, 1257, 1259, 1261, 1263, 1265, 1267, 1269, 1270, 1272, 1274, 1275, 1277, 1279, 1280,
    1282, 1283, 1284, 1286, 1287, 1288, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1297, 1298,
    1299, 1300, 1300, 1301, 1302, 1302, 1303, 1303, 1303, 1304, 1304, 1304, 1304, 1304, 1305, 1305,
};

/* ---- rates ---------------------------------------------------------------------------
   fullsnes: a 5-bit rate is a step every N frames, N one of 1, 3 or 5
   times a power of two (rate 0 never). The chip drives them from one
   counter of 30,720 frames, which counts down at clock 30 from 0 at reset;
   each family of rates steps at its own phase of it (the powers of two at
   0, the 3s at 1,040 frames, the 5s at 536: measured by mix_replay). */
#define COUNTER_PERIOD 30720
static const uint16_t rate_frames[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

static int rate_due(int rate)
{
    unsigned n = rate_frames[rate], phase;
    if (!n)
        return 0;
    phase = n % 3 == 0 ? 1040 : n % 5 == 0 ? 536 : 0;
    return ((unsigned)counter + phase) % n == 0;
}

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static int wrap15(int v) { return (int16_t)(v << 1) >> 1; }
static int rd16(uint16_t at) { return mem[at] | mem[(uint16_t)(at + 1)] << 8; }

/* ---- setup ----------------------------------------------------------------------------- */
void mix_init(uint8_t *memory)
{
    int v;
    mem = memory;
    memset(voice, 0, sizeof voice);
    for (v = 0; v < 8; v++)
        voice[v].byte = 1;
    dir_page = echo_page = echo_len_set = 0;
    mvol_l = mvol_r = evol_l = evol_r = efb = 0;
    memset(fir, 0, sizeof fir);
    pmon = non = eon = released = 0;
    /* fullsnes: after reset, released, muted, echo writes off, noise stopped */
    reset_held = muted = echo_off = 1;
    noise_rate = 0;
    counter = 0;
    every_other = 1;
    noise = 0x4000;                        /* -4000h */
    phase = 0;
    pmon_now = non_now = eon_now = dir_now = koff_now = kon_now = kon_pending = 0;
    endx = endx_next = outx_next = envx_next = 0;
    memset(main_sum, 0, sizeof main_sum);
    memset(echo_sum, 0, sizeof echo_sum);
    memset(echo_in, 0, sizeof echo_in);
    memset(echo_hist, 0, sizeof echo_hist);
    echo_at = 0;
    echo_index = echo_length = echo_esa = echo_ptr = echo_writes_off = 0;
    made = 0;
}

void mix_voice_volume(int v, int8_t left, int8_t right) { voice[v & 7].vol_l = left; voice[v & 7].vol_r = right; }
void mix_voice_pitch(int v, uint16_t pitch) { voice[v & 7].pitch = pitch & 0x3FFF; }
void mix_voice_sample(int v, uint8_t entry) { voice[v & 7].entry = entry; }

void mix_voice_envelope(int v, int adsr, int attack, int decay, int sustain_level, int sustain_rate)
{
    Voice *vc = &voice[v & 7];
    vc->adsr = adsr != 0;
    vc->attack = attack & 15;
    vc->decay = decay & 7;
    vc->sustain_level = sustain_level & 7;
    vc->sustain_rate = sustain_rate & 31;
}

void mix_voice_gain(int v, uint8_t gain) { voice[v & 7].gain = gain; }
void mix_key_on(uint8_t voices) { kon_pending = voices; }
void mix_key_off(uint8_t voices) { released = voices; }
void mix_pitch_modulation(uint8_t voices) { pmon = voices; }
void mix_noise(uint8_t voices) { non = voices; }
void mix_echo_voices(uint8_t voices) { eon = voices; }
void mix_master_volume(int8_t left, int8_t right) { mvol_l = left; mvol_r = right; }

void mix_control(int reset, int mute, int echo_writes_off, int rate)
{
    reset_held = reset != 0;
    muted = mute != 0;
    echo_off = echo_writes_off != 0;
    noise_rate = rate & 31;
}

void mix_directory(uint8_t page) { dir_page = page; }
void mix_echo_buffer(uint8_t page, uint8_t length) { echo_page = page; echo_len_set = length & 15; }
void mix_echo_volume(int8_t left, int8_t right) { evol_l = left; evol_r = right; }
void mix_echo_feedback(int8_t feedback) { efb = feedback; }
void mix_echo_filter(int tap, int8_t coefficient) { fir[tap & 7] = coefficient; }

uint8_t mix_voices_ended(void) { return endx; }
/* any write to ENDX clears it */
void mix_clear_ended(void) { endx = endx_next = 0; }
uint8_t mix_voice_level(int v) { return voice[v & 7].envx; }
int8_t mix_voice_output(int v) { return (int8_t)voice[v & 7].outx; }
/* a write to ENVX or OUTX: kept until the voice's step gives it its own */
void mix_voice_level_write(int v, uint8_t value) { voice[v & 7].envx = envx_next = value; }
void mix_voice_output_write(int v, uint8_t value) { voice[v & 7].outx = outx_next = value; }
int mix_phase(void) { return phase; }

void mix_voice_states(int32_t *o)
{
    int i;
    for (i = 0; i < 8; i++) {
        o[5 * i] = voice[i].level;
        o[5 * i + 1] = voice[i].stage;
        o[5 * i + 2] = voice[i].pos;
        o[5 * i + 3] = voice[i].block;
        o[5 * i + 4] = voice[i].byte;
    }
}

/* ---- samples ---------------------------------------------------------------------------- */
/* four more values of the voice's sample: one byte pair of its block,
   rebuilt with the block's shift and prediction filter (fullsnes, BRR);
   the header and the pair's first byte as step 3 read them */
static void decode(Voice *vc)
{
    int shift = vc->header >> 4, filter = (vc->header >> 2) & 3, i;
    unsigned bits = (unsigned)vc->data << 8 | mem[(uint16_t)(vc->block + vc->byte + 1)];
    for (i = 0; i < 4; i++) {
        int k = vc->hist_next + i;
        int old = vc->hist[k + 11], older = vc->hist[k + 10];
        int nib = (int16_t)(bits << (i * 4)) >> 12, s;
        s = shift <= 12 ? (nib << shift) >> 1 : (nib >> 3) << 11;
        switch (filter) {
        case 1: s += old + ((-old) >> 4); break;
        case 2: s += 2 * old + ((-old * 3) >> 5) - older + (older >> 4); break;
        case 3: s += 2 * old + ((-old * 13) >> 6) - older + ((older * 3) >> 4); break;
        }
        s = wrap15(clamp16(s));
        vc->hist[k] = (int16_t)s;
        vc->hist[k + 12] = (int16_t)s;
    }
    /* the history is read as twelve values ending before hist_next: the
       slot before index 0 is index 11 */
    vc->hist_next = (vc->hist_next + 4) % 12;
}

/* fullsnes, Gaussian interpolation: the newest four values at the pitch
   counter's fraction; the first three sums wrap at 16 bits, the last
   saturates; 15 bits out */
static int interpolate(const Voice *vc)
{
    int i = (vc->pos >> 4) & 0xFF;
    const int16_t *h = &vc->hist[((vc->pos >> 12) + vc->hist_next) % 12];
    int out = (gauss[0xFF - i] * h[0]) >> 10;
    out += (gauss[0x1FF - i] * h[1]) >> 10;
    out += (gauss[0x100 + i] * h[2]) >> 10;
    out = (int16_t)out;
    out += (gauss[i] * h[3]) >> 10;
    return clamp16(out) >> 1;
}

/* the envelope's next level (fullsnes, ADSR/Gain): ADSR's first byte as
   step 2 took it, its second and the gain as they are now. The stage moves
   on from the level computed even on a frame its rate does not step (as
   measured): attack ends past 7FFh, decay at the sustain level's top byte
   (in gain mode the gain's top bits, fullsnes's noted quirk). */
static void envelope(Voice *vc)
{
    int level = vc->level, rate, top;
    if (vc->stage == RELEASE) {
        level -= 8;
        vc->level = level < 0 ? 0 : level;
        return;
    }
    if (vc->adsr_on) {
        top = vc->sustain_level;
        if (vc->stage == ATTACK) {
            rate = vc->adsr_attack * 2 + 1;
            level += rate == 31 ? 0x400 : 0x20;
        } else {
            rate = vc->stage == DECAY ? vc->adsr_decay * 2 + 16 : vc->sustain_rate;
            level -= ((level - 1) >> 8) + 1;
        }
    } else {
        int mode = vc->gain >> 5;
        top = vc->gain >> 5;
        if (!(vc->gain & 0x80)) {          /* direct: a fixed level */
            level = vc->gain * 16;
            rate = 31;
        } else {
            rate = vc->gain & 31;
            if (mode == 4)
                level -= 0x20;
            else if (mode == 5)
                level -= ((level - 1) >> 8) + 1;
            else                           /* the bent line: a last level below 0 counts as high */
                level += mode == 7 && (unsigned)vc->bent >= 0x600 ? 8 : 0x20;
        }
    }
    if (vc->stage == DECAY && (level >> 8) == top)
        vc->stage = SUSTAIN;
    vc->bent = level;
    if (level < 0 || level > 0x7FF) {
        level = level < 0 ? 0 : 0x7FF;
        if (vc->stage == ATTACK)
            vc->stage = DECAY;
    }
    if (rate_due(rate))
        vc->level = level;
}

/* ---- a voice's nine steps ---------------------------------------------------------------
   fullsnes's DSP timing chart, the steps named by what each does. */

/* 1: the voice's directory entry taken; the voice before it, which took
   its own a step 1 ago, has its entry's address worked out (from the
   directory page as taken at clock 28) */
static void step_entry(int v)
{
    Voice *before = &voice[(v + 7) & 7];
    /* added: an entry past 63 carries into the page */
    before->dir_addr = (uint16_t)((dir_now << 8) + (before->srcn << 2));
    voice[v].srcn = voice[v].entry;
}

/* 2: the entry read (its start while a key-on is under way, else its
   loop), ADSR's first byte and the pitch's low byte taken */
static void step_directory(int v)
{
    Voice *vc = &voice[v];
    vc->next_block = (uint16_t)rd16((uint16_t)(vc->dir_addr + (vc->kon_delay ? 0 : 2)));
    vc->adsr_on = vc->adsr;
    vc->adsr_attack = vc->attack;
    vc->adsr_decay = vc->decay;
    vc->step_pitch = vc->pitch & 0xFF;
}

/* 3, in three parts (voice 0's on clocks of their own): the pitch's high
   bits; the block's header and the byte pair's first byte; the sample */
static void step_pitch_high(int v) { voice[v].step_pitch += voice[v].pitch & 0x3F00; }

static void step_fetch(int v)
{
    Voice *vc = &voice[v];
    vc->data = mem[(uint16_t)(vc->block + vc->byte)];
    vc->header = mem[vc->block];
}

static void step_sample(int v)
{
    Voice *vc = &voice[v];
    int s;
    /* pitch modulation by the voice before's output, made this frame */
    if (pmon_now >> v & 1)
        vc->step_pitch += ((voice[(v + 7) & 7].out >> 4) * vc->step_pitch) >> 10;
    if (vc->kon_delay) {
        /* a key-on's five frames: the first sets the voice on its sample's
           start (this frame's header counts for nothing), the next three
           decode its first three byte pairs; no pitch, no envelope */
        if (vc->kon_delay == 5) {
            vc->block = vc->next_block;
            vc->byte = 1;
            vc->hist_next = 0;
            vc->header = 0;
        }
        vc->level = vc->bent = 0;
        vc->pos = --vc->kon_delay & 3 ? 0x4000 : 0;
        vc->step_pitch = 0;
    }
    s = (non_now >> v & 1) ? (int16_t)(noise << 1) >> 1 : interpolate(vc);
    vc->out = (s * vc->level) >> 11;
    vc->level_out = (uint8_t)(vc->level >> 4);
    /* cut at once: the reset flag, or a block that ends with no loop */
    if (reset_held || (vc->header & 3) == 1) {
        vc->stage = RELEASE;
        vc->level = 0;
    }
    /* fullsnes: key-on and release are taken every other frame */
    if (every_other) {
        if (koff_now >> v & 1)
            vc->stage = RELEASE;
        if (kon_now >> v & 1) {
            vc->kon_delay = 5;
            vc->stage = ATTACK;
        }
    }
    if (!vc->kon_delay)
        envelope(vc);
}

/* fullsnes, output mixer: the sample times the volume, SAR 6, the sums
   saturating at 16 bits; the echo's sum as EON was taken at clock 28 */
static void output(const Voice *vc, int v, int ch)
{
    int amp = (vc->out * (ch ? vc->vol_r : vc->vol_l)) >> 6;
    main_sum[ch] = clamp16(main_sum[ch] + amp);
    if (eon_now >> v & 1)
        echo_sum[ch] = clamp16(echo_sum[ch] + amp);
}

/* 4: the byte pair decoded when the pitch counter has passed four values,
   the block moved on at its end; the pitch added; the left output */
static void step_decode(int v)
{
    Voice *vc = &voice[v];
    vc->looped = 0;
    if (vc->pos >= 0x4000) {
        decode(vc);
        vc->byte += 2;
        if (vc->byte >= 9) {
            vc->block = (uint16_t)(vc->block + 9);
            if (vc->header & 1) {          /* the end: on at the loop address */
                vc->block = vc->next_block;
                vc->looped = 1;
            }
            vc->byte = 1;
        }
    }
    vc->pos = (vc->pos & 0x3FFF) + vc->step_pitch;
    if (vc->pos > 0x7FFF)
        vc->pos = 0x7FFF;
    output(vc, v, 0);
}

/* 5: the right output; ENDX's next value (the voice's bit set on a loop,
   cleared as a key-on starts) */
static void step_right(int v)
{
    Voice *vc = &voice[v];
    output(vc, v, 1);
    endx_next = (uint8_t)((endx | vc->looped << v) & ~(vc->kon_delay == 5 ? 1u << v : 0u));
}

/* 6-9: OUTX's and ENVX's next values, then ENDX, OUTX and ENVX set */
static void step_outx(int v) { outx_next = (uint8_t)(voice[v].out >> 7); }
static void step_endx(int v)
{
    endx = endx_next;
    envx_next = voice[v].level_out;
}
static void step_set_outx(int v) { voice[v].outx = outx_next; }
static void step_set_envx(int v) { voice[v].envx = envx_next; }

enum { ENTRY, DIRECTORY, PITCH_HIGH, FETCH, SAMPLE, DECODE, RIGHT, OUTX, ENDX, SET_OUTX, SET_ENVX, STEPS };

static void (*const step_fn[STEPS])(int) = {
    step_entry, step_directory, step_pitch_high, step_fetch, step_sample, step_decode,
    step_right, step_outx, step_endx, step_set_outx, step_set_envx,
};

/* the clock of each step, voice by voice (fullsnes's timing chart): a
   voice's steps 2 to 9 on consecutive clocks, three voices in hand at
   once, voice 0's step 3 spread out round the echo's clocks */
static const uint8_t step_clock[STEPS][8] = {
    [ENTRY] = { 17, 20, 31, 2, 5, 8, 11, 14 },
    [DIRECTORY] = { 21, 0, 3, 6, 9, 12, 15, 18 },
    [PITCH_HIGH] = { 22, 1, 4, 7, 10, 13, 16, 19 },
    [FETCH] = { 25, 1, 4, 7, 10, 13, 16, 19 },
    [SAMPLE] = { 30, 1, 4, 7, 10, 13, 16, 19 },
    [DECODE] = { 31, 2, 5, 8, 11, 14, 17, 20 },
    [RIGHT] = { 0, 3, 6, 9, 12, 15, 18, 21 },
    [OUTX] = { 1, 4, 7, 10, 13, 16, 19, 22 },
    [ENDX] = { 2, 5, 8, 11, 14, 17, 20, 23 },
    [SET_OUTX] = { 3, 6, 9, 12, 15, 18, 21, 24 },
    [SET_ENVX] = { 4, 7, 10, 13, 16, 19, 22, 25 },
};

/* each clock's steps, built from step_clock: up to five, of up to three
   voices (each a step << 3 | voice), in step order */
static uint8_t clock_steps[32][6];
static int clock_steps_n[32] = { -1 };

static void build_clock_steps(void)
{
    int c, s, v;
    for (c = 0; c < 32; c++)
        clock_steps_n[c] = 0;
    for (s = 0; s < STEPS; s++)
        for (v = 0; v < 8; v++) {
            c = step_clock[s][v];
            clock_steps[c][clock_steps_n[c]++] = (uint8_t)(s << 3 | v);
        }
}

/* ---- the echo, clocks 22-30 -------------------------------------------------------------
   fullsnes, echo: the oldest entry out of the delay line, through the
   8-tap filter (the oldest seven summed without saturation, the newest
   with), mixed in at the echo volume; the echo voices and the feedback
   written back in its place. Each setting is read at the clock that uses
   it. */
static int tap(int i, int ch) { return (echo_hist[(echo_at + 1 + i) & 7][ch] * fir[i]) >> 6; }

static void echo_read(int ch)
{
    echo_hist[echo_at][ch] = (int16_t)rd16((uint16_t)(echo_ptr + ch * 2)) >> 1;
}

/* the main sum at the master volume and the filtered echo at the echo
   volume, each term 16 bits, the sum saturating */
static int final(int ch)
{
    int m = (int16_t)((main_sum[ch] * (ch ? mvol_r : mvol_l)) >> 7);
    int e = (int16_t)((echo_in[ch] * (ch ? evol_r : evol_l)) >> 7);
    return clamp16(m + e);
}

static void echo_write(int ch)
{
    if (!echo_writes_off) {
        mem[(uint16_t)(echo_ptr + ch * 2)] = (uint8_t)echo_sum[ch];
        mem[(uint16_t)(echo_ptr + ch * 2 + 1)] = (uint8_t)(echo_sum[ch] >> 8);
    }
    echo_sum[ch] = 0;
}

static void echo_clock(int c)
{
    int ch;
    switch (c) {
    case 22:                              /* the left's newest entry, tap 0 */
        echo_at = (echo_at + 1) & 7;
        echo_ptr = (uint16_t)((echo_esa << 8) + echo_index);
        echo_read(0);
        for (ch = 0; ch < 2; ch++)
            echo_in[ch] = tap(0, ch);
        break;
    case 23:                              /* taps 1-2, the right's newest entry */
        for (ch = 0; ch < 2; ch++)
            echo_in[ch] += tap(1, ch) + tap(2, ch);
        echo_read(1);
        break;
    case 24:
        for (ch = 0; ch < 2; ch++)
            echo_in[ch] += tap(3, ch) + tap(4, ch) + tap(5, ch);
        break;
    case 25:                              /* tap 6 wraps, the newest saturates */
        for (ch = 0; ch < 2; ch++) {
            int s = (int16_t)(echo_in[ch] + tap(6, ch));
            echo_in[ch] = clamp16(s + (int16_t)tap(7, ch)) & ~1;
        }
        break;
    case 26:                              /* the left out (held a clock); the feedback */
        made_l = (int16_t)final(0);
        for (ch = 0; ch < 2; ch++)
            echo_sum[ch] = clamp16(echo_sum[ch] + (int16_t)((echo_in[ch] * efb) >> 7)) & ~1;
        break;
    case 27:                              /* the right out: the frame made */
        made_r = (int16_t)final(1);
        main_sum[0] = main_sum[1] = 0;
        if (muted)
            made_l = made_r = 0;
        made = 1;
        break;
    case 28:
        echo_writes_off = echo_off;
        break;
    case 29:                              /* the next entry; the left written */
        echo_esa = echo_page;
        /* the length is taken when the index comes round to 0 */
        if (!echo_index)
            echo_length = echo_len_set * 0x800;
        echo_index += 4;
        if (echo_index >= echo_length)
            echo_index = 0;
        echo_write(0);
        echo_writes_off = echo_off;
        break;
    case 30:
        echo_write(1);
        break;
    }
}

/* ---- the shared settings, clocks 27-30 -------------------------------------------------- */
static void shared_clock(int c)
{
    switch (c) {
    case 27:
        pmon_now = pmon & 0xFE;           /* voice 0 has no voice before it */
        break;
    case 28:
        non_now = non;
        eon_now = eon;
        dir_now = dir_page;
        break;
    case 29:
        /* a key-on is cleared from KON's latch 63 clocks after it is taken */
        if ((every_other ^= 1) != 0)
            kon_pending &= (uint8_t)~kon_now;
        break;
    case 30:
        if (every_other) {
            kon_now = kon_pending;
            koff_now = released;
        }
        if (--counter < 0)
            counter = COUNTER_PERIOD - 1;
        if (rate_due(noise_rate))
            noise = ((noise >> 1) & 0x3FFF) | (((noise ^ (noise >> 1)) & 1) << 14);
        break;
    }
}

int mix_clock(int16_t *out_l, int16_t *out_r)
{
    int c = phase, i;
    if (clock_steps_n[0] < 0)
        build_clock_steps();
    if (c >= 27)
        shared_clock(c);
    for (i = 0; i < clock_steps_n[c]; i++)
        step_fn[clock_steps[c][i] >> 3](clock_steps[c][i] & 7);
    if (c >= 22)
        echo_clock(c);
    phase = (c + 1) & 31;
    if (!made)
        return 0;
    made = 0;
    *out_l = made_l;
    *out_r = made_r;
    return 1;
}
