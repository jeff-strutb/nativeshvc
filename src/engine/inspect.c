/* inspect.c: what the game feeds the engine, written out while it runs, on
 * every platform: each picture's layers, sprites, palette and per-line table,
 * its draw lists, and each voice. F12 takes one, to --inspect's file. */
#include <stdio.h>

#include "engine.h"
#include "inspect.h"

static FILE *out;
static int asked;

void eng_inspect_start(const char *path)
{
    if (path && !(out = fopen(path, "w")))
        fprintf(stderr, "nativeshvc: cannot write %s\n", path);
}

void eng_inspect_ask(void) { asked = 1; }

/* the game thread, at the end of a draw */
FILE *eng_inspect_take(void)
{
    if (!out || !asked)
        return NULL;
    asked = 0;
    return out;
}

void eng_inspect_stop(void)
{
    if (out)
        fclose(out);
}
