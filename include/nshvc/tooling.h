/*
 * tooling.h: a call for checking a decomp against its original, outside
 * the game contract (docs/contract.md). It changes nothing in the machine
 * and takes no time; a game plays the same without it, and a build that
 * leaves it out loses only what the tools see.
 */
#ifndef NSHVC_TOOLING_H
#define NSHVC_TOOLING_H

#include <stdint.h>

/* A mark in NSHVC_CYCLOG's stream (an M line, `id`, between the lines of
   the accesses and time around it), for lining a record up with the C
   that made it. Without NSHVC_CYCLOG it does nothing. */
void nshvc_mark(uint32_t id);

#endif
