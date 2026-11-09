/* jobs.c: the job pool. The game thread starts groups of jobs and waits for
 * them, working on its own group while it waits. A job writes only its own
 * results and the game reads them after the wait, so timing cannot change game
 * state. With one core there are no workers and jobs run in order on the
 * calling thread. */
#include <string.h>

#include "engine.h"
#include "host.h"

#define MAX_WORKERS 64
#define MAX_GROUPS 64

typedef struct {
    int used;
    uint32_t gen;
    nshvc_job_fn fn;
    void *context;
    uint32_t count, next, done;
} Group;

static Group groups[MAX_GROUPS];
static host_mutex *lock;
static host_cond *work, *finished;
static host_thread *workers[MAX_WORKERS];
static uint32_t nworkers, cores;
static int stopping;

static void run_one(Group *g, uint32_t i)
{
    int role = eng_role;
    nshvc_job_fn fn = g->fn;
    void *context = g->context;
    host_mutex_unlock(lock);
    eng_role = ENG_JOB;
    fn(context, i);
    eng_role = role;
    host_mutex_lock(lock);
    if (++g->done == g->count)
        host_cond_broadcast(finished);
}

static Group *find_work(void)
{
    int i;
    for (i = 0; i < MAX_GROUPS; i++)
        if (groups[i].used && groups[i].next < groups[i].count)
            return &groups[i];
    return NULL;
}

static void worker(void *arg)
{
    (void)arg;
    eng_role = ENG_JOB;
    host_mutex_lock(lock);
    for (;;) {
        Group *g = find_work();
        if (g) {
            run_one(g, g->next++);
        } else if (stopping) {
            break;
        } else {
            host_cond_wait(work, lock, HOST_WAIT_FOREVER);
        }
    }
    host_mutex_unlock(lock);
}

void eng_jobs_start(void)
{
    uint32_t i;
    cores = host_cores();
    if (cores < 1)
        cores = 1;
    nworkers = cores - 1 > MAX_WORKERS ? MAX_WORKERS : cores - 1;
    lock = host_mutex_new();
    work = host_cond_new();
    finished = host_cond_new();
    stopping = 0;
    for (i = 0; i < nworkers; i++)
        workers[i] = host_thread_start(worker, NULL, "nshvc job");
}

void eng_jobs_stop(void)
{
    uint32_t i;
    host_mutex_lock(lock);
    stopping = 1;
    host_cond_broadcast(work);
    host_mutex_unlock(lock);
    for (i = 0; i < nworkers; i++)
        host_thread_join(workers[i]);
    nworkers = 0;
}

uint32_t nshvc_cores(void) { return cores; }

static void run_inline(nshvc_job_fn fn, void *context, uint32_t count)
{
    int role = eng_role;
    uint32_t i;
    eng_role = ENG_JOB;
    for (i = 0; i < count; i++)
        fn(context, i);
    eng_role = role;
}

nshvc_job_group nshvc_jobs_begin(nshvc_job_fn fn, void *context, uint32_t count)
{
    static const char call[] = "nshvc_jobs_begin";
    nshvc_job_group none = { 0 };
    int i;
    if (!eng_on(call, ENG_GAME))
        return none;
    if (!fn) {
        eng_report(call, "no job function");
        return none;
    }
    if (!count)
        return none;
    if (!nworkers) {
        run_inline(fn, context, count);
        return none;
    }
    host_mutex_lock(lock);
    for (i = 0; i < MAX_GROUPS; i++)
        if (!groups[i].used)
            break;
    if (i == MAX_GROUPS) {
        /* every slot busy: the work is still done, here and now */
        host_mutex_unlock(lock);
        run_inline(fn, context, count);
        return none;
    }
    {
        Group *g = &groups[i];
        nshvc_job_group r;
        g->used = 1;
        g->gen = (g->gen + 1) & 0xFFFFFFu;
        g->fn = fn;
        g->context = context;
        g->count = count;
        g->next = g->done = 0;
        r.id = (g->gen << 8) | (uint32_t)(i + 1);
        host_cond_broadcast(work);
        host_mutex_unlock(lock);
        return r;
    }
}

void nshvc_jobs_wait(nshvc_job_group group)
{
    static const char call[] = "nshvc_jobs_wait";
    uint32_t slot = group.id & 0xFFu;
    Group *g;
    if (!eng_on(call, ENG_GAME) || !group.id)
        return;
    if (slot < 1 || slot > MAX_GROUPS) {
        eng_report(call, "bad group %u", (unsigned)group.id);
        return;
    }
    g = &groups[slot - 1];
    host_mutex_lock(lock);
    if (!g->used || g->gen != group.id >> 8) {
        host_mutex_unlock(lock);
        eng_report(call, "group %u already waited for", (unsigned)group.id);
        return;
    }
    while (g->next < g->count)
        run_one(g, g->next++);
    while (g->done < g->count)
        host_cond_wait(finished, lock, HOST_WAIT_FOREVER);
    g->used = 0;
    host_mutex_unlock(lock);
}

void nshvc_jobs(nshvc_job_fn fn, void *context, uint32_t count)
{
    if (!eng_on("nshvc_jobs", ENG_GAME))
        return;
    nshvc_jobs_wait(nshvc_jobs_begin(fn, context, count));
}
