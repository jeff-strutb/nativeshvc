/* loop.c: the engine's loop and its threads.
 *
 * The system thread (the process's main) owns the window and its events. The
 * game thread calls the game: update at its fixed rate in real time, as many
 * as are due, and draw once for each picture the render thread asks for. The
 * render thread never holds up the game: when it has not asked for a picture,
 * the game thread waits only for the next update's time.
 *
 * Game state depends on the update count and the input alone. A machine too
 * slow for the update rate catches up on at most MAX_BACKLOG_NS of real time;
 * older time is dropped, so it runs slower instead of falling further behind.
 *
 * Headless, real time is not used at all: one update, then one draw, each
 * picture finished before the next update.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "inspect.h"
#include "picture.h"
#include "rr.h"

#define MAX_BACKLOG_NS 250000000ull
#define NS_PER_S 1000000000ull

static nshvc_game game;
static int headless;
static uint64_t max_updates = UINT64_MAX;
static const char *sound_path, *input_path, *inspect_path, *shots_dir;

/* the game thread's own */
static uint64_t updates;
static uint64_t started_ns;
static int paused;
static uint64_t paused_at, paused_total;

/* the threads' handshake, all under hs_lock */
static host_mutex *hs_lock;
static host_cond *game_wake, *render_wake;
static int hs_want, hs_ready, hs_quit;
static int hs_busy;          /* the render thread is drawing a picture */
static int hs_window;        /* the window went, came or changed since the render thread last looked */
static uint64_t hs_updates;  /* the game's updates so far */
static rr_frame *hs_packet;

static atomic_int quit_asked;

static void ask_quit(void)
{
    atomic_store(&quit_asked, 1);
    host_mutex_lock(hs_lock);
    hs_quit = 1;
    host_cond_broadcast(game_wake);
    host_cond_broadcast(render_wake);
    host_mutex_unlock(hs_lock);
}

/* ---- calls for the game ---- */

uint64_t nshvc_updates(void) { return eng_on("nshvc_updates", ENG_GAME) ? updates : 0; }

double nshvc_seconds(void)
{
    if (!eng_on("nshvc_seconds", ENG_GAME))
        return 0;
    return (double)(host_ticks_ns() - started_ns) / (double)NS_PER_S;
}

void nshvc_quit(void)
{
    if (eng_on("nshvc_quit", ENG_GAME))
        atomic_store(&quit_asked, 1);
}

void nshvc_pause(int on)
{
    if (!eng_on("nshvc_pause", ENG_GAME))
        return;
    on = on != 0;
    if (on == paused)
        return;
    if (on)
        paused_at = host_ticks_ns();
    else
        paused_total += host_ticks_ns() - paused_at;
    paused = on;
}

nshvc_display nshvc_display_info(void)
{
    nshvc_display d;
    float l = 0, t = 0, r = 0, b = 0, pw = 0, ph = 0;
    memset(&d, 0, sizeof d);
    if (!eng_on("nshvc_display_info", ENG_GAME))
        return d;
    host_window_sizes(&d.width, &d.height, &pw, &ph);   /* from the same resize */
    host_safe_area(&l, &t, &r, &b);
    if (pw > 0 && ph > 0) {
        d.safe_left = l * (float)d.width / pw;
        d.safe_right = r * (float)d.width / pw;
        d.safe_top = t * (float)d.height / ph;
        d.safe_bottom = b * (float)d.height / ph;
    }
    d.keyboard = host_keyboard_attached() != 0;
    return d;
}

void nshvc_text_input(int on)
{
    if (eng_on("nshvc_text_input", ENG_GAME))
        host_text_input(on);
}

/* ---- the render thread ---- */

/* the picture drawn and shown; with no display to pace it, a real-time run
   takes one a millisecond */
static double render_ms;
static uint64_t shots;

static void write_shot(void)
{
    static uint32_t *px;
    static size_t cap;
    char path[1200];
    FILE *f;
    int w = 0, h = 0, x, y;
    if (!rr_read(NULL, &w, &h) || !w || !h)
        return;
    if ((size_t)w * h > cap) {
        free(px);
        cap = (size_t)w * h;
        if (!(px = (uint32_t *)malloc(cap * 4)))
            exit(70);
    }
    rr_read(px, &w, &h);
    snprintf(path, sizeof path, "%s/%06llu.ppm", shots_dir, (unsigned long long)shots++);
    if (!(f = fopen(path, "wb"))) {
        fprintf(stderr, "nativeshvc: cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * w + x];
            putc((int)(c >> 16 & 255), f);
            putc((int)(c >> 8 & 255), f);
            putc((int)(c & 255), f);
        }
    fclose(f);
}

/* the picture drawn and shown: 1 if a display paced it */
static int show(rr_frame *p)
{
    uint64_t t = host_ticks_ns();
    int paced = rr_draw(p);
    host_mutex_lock(hs_lock);
    render_ms = (double)(host_ticks_ns() - t) / 1e6;
    host_mutex_unlock(hs_lock);
    if (shots_dir)
        write_shot();
    return paced;
}

/* the host's word that the window went, came or changed (any thread) */
static void window_changed(void)
{
    host_mutex_lock(hs_lock);
    hs_window = 1;
    host_cond_broadcast(render_wake);
    host_mutex_unlock(hs_lock);
}

static void render_thread(void *arg)
{
    (void)arg;
    eng_role = ENG_RENDER;
    host_thread_interactive();
    for (;;) {
        rr_frame *p;
        uint64_t drawn;
        int paced;
        host_mutex_lock(hs_lock);
        hs_want = 1;
        host_cond_broadcast(game_wake);
        while (!hs_ready && !hs_quit && !hs_window)
            host_cond_wait(render_wake, hs_lock, HOST_WAIT_FOREVER);
        if (!hs_ready && !hs_quit) {
            /* the window went or came between pictures (during the game's
               init, say): anything kept on one that went let go now */
            hs_window = 0;
            host_mutex_unlock(hs_lock);
            rr_window();
            continue;
        }
        if (!hs_ready) {
            host_mutex_unlock(hs_lock);
            return;
        }
        hs_ready = 0;
        hs_busy = 1;
        p = hs_packet;
        drawn = p->update;
        host_mutex_unlock(hs_lock);
        paced = show(p);
        host_mutex_lock(hs_lock);
        hs_busy = 0;
        host_cond_broadcast(game_wake);
        /* nothing shown it and nothing paced it (no window, or one minimised):
           the next picture once the game has moved on or the window changes,
           not as fast as pictures can be drawn */
        if (!paced && !headless)
            while (hs_updates == drawn && !hs_window && !hs_quit)
                host_cond_wait(render_wake, hs_lock, HOST_WAIT_FOREVER);
        host_mutex_unlock(hs_lock);
    }
}

/* ---- the game thread ---- */

static double last_update_ms, last_draw_ms, last_engine_ms;

/* the game's own zones, a frame's worth */
static struct { const char *name; uint64_t ns; } zones[NSHVC_PROFILE_ZONES];
static uint32_t nzones;
static struct { uint32_t zone; uint64_t at; } zone_stack[16];
static uint32_t zone_depth;

static void zone_clear(void)
{
    nzones = 0;
    zone_depth = 0;
}

void nshvc_profile_begin(const char *zone)
{
    uint32_t i;
    if (!eng_on("nshvc_profile_begin", ENG_GAME) || !zone)
        return;
    for (i = 0; i < nzones; i++)
        if (zones[i].name == zone)
            break;
    if (i == nzones) {
        if (nzones == NSHVC_PROFILE_ZONES)
            return;
        zones[i].name = zone;
        zones[i].ns = 0;
        nzones++;
    }
    if (zone_depth < 16) {
        zone_stack[zone_depth].zone = i;
        zone_stack[zone_depth].at = host_ticks_ns();
    }
    zone_depth++;
}

void nshvc_profile_end(void)
{
    if (!eng_on("nshvc_profile_end", ENG_GAME) || !zone_depth)
        return;
    zone_depth--;
    if (zone_depth < 16)
        zones[zone_stack[zone_depth].zone].ns += host_ticks_ns() - zone_stack[zone_depth].at;
}

nshvc_profile nshvc_profile_frame(void)
{
    nshvc_profile r;
    uint32_t i;
    memset(&r, 0, sizeof r);
    if (!eng_on("nshvc_profile_frame", ENG_GAME))
        return r;
    r.update_ms = last_update_ms;
    r.draw_ms = last_draw_ms;
    r.engine_ms = last_engine_ms;
    host_mutex_lock(hs_lock);
    r.render_ms = render_ms;
    host_mutex_unlock(hs_lock);
    r.audio_ms = eng_sound_cost_ms();
    r.zone_count = nzones;
    for (i = 0; i < nzones; i++) {
        r.zones[i].name = zones[i].name;
        r.zones[i].ms = (double)zones[i].ns / 1e6;
    }
    return r;
}

const uint32_t *nshvc_picture_read(int *width, int *height)
{
    static uint32_t *px;
    static size_t cap;
    int w = 0, h = 0;
    if (!eng_on("nshvc_picture_read", ENG_GAME))
        return NULL;
    /* the last finished picture: once a picture being drawn is done. The
       render thread then waits for the game's next one, which this thread
       sends, so it stays idle while the picture is read (from update, draw
       or shutdown alike) */
    host_mutex_lock(hs_lock);
    while (hs_busy && !hs_quit)
        host_cond_wait(game_wake, hs_lock, HOST_WAIT_FOREVER);
    host_mutex_unlock(hs_lock);
    if (!rr_read(NULL, &w, &h) || !w || !h)
        return NULL;
    if ((size_t)w * h > cap) {
        free(px);
        cap = (size_t)w * h;
        if (!(px = (uint32_t *)malloc(cap * 4)))
            exit(70);
    }
    if (!rr_read(px, &w, &h))
        return NULL;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return px;
}


static void one_update(void)
{
    nshvc_input in;
    uint64_t t;
    eng_input_sample(&in, updates, headless);
    t = host_ticks_ns();
    game.update(&in);
    last_update_ms = (double)(host_ticks_ns() - t) / 1e6;
    updates++;
}

static void one_draw(float between)
{
    rr_frame *p;
    uint64_t t0, t1;
    eng_picture_begin();
    zone_clear();
    t0 = host_ticks_ns();
    game.draw(between);
    t1 = host_ticks_ns();
    p = eng_picture_end();
    last_draw_ms = (double)(t1 - t0) / 1e6;
    last_engine_ms = (double)(host_ticks_ns() - t1) / 1e6;
    p->update = updates;
    p->between = between;
    host_mutex_lock(hs_lock);
    hs_packet = p;
    hs_ready = 1;
    host_cond_broadcast(render_wake);
    host_mutex_unlock(hs_lock);
}

static int ended(void) { return atomic_load(&quit_asked) || updates >= max_updates; }

/* until the render thread has finished the last picture; 0 if the run ended */
static int wait_for_render(void)
{
    int ok;
    host_mutex_lock(hs_lock);
    while (!hs_want && !hs_quit)
        host_cond_wait(game_wake, hs_lock, HOST_WAIT_FOREVER);
    ok = hs_want;
    hs_want = 0;
    host_mutex_unlock(hs_lock);
    return ok;
}

static void run_headless(void)
{
    while (!ended() && wait_for_render()) {
        one_update();
        eng_sound_advance(updates, game.update_hz);
        one_draw(0);
    }
    /* the last picture finished; the render thread's request stands, for a
       read of it at shutdown */
    host_mutex_lock(hs_lock);
    while (!hs_want && !hs_quit)
        host_cond_wait(game_wake, hs_lock, HOST_WAIT_FOREVER);
    host_mutex_unlock(hs_lock);
}

/* the game's clock: real time, stopped while paused */
static uint64_t game_ns(void)
{
    return (paused ? paused_at : host_ticks_ns()) - paused_total;
}

static void run_live(void)
{
    uint64_t base_ns = game_ns(), base_n = 0;
    const uint64_t hz = game.update_hz;
#define DUE(n) (base_ns + ((n) - base_n) * NS_PER_S / hz)
    while (!ended()) {
        uint64_t now = game_ns();
        int take = 0;
        if (!paused) {
            if (now > DUE(updates) + MAX_BACKLOG_NS) {
                base_n = updates;
                base_ns = now - MAX_BACKLOG_NS;
            }
            while (DUE(updates) <= now && !ended())
                one_update();
        }
        host_mutex_lock(hs_lock);
        if (hs_updates != updates) {
            hs_updates = updates;
            host_cond_broadcast(render_wake);
        }
        if (!hs_want && !hs_quit && !atomic_load(&quit_asked)) {
            if (paused) {
                host_cond_wait(game_wake, hs_lock, HOST_WAIT_FOREVER);
            } else {
                uint64_t t = game_ns(), due = DUE(updates);
                if (due > t)
                    host_cond_wait(game_wake, hs_lock, due - t);
            }
        }
        /* once the run has ended the render thread's request stands, for a read
           of the last picture at shutdown */
        if (hs_want && !ended()) {
            hs_want = 0;
            take = 1;
        }
        host_mutex_unlock(hs_lock);
        if (take) {
            float between = 0;
            if (updates) {
                uint64_t from = DUE(updates - 1), to = DUE(updates), t = game_ns();
                /* (updates - 1 can be before base_n just after dropped time;
                   unsigned arithmetic would wrap, so such a picture is at 0) */
                if (updates > base_n && t > from && to > from) {
                    double b = (double)(t - from) / (double)(to - from);
                    between = b >= 1 ? 1.0f : (float)b;
                }
            }
            one_draw(between);
        }
    }
#undef DUE
}

static void game_thread(void *arg)
{
    (void)arg;
    eng_role = ENG_GAME;
    host_thread_interactive();
    started_ns = host_ticks_ns();
    if (game.init)
        game.init();
    eng_sound_start(&game.music, headless, sound_path);
    if (headless)
        run_headless();
    else
        run_live();
    eng_sound_stop();
    if (game.shutdown)
        game.shutdown();
    ask_quit();
}

/* ---- the system thread ---- */

static int id_ok(const char *id)
{
    if (!id || !*id)
        return 0;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') || *id == '-'))
            return 0;
    return 1;
}

int nshvc_run(const nshvc_game *g, int argc, char **argv)
{
    static const char call[] = "nshvc_run";
    host_thread *game_t, *render_t;
    int i;

    eng_role = ENG_SYSTEM;
    if (!g || !g->update || !g->draw || !g->update_hz || !id_ok(g->id)) {
        fprintf(stderr, "nativeshvc: %s: a game needs update, draw, update_hz and an id of letters, digits and dashes\n",
                call);
        return 2;
    }
    game = *g;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--updates") && i + 1 < argc)
            max_updates = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--sound") && i + 1 < argc)
            sound_path = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)
            input_path = argv[++i];
        else if (!strcmp(argv[i], "--inspect") && i + 1 < argc)
            inspect_path = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc)
            shots_dir = argv[++i];
    }

    host_set_app_name(game.id, game.title ? game.title : game.id);
    host_init(argc, argv);
    hs_lock = host_mutex_new();
    game_wake = host_cond_new();
    render_wake = host_cond_new();
    host_window_watch(window_changed);
    eng_jobs_start();
    eng_inspect_start(inspect_path);
    eng_input_start(input_path);
    eng_files_start();
    if (!headless && !host_window_open(game.window_aspect > 0.0f ? (int)(222.0f * game.window_aspect + 0.5f) : 296, 222, game.title ? game.title : game.id)) {
        host_message_box("The window could not be opened.", game.title ? game.title : game.id);
        return 1;
    }

    if (!rr_open()) {
        host_message_box("The renderer could not start on this computer.", game.title ? game.title : game.id);
        return 1;
    }
    eng_picture_init();
    render_t = host_thread_start(render_thread, NULL, "nshvc render");
    game_t = host_thread_start(game_thread, NULL, "nshvc game");

    if (!headless) {
        for (;;) {
            host_event e;
            int closed = 0;
            host_mutex_lock(hs_lock);
            closed = hs_quit;
            host_mutex_unlock(hs_lock);
            if (closed)
                break;
            if (host_poll_event(&e, 4)) {
                if (e.type == HOST_EV_CLOSE)
                    ask_quit();
                else {
                    if (e.type == HOST_EV_KEY && e.down && e.vk == 0x7B)
                        eng_inspect_ask();
                    eng_input_event(&e);
                }
            }
        }
    }

    host_thread_join(game_t);
    host_thread_join(render_t);
    eng_jobs_stop();
    eng_files_stop();
    eng_inspect_stop();
    rr_close();
    if (!headless)
        host_window_close();
    host_shutdown();
    return 0;
}
