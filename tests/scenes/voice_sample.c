/* voice_sample.c: nshvc_voice_sample (6.6) against the voice it stands for.
 *
 * A voice that is given sample B while it plays sample A goes on, when A next
 * ends (its block with the end flag), into B's loop, keeping its filter
 * history, as the SNES's DSP does when SRCN changes. That is the same block
 * stream as one sample C made of A's blocks then B's looped blocks, looping
 * into B's part. So:
 *   voice 0  key-on A, then nshvc_voice_sample(0, B) before A's first end,
 *            heard on the left only;
 *   voice 1  key-on C, heard on the right only;
 *   voice 2  key-on A and nothing more, silent: the control, which must
 *            differ from voice 1 once A has ended, or the test proves nothing.
 *
 *   voice_sample --headless --updates 120 --sound FILE
 *       prints, from nshvc_voice_read each sample, how many samples voices 0
 *       and 1 differ in (level, output, ended; expected 0) and how many voices
 *       2 and 1 differ in (expected many), and exits 1 if either is wrong;
 *   voice_sample --check FILE
 *       compares the run's left and right, every sample at full precision;
 *       exits 1 on the first difference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"

#define BLOCK 9

static nshvc_sample a, b, c;
static uint32_t samples_run;
static uint32_t differ_switched, differ_control, compared;

/* a BRR block: header, then 16 nibbles from a fixed sequence */
static void block(uint8_t *out, uint8_t header, uint32_t *seed)
{
    int i;
    out[0] = header;
    for (i = 1; i < BLOCK; i++) {
        *seed = *seed * 1103515245u + 12345u;
        out[i] = (uint8_t)(*seed >> 16);
    }
}

/* header: shift in bits 4-7, filter in bits 2-3, loop bit 1, end bit 0 */
#define HDR(shift, filter, loop, end) (uint8_t)((shift) << 4 | (filter) << 2 | (loop) << 1 | (end))

static void init(void)
{
    uint8_t da[4 * BLOCK], db[3 * BLOCK], dc[6 * BLOCK];
    uint32_t seed = 7;
    block(da + 0 * BLOCK, HDR(10, 0, 0, 0), &seed);
    block(da + 1 * BLOCK, HDR(10, 1, 0, 0), &seed);
    block(da + 2 * BLOCK, HDR(9, 2, 0, 0), &seed);
    block(da + 3 * BLOCK, HDR(9, 1, 1, 1), &seed);
    block(db + 0 * BLOCK, HDR(11, 0, 0, 0), &seed);
    block(db + 1 * BLOCK, HDR(8, 3, 0, 0), &seed);
    block(db + 2 * BLOCK, HDR(10, 1, 1, 1), &seed);
    memcpy(dc, da, sizeof da);
    memcpy(dc + 4 * BLOCK, db + 1 * BLOCK, 2 * BLOCK);
    a = nshvc_sample_load(da, sizeof da, NSHVC_SAMPLE_BRR, 2 * BLOCK);
    b = nshvc_sample_load(db, sizeof db, NSHVC_SAMPLE_BRR, 1 * BLOCK);
    c = nshvc_sample_load(dc, sizeof dc, NSHVC_SAMPLE_BRR, 4 * BLOCK);
}

static int same(nshvc_voice_state x, nshvc_voice_state y)
{
    return x.level == y.level && x.output == y.output && x.ended == y.ended;
}

/* at the DSP's rate: one call a sample */
static void tick(void)
{
    uint32_t v;
    if (samples_run == 0) {
        nshvc_sound_volume(127, 127);
        for (v = 0; v < 3; v++) {
            nshvc_voice_pitch(v, 0x1000);
            nshvc_voice_gain(v, NSHVC_GAIN_DIRECT, 0x7F);
        }
        nshvc_voice_volume(0, 127, 0);
        nshvc_voice_volume(1, 0, 127);
        nshvc_voice_volume(2, 0, 0);
        nshvc_voice_key_on(0, a);
        nshvc_voice_key_on(1, c);
        nshvc_voice_key_on(2, a);
    } else {
        nshvc_voice_state s0 = nshvc_voice_read(0), s1 = nshvc_voice_read(1), s2 = nshvc_voice_read(2);
        if (samples_run == 20)       /* A plays its first block: its end is about 64 samples on */
            nshvc_voice_sample(0, b);
        compared++;
        differ_switched += !same(s0, s1);
        differ_control += !same(s2, s1);
    }
    samples_run++;
}

static void update(const nshvc_input *in) { (void)in; }
static void draw(float between) { (void)between; }

static void finish(void)
{
    int ok = differ_switched == 0 && differ_control > 0;
    printf("voice_sample: %u samples compared; switched voice differs from the joined sample in %u (expected 0), "
           "the control in %u (expected more than 0): %s\n", compared, differ_switched, differ_control, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok)
        exit(1);
}

/* the run's --sound: left (voice 0) against right (voice 1) */
static int check(const char *path)
{
    FILE *f = fopen(path, "rb");
    int16_t lr[2];
    unsigned long n = 0, nonzero = 0;
    if (!f) {
        fprintf(stderr, "voice_sample: cannot read %s\n", path);
        return 2;
    }
    while (fread(lr, 2, 2, f) == 2) {
        if (lr[0] != lr[1]) {
            printf("voice_sample --check: left %d and right %d differ at sample %lu: FAIL\n", lr[0], lr[1], n);
            fclose(f);
            return 1;
        }
        nonzero += lr[0] != 0;
        n++;
    }
    fclose(f);
    printf("voice_sample --check: %lu samples, left and right the same in every one (%lu not 0): %s\n", n, nonzero,
           n && nonzero ? "pass" : "FAIL");
    return n && nonzero ? 0 : 1;
}

int main(int argc, char **argv)
{
    static nshvc_game g;
    if (argc == 3 && !strcmp(argv[1], "--check"))
        return check(argv[2]);
    g.title = "Voice sample";
    g.id = "voice-sample";
    g.update_hz = 60;
    g.init = init;
    g.update = update;
    g.draw = draw;
    g.shutdown = finish;
    g.music.hz = 32040;
    g.music.tick = tick;
    return nshvc_run(&g, argc, argv);
}
