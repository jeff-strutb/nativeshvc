/*
 * hello.c: the smallest game on the engine. Its state advances by a job for
 * each cell, once per update, and its final state is printed at exit: the same
 * number of updates gives the same state however the threads ran.
 *
 *   hello                          real time, until the window closes
 *   hello --headless --updates N   N updates as fast as they run
 */
#include <inttypes.h>
#include <stdio.h>

#include "nshvc/nshvc.h"

#define CELLS 256
static uint32_t cells[CELLS];
static uint64_t pictures;

static void step_cell(void *context, uint32_t index)
{
    (void)context;
    cells[index] = cells[index] * 2654435761u + index + 1u;
}

static void update(const nshvc_input *input)
{
    (void)input;
    nshvc_jobs(step_cell, NULL, CELLS);
}

static void draw(float between)
{
    (void)between;
    pictures++;                 /* not game state: it depends on the display */
}

static void shutdown(void)
{
    uint64_t hash = 14695981039346656037ull;
    uint32_t i;
    for (i = 0; i < CELLS; i++)
        hash = (hash ^ cells[i]) * 1099511628211ull;
    printf("updates %" PRIu64 " pictures %" PRIu64 " state %016" PRIx64 "\n", nshvc_updates(), pictures, hash);
}

int main(int argc, char **argv)
{
    static nshvc_game game;
    game.title = "Hello";
    game.id = "hello";
    game.update_hz = 60;
    game.update = update;
    game.draw = draw;
    game.shutdown = shutdown;
    return nshvc_run(&game, argc, argv);
}
