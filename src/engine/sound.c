/* sound.c: the engine's sound on the audio thread: voices, envelopes, noise,
 * pitch modulation, echo and its filter by the SNES's rules, a sample at a
 * time at 32,040 Hz, resampled to the device. The game's music code runs here
 * at its own rate. Nothing on this thread waits on the game: samples are
 * published once made, and messages cross in a one-way queue. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "inspect.h"
#include "resample.h"

#define DSP_RATE 32040
#define OUT_RATE 48000
#define MAX_VOICES 64
#define MAX_SAMPLES 4096
#define ECHO_BYTES (15 * 2048)

typedef struct {
    const uint8_t *data;
    uint32_t size, loop;
    uint8_t pcm;
} Sample;

enum { RELEASE, ATTACK, DECAY, SUSTAIN };

typedef struct {
    const Sample *sample, *queued;
    int adsr_on, attack, decay, sustain_level, sustain_rate;
    uint8_t gain;
    int pitch;
    int8_t vol_l, vol_r;
    uint8_t noise, echo, pmod;
    uint8_t key_on, key_off, kon_now, koff_now;
    /* playing */
    uint32_t next_block, block;
    int byte;
    int step_pitch;
    uint8_t header, data, second;
    int looped;
    int16_t hist[24];
    int hist_next, pos;
    int kon_delay, stage, level, bent, out;
    uint8_t ended;
} Voice;

static const Sample *_Atomic samples[MAX_SAMPLES];
static uint32_t nsamples = 1, used_bytes;
static uint32_t limit_voices, limit_bytes;

static Voice voice[MAX_VOICES];
static int every_other = 1, counter, c3 = 1040 % 3, c5 = 536 % 5, noise_bits = 0x4000, noise_rate;
static int main_sum[2], echo_sum[2], echo_in[2];
static int echo_hist[8][2], echo_at, echo_index, echo_length, echo_ptr, echo_len_set;
static uint8_t echo_mem[ECHO_BYTES];
static int8_t mvol_l, mvol_r, evol_l, evol_r, efb, fir[8];
static int muted;
static int echo_writes_off;

/* the music code */
static nshvc_music music;
static uint64_t ticks_done, samples_done;

/* the one-way queue */
static uint8_t *queue;
static uint32_t queue_size;
static atomic_uint queue_head, queue_tail;

static _Atomic uint64_t cost_ns, cost_frames;
static Resampler *rs;
static FILE *sound_file;

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


#define COUNTER_PERIOD 30720
static const uint16_t rate_frames[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};
static const uint16_t rate_phase[32] = {
    0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0,
};

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

static uint8_t brr_byte(const Sample *s, uint32_t at) { return s && at < s->size ? s->data[at] : 0; }

static void decode(Voice *vc)
{
    int i;
    if (vc->sample && vc->sample->pcm) {
        /* 16-bit PCM: the next four values as they are, 15 bits */
        for (i = 0; i < 4; i++) {
            int k = vc->hist_next + i, s = 0;
            uint32_t at = vc->block + (uint32_t)i * 2;
            if (at + 1 < vc->sample->size)
                s = (int16_t)(vc->sample->data[at] | vc->sample->data[at + 1] << 8) >> 1;
            vc->hist[k] = (int16_t)s;
            vc->hist[k + 12] = (int16_t)s;
        }
    } else {
        int shift = vc->header >> 4, filter = (vc->header >> 2) & 3;
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
    }
    vc->hist_next = vc->hist_next == 8 ? 0 : vc->hist_next + 4;
}

static int interpolate(const Voice *vc)
{
    int i = (vc->pos >> 4) & 0xFF;
    int at = (vc->pos >> 12) + vc->hist_next;
    const int16_t *h = &vc->hist[at >= 12 ? at - 12 : at];
    int out = (gauss[0xFF - i] * h[0]) >> 10;
    out += (gauss[0x1FF - i] * h[1]) >> 10;
    out += (gauss[0x100 + i] * h[2]) >> 10;
    out = (int16_t)out;
    out += (gauss[i] * h[3]) >> 10;
    return clamp16(out) >> 1;
}

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
            rate = vc->attack * 2 + 1;
            level += rate == 31 ? 0x400 : 0x20;
        } else {
            rate = vc->stage == DECAY ? vc->decay * 2 + 16 : vc->sustain_rate;
            level -= ((level - 1) >> 8) + 1;
        }
    } else {
        int mode = vc->gain >> 5;
        top = vc->gain >> 5;
        if (!(vc->gain & 0x80)) {
            level = vc->gain * 16;
            rate = 31;
        } else {
            rate = vc->gain & 31;
            if (mode == 4)
                level -= 0x20;
            else if (mode == 5)
                level -= ((level - 1) >> 8) + 1;
            else
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

static void output(const Voice *vc, int ch)
{
    int amp;
    if (!vc->out)
        return;
    amp = (vc->out * (ch ? vc->vol_r : vc->vol_l)) >> 6;
    main_sum[ch] = clamp16(main_sum[ch] + amp);
    if (vc->echo)
        echo_sum[ch] = clamp16(echo_sum[ch] + amp);
}

static void voice_sample(int v, int nvoices)
{
    Voice *vc = &voice[v];
    const Sample *s = vc->sample;
    int d;
    vc->next_block = vc->kon_delay ? 0 : (s ? s->loop : 0);
    vc->header = brr_byte(s, vc->block);
    vc->data = brr_byte(s, vc->block + (uint32_t)vc->byte);
    vc->second = brr_byte(s, vc->block + (uint32_t)vc->byte + 1);
    vc->step_pitch = vc->pitch;
    if (vc->pmod && v > 0)
        vc->step_pitch += ((voice[v - 1].out >> 4) * vc->step_pitch) >> 10;
    if (vc->kon_delay) {
        if (vc->kon_delay == 5) {
            /* this sample's bytes were the old sample's; the new one starts here */
            if (vc->queued)
                vc->sample = s = vc->queued;
            vc->block = vc->next_block;
            vc->byte = 1;
            vc->hist_next = 0;
            vc->header = 0;
        }
        vc->level = vc->bent = 0;
        vc->pos = --vc->kon_delay & 3 ? 0x4000 : 0;
        vc->step_pitch = 0;
    }
    if (!vc->level)
        d = 0;
    else
        d = vc->noise ? (int16_t)(noise_bits << 1) >> 1 : interpolate(vc);
    vc->out = (d * vc->level) >> 11;
    if (s && !s->pcm && (vc->header & 3) == 1) {
        vc->stage = RELEASE;
        vc->level = 0;
    }
    if (every_other) {
        if (vc->koff_now)
            vc->stage = RELEASE;
        if (vc->kon_now) {
            vc->kon_delay = 5;
            vc->stage = ATTACK;
        }
    }
    if (!vc->kon_delay)
        envelope(vc);
    /* the decode, the block moved on at its end, the pitch added */
    vc->looped = 0;
    if (vc->pos >= 0x4000) {
        decode(vc);
        if (s && s->pcm) {
            vc->block += 8;
            if (vc->block + 8 > s->size) {
                if (s->loop < s->size) {
                    vc->block = s->loop;
                    vc->looped = 1;
                } else {
                    vc->stage = RELEASE;
                    vc->level = 0;
                }
            }
        } else {
            vc->byte += 2;
            if (vc->byte >= 9) {
                vc->block += 9;
                if (vc->header & 1) {
                    vc->block = vc->next_block;
                    vc->looped = 1;
                }
                vc->byte = 1;
            }
        }
    }
    vc->pos = (vc->pos & 0x3FFF) + vc->step_pitch;
    if (vc->pos > 0x7FFF)
        vc->pos = 0x7FFF;
    output(vc, 0);
    output(vc, 1);
    if (vc->looped)
        vc->ended = 1;
    if (vc->kon_delay == 5)
        vc->ended = 0;
    (void)nvoices;
}

static int tap(int i, int ch) { return (echo_hist[(echo_at + 1 + i) & 7][ch] * fir[i]) >> 6; }

static int rd16(int at) { return echo_mem[at] | echo_mem[at + 1] << 8; }

static void echo_write(int ch)
{
    if (!echo_writes_off) {
        echo_mem[echo_ptr + ch * 2] = (uint8_t)echo_sum[ch];
        echo_mem[echo_ptr + ch * 2 + 1] = (uint8_t)(echo_sum[ch] >> 8);
    }
    echo_sum[ch] = 0;
}

static int final_out(int ch)
{
    int m = (int16_t)((main_sum[ch] * (ch ? mvol_r : mvol_l)) >> 7);
    int e = (int16_t)((echo_in[ch] * (ch ? evol_r : evol_l)) >> 7);
    return clamp16(m + e);
}

static void one_sample(int16_t *l, int16_t *r)
{
    int v, ch, nvoices = limit_voices ? (int)limit_voices : MAX_VOICES;
    if (nvoices > MAX_VOICES)
        nvoices = MAX_VOICES;
    if ((every_other ^= 1) != 0)
        for (v = 0; v < nvoices; v++)
            if (voice[v].kon_now)
                voice[v].key_on = 0;
    if (!echo_index)
        echo_length = echo_len_set * 2048;
    echo_index += 4;
    if (echo_index >= echo_length)
        echo_index = 0;
    echo_write(0);
    if (every_other)
        for (v = 0; v < nvoices; v++) {
            voice[v].kon_now = voice[v].key_on;
            voice[v].koff_now = voice[v].key_off;
        }
    if (--counter < 0)
        counter = COUNTER_PERIOD - 1;
    c3 = c3 ? c3 - 1 : 2;
    c5 = c5 ? c5 - 1 : 4;
    if (rate_due(noise_rate))
        noise_bits = ((noise_bits >> 1) & 0x3FFF) | (((noise_bits ^ (noise_bits >> 1)) & 1) << 14);
    echo_write(1);
    for (v = 0; v < nvoices; v++)
        voice_sample(v, nvoices);
    echo_at = (echo_at + 1) & 7;
    echo_ptr = echo_index;
    for (ch = 0; ch < 2; ch++) {
        int s;
        echo_hist[echo_at][ch] = (int16_t)rd16(echo_ptr + ch * 2) >> 1;
        echo_in[ch] = tap(0, ch) + tap(1, ch) + tap(2, ch) + tap(3, ch) + tap(4, ch) + tap(5, ch);
        s = (int16_t)(echo_in[ch] + tap(6, ch));
        echo_in[ch] = clamp16(s + (int16_t)tap(7, ch)) & ~1;
    }
    *l = (int16_t)final_out(0);
    for (ch = 0; ch < 2; ch++)
        echo_sum[ch] = clamp16(echo_sum[ch] + (int16_t)((echo_in[ch] * efb) >> 7)) & ~1;
    *r = (int16_t)final_out(1);
    main_sum[0] = main_sum[1] = 0;
    if (muted)
        *l = *r = 0;
}

/* a sample's time: the music code when it is due, then the sample */
static void step(int16_t *lr)
{
    if (music.tick && music.hz) {
        uint64_t due = samples_done * music.hz / DSP_RATE;
        while (ticks_done <= due) {
            music.tick();
            ticks_done++;
        }
    }
    one_sample(&lr[0], &lr[1]);
    samples_done++;
}

/* ---- on the host's audio thread ---- */
static void audio_cb(float *lr, int frames, void *user)
{
    int16_t in[128 * 2];
    int k, made;
    uint64_t t0 = host_ticks_ns();
    (void)user;
    eng_role = ENG_AUDIO;
    while (resample_queued(rs) < frames * 2 + 64) {
        for (k = 0; k < 128; k++)
            step(in + 2 * k);
        resample_push(rs, in, 128);
    }
    made = resample_pull(rs, lr, frames);
    if (made < frames)
        memset(lr + 2 * made, 0, (size_t)(frames - made) * 2 * sizeof *lr);
    atomic_fetch_add(&cost_ns, host_ticks_ns() - t0);
    atomic_fetch_add(&cost_frames, (uint64_t)frames);
}

/* the audio thread's time per second of sound, since last asked */
double eng_sound_cost_ms(void)
{
    uint64_t ns = atomic_exchange(&cost_ns, 0), n = atomic_exchange(&cost_frames, 0);
    return n ? (double)ns / 1e6 * (double)OUT_RATE / (double)n : 0;
}

/* ---- for the loop ---- */
static int have_device, stop_flag;
static host_thread *silent;

/* with no device the music still runs at its own rate */
static void silent_thread(void *arg)
{
    uint64_t start = host_ticks_ns();
    (void)arg;
    eng_role = ENG_AUDIO;
    while (!stop_flag) {
        uint64_t due = (host_ticks_ns() - start) * DSP_RATE / 1000000000u;
        int16_t lr[2];
        while (samples_done < due)
            step(lr);
        host_sleep_ms(4);
    }
}

void eng_sound_start(const nshvc_music *m, int headless, const char *sound_path)
{
    int v;
    music = *m;
    for (v = 0; v < MAX_VOICES; v++)
        voice[v].byte = 1;
    if (music.queue_bytes) {
        queue_size = music.queue_bytes + 1;
        queue = (uint8_t *)malloc(queue_size);
        if (!queue)
            exit(70);
    }
    if (sound_path && !(sound_file = fopen(sound_path, "wb")))
        fprintf(stderr, "nativeshvc: cannot write %s\n", sound_path);
    if (headless)
        return;
    rs = resample_new(DSP_RATE, OUT_RATE);
    have_device = host_audio_open(OUT_RATE, audio_cb, NULL);
    if (!have_device)
        silent = host_thread_start(silent_thread, NULL, "nshvc audio");
}

void eng_sound_stop(void)
{
    if (have_device)
        host_audio_close();
    if (silent) {
        stop_flag = 1;
        host_thread_join(silent);
    }
    if (sound_file)
        fclose(sound_file);
}

/* headless: the sound up to `updates` updates, in order on the game thread */
void eng_sound_advance(uint64_t updates, uint32_t update_hz)
{
    int role = eng_role;
    uint64_t due = updates * DSP_RATE / update_hz;
    eng_role = ENG_AUDIO;
    while (samples_done < due) {
        int16_t lr[2];
        step(lr);
        if (sound_file)
            fwrite(lr, 2, 2, sound_file);
    }
    eng_role = role;
}

/* a look from the game thread: each field is read whole, and a voice may be
   a sample ahead of another */
void eng_inspect_voices(FILE *f)
{
    int v;
    for (v = 0; v < MAX_VOICES; v++) {
        const Voice *x = &voice[v];
        if (x->sample || x->level)
            fprintf(f, "  voice %d sample %p pitch %04x volume %d,%d adsr %d %d %d %d %d gain %02x level %d stage %d out %d\n", v, (const void *)x->sample,
                    (unsigned)x->pitch, x->vol_l, x->vol_r, x->adsr_on, x->attack, x->decay, x->sustain_level, x->sustain_rate, x->gain, x->level, x->stage, x->out);
    }
}

/* ---- calls ---- */
#define V(call) if (!eng_on(call, ENG_AUDIO)) return; if (v >= MAX_VOICES || (limit_voices && v >= limit_voices)) { eng_report(call, "voice %u", v); return; }

nshvc_sample nshvc_sample_load(const void *data, uint32_t size, nshvc_sample_format f, uint32_t loop_start)
{
    static const char call[] = "nshvc_sample_load";
    nshvc_sample r = { 0 };
    Sample *s;
    uint8_t *copy;
    if (!eng_on(call, ENG_GAME))
        return r;
    if (!data || !size || nsamples == MAX_SAMPLES || (limit_bytes && used_bytes + size > limit_bytes)) {
        eng_report(call, "%u bytes (%u of the limit %u used)", size, used_bytes, limit_bytes);
        return r;
    }
    s = (Sample *)malloc(sizeof *s);
    copy = (uint8_t *)malloc(size);
    if (!s || !copy)
        exit(70);
    memcpy(copy, data, size);
    s->data = copy;
    s->size = size;
    s->loop = loop_start;
    s->pcm = f == NSHVC_SAMPLE_PCM16;
    used_bytes += size;
    atomic_store(&samples[nsamples], s);
    r.id = nsamples++;
    return r;
}

void nshvc_sound_limits_set(const nshvc_sound_limits *l)
{
    if (eng_on("nshvc_sound_limits_set", ENG_GAME) && l) {
        limit_voices = l->voices;
        limit_bytes = l->sample_bytes;
    }
}

void nshvc_music_send(const void *message, uint32_t size)
{
    static const char call[] = "nshvc_music_send";
    uint32_t h, t, room, i;
    if (!eng_on(call, ENG_GAME))
        return;
    if (!queue) {
        eng_report(call, "no queue: music.queue_bytes is 0");
        return;
    }
    h = atomic_load(&queue_head);
    t = atomic_load(&queue_tail);
    room = (t + queue_size - h - 1) % queue_size;
    if (size + 4 > room) {
        eng_report(call, "%u bytes do not fit the queue's %u", size, room);
        return;
    }
    for (i = 0; i < 4; i++)
        queue[(h + i) % queue_size] = (uint8_t)(size >> (8 * i));
    for (i = 0; i < size; i++)
        queue[(h + 4 + i) % queue_size] = ((const uint8_t *)message)[i];
    atomic_store(&queue_head, (h + 4 + size) % queue_size);
}

uint32_t nshvc_music_receive(void *buffer, uint32_t max)
{
    static const char call[] = "nshvc_music_receive";
    uint32_t h, t, size = 0, i;
    if (!eng_on(call, ENG_AUDIO) || !queue)
        return 0;
    h = atomic_load(&queue_head);
    t = atomic_load(&queue_tail);
    if (h == t)
        return 0;
    for (i = 0; i < 4; i++)
        size |= (uint32_t)queue[(t + i) % queue_size] << (8 * i);
    if (size > max) {
        eng_report(call, "a message of %u bytes for a buffer of %u", size, max);
        atomic_store(&queue_tail, (t + 4 + size) % queue_size);
        return 0;
    }
    for (i = 0; i < size; i++)
        ((uint8_t *)buffer)[i] = queue[(t + 4 + i) % queue_size];
    atomic_store(&queue_tail, (t + 4 + size) % queue_size);
    return size;
}

void nshvc_voice_key_on(uint32_t v, nshvc_sample s)
{
    V("nshvc_voice_key_on")
    if (!s.id || s.id >= MAX_SAMPLES || !atomic_load(&samples[s.id])) {
        eng_report("nshvc_voice_key_on", "sample %u", s.id);
        return;
    }
    voice[v].queued = atomic_load(&samples[s.id]);
    voice[v].key_on = 1;
    voice[v].key_off = 0;
}

void nshvc_voice_key_off(uint32_t v) { V("nshvc_voice_key_off") voice[v].key_off = 1; }
void nshvc_voice_pitch(uint32_t v, uint16_t pitch) { V("nshvc_voice_pitch") voice[v].pitch = pitch & 0x3FFF; }
void nshvc_voice_volume(uint32_t v, int8_t l, int8_t r) { V("nshvc_voice_volume") voice[v].vol_l = l; voice[v].vol_r = r; }

void nshvc_voice_adsr(uint32_t v, uint8_t a, uint8_t d, uint8_t sl, uint8_t sr)
{
    V("nshvc_voice_adsr")
    voice[v].adsr_on = 1;
    voice[v].attack = a & 15;
    voice[v].decay = d & 7;
    voice[v].sustain_level = sl & 7;
    voice[v].sustain_rate = sr & 31;
}

void nshvc_voice_gain(uint32_t v, nshvc_gain_mode mode, uint8_t value)
{
    V("nshvc_voice_gain")
    voice[v].adsr_on = 0;
    voice[v].gain = mode == NSHVC_GAIN_DIRECT ? (uint8_t)(value & 0x7F) : (uint8_t)(0x80 | (uint8_t)mode << 5 | (value & 31));
}

void nshvc_voice_options(uint32_t v, uint8_t noise, uint8_t echo, uint8_t pmod)
{
    V("nshvc_voice_options")
    voice[v].noise = noise != 0;
    voice[v].echo = echo != 0;
    voice[v].pmod = pmod != 0;
}

nshvc_voice_state nshvc_voice_read(uint32_t v)
{
    nshvc_voice_state st;
    memset(&st, 0, sizeof st);
    if (!eng_on("nshvc_voice_read", ENG_AUDIO) || v >= MAX_VOICES)
        return st;
    st.level = (uint8_t)(voice[v].level >> 4);
    st.output = (int8_t)(voice[v].out >> 7);
    st.ended = voice[v].ended;
    return st;
}

void nshvc_sound_volume(int8_t l, int8_t r) { if (eng_on("nshvc_sound_volume", ENG_AUDIO)) { mvol_l = l; mvol_r = r; } }
void nshvc_sound_noise_rate(uint8_t rate) { if (eng_on("nshvc_sound_noise_rate", ENG_AUDIO)) noise_rate = rate & 31; }
void nshvc_echo_writes(int on) { if (eng_on("nshvc_echo_writes", ENG_AUDIO)) echo_writes_off = !on; }
void nshvc_sound_mute(int on) { if (eng_on("nshvc_sound_mute", ENG_AUDIO)) muted = on != 0; }

void nshvc_echo(uint8_t delay, int8_t feedback, int8_t l, int8_t r, const int8_t f[8])
{
    if (!eng_on("nshvc_echo", ENG_AUDIO))
        return;
    echo_len_set = delay & 15;
    efb = feedback;
    evol_l = l;
    evol_r = r;
    if (f)
        memcpy(fir, f, 8);
}
