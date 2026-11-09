#ifndef ENGINE_INSPECT_H
#define ENGINE_INSPECT_H
#include <stdio.h>
void eng_inspect_start(const char *path);
void eng_inspect_ask(void);
FILE *eng_inspect_take(void);       /* non-NULL once for each request */
void eng_inspect_stop(void);
void eng_inspect_voices(FILE *f);   /* sound.c */
#endif
