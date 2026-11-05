/* host_win32.c: time and files on Windows, what host_posix.c is on macOS
 * and Linux. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

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
