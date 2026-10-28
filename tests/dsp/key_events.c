/*
 * key_events.c: the notes a run's DSP played: the native mixer (src/dsp)
 * given a run's SPC700 writes at their clocks, as mix_replay gives it the
 * reference's, and each voice's key events read off its envelope: a
 * key-on (the envelope back to its attack, from any stage or from a
 * level above where a held attack goes on), a key-off (to its release).
 *
 *   key_events SPCLOG
 *
 * SPCLOG is the oracle's --spclog or nativeshvc's NSHVC_SPCLOG of the run
 * (each SPC700 write at its time in half cycles from power-on). Prints a
 * line an event: the sample it came at, the voice, "on" with the BRR block
 * the voice plays 8 samples on (its sample's start, from the directory as
 * the key-on read it), or "off".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mixer.h"
#include "mixer_regs.h"

enum { RELEASE, ATTACK };

int main(int argc, char **argv)
{
    FILE *lf;
    static uint8_t ram[65536];
    uint8_t rec[8], f2 = 0;
    int have = 0, v;
    uint64_t clocks = 0;
    uint32_t made = 0;
    int32_t was[40] = { 0 }, now[40];
    uint32_t on_at[8];
    int pending[8] = { 0 };
    if (argc < 2 || !(lf = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: key_events SPCLOG\n");
        return 2;
    }
    mix_init(ram);
    mix_regs_reset();
    for (;;) {
        int16_t l, r;
        int more = 1;
        for (;;) {
            uint32_t t;
            uint16_t a;
            if (!have) {
                if (fread(rec, 1, 8, lf) != 8) {
                    more = 0;
                    break;
                }
                have = 1;
            }
            t = rec[0] | rec[1] << 8 | rec[2] << 16 | (uint32_t)rec[3] << 24;
            if ((t + 1) / 2 > clocks)
                break;
            a = (uint16_t)(rec[5] | rec[6] << 8);
            if (rec[4] == 1) {
                ram[a] = rec[7];
                if (a == 0xF2)
                    f2 = rec[7];
                else if (a == 0xF3 && !(f2 & 0x80))
                    mix_reg_write(f2, rec[7]);
            }
            have = 0;
        }
        if (!more)
            break;
        clocks++;
        if (!mix_clock(&l, &r))
            continue;
        mix_voice_states(now);
        for (v = 0; v < 8; v++) {
            int stage = now[5 * v + 1], level = now[5 * v];
            if (stage == ATTACK && (was[5 * v + 1] != ATTACK || level < was[5 * v])) {
                on_at[v] = made;
                pending[v] = 8;
            } else if (stage == RELEASE && was[5 * v + 1] != RELEASE) {
                printf("%u %d off\n", made, v);
            }
            if (pending[v] && !--pending[v])
                printf("%u %d on %04X\n", on_at[v], v, (unsigned)now[5 * v + 3]);
        }
        for (v = 0; v < 40; v++)
            was[v] = now[v];
        made++;
    }
    return 0;
}
