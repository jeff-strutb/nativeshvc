/*
 * mix_replay.c: the native mixer (src/dsp) given what the reference's
 * sound chip was given, at the clocks it was given it, held to the chip's
 * own output and replies, sample for sample.
 *
 *   mix_replay SPCLOG SOUND [VOICES]
 *
 * SPCLOG is the oracle's --spclog record (each SPC700 write, and each read
 * of $F0-$FF, at its time in half cycles from power-on); SOUND the
 * oracle's --sound of the same run (each sample as the chip made it);
 * VOICES its --voices (each voice's envelope level and stage, pitch
 * counter, BRR block and byte, at each sample). The chip's clock is the
 * SPC700's cycle, from power-on with the APU RAM clear, so the mixer runs
 * clock by clock and each write lands between the same two clocks it did
 * on the chip: in the APU RAM and, for $F3, in the DSP register $F2 names.
 * Each read of $F3 is held to the mixer's register; each read of the APU
 * RAM, when the log has them (--spcreads), to the mixer's, which holds
 * what the echo wrote.
 * Prints the first difference of each kind and the counts; exit 1 on any.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mixer.h"
#include "mixer_regs.h"

static const char *field[] = { "level", "stage", "position", "block", "byte" };

int main(int argc, char **argv)
{
    FILE *lf, *sf, *vf = NULL;
    static uint8_t ram[65536];
    uint8_t rec[8], f2 = 0;
    int have = 0;
    uint64_t clocks = 0;
    uint32_t made = 0;
    unsigned long bad = 0, vbad = 0, rbad = 0, mbad = 0, reads = 0, mreads = 0;
    if (argc < 3 || !(lf = fopen(argv[1], "rb")) || !(sf = fopen(argv[2], "rb")) ||
        (argc > 3 && !(vf = fopen(argv[3], "rb")))) {
        fprintf(stderr, "usage: mix_replay SPCLOG SOUND [VOICES]\n");
        return 2;
    }
    mix_init(ram);
    mix_regs_reset();
    for (;;) {
        int16_t l, r, want[2];
        /* the accesses made after this many clocks */
        for (;;) {
            uint32_t t;
            uint16_t a;
            if (!have) {
                if (fread(rec, 1, 8, lf) != 8)
                    break;
                have = 1;
            }
            t = rec[0] | rec[1] << 8 | rec[2] << 16 | (uint32_t)rec[3] << 24;
            /* a port read is made between its cycle's two halves */
            if ((t + 1) / 2 > clocks)
                break;
            a = (uint16_t)(rec[5] | rec[6] << 8);
            if (!rec[4]) {
                if ((a < 0xF0 || a > 0xFF) && a < 0xFFC0) {
                    mreads++;
                    if (ram[a] != rec[7] && !mbad++)
                        fprintf(stderr, "first RAM read that differs at clock %llu: $%04X, reference %02X, mixer %02X\n",
                                (unsigned long long)clocks, a, rec[7], ram[a]);
                } else if (a == 0xF3) {
                    uint8_t mine = mix_reg_read(f2 & 0x7F);
                    reads++;
                    if (mine != rec[7] && !rbad++)
                        fprintf(stderr, "first read that differs at clock %llu: DSP $%02X, reference %02X, mixer %02X\n",
                                (unsigned long long)clocks, f2 & 0x7F, rec[7], mine);
                }
            } else {
                ram[a] = rec[7];
                if (a == 0xF2)
                    f2 = rec[7];
                else if (a == 0xF3 && !(f2 & 0x80))
                    mix_reg_write(f2, rec[7]);
            }
            have = 0;
        }
        clocks++;
        if (!mix_clock(&l, &r))
            continue;
        if (fread(want, 2, 2, sf) != 2)
            break;
        if ((l != want[0] || r != want[1]) && !bad++)
            fprintf(stderr, "first difference at sample %u: reference %d %d, mixer %d %d\n", made, want[0], want[1], l,
                    r);
        if (vf) {
            int32_t chip[40], mine[40];
            int k;
            if (fread(chip, 4, 40, vf) != 40)
                break;
            mix_voice_states(mine);
            for (k = 0; k < 40; k++)
                if (chip[k] != mine[k]) {
                    if (!vbad)
                        fprintf(stderr, "voice state first differs at sample %u: voice %d %s, reference %d, mixer %d\n",
                                made, k / 5, field[k % 5], chip[k], mine[k]);
                    vbad++;
                    break;
                }
        }
        made++;
    }
    printf("%lu of %u samples differ, %lu with voice states that differ; %lu of %lu DSP reads, %lu of %lu RAM reads\n",
           bad, made, vbad, rbad, reads, mbad, mreads);
    return bad || vbad || rbad || mbad;
}
