/* engine.h: what the engine's own parts share. */
#ifndef ENGINE_H
#define ENGINE_H

#include "nshvc/nshvc.h"

/* which of the engine's threads this is, as a bit for eng_on */
enum { ENG_SYSTEM = 1, ENG_GAME = 2, ENG_RENDER = 4, ENG_AUDIO = 8, ENG_JOB = 16 };
extern _Thread_local int eng_role;

/* A game's misuse of the API, naming the call and the bad value: ends the run
   in a debug build; in a release build reported once a call. */
void eng_report(const char *call, const char *fmt, ...);

/* 1 if this thread is one of `roles`, else reported */
int eng_on(const char *call, int roles);

/* the job pool (jobs.c) */
void eng_jobs_start(void);
void eng_jobs_stop(void);

/* the input (input.c); the host's event type is opaque here */
struct host_event;
void eng_input_start(const char *replay_path);
void eng_input_event(const struct host_event *e);
void eng_input_sample(nshvc_input *in, uint64_t n, int headless);
int  eng_input_touch_host(void);
int  eng_input_zones(void);
void eng_input_zone(int z, float *x, float *y, float *r);

/* the screen last drawn (picture.c) */
void eng_picture_size(int *w, int *h);

/* saves (files.c) */
void eng_files_start(void);
void eng_files_stop(void);

/* the sound (sound.c) */
void eng_sound_start(const nshvc_music *m, int headless, const char *sound_path);
void eng_sound_stop(void);
void eng_sound_advance(uint64_t updates, uint32_t update_hz);
double eng_sound_cost_ms(void);

#endif
