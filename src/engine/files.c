/* files.c: saves and game data. Saves are the game's own files in the system's
 * save folder, written off the game's thread; a read waits for earlier writes
 * of the same run. Game data is read from the game's package. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"

typedef struct Job {
    struct Job *next;
    char name[128];
    uint32_t size;
    int del;
    uint8_t data[];
} Job;

static host_thread *writer;
static host_mutex *lock;
static host_cond *wake, *idle;
static Job *first, *last;
static int stopping, busy;

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", host_save_dir(), name); }

static void write_job(const Job *j)
{
    char path[1400], tmp[1500];
    FILE *f;
    path_of(path, sizeof path, j->name);
    if (j->del) {
        remove(path);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "wb"))) {
        fprintf(stderr, "nativeshvc: cannot write %s\n", tmp);
        return;
    }
    fwrite(j->data, 1, j->size, f);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static void writer_thread(void *arg)
{
    (void)arg;
    host_mutex_lock(lock);
    for (;;) {
        Job *j = first;
        if (!j) {
            if (stopping)
                break;
            host_cond_wait(wake, lock, HOST_WAIT_FOREVER);
            continue;
        }
        first = j->next;
        if (!first)
            last = NULL;
        busy = 1;
        host_mutex_unlock(lock);
        write_job(j);
        free(j);
        host_mutex_lock(lock);
        busy = 0;
        host_cond_broadcast(idle);
    }
    host_mutex_unlock(lock);
}

void eng_files_start(void)
{
    lock = host_mutex_new();
    wake = host_cond_new();
    idle = host_cond_new();
    writer = host_thread_start(writer_thread, NULL, "nshvc saves");
}

void eng_files_stop(void)
{
    host_mutex_lock(lock);
    stopping = 1;
    host_cond_broadcast(wake);
    host_mutex_unlock(lock);
    host_thread_join(writer);
}

static int name_ok(const char *call, const char *name)
{
    const char *c;
    if (!name || !*name || strlen(name) >= 128) {
        eng_report(call, "a bad name");
        return 0;
    }
    for (c = name; *c; c++)
        if (*c == '/' || *c == '\\' || (*c == '.' && c[1] == '.')) {
            eng_report(call, "name %s", name);
            return 0;
        }
    return 1;
}

static int enqueue(const char *name, const void *data, uint32_t size, int del)
{
    Job *j = (Job *)malloc(sizeof *j + size);
    if (!j)
        return -1;
    j->next = NULL;
    snprintf(j->name, sizeof j->name, "%s", name);
    j->size = size;
    j->del = del;
    if (size)
        memcpy(j->data, data, size);
    host_mutex_lock(lock);
    if (last)
        last->next = j;
    else
        first = j;
    last = j;
    host_cond_broadcast(wake);
    host_mutex_unlock(lock);
    return 0;
}

int nshvc_save_write(const char *name, const void *data, uint32_t size)
{
    static const char call[] = "nshvc_save_write";
    if (!eng_on(call, ENG_GAME) || !name_ok(call, name) || (size && !data))
        return -1;
    return enqueue(name, data, size, 0);
}

void nshvc_save_delete(const char *name)
{
    static const char call[] = "nshvc_save_delete";
    if (eng_on(call, ENG_GAME) && name_ok(call, name))
        enqueue(name, NULL, 0, 1);
}

int nshvc_save_read(const char *name, void *data, uint32_t size)
{
    static const char call[] = "nshvc_save_read";
    char path[1400];
    FILE *f;
    size_t n;
    if (!eng_on(call, ENG_GAME) || !name_ok(call, name) || (size && !data))
        return -1;
    /* the file as this run last wrote it */
    host_mutex_lock(lock);
    while (first || busy)
        host_cond_wait(idle, lock, HOST_WAIT_FOREVER);
    host_mutex_unlock(lock);
    path_of(path, sizeof path, name);
    if (!(f = fopen(path, "rb")))
        return -1;
    n = fread(data, 1, size, f);
    fclose(f);
    return (int)n;
}

/* ---- game data ---- */
#define MAX_ASSETS 64
#ifdef __ANDROID__
/* inside the package: the host's (host_android.c) */
void *host_asset_open(const char *path);
uint32_t host_asset_read(void *a, void *data, uint32_t size);
void host_asset_close(void *a);
typedef void AssetFile;
#define ASSET_OPEN(full, path) host_asset_open(path)
#define ASSET_READ(a, d, n) host_asset_read(a, d, n)
#define ASSET_CLOSE(a) host_asset_close(a)
#else
typedef FILE AssetFile;
#define ASSET_OPEN(full, path) fopen(full, "rb")
#define ASSET_READ(a, d, n) ((uint32_t)fread(d, 1, n, a))
#define ASSET_CLOSE(a) fclose(a)
#endif
static AssetFile *assets[MAX_ASSETS + 1];

nshvc_asset nshvc_asset_open(const char *path)
{
    static const char call[] = "nshvc_asset_open";
    nshvc_asset r = { 0 };
    char full[1400];
    int i;
    if (!eng_on(call, ENG_GAME | ENG_JOB))
        return r;
    if (!path || strstr(path, "..")) {
        eng_report(call, "path %s", path ? path : "(none)");
        return r;
    }
    for (i = 1; i <= MAX_ASSETS && assets[i]; i++)
        ;
    if (i > MAX_ASSETS) {
        eng_report(call, "more than %d open", MAX_ASSETS);
        return r;
    }
    snprintf(full, sizeof full, "%s/%s", host_resource_dir(), path);
    if (!(assets[i] = ASSET_OPEN(full, path)))
        return r;
    r.id = (uint32_t)i;
    return r;
}

uint32_t nshvc_asset_read(nshvc_asset a, void *data, uint32_t size)
{
    if (!eng_on("nshvc_asset_read", ENG_GAME | ENG_JOB) || !a.id || a.id > MAX_ASSETS || !assets[a.id])
        return 0;
    return ASSET_READ(assets[a.id], data, size);
}

void nshvc_asset_close(nshvc_asset a)
{
    if (eng_on("nshvc_asset_close", ENG_GAME | ENG_JOB) && a.id && a.id <= MAX_ASSETS && assets[a.id]) {
        ASSET_CLOSE(assets[a.id]);
        assets[a.id] = NULL;
    }
}
