/* host_win32.c: time and files on Windows, what host_posix.c is on macOS
 * and Linux. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

uint64_t host_ticks_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000u +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000u / (uint64_t)f.QuadPart;
}

static uint32_t s_every_ms;
static void (*s_every)(void);

static DWORD WINAPI every_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(s_every_ms);
        s_every();
    }
    return 0;
}

void host_every(uint32_t ms, void (*fn)(void))
{
    HANDLE t;
    s_every_ms = ms;
    s_every = fn;
    t = CreateThread(NULL, 0, every_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

/* Sleep() wakes on the system timer's tick (15.6 ms unless a program asks
   for finer): the first sleep asks for 1 ms */
void host_sleep_ms(uint32_t ms)
{
    static int fine;
    if (!fine) {
        timeBeginPeriod(1);
        fine = 1;
    }
    Sleep(ms);
}

void host_thread_interactive(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }

int host_mkdir(const char *path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* ---- threads ---- */
struct host_thread {
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};
struct host_mutex { SRWLOCK l; };
struct host_cond { CONDITION_VARIABLE c; };

static void die(const char *what)
{
    fprintf(stderr, "nativeshvc: cannot make a %s\n", what);
    exit(70);
}

uint32_t host_cores(void)
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n < 1 ? 1u : (uint32_t)n;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    host_thread *t = (host_thread *)arg;
    t->fn(t->arg);
    return 0;
}

host_thread *host_thread_start(void (*fn)(void *), void *arg, const char *name)
{
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    (void)name;
    if (!t)
        die("thread");
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (!t->h)
        die("thread");
    return t;
}

void host_thread_join(host_thread *t)
{
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

host_mutex *host_mutex_new(void)
{
    host_mutex *m = (host_mutex *)malloc(sizeof *m);
    if (!m)
        die("mutex");
    InitializeSRWLock(&m->l);
    return m;
}

void host_mutex_lock(host_mutex *m) { AcquireSRWLockExclusive(&m->l); }
void host_mutex_unlock(host_mutex *m) { ReleaseSRWLockExclusive(&m->l); }

host_cond *host_cond_new(void)
{
    host_cond *c = (host_cond *)malloc(sizeof *c);
    if (!c)
        die("condition");
    InitializeConditionVariable(&c->c);
    return c;
}

void host_cond_broadcast(host_cond *c) { WakeAllConditionVariable(&c->c); }

void host_cond_wait(host_cond *c, host_mutex *m, uint64_t timeout_ns)
{
    DWORD ms = INFINITE;
    if (timeout_ns != HOST_WAIT_FOREVER) {
        uint64_t up = (timeout_ns + 999999u) / 1000000u;
        ms = up >= INFINITE ? INFINITE - 1 : (DWORD)up;
    }
    SleepConditionVariableSRW(&c->c, &m->l, ms, 0);
}
