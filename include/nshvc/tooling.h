/*
 * tooling.h: calls for checking a decomp against its original, outside the
 * game contract (docs/contract.md). Neither changes the machine or takes
 * any time; a game plays the same without them, and a build that leaves
 * them out loses only what the tools see.
 */
#ifndef NSHVC_TOOLING_H
#define NSHVC_TOOLING_H

#include <stdint.h>

/* A mark in NSHVC_CYCLOG's stream (an M line, `id`, between the lines of
   the accesses and time around it), for lining a record up with the C
   that made it. Without NSHVC_CYCLOG it does nothing. */
void nshvc_mark(uint32_t id);

/* The SPC700 instruction at `at` begins: a place a script's spccall may
   run its routine (called from a stub, its registers and the stub put back
   after, the driver going on here), as a reference harness runs one when
   the SPC700 reaches that instruction. The driver's C marks the places
   such calls start from (a timer wait). With no call waiting it does
   nothing. */
void spc_at(uint16_t at);

#endif
