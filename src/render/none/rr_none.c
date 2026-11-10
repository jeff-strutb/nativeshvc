/* rr_none.c: no GPU: the loop and threads run, and no picture is made. */
#include <stdlib.h>

#include "rr.h"

const char *rr_shader_ext(void) { return "spv"; }
int rr_open(void) { return 1; }
void rr_close(void) {}
void rr_window(void) {}
const char *rr_name(void) { return "none"; }
int rr_samples(void) { return 4; }
int rr_samples_at_most(int n) { return n >= 4 ? 4 : n >= 2 ? 2 : 1; }
int rr_draw(rr_frame *f)
{
    uint32_t i;
    /* a game's shaders are ours to let go */
    for (i = 0; i < f->nshaders; i++)
        free((void *)f->shaders[i].code);
    f->nshaders = 0;
    return 0;
}
int rr_read(uint32_t *argb, int *w, int *h) { (void)argb; *w = *h = 0; return 0; }
