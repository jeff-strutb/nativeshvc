/*
 * coro.h: coroutines, for the processors that run beside the S-CPU.
 *
 * The SPC700's C (and a cartridge chip's: cart.h nshvc_coro_*) run on
 * stacks of their own and hand
 * control back and forth with the S-CPU's (the program's own stack), one
 * running at a time: a deterministic schedule, no threads. The switch is a
 * few instructions of assembly on arm64, x86-64 and 32-bit MIPS
 * (coro_arm64.S, coro_x86_64.S, coro_mips.S); on Windows, the system's
 * fibers.
 */
#ifndef PORT_CORO_H
#define PORT_CORO_H

#include <stddef.h>

typedef struct Coro Coro;

/* the program's own stack, as a coroutine to switch back to */
Coro *coro_self(void);
/* the coroutine running now (the program's own stack until a switch) */
Coro *coro_current(void);
/* a coroutine that runs fn(arg) on a stack of `stack` bytes when first
   switched to; fn never returns */
Coro *coro_new(void (*fn)(void *), void *arg, size_t stack);
/* leave the running coroutine `from` for `to` (until something switches
   back to `from`) */
void coro_switch(Coro *from, Coro *to);
/* 0 once the coroutine has used its stack to within 64 bytes of its end
   (those bytes written: a check after the fact, for a host with no guard
   below a stack; Windows' fibers have one, and say 1) */
int coro_stack_ok(const Coro *c);

/* the S-CPU's and the SPC700's stacks: far more than the console's code
   takes (under 2 KB each, measured), smaller on the PSP, whose memory is
   24 MB in all */
#if defined(__PSP__)
#define CORO_CPU_STACK (1u << 20)
#define CORO_SPC_STACK (256u << 10)
#else
#define CORO_CPU_STACK (16u << 20)
#define CORO_SPC_STACK (4u << 20)
#endif

#endif
