/* sys_psp.c: time and files on the PSP, by the system's own calls, and
   exit as C has it. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include "host.h"

uint64_t host_ticks_ns(void) { return (uint64_t)sceKernelGetSystemTimeWide() * 1000u; }

void host_sleep_ms(uint32_t ms) { sceKernelDelayThread((SceUInt)ms * 1000u); }

/* threads run by priority, with no timer coalescing to ask off */
void host_thread_interactive(void) {}

static uint32_t s_every_ms;
static void (*s_every)(void);

static int every_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread((SceUInt)s_every_ms * 1000u);
        s_every();
    }
    return 0;
}

/* (a priority over the game's thread, so it wakes while the game runs) */
void host_every(uint32_t ms, void (*fn)(void))
{
    SceUID t;
    s_every_ms = ms;
    s_every = fn;
    t = sceKernelCreateThread("nshvc_every", every_thread, 0x11, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
}

/* made, or there already (0x80010011: the system's EEXIST) */
int host_mkdir(const char *path)
{
    int r = sceIoMkdir(path, 0777);
    return r >= 0 || r == (int)0x80010011;
}

/* exit (C11 7.22.4.4): the functions atexit registered, the streams
   flushed, then the system's _exit, as newlib's own exit does. pspsdk's
   (libcglue's, weak) goes straight to _exit: nothing atexit registered
   runs and buffered output is lost. */
void __call_exitprocs(int code, void *d);

void exit(int code)
{
    __call_exitprocs(code, NULL);
    fflush(NULL);
    _exit(code);
}
