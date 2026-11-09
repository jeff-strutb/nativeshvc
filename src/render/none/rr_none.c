/* rr_none.c: no GPU: the loop and threads run, and no picture is made. */
#include "rr.h"

const char *rr_shader_ext(void) { return "spv"; }
int rr_open(void) { return 1; }
void rr_close(void) {}
const char *rr_name(void) { return "none"; }
int rr_draw(eng_packet *p) { (void)p; return 0; }
int rr_read(uint32_t *argb, int *w, int *h) { (void)argb; *w = *h = 0; return 0; }
int rr_read_window(uint32_t *argb, int *w, int *h) { (void)argb; *w = *h = 0; return 0; }
