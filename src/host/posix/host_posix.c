/* host_posix.c: time and files for POSIX systems (macOS, Linux, iOS). */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* ---- threads ---- */
struct host_thread {
    pthread_t t;
    void (*fn)(void *);
    void *arg;
    char name[16];
};
struct host_mutex { pthread_mutex_t m; };
struct host_cond { pthread_cond_t c; };

static void die(const char *what)
{
    fprintf(stderr, "nativeshvc: cannot make a %s\n", what);
    exit(70);
}

uint32_t host_cores(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1u : (uint32_t)n;
}

static void *thread_main(void *arg)
{
    host_thread *t = (host_thread *)arg;
#ifdef __APPLE__
    pthread_setname_np(t->name);
#else
    pthread_setname_np(pthread_self(), t->name);
#endif
    t->fn(t->arg);
    return NULL;
}

host_thread *host_thread_start(void (*fn)(void *), void *arg, const char *name)
{
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    if (!t)
        die("thread");
    t->fn = fn;
    t->arg = arg;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "nshvc");
    if (pthread_create(&t->t, NULL, thread_main, t) != 0)
        die("thread");
    return t;
}

void host_thread_join(host_thread *t)
{
    pthread_join(t->t, NULL);
    free(t);
}

host_mutex *host_mutex_new(void)
{
    host_mutex *m = (host_mutex *)malloc(sizeof *m);
    if (!m || pthread_mutex_init(&m->m, NULL) != 0)
        die("mutex");
    return m;
}

void host_mutex_lock(host_mutex *m) { pthread_mutex_lock(&m->m); }
void host_mutex_unlock(host_mutex *m) { pthread_mutex_unlock(&m->m); }

host_cond *host_cond_new(void)
{
    host_cond *c = (host_cond *)malloc(sizeof *c);
    if (!c)
        die("condition");
#ifdef __APPLE__
    if (pthread_cond_init(&c->c, NULL) != 0)
        die("condition");
#else
    {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        if (pthread_cond_init(&c->c, &a) != 0)
            die("condition");
        pthread_condattr_destroy(&a);
    }
#endif
    return c;
}

void host_cond_broadcast(host_cond *c) { pthread_cond_broadcast(&c->c); }

void host_cond_wait(host_cond *c, host_mutex *m, uint64_t timeout_ns)
{
    struct timespec ts;
    if (timeout_ns == HOST_WAIT_FOREVER) {
        pthread_cond_wait(&c->c, &m->m);
        return;
    }
#ifdef __APPLE__
    ts.tv_sec = (time_t)(timeout_ns / 1000000000u);
    ts.tv_nsec = (long)(timeout_ns % 1000000000u);
    pthread_cond_timedwait_relative_np(&c->c, &m->m, &ts);
#else
    {
        uint64_t at = host_ticks_ns() + timeout_ns;
        ts.tv_sec = (time_t)(at / 1000000000u);
        ts.tv_nsec = (long)(at % 1000000000u);
        pthread_cond_timedwait(&c->c, &m->m, &ts);
    }
#endif
}
