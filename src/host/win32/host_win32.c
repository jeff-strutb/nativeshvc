/* host_win32.c: time and files on Windows, what host_posix.c is on macOS
 * and Linux. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- the save folder: the game's own, or the system's (host.h) ---------------------- */
int host_save_place(const char *want, const char *old, char *out, size_t n, char *note, size_t note_n)
{
    char probe[2 * MAX_PATH + 32], mark[2 * MAX_PATH + 32], pat[2 * MAX_PATH + 8], from[2 * MAX_PATH + 8], to[2 * MAX_PATH + 8], tmp[2 * MAX_PATH + 16];
    HANDLE h;
    WIN32_FIND_DATAA fd;
    int copied = 0, failed = 0;
    note[0] = 0;
    snprintf(probe, sizeof probe, "%s\\.write-test", want);
    snprintf(mark, sizeof mark, "%s\\.carried-over", want);
    if (!host_mkdir(want) ||
        (h = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL)) == INVALID_HANDLE_VALUE) {
        snprintf(note, note_n, "nativeshvc: cannot write in %s, so saves stay in %s", want, old);
        snprintf(out, n, "%s", old);
        host_mkdir(old);
        return 0;
    }
    CloseHandle(h);
    snprintf(out, n, "%s", want);
    if (GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES)
        return 1;
    snprintf(pat, sizeof pat, "%s\\*", old);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            size_t k = strlen(fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || fd.cFileName[0] == '.' || !_stricmp(fd.cFileName, "log.txt") ||
                (k >= 4 && !_stricmp(fd.cFileName + k - 4, ".new")))
                continue;
            snprintf(from, sizeof from, "%s\\%s", old, fd.cFileName);
            snprintf(to, sizeof to, "%s\\%s", want, fd.cFileName);
            if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES)
                continue;
            snprintf(tmp, sizeof tmp, "%s.new", to);
            if (CopyFileA(from, tmp, FALSE) && MoveFileA(tmp, to))
                copied++;
            else {
                DeleteFileA(tmp);
                failed++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!failed && (h = CreateFileA(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (copied || failed)
        snprintf(note, note_n, "nativeshvc: carried %d save file(s) from %s into %s%s", copied, old, want,
                 failed ? " (some could not be copied; tried again next start)" : "");
    return 1;
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
