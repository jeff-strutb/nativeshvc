/*
 * jump.h: a non-local jump within one stack (a coroutine's, or the
 * program's), with no unwinding and no signal mask: GCC's builtin pair on
 * x86-64 (Windows's longjmp unwinds), else _setjmp/_longjmp. Not clang's
 * builtin pair: its __builtin_setjmp keeps the frame pointer in a stack
 * slot the function's own spills may reuse (seen in spc_main on x86-64
 * macOS), and the jump back then restores another value.
 */
#ifndef PORT_JUMP_H
#define PORT_JUMP_H

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
typedef void *port_jmp_buf[5];
#define port_setjmp(b) __builtin_setjmp(b)
#define port_longjmp(b) __builtin_longjmp(b, 1)
#else
#include <setjmp.h>
typedef jmp_buf port_jmp_buf;
#define port_setjmp(b) _setjmp(b)
#define port_longjmp(b) _longjmp(b, 1)
#endif

#endif
