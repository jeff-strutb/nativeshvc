/* report.c: errors in a game's use of the API (docs/api.md section 0). */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine.h"

_Thread_local int eng_role;

/* the calls already reported, by the address of their name */
#define SEEN 128
static _Atomic(const char *) seen[SEEN];

static int first_time(const char *call)
{
    int i;
    for (i = 0; i < SEEN; i++) {
        const char *none = NULL;
        if (atomic_compare_exchange_strong(&seen[i], &none, call))
            return 1;
        if (none == call)
            return 0;
    }
    return 0;
}

void eng_report(const char *call, const char *fmt, ...)
{
    va_list ap;
#ifdef NDEBUG
    if (!first_time(call))
        return;
#else
    (void)first_time;
#endif
    fprintf(stderr, "nativeshvc: %s: ", call);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
#ifndef NDEBUG
    exit(70);
#endif
}

int eng_on(const char *call, int roles)
{
    if (eng_role & roles)
        return 1;
    eng_report(call, "called on a thread that may not call it (thread role %d)", eng_role);
    return 0;
}
