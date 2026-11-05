/* host_posix.c: time and files for POSIX systems (macOS, Linux, iOS). */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include "host.h"

uint64_t host_ticks_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void host_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

void host_thread_interactive(void)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

static uint32_t s_every_ms;
static void (*s_every)(void);

static void *every_thread(void *arg)
{
    (void)arg;
    for (;;) {
        host_sleep_ms(s_every_ms);
        s_every();
    }
    return NULL;
}

void host_every(uint32_t ms, void (*fn)(void))
{
    pthread_t t;
    s_every_ms = ms;
    s_every = fn;
    if (pthread_create(&t, NULL, every_thread, NULL) == 0)
        pthread_detach(t);
}

int host_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }
