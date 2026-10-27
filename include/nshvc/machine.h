/*
 * machine.h: the machine as the decomp's C sees it.
 *
 * The C reads and writes the S-CPU's 24-bit address space through rd8/wr8,
 * exactly as the original code does, so a routine's C has the same inputs
 * and outputs as its assembly. nativeshvc answers each access as the
 * console does; a decomp's verification harness answers the same calls
 * from the original's record. This header, with spc.h, is the game's side
 * of the contract (docs/contract.md); it changes only with nativeshvc's
 * major version.
 *
 * Routines that assembly still calls also need the register file: each such
 * routine has an abi_ wrapper that takes its inputs from `cpu` and leaves its
 * outputs there, as its entry contract states.
 */
#ifndef NSHVC_MACHINE_H
#define NSHVC_MACHINE_H

#include <stdint.h>

uint8_t rd8(uint32_t addr);
void wr8(uint32_t addr, uint8_t value);

/* Multi-byte accesses are little-endian and linear in the 24-bit space,
   the low byte first, as the CPU makes them (lockstep places each access
   on the original's, so the order is part of the behaviour). Each call is
   one operation: no interrupt comes between its bytes. */
uint16_t rd16(uint32_t addr);
void wr16(uint32_t addr, uint16_t value);
uint32_t rd24(uint32_t addr);

/* A read-modify-write instruction (ASL, LSR, ROL, ROR, INC, DEC, TSB, TRB
   on memory): its read, then its write, one operation, no interrupt
   between them: rd8_rmw then wr8, or rd16_rmw then wr16_rmw, whose write
   is the high byte first, then the low, as the CPU makes them. The C may
   state time between the two (cyc); another read before the write is a
   fatal error. */
uint8_t rd8_rmw(uint32_t addr);
uint16_t rd16_rmw(uint32_t addr);
void wr16_rmw(uint32_t addr, uint16_t value);

/* Interrupts: the CPU takes one between two of its operations, never in
   one. The console takes it at the first point between two of the C's
   operations (an access, a word, a read-modify-write, a stretch of time)
   at or after the time it comes due, the IRQ unless the I flag in `cpu`
   masks it: at an operation's end, or, in a stretch of time (cyc, cycs,
   cpu_idle), at the clock it comes due. The C marks no instruction's end:
   the original takes the interrupt at its instruction's end, later than
   here by the rest of that instruction (README.md, Time). */

/* The 65816 register file, for abi_ wrappers. */
typedef struct {
    uint16_t a, x, y, s, d;
    uint8_t db, p, e;
    uint32_t pc;
    /* Set by C that leaves through its caller's frame (pulling the
       caller's saved registers and return address): s and pc are then
       where it lands, not the routine's own return. */
    uint8_t unwound;
} Regs65816;

extern Regs65816 cpu;

enum {
    P_C = 0x01, P_Z = 0x02, P_I = 0x04, P_D = 0x08,
    P_X = 0x10, P_M = 0x20, P_V = 0x40, P_N = 0x80,
};

/* N and Z as a 16-bit load sets them. */
static inline uint8_t nz16(uint8_t p, uint16_t v)
{
    p &= (uint8_t)~(P_N | P_Z);
    if (v == 0) p |= P_Z;
    if (v & 0x8000) p |= P_N;
    return p;
}

/* N and Z as an 8-bit load (or pull) sets them. */
static inline uint8_t nz8(uint8_t p, uint8_t v)
{
    p &= (uint8_t)~(P_N | P_Z);
    if (v == 0) p |= P_Z;
    if (v & 0x80) p |= P_N;
    return p;
}

/* Call the routine at `addr` (a JSL when `long_call`, else a JSR) with the
   register file in `cpu` as its entry state, leaving its exit state there:
   how the C makes the calls the original makes through an address it
   computes (handler tables, addresses pushed for an RTL): the abi_
   wrapper of the routine with C that starts there. (Every routine has C;
   the verification harness fails a run in which C reaches an address where
   none starts. A computed address past a table's end, which the game
   never makes, is the original's own runaway.) */
void call_routine(uint32_t addr, int long_call);
/* The original JMPs to addr (a jump table's handler, a tail jump): run it
   in the calling routine's own frame, to wherever it leaves that frame (its
   return, or through its caller's). The calling routine's last action. */
void jump_routine(uint32_t addr);
/* As jump_routine, for code the original reaches with values it pushed
   first (PHP, PHB...) still on the stack: pushed[0] the first pushed. */
void jump_routine_pushed(uint32_t addr, const uint8_t *pushed, int n);
/* A branch to itself (BRA to its own address, at `at`): the CPU never goes
   on. It makes no access, so nothing changes but time; the interrupts the
   machine raises still run between its passes. Never returns. (`at` is
   where the loop is: the verification harness finds it in the original's
   record there.) */
void cpu_loop_forever(uint32_t at);
/* The original jumps to `at`, code that resets the stack (LDX #$02FF,
   TXS): every frame of the calls under it is gone, the C's own included,
   and `entry`, the C of the code at `at`, runs from there with the
   registers in `cpu`. Never returns. */
void cpu_restart(uint32_t at, void (*entry)(void));
/* WAI at `at`: the CPU stops until an interrupt is due (an NMI, or the IRQ
   line, masked or not), then goes on: the interrupt is taken there when it
   is not masked. Its opcode's fetch, then internal cycles: the one that
   begins with the interrupt due and one more. */
void cpu_wai(uint32_t at);
/* Time the original spends with no access the C makes: a delay loop's
   turns (its fetches and internal cycles), `clocks` master clocks (21.477
   MHz). The rest of the machine runs meanwhile, so the time matters: the
   sound CPU acting on a command before the next one arrives. An interrupt
   that comes due in it is taken at its time. */
void cpu_idle(uint32_t clocks);
/* The time the original spends here between two bus accesses, besides
   their own: its instruction fetches and internal cycles, `clocks` master
   clocks. The C states it where its code stands for the original's, so
   each access comes at the original's time. */
void cyc(uint32_t clocks);
/* The same time stated cycle by cycle, in order: each hex digit of
   `lengths` one cycle's master clocks (6, 8, or C for 12), the first cycle
   the leftmost digit: cycs(0x8668) is cycles of 8, 6, 6 and 8 clocks, up
   to 16 a call. Each is a cycle of the original's, where the console's
   DMA unit takes the bus as the S-CPU's does: a DMA a $420B write starts
   runs at the second cycle after it, an HDMA due at the first cycle that
   begins after its time. A stretch a DMA or HDMA falls in wants stating
   this way (NSHVC_AUDIT=1 names each cyc that one falls in). */
void cycs(uint64_t lengths);
/* `v`, with the time before an access stated after the access's other
   arguments are worked out: where the access is reached in the middle of
   an expression, so no statement before it stands for that place. */
static inline uint32_t cyc_then(uint32_t clocks, uint32_t v)
{
    cyc(clocks);
    return v;
}
/* `again`, a loop's test, with the time the original takes to go round
   once more stated when it does. */
static inline int cyc_loop(uint32_t clocks, int again)
{
    if (again)
        cyc(clocks);
    return again;
}
/* cyc_then and cyc_loop, the time stated cycle by cycle (cycs) */
static inline uint32_t cycs_then(uint64_t lengths, uint32_t v)
{
    cycs(lengths);
    return v;
}
static inline int cycs_loop(uint64_t lengths, int again)
{
    if (again)
        cycs(lengths);
    return again;
}

#endif
