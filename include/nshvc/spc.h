/*
 * spc.h: the SPC700 as the sound driver's C sees it.
 *
 * The C of an SPC700 routine works on `spc` (A, X, Y, SP and the PSW flags)
 * and makes every APU RAM and I/O access through spc_rd/spc_wr, in the
 * order and number the assembly makes them (a decomp's checks compare the
 * accesses with the original's; nativeshvc answers them as the sound unit
 * does: APU RAM, the DSP through $F2/$F3, the ports $F4-$F7, the timers
 * $FA-$FF). The stack is page 1; direct page is page 0 or 1 by P. The
 * SPC700 takes no interrupts, so nothing comes between the C's operations.
 * With machine.h, the game's side of the contract (docs/contract.md).
 *
 * Each access the C makes costs its one bus cycle of the sound unit's
 * clock (1,025,280 Hz, the DSP's 32,040 samples a second), and before it
 * the weight of its kind from the cartridge's cost profile (nshvc.h
 * spc_tenths, docs/cost-profile.md; 0 with none): the driver's own work
 * around it. The C states no time. The clock also
 * moves on where the code waits for the hardware (spc_wait_until: a
 * timer's counter, a port), the DSP and the timers running meanwhile. So
 * an S-CPU that writes a port's index a cycle before its byte (a word to
 * $2140) is answered as on the console: the read after the one that sees
 * the index comes a cycle later and sees the byte. One rule keeps the DSP hearing what the original
 * made it hear: after a write of KON or KOFF, the next write to the DSP
 * waits until the DSP has taken that key event (it takes them every other
 * sample), so no note is lost or cut short, and none starts with the
 * voice's registers written after it.
 *
 * A routine is entered by CALL, PCALL or TCALL: the caller pushes the return
 * address (spc_call), the routine's RET pulls it (spc_ret). abi_Name(void)
 * is the routine at its entry.
 */
#ifndef NSHVC_SPC_H
#define NSHVC_SPC_H

#include <stdint.h>

typedef struct {
    uint8_t a, x, y, sp;
    uint8_t n, v, p, b, h, i, z, c;   /* PSW flags, each 0 or 1 */
} SpcRegs;

extern SpcRegs spc;

uint8_t spc_rd(uint16_t addr);
void spc_wr(uint16_t addr, uint8_t v);

/* Wait for the hardware: the original's poll loop (a timer's counter at
   $FD-$FF, a port the S-CPU writes). `ready` makes the loop's reads
   (spc_rd: real accesses, a counter's read clearing it as the original's
   does) and says whether the loop ends; it is called at once, and while
   it says no the sound unit's clock moves on a cycle at a time, the DSP,
   the timers and the S-CPU running meanwhile. Returns the SPC700 cycles
   waited. (A loop of reads with no wait sees the clock move too, a cycle
   an access.) */
uint32_t spc_wait_until(int (*ready)(void *ctx), void *ctx);

static inline uint16_t spc_dp(uint8_t d) { return (uint16_t)(spc.p ? 0x0100 | d : d); }
static inline uint16_t spc_ya(void) { return (uint16_t)(spc.y << 8 | spc.a); }
static inline void spc_set_ya(uint16_t v) { spc.a = (uint8_t)v; spc.y = (uint8_t)(v >> 8); }

static inline void spc_push(uint8_t v) { spc_wr((uint16_t)(0x0100 | spc.sp), v); spc.sp--; }
static inline uint8_t spc_pull(void) { spc.sp++; return spc_rd((uint16_t)(0x0100 | spc.sp)); }

static inline uint8_t spc_psw(void)
{
    return (uint8_t)(spc.n << 7 | spc.v << 6 | spc.p << 5 | spc.b << 4 | spc.h << 3 | spc.i << 2 | spc.z << 1 | spc.c);
}

static inline void spc_set_psw(uint8_t v)
{
    spc.n = v >> 7 & 1; spc.v = v >> 6 & 1; spc.p = v >> 5 & 1; spc.b = v >> 4 & 1;
    spc.h = v >> 3 & 1; spc.i = v >> 2 & 1; spc.z = v >> 1 & 1; spc.c = v & 1;
}

/* CALL: the return address pushed (high byte first), then the routine. */
static inline void spc_call(void (*abi)(void), uint16_t ret)
{
    spc_push((uint8_t)(ret >> 8));
    spc_push((uint8_t)ret);
    abi();
}

/* The driver jumps to `at`, code that never comes back to what called it
   (a reload loop that ends by restarting the driver): every C frame of
   the calls under it is gone, and `entry`, the C of the code at `at`,
   runs from there. Never returns. A decomp's checks end the call there
   and take `entry` on its own. */
void spc_restart(uint16_t at, void (*entry)(void));

/* JMP $FFC0: the console's boot ROM (the SPC700's own, not the game's): it
   takes the next upload from the S-CPU by its transfer protocol and runs
   it. Never returns. */
void spc_boot_rom(void);

/* RET: the return address pulled (low byte first); the checks end the
   call at it. */
static inline uint16_t spc_ret(void)
{
    uint16_t lo = spc_pull();
    return (uint16_t)(lo | spc_pull() << 8);
}

#endif
