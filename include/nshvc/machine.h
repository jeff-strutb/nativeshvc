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
 * The C states no time. Each access it makes costs what the console's bus
 * takes for it, and the weight of its kind from the cartridge's cost
 * profile (nshvc.h nshvc_cart.cost_profile, 0 with none); its own
 * arithmetic between accesses costs nothing. The clock also moves on where
 * the code waits for the hardware (the waits below) and while a DMA its
 * write starts stops the S-CPU. The machine is exact in what it does, not
 * in how long the C's own work takes (README.md, Time).
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
   the low byte first, as the CPU makes them (the order is part of the
   behaviour). Each call is one operation: no interrupt comes between its
   bytes. */
uint16_t rd16(uint32_t addr);
void wr16(uint32_t addr, uint16_t value);
uint32_t rd24(uint32_t addr);

/* A read-modify-write instruction (ASL, LSR, ROL, ROR, INC, DEC, TSB, TRB
   on memory): its read, then its write, one operation, no interrupt
   between them: rd8_rmw then wr8, or rd16_rmw then wr16_rmw, whose write
   is the high byte first, then the low, as the CPU makes them. Another
   read, or a wait, before the write is a fatal error. */
uint8_t rd8_rmw(uint32_t addr);
uint16_t rd16_rmw(uint32_t addr);
void wr16_rmw(uint32_t addr, uint16_t value);

/* Interrupts: the CPU takes one between two of its operations, never in
   one, the IRQ unless the I flag in `cpu` masks it. One that comes due in
   an access (or a DMA its write started) is taken at that operation's end;
   one that comes due in a wait is taken there at the clock it comes due,
   its handler run before the wait looks again. A handler may
   wait too: the I flag the interrupt sets holds the IRQ, as on the
   console, and an NMI may still come in an IRQ's handler. */

/* Wait for the hardware: the original's poll loop (a status bit, a
   counter, a chip's flag, the sound unit's reply). `ready` makes the
   loop's reads (rd8 and the rest: real accesses, their side effects as the
   original's) and says whether the loop ends; it is called at once, and
   while it says no the console's clock moves on in steps of a few of the
   S-CPU's cycles, everything else running meanwhile (lines drawn, HDMA,
   DMA, the sound unit, the cartridge's chip), each interrupt taken at the
   clock it comes due, before `ready` is called again. A change a line, the
   IRQ's time or the chip's IRQ makes is seen at its own clock; another
   (a port the sound unit writes, a chip's register) within 64 clocks.
   Returns the master clocks waited (21.477 MHz): a counter the original
   ticked in each turn of the loop is that over the turn's length. Fatal
   with interrupts held (nshvc_hold_interrupts) and between rd8_rmw and its
   write. */
uint64_t nshvc_wait_until(int (*ready)(void *ctx), void *ctx);
/* the wait for the next vertical blank's start */
void nshvc_wait_vblank(void);

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
   machine raises still run, each at the clock it comes due. Never returns.
   (`at` is where the loop is: the verification harness finds it in the
   original's record there.) */
void cpu_loop_forever(uint32_t at);
/* The original jumps to `at`, code that resets the stack (LDX #$02FF,
   TXS): every frame of the calls under it is gone, the C's own included,
   and `entry`, the C of the code at `at`, runs from there with the
   registers in `cpu`. Never returns. */
void cpu_restart(uint32_t at, void (*entry)(void));
/* WAI at `at`: the CPU stops until an interrupt is due (an NMI, or the IRQ
   line, masked or not), the clock moving on as in a wait, then goes on:
   the interrupt is taken there when it is not masked. */
void cpu_wai(uint32_t at);

#endif
