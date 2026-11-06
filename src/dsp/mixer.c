/*
 * mixer.c: the sound, native (mixer.h).
 *
 * Written from Nocash's fullsnes ("SNES APU DSP" chapters) and checked
 * against the reference's sound chip by mix_replay (tests/dsp), which feeds
 * this mixer the chip's inputs at the chip's clocks and compares every
 * sample. Where fullsnes is silent the behaviour here is what that check
 * measured, and says so.
 *
 * A sample is made whole, not clock by clock. The chip takes 32 of its clocks
 * to a sample (one an SPC700 cycle) and reads each setting and the sound
 * memory at a fixed clock: the at_ tables below. Counted from clock 28, where
 * the last sample has just come out, those clocks fall in one order: the
 * echo's writes, the voices from 0 to 7, the echo's reads and the sample's
 * output. So a setting written at a clock reaches this sample if its read is
 * still to come, and the next if not (a value already read stays as it was
 * until the sample ends), and the sample is worked out from those values at
 * its end, voice by voice. Values are the chip's: decoded samples 15 bits
 * signed, envelope levels 11 bits, sums 16 bits.
 *
 * Something may need part of a sample before its end: a read of ENDX, ENVX or
 * OUTX, or a write of the sound memory the voices read. Then the voices whose
 * reads have all passed are worked out at once, by the same tables, and the
 * rest at the end.
 */
#include <string.h>

#include "mixer.h"

enum { RELEASE, ATTACK, DECAY, SUSTAIN };

typedef struct {
    /* taken by the steps a few clocks before they are used */
    uint8_t srcn;            /* the directory entry */
    uint16_t dir_addr;       /* its address (worked out by the next voice's entry) */
    uint16_t next_block;     /* the entry's start or loop address */
    int adsr_on, adsr_attack, adsr_decay;   /* ADSR's first byte */
    int step_pitch;          /* the pitch this sample */
    uint8_t header, data, second;   /* the block's header and the byte pair */
    int looped;              /* the block just ended with a loop (for ENDX) */
    uint8_t level_out;       /* the level ENVX will be given */
    /* playing */
    int16_t hist[24];        /* the last 12 decoded values, twice (no wrap when read) */
    int hist_next;           /* where the next four go: 0, 4 or 8 */
    int pos;                 /* the pitch counter: bits 12-13 the value, 4-11 the Gaussian index */
    uint16_t block;          /* the BRR block being decoded */
    int byte;                /* the next pair of bytes in it: 1, 3, 5, 7 */
    int kon_delay;           /* the samples left of a key-on's start (5 to 0) */
    int stage, level;
    int bent;                /* the last level computed (bent increase) */
    int out;                 /* this sample's output, 15 bits (pitch modulation, OUTX) */
    /* what the game reads back, and what this sample's end has it become */
    uint8_t envx, outx, envx_new, outx_new;
} __attribute__((aligned(256))) Voice;   /* (256 bytes: a voice's address a shift of its number) */

/* What the game set: `now` as this sample's reads see it, `next` the latest,
   which `now` becomes when the sample ends */
typedef struct {
    int8_t vol_l, vol_r;
    uint8_t pitch_lo, pitch_hi, entry;
    uint8_t adsr, attack, decay, sustain_level, sustain_rate, gain;
} VoiceSettings;

typedef struct {
    VoiceSettings v[8];
    int8_t mvol_l, mvol_r, evol_l, evol_r, efb, fir[8];
    uint8_t pmon, non, eon, dir_page, echo_page, echo_len, reset, muted, echo_off_28, echo_off_29, noise_rate;
} Settings;

static Settings now, next;
static int deferred;                      /* a write that waits for the sample's end */

/* The chip's read clocks, counted from clock 28 (the chip's clock is this +
   28, mod 32), fullsnes's timing chart as measured; voices 0-7. A write made
   at `rel` lands before that clock's work. */
static const uint8_t at_entry[8] = { 21, 24, 3, 6, 9, 12, 15, 18 };      /* SRCN */
static const uint8_t at_dir[8] = { 25, 4, 7, 10, 13, 16, 19, 22 };       /* the directory, ADSR1, the pitch's low byte */
static const uint8_t at_hi[8] = { 26, 5, 8, 11, 14, 17, 20, 23 };        /* the pitch's high byte */
static const uint8_t at_fetch[8] = { 29, 5, 8, 11, 14, 17, 20, 23 };     /* the block's header, the pair's first byte */
static const uint8_t at_sample[8] = { 2, 5, 8, 11, 14, 17, 20, 23 };     /* ADSR2, GAIN, FLG's reset */
static const uint8_t at_left[8] = { 3, 6, 9, 12, 15, 18, 21, 24 };       /* VOLL, the pair's second byte */
static const uint8_t at_right[8] = { 4, 7, 10, 13, 16, 19, 22, 25 };     /* VOLR: the voice's last read */
static const uint8_t at_outx_next[8] = { 5, 8, 11, 14, 17, 20, 23, 26 }; /* the value OUTX will be given */
static const uint8_t at_endx[8] = { 6, 9, 12, 15, 18, 21, 24, 27 };      /* ENDX set; the value ENVX will be given */
static const uint8_t at_outx[8] = { 7, 10, 13, 16, 19, 22, 25, 28 };     /* OUTX set */
static const uint8_t at_envx[8] = { 8, 11, 14, 17, 20, 23, 26, 29 };     /* ENVX set */
static const uint8_t at_tap[8] = { 26, 27, 27, 28, 28, 28, 29, 29 };     /* the echo's filter */
/* the shared settings */
enum {
    AT_DIR_NON_EON = 0,                   /* and FLG's echo bit, first */
    AT_ESA_EDL = 1,                       /* and FLG's echo bit, again; KON's latch cleared */
    AT_KEYS_NOISE = 2,                    /* KON and KOFF taken; the noise's rate */
    AT_EFB_LEFT = 30,                     /* EFB, MVOLL, EVOLL */
    AT_PMON_RIGHT = 31,                   /* PMON, MVOLR, EVOLR, FLG's mute */
};
/* the echo's reads of the sound memory, and voice 0's last two of a sample */
static const uint8_t at_head[4] = { 25, 26, 27, 29 };

/* A sample's work, in the order of those clocks */
enum {
    S_28, S_29, S_30, S_31,               /* the shared settings and the echo's writes, voice 0's sample and left */
    S_VOICES,                             /* voice 0's right; each next one a voice's whole sample, 1-7 */
    S_END = S_VOICES + 8,                 /* voice 0's next reads; the echo's filter; the sample's output */
    S_DONE
};

static int rel;                           /* the clock to run next, counted from clock 28: 0-31 */
static int sect;                          /* the sample's work done so far (S_) */
static int rd;                            /* the voice in hand's reads of the memory taken */
static int ram_seen;                      /* the clock of the last write to the memory */
static int outx_set, envx_set;            /* the voices whose OUTX, ENVX have been given theirs, this sample */

static uint8_t *mem;
static Voice voice[8];
static uint8_t released;
static int counter, every_other;
static int c3, c5;                        /* (counter + 1,040) mod 3 and (counter + 536) mod 5 */
static int noise;                         /* 15 bits */
/* the shared settings as taken at clocks 27-30 */
static uint8_t pmon_now, non_now, eon_now, dir_now, koff_now, kon_now, kon_pending;
static int key_unread;        /* a KON or KOFF written, not yet taken (mix_key_unread) */
/* ENDX; as it was before the last voice worked out, which is the value until
   its clock (at_endx) */
static uint8_t endx, endx_before;
static int main_sum[2], echo_sum[2], echo_in[2];
static int echo_hist[8][2], echo_at;
static int echo_index, echo_length, echo_esa, echo_ptr, echo_writes_off;
static int16_t made_l, made_r;

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

/* each rate's phase: its family's (rate_frames[r] % 3 == 0: 1,040; % 5 == 0:
   536; else 0) */
static const uint16_t rate_phase[32] = {
    0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0,
};

/* (counter + phase) % n == 0, with no divide: n is 2^k, 3 x 2^k or 5 x
   2^k, so the counter at its family's phase a multiple of 3 or 5 (c3, c5,
   kept as the counter counts: 30,720 is a multiple of both) and of 2^k */
static int rate_due(int rate)
{
    unsigned n = rate_frames[rate], x = (unsigned)counter + rate_phase[rate];
    if (!n)
        return 0;
    if (n % 3 == 0)
        return !c3 && !(x & (n / 3 - 1));
    if (n % 5 == 0)
        return !c5 && !(x & (n / 5 - 1));
    return !(x & (n - 1));
}

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static int wrap15(int v) { return (int16_t)(v << 1) >> 1; }
static int rd16(uint16_t at) { return mem[at] | mem[(uint16_t)(at + 1)] << 8; }

/* ---- samples ---------------------------------------------------------------------------- */
/* four more values of the voice's sample: one byte pair of its block,
   rebuilt with the block's shift and prediction filter (fullsnes, BRR);
   the header and the pair's bytes as their clocks read them */
static inline __attribute__((always_inline)) void decode(Voice *vc)
{
    int shift = vc->header >> 4, filter = (vc->header >> 2) & 3, i;
    unsigned bits = (unsigned)vc->data << 8 | vc->second;
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
    vc->hist_next = vc->hist_next == 8 ? 0 : vc->hist_next + 4;
}

/* fullsnes, Gaussian interpolation: the newest four values at the pitch
   counter's fraction; the first three sums wrap at 16 bits, the last
   saturates; 15 bits out */
static inline __attribute__((always_inline)) int interpolate(const Voice *vc)
{
    int i = (vc->pos >> 4) & 0xFF;
    int at = (vc->pos >> 12) + vc->hist_next;        /* 0-15 */
    const int16_t *h = &vc->hist[at >= 12 ? at - 12 : at];
    int out = (gauss[0xFF - i] * h[0]) >> 10;
    out += (gauss[0x1FF - i] * h[1]) >> 10;
    out += (gauss[0x100 + i] * h[2]) >> 10;
    out = (int16_t)out;
    out += (gauss[i] * h[3]) >> 10;
    return clamp16(out) >> 1;
}

/* the envelope's next level (fullsnes, ADSR/Gain): ADSR's first byte as its
   clock took it, its second and the gain as they are now. The stage moves
   on from the level computed even on a sample its rate does not step (as
   measured): attack ends past 7FFh, decay at the sustain level's top byte
   (in gain mode the gain's top bits, fullsnes's noted quirk). */
static inline __attribute__((always_inline)) void envelope(Voice *vc, const VoiceSettings *s)
{
    int level = vc->level, rate, top;
    if (vc->stage == RELEASE) {
        level -= 8;
        vc->level = level < 0 ? 0 : level;
        return;
    }
    if (vc->adsr_on) {
        top = s->sustain_level;
        if (vc->stage == ATTACK) {
            rate = vc->adsr_attack * 2 + 1;
            level += rate == 31 ? 0x400 : 0x20;
        } else {
            rate = vc->stage == DECAY ? vc->adsr_decay * 2 + 16 : s->sustain_rate;
            level -= ((level - 1) >> 8) + 1;
        }
    } else {
        int mode = s->gain >> 5;
        top = s->gain >> 5;
        if (!(s->gain & 0x80)) {           /* direct: a fixed level */
            level = s->gain * 16;
            rate = 31;
        } else {
            rate = s->gain & 31;
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

/* ---- a voice's sample -------------------------------------------------------------------
   fullsnes's DSP timing chart, the steps named by what each does. */

/* the voice's directory entry taken; the voice before it, which took its own
   a step ago, has its entry's address worked out (from the directory page as
   taken at clock 28) */
static inline __attribute__((always_inline)) void step_entry(int v)
{
    Voice *before = &voice[(v + 7) & 7];
    /* added: an entry past 63 carries into the page */
    before->dir_addr = (uint16_t)((dir_now << 8) + (before->srcn << 2));
    voice[v].srcn = now.v[v].entry;
}

/* the entry read: its start while a key-on is under way, else its loop */
static inline __attribute__((always_inline)) void step_directory(Voice *vc)
{
    vc->next_block = (uint16_t)rd16((uint16_t)(vc->dir_addr + (vc->kon_delay ? 0 : 2)));
}

static inline __attribute__((always_inline)) void step_fetch(Voice *vc)
{
    vc->data = mem[(uint16_t)(vc->block + vc->byte)];
    vc->header = mem[vc->block];
}

static inline __attribute__((always_inline)) void step_second(Voice *vc)
{
    vc->second = mem[(uint16_t)(vc->block + vc->byte + 1)];
}

/* ADSR's first byte and the pitch, as their clocks took them */
static inline __attribute__((always_inline)) void step_settings(int v)
{
    Voice *vc = &voice[v];
    const VoiceSettings *s = &now.v[v];
    vc->adsr_on = s->adsr;
    vc->adsr_attack = s->attack;
    vc->adsr_decay = s->decay;
    vc->step_pitch = s->pitch_lo | s->pitch_hi << 8;
}

static inline __attribute__((always_inline)) void step_sample(int v)
{
    Voice *vc = &voice[v];
    int s;
    /* pitch modulation by the voice before's output, made this sample */
    if (pmon_now >> v & 1)
        vc->step_pitch += ((voice[(v + 7) & 7].out >> 4) * vc->step_pitch) >> 10;
    if (vc->kon_delay) {
        /* a key-on's five samples: the first sets the voice on its sample's
           start (this sample's header counts for nothing), the next three
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
    /* (at level 0 the output is 0 whatever the sample) */
    if (!vc->level)
        s = 0;
    else
        s = (non_now >> v & 1) ? (int16_t)(noise << 1) >> 1 : interpolate(vc);
    vc->out = (s * vc->level) >> 11;
    vc->level_out = (uint8_t)(vc->level >> 4);
    /* cut at once: the reset flag, or a block that ends with no loop */
    if ((now.reset >> v & 1) || (vc->header & 3) == 1) {
        vc->stage = RELEASE;
        vc->level = 0;
    }
    /* fullsnes: key-on and release are taken every other sample */
    if (every_other) {
        if (koff_now >> v & 1)
            vc->stage = RELEASE;
        if (kon_now >> v & 1) {
            vc->kon_delay = 5;
            vc->stage = ATTACK;
        }
    }
    if (!vc->kon_delay)
        envelope(vc, &now.v[v]);
}

/* fullsnes, output mixer: the sample times the volume, SAR 6, the sums
   saturating at 16 bits; the echo's sum as EON was taken at clock 28 */
static inline __attribute__((always_inline)) void output(const Voice *vc, int v, int ch)
{
    int amp;
    /* (no output: the sums unchanged, each already within 16 bits) */
    if (!vc->out)
        return;
    amp = (vc->out * (ch ? now.v[v].vol_r : now.v[v].vol_l)) >> 6;
    main_sum[ch] = clamp16(main_sum[ch] + amp);
    if (eon_now >> v & 1)
        echo_sum[ch] = clamp16(echo_sum[ch] + amp);
}

/* the byte pair decoded when the pitch counter has passed four values, the
   block moved on at its end; the pitch added; the left output */
static inline __attribute__((always_inline)) void step_decode(int v)
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

/* the right output; ENDX (the voice's bit set on a loop, cleared as a key-on
   starts), and the values OUTX and ENVX will be given */
static inline __attribute__((always_inline)) void step_right(int v)
{
    Voice *vc = &voice[v];
    output(vc, v, 1);
    endx_before = endx;
    endx = (uint8_t)((endx | vc->looped << v) & ~(vc->kon_delay == 5 ? 1u << v : 0u));
    vc->outx_new = (uint8_t)(vc->out >> 7);
    vc->envx_new = vc->level_out;
}

/* Voices 1-7's reads of the memory, each at its clock: a write of the memory
   between them comes after those before it (mix_before_ram_write) */
static inline __attribute__((always_inline)) void voice_read(int v, int which)
{
    Voice *vc = &voice[v];
    switch (which) {
    case 0:
        step_entry((v + 1) & 7);
        step_directory(vc);
        break;
    case 1:
        step_fetch(vc);
        break;
    case 2:
        step_second(vc);
        break;
    }
}

/* A voice with nothing in hand: released at level 0, no key-on under way or
   being taken, no pitch, its counter short of the next values. Its sample
   changes nothing and nothing it reads is used: the block's bytes go to a decode
   or a cut, and a key-on's first sample reads the directory afresh. (A voice
   with a pitch goes on decoding at level 0: what it decodes, a key-on's first
   decode and the interpolation use, from the RAM as it stands then.) */
static inline __attribute__((always_inline)) int voice_idle(const Voice *vc, int v)
{
    return !vc->level && vc->stage == RELEASE && !vc->kon_delay && !vc->step_pitch && vc->pos < 0x4000 &&
           !(every_other && (kon_now >> v & 1));
}

/* Voices 1-7's sample, all its reads passed */
static inline __attribute__((always_inline)) void voice_sample(int v)
{
    Voice *vc = &voice[v];
    vc->step_pitch = now.v[v].pitch_lo | now.v[v].pitch_hi << 8;
    if (voice_idle(vc, v)) {
        if (rd < 1)
            step_entry((v + 1) & 7);
        rd = 0;
        vc->looped = vc->out = 0;
        vc->level_out = vc->outx_new = vc->envx_new = 0;
        endx_before = endx;
        return;
    }
    if (rd < 1)
        voice_read(v, 0);
    if (rd < 2)
        voice_read(v, 1);
    if (rd < 3)
        voice_read(v, 2);
    rd = 0;
    step_settings(v);
    step_sample(v);
    step_decode(v);
    step_right(v);
}

/* ---- the echo ---------------------------------------------------------------------------
   fullsnes, echo: the oldest entry out of the delay line, through the
   8-tap filter (the oldest seven summed without saturation, the newest
   with), mixed in at the echo volume; the echo voices and the feedback
   written back in its place. */
static int tap(int i, int ch) { return (echo_hist[(echo_at + 1 + i) & 7][ch] * now.fir[i]) >> 6; }

static void echo_read(int ch)
{
    echo_hist[echo_at][ch] = (int16_t)rd16((uint16_t)(echo_ptr + ch * 2)) >> 1;
}

/* the main sum at the master volume and the filtered echo at the echo
   volume, each term 16 bits, the sum saturating */
static int final(int ch)
{
    int m = (int16_t)((main_sum[ch] * (ch ? now.mvol_r : now.mvol_l)) >> 7);
    int e = (int16_t)((echo_in[ch] * (ch ? now.evol_r : now.evol_l)) >> 7);
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

/* The sample's last reads of the memory, each at its clock (at_head) */
static inline __attribute__((always_inline)) void head_read(int which)
{
    switch (which) {
    case 0:
        step_entry(1);
        step_directory(&voice[0]);
        break;
    case 1:                               /* the next entry, left */
        echo_at = (echo_at + 1) & 7;
        echo_ptr = (uint16_t)((echo_esa << 8) + echo_index);
        echo_read(0);
        break;
    case 2:                               /* right (only tap 7 reads the newest) */
        echo_read(1);
        break;
    case 3:
        step_fetch(&voice[0]);
        break;
    }
}

/* ---- a sample's work, in its clocks' order ---------------------------------------------- */
static inline __attribute__((always_inline)) void clock_28(void)
{
    non_now = now.non;
    eon_now = now.eon;
    dir_now = now.dir_page;
    echo_writes_off = now.echo_off_28;
}

/* the next entry; the left written */
static inline __attribute__((always_inline)) void clock_29(void)
{
    /* a key-on is cleared from KON's latch 63 clocks after it is taken */
    if ((every_other ^= 1) != 0)
        kon_pending &= (uint8_t)~kon_now;
    echo_esa = now.echo_page;
    /* the length is taken when the index comes round to 0 */
    if (!echo_index)
        echo_length = now.echo_len * 0x800;
    echo_index += 4;
    if (echo_index >= echo_length)
        echo_index = 0;
    echo_write(0);
    echo_writes_off = now.echo_off_29;
}

static inline __attribute__((always_inline)) void clock_30(void)
{
    if (every_other) {
        kon_now = kon_pending;
        koff_now = released;
        key_unread = 0;
    }
    if (--counter < 0)
        counter = COUNTER_PERIOD - 1;
    c3 = c3 ? c3 - 1 : 2;
    c5 = c5 ? c5 - 1 : 4;
    if (rate_due(now.noise_rate))
        noise = ((noise >> 1) & 0x3FFF) | (((noise ^ (noise >> 1)) & 1) << 14);
    step_sample(0);
    echo_write(1);
}

/* voice 0's next reads, the echo's filter and the sample's output */
static inline __attribute__((always_inline)) void sample_end(void)
{
    int ch, idle;
    step_settings(0);
    /* (a key-on it takes next sample reads the directory then, its first) */
    idle = !voice[0].level && voice[0].stage == RELEASE && !voice[0].kon_delay && !voice[0].step_pitch &&
           voice[0].pos < 0x4000;
    if (rd < 1) {
        if (idle)
            step_entry(1);
        else
            head_read(0);
    }
    if (rd < 2)
        head_read(1);
    for (ch = 0; ch < 2; ch++)
        echo_in[ch] = tap(0, ch);
    for (ch = 0; ch < 2; ch++)
        echo_in[ch] += tap(1, ch) + tap(2, ch);
    if (rd < 3)
        head_read(2);
    for (ch = 0; ch < 2; ch++)
        echo_in[ch] += tap(3, ch) + tap(4, ch) + tap(5, ch);
    if (rd < 4 && !idle)
        head_read(3);
    rd = 0;
    for (ch = 0; ch < 2; ch++) {           /* tap 6 wraps, the newest saturates */
        int s = (int16_t)(echo_in[ch] + tap(6, ch));
        echo_in[ch] = clamp16(s + (int16_t)tap(7, ch)) & ~1;
    }
    made_l = (int16_t)final(0);
    for (ch = 0; ch < 2; ch++)
        echo_sum[ch] = clamp16(echo_sum[ch] + (int16_t)((echo_in[ch] * now.efb) >> 7)) & ~1;
    made_r = (int16_t)final(1);
    main_sum[0] = main_sum[1] = 0;
    if (now.muted)
        made_l = made_r = 0;
    pmon_now = now.pmon & 0xFE;            /* voice 0 has no voice before it */
}

/* The sample's work from where it stands to `end` (S_) */
static void sections(int end)
{
    if (sect >= end)
        return;
#define SECTION(k) case k: if (k >= end) break; /* fall through */
    switch (sect) {
    SECTION(S_28) clock_28();
    SECTION(S_29) clock_29();
    SECTION(S_30) clock_30();
    SECTION(S_31)
        step_second(&voice[0]);
        step_decode(0);
    SECTION(S_VOICES) step_right(0);
    SECTION(S_VOICES + 1) voice_sample(1);
    SECTION(S_VOICES + 2) voice_sample(2);
    SECTION(S_VOICES + 3) voice_sample(3);
    SECTION(S_VOICES + 4) voice_sample(4);
    SECTION(S_VOICES + 5) voice_sample(5);
    SECTION(S_VOICES + 6) voice_sample(6);
    SECTION(S_VOICES + 7) voice_sample(7);
    SECTION(S_END) sample_end();
    }
#undef SECTION
    sect = end;
}

/* OUTX and ENVX given what they get at clocks already passed */
static void sets_passed(void)
{
    int run = sect <= S_VOICES ? 0 : sect >= S_END ? 8 : sect - S_VOICES;
    while (outx_set < run && at_outx[outx_set] < rel) {
        voice[outx_set].outx = voice[outx_set].outx_new;
        outx_set++;
    }
    while (envx_set < run && at_envx[envx_set] < rel) {
        voice[envx_set].envx = voice[envx_set].envx_new;
        envx_set++;
    }
}

/* What can be worked out by this clock: the shared settings' work and voices
   whose last read has passed. Voice 0's next reads and the echo's filter wait
   for the sample's end: nothing sees them before it. (The first S_VOICES
   sections' clocks are 0 to S_VOICES - 1, voice 0's last read the next, and
   each voice's after it comes 3 clocks on: at_right.) */
void mix_settle(void)
{
    int end = rel <= S_VOICES ? rel : S_VOICES + 1 + (rel - S_VOICES - 1) / 3;
    sections(end < S_END ? end : S_END);
    sets_passed();
}

/* Before a write to the memory lands: every read at an earlier clock is made,
   and those to come will see it. (Many writes at one clock need it once.) */
void mix_before_ram_write(void)
{
    if (rel == ram_seen)
        return;
    ram_seen = rel;
    mix_settle();
    if (sect == S_END) {
        while (rd < 4 && at_head[rd] < rel) {
            head_read(rd);
            rd++;
        }
    } else if (sect > S_VOICES) {
        int v = sect - S_VOICES;
        while (rd < 3 && (rd == 0 ? at_dir[v] : rd == 1 ? at_fetch[v] : at_left[v]) < rel) {
            voice_read(v, rd);
            rd++;
        }
    }
}

/* ---- setup ----------------------------------------------------------------------------- */
void mix_init(uint8_t *memory)
{
    int v;
    mem = memory;
    memset(voice, 0, sizeof voice);
    memset(&now, 0, sizeof now);
    for (v = 0; v < 8; v++)
        voice[v].byte = 1;
    released = 0;
    /* fullsnes: after reset, released, muted, echo writes off, noise stopped */
    now.reset = 0xFF;
    now.muted = now.echo_off_28 = now.echo_off_29 = 1;
    next = now;
    deferred = 0;
    counter = 0;
    c3 = 1040 % 3;
    c5 = 536 % 5;
    every_other = 1;
    noise = 0x4000;                        /* -4000h */
    /* it starts at the chip's clock 0, none of the clocks before it run */
    rel = at_right[0];
    sect = S_VOICES;
    rd = outx_set = envx_set = 0;
    ram_seen = -1;
    pmon_now = non_now = eon_now = dir_now = koff_now = kon_now = kon_pending = 0;
    key_unread = 0;
    endx = endx_before = 0;
    memset(main_sum, 0, sizeof main_sum);
    memset(echo_sum, 0, sizeof echo_sum);
    memset(echo_in, 0, sizeof echo_in);
    memset(echo_hist, 0, sizeof echo_hist);
    echo_at = 0;
    echo_index = echo_length = echo_esa = echo_ptr = echo_writes_off = 0;
}

/* ---- what the game sets: this sample if its read is still to come --------------------- */
#define SET(field, value, at) do { \
        next.field = (value); \
        if ((at) >= rel) \
            now.field = next.field; \
        else \
            deferred = 1; \
    } while (0)

void mix_voice_volume(int v, int8_t left, int8_t right)
{
    v &= 7;
    SET(v[v].vol_l, left, at_left[v]);
    SET(v[v].vol_r, right, at_right[v]);
}

void mix_voice_pitch(int v, uint16_t pitch)
{
    v &= 7;
    SET(v[v].pitch_lo, (uint8_t)pitch, at_dir[v]);
    SET(v[v].pitch_hi, (uint8_t)(pitch >> 8 & 0x3F), at_hi[v]);
}

void mix_voice_sample(int v, uint8_t entry)
{
    v &= 7;
    SET(v[v].entry, entry, at_entry[v]);
}

void mix_voice_envelope(int v, int adsr, int attack, int decay, int sustain_level, int sustain_rate)
{
    v &= 7;
    SET(v[v].adsr, adsr != 0, at_dir[v]);
    SET(v[v].attack, (uint8_t)(attack & 15), at_dir[v]);
    SET(v[v].decay, (uint8_t)(decay & 7), at_dir[v]);
    SET(v[v].sustain_level, (uint8_t)(sustain_level & 7), at_sample[v]);
    SET(v[v].sustain_rate, (uint8_t)(sustain_rate & 31), at_sample[v]);
}

void mix_voice_gain(int v, uint8_t gain)
{
    v &= 7;
    SET(v[v].gain, gain, at_sample[v]);
}

/* KON's latch and KOFF change between the sample's clocks that use them */
void mix_key_on(uint8_t voices)
{
    mix_settle();
    kon_pending = voices;
    key_unread = 1;
}

void mix_key_off(uint8_t voices)
{
    mix_settle();
    released = voices;
    key_unread = 1;
}

int mix_key_unread(void)
{
    mix_settle();
    return key_unread;
}

void mix_pitch_modulation(uint8_t voices) { SET(pmon, voices, AT_PMON_RIGHT); }
void mix_noise(uint8_t voices) { SET(non, voices, AT_DIR_NON_EON); }
void mix_echo_voices(uint8_t voices) { SET(eon, voices, AT_DIR_NON_EON); }

void mix_master_volume(int8_t left, int8_t right)
{
    SET(mvol_l, left, AT_EFB_LEFT);
    SET(mvol_r, right, AT_PMON_RIGHT);
}

void mix_control(int reset, int mute, int echo_writes_off_set, int rate)
{
    uint8_t all = reset ? 0xFF : 0, still = 0;
    int v;
    /* (what mix_echo_may_write goes by, up to this write) */
    mix_settle();
    for (v = 0; v < 8; v++)
        still |= (uint8_t)((at_sample[v] >= rel) << v);
    next.reset = all;
    now.reset = (uint8_t)((now.reset & ~still) | (all & still));
    if (still != 0xFF)
        deferred = 1;
    SET(muted, mute != 0, AT_PMON_RIGHT);
    SET(echo_off_28, echo_writes_off_set != 0, AT_DIR_NON_EON);
    SET(echo_off_29, echo_writes_off_set != 0, AT_ESA_EDL);
    SET(noise_rate, (uint8_t)(rate & 31), AT_KEYS_NOISE);
}

void mix_directory(uint8_t page) { SET(dir_page, page, AT_DIR_NON_EON); }

void mix_echo_buffer(uint8_t page, uint8_t length)
{
    /* (what mix_echo_may_write goes by, up to this write) */
    mix_settle();
    SET(echo_page, page, AT_ESA_EDL);
    SET(echo_len, (uint8_t)(length & 15), AT_ESA_EDL);
}

void mix_echo_volume(int8_t left, int8_t right)
{
    SET(evol_l, left, AT_EFB_LEFT);
    SET(evol_r, right, AT_PMON_RIGHT);
}

void mix_echo_feedback(int8_t feedback) { SET(efb, feedback, AT_EFB_LEFT); }

void mix_echo_filter(int tap_n, int8_t coefficient)
{
    tap_n &= 7;
    SET(fir[tap_n], coefficient, at_tap[tap_n]);
}

/* ---- what the game reads back, by the same clocks ------------------------------------- */
/* ENDX: a voice worked out has given it its bit only at its clock */
uint8_t mix_voices_ended(void)
{
    mix_settle();
    if (rel > at_right[0]) {
        int v = (rel - at_right[0] - 1) / 3;
        if (v < 8 && at_right[v] < rel && at_endx[v] >= rel)
            return endx_before;
    }
    return endx;
}

/* the echo writes the RAM while FLG lets it or its write already under way
   was let */
int mix_writes_ram(void) { return !next.echo_off_29 || !echo_writes_off; }

int mix_echo_may_write(uint16_t addr)
{
    /* an entry is 4 bytes at an index under the length (0: the one entry) */
    int span = echo_length > next.echo_len * 0x800 ? echo_length : next.echo_len * 0x800;
    if (!mix_writes_ram())
        return 0;
    if (span < 4)
        span = 4;
    return (uint16_t)(addr - (echo_esa << 8)) < span || (uint16_t)(addr - (next.echo_page << 8)) < span;
}

/* any write to ENDX clears it, and a voice's bit on its way (at its clock it
   gives what the write left) */
void mix_clear_ended(void)
{
    mix_settle();
    endx = endx_before = 0;
}

uint8_t mix_voice_level(int v)
{
    mix_settle();
    return voice[v & 7].envx;
}

int8_t mix_voice_output(int v)
{
    mix_settle();
    return (int8_t)voice[v & 7].outx;
}

/* A write to ENVX or OUTX: kept until the voice's clock gives it its own. The
   chip passes each value on through one latch, which the write also sets: the
   voice whose value is in the latch gets the write's instead. */
void mix_voice_level_write(int v, uint8_t value)
{
    mix_settle();
    voice[v & 7].envx = value;
    if (rel > at_endx[0]) {
        int in_latch = (rel - at_endx[0] - 1) / 3;
        if (in_latch < 8 && at_endx[in_latch] < rel && at_envx[in_latch] >= rel)
            voice[in_latch].envx_new = value;
    }
}

void mix_voice_output_write(int v, uint8_t value)
{
    mix_settle();
    voice[v & 7].outx = value;
    if (rel > at_outx_next[0]) {
        int in_latch = (rel - at_outx_next[0] - 1) / 3;
        if (in_latch < 8 && at_outx_next[in_latch] < rel && at_outx[in_latch] >= rel)
            voice[in_latch].outx_new = value;
    }
}

/* The clocks from now until a read of register `reg` could next answer otherwise
   (a set's clock is seen a clock on), but for a write of the game's: 0 for a
   register only the game changes. */
int mix_reg_next(uint8_t reg)
{
    int v, at;
    reg &= 0x7F;
    if (reg == 0x7C) {
        for (v = 0; v < 8; v++)
            if (at_endx[v] + 1 > rel)
                return at_endx[v] + 1 - rel;
        return 32 - rel + at_endx[0] + 1;
    }
    if ((reg & 0x0F) != 0x8 && (reg & 0x0F) != 0x9)
        return 0;
    at = ((reg & 0x0F) == 0x8 ? at_envx : at_outx)[reg >> 4] + 1;
    return at > rel ? at - rel : 32 - rel + at;
}

/* The clocks from now until a read of the RAM could see the echo's next write:
   after clock 29's, after clock 30's */
int mix_ram_next(void)
{
    if (rel < AT_ESA_EDL + 1)
        return AT_ESA_EDL + 1 - rel;
    if (rel < AT_KEYS_NOISE + 1)
        return AT_KEYS_NOISE + 1 - rel;
    return 32 - rel + AT_ESA_EDL + 1;
}

int mix_phase(void) { return (rel + 28) & 31; }

/* the clocks to run until a KON or KOFF written now is taken: at clock 30
   of a sample whose clock 29 left every_other set (it flips there) */
int mix_key_clocks(void)
{
    int p, e;
    mix_settle();
    p = mix_phase();
    e = every_other;
    if (p <= 29)
        return (e ^ 1) ? 31 - p : 63 - p;
    if (p == 30)
        return e ? 1 : 33;
    return (e ^ 1) ? 32 : 64;
}

/* (at a sample's end: within one, voices not yet worked out show the last) */
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

/* ---- time ------------------------------------------------------------------------------- */
__attribute__((noinline)) int mix_run(int n, int16_t *out, int max)
{
    int k = 0;
    /* each sample that ends in these clocks; no work for the clocks between */
    while (n >= 32 - rel) {
        n -= 32 - rel;
        sections(S_DONE);
        rel = 32;
        sets_passed();
        rel = 0;
        sect = S_28;
        outx_set = envx_set = 0;
        ram_seen = -1;
        if (deferred) {
            now = next;
            deferred = 0;
        }
        if (k < max) {
            out[2 * k] = made_l;
            out[2 * k + 1] = made_r;
        }
        k++;
    }
    rel += n;
    return k < max ? k : max;
}

/* (one clock as a run of one) */
int mix_clock(int16_t *out_l, int16_t *out_r)
{
    int16_t f[2];
    if (!mix_run(1, f, 1))
        return 0;
    *out_l = f[0];
    *out_r = f[1];
    return 1;
}
