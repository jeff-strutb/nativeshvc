/* host_posix.c: time and files for POSIX systems (macOS, Linux, iOS). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int host_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

/* ---- the save folder: the game's own, or the system's (host.h) ---------------------- */
static int skip_carry(const char *name)
{
    size_t n = strlen(name);
    return name[0] == '.' || !strcmp(name, "log.txt") || (n >= 4 && !strcmp(name + n - 4, ".new"));
}

static int copy_file(const char *from, const char *to)
{
    char tmp[1500];
    char buf[8192];
    size_t k;
    FILE *in, *out;
    int ok = 1;
    snprintf(tmp, sizeof tmp, "%s.new", to);
    if (!(in = fopen(from, "rb")))
        return 0;
    if (!(out = fopen(tmp, "wb"))) {
        fclose(in);
        return 0;
    }
    while ((k = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, k, out) != k)
            ok = 0;
    ok = ok && !ferror(in);
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    if (ok)
        ok = rename(tmp, to) == 0;
    if (!ok)
        remove(tmp);
    return ok;
}

int host_save_place(const char *want, const char *old, char *out, size_t n, char *note, size_t note_n)
{
    char probe[1500], mark[1500], from[1500], to[1500];
    FILE *f;
    DIR *d;
    struct dirent *e;
    struct stat st;
    int copied = 0, failed = 0;
    note[0] = 0;
    snprintf(probe, sizeof probe, "%s/.write-test", want);
    snprintf(mark, sizeof mark, "%s/.carried-over", want);
    if (!host_mkdir(want) || !(f = fopen(probe, "wb"))) {
        snprintf(note, note_n, "nativeshvc: cannot write in %s, so saves stay in %s", want, old);
        snprintf(out, n, "%s", old);
        host_mkdir(old);
        return 0;
    }
    fclose(f);
    remove(probe);
    snprintf(out, n, "%s", want);
    if (stat(mark, &st) == 0)
        return 1;
    if ((d = opendir(old))) {
        while ((e = readdir(d))) {
            if (skip_carry(e->d_name))
                continue;
            snprintf(from, sizeof from, "%s/%s", old, e->d_name);
            snprintf(to, sizeof to, "%s/%s", want, e->d_name);
            if (stat(from, &st) != 0 || !S_ISREG(st.st_mode) || stat(to, &st) == 0)
                continue;
            if (copy_file(from, to))
                copied++;
            else
                failed++;
        }
        closedir(d);
    }
    if (!failed && (f = fopen(mark, "wb")))
        fclose(f);
    if (copied || failed)
        snprintf(note, note_n, "nativeshvc: carried %d save file(s) from %s into %s%s", copied, old, want,
                 failed ? " (some could not be copied; tried again next start)" : "");
    return 1;
}

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
