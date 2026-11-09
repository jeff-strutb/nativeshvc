/*
 * host_android.c: the native activity: a full-screen surface for Vulkan, touch,
 * keys and game controllers, AAudio, the app's private folder for saves and the
 * package's assets. Android keeps the activity's thread; the game's main runs
 * on a thread of its own.
 */
#include <stdatomic.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/asset_manager.h>
#include <aaudio/AAudio.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"

int nshvc_game_main(int argc, char **argv);

static struct android_app *app;
static ANativeWindow *volatile window;
static char save_dir[1024];
static volatile int surface_ready;

/* ---- events, from the activity's thread to the system thread ---- */
#define QUEUE 512
static host_event queue[QUEUE];
static unsigned q_head, q_tail;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;

static void qpush(const host_event *e)
{
    pthread_mutex_lock(&q_lock);
    if (((q_head + 1) % QUEUE) != q_tail) {
        queue[q_head] = *e;
        q_head = (q_head + 1) % QUEUE;
    }
    pthread_mutex_unlock(&q_lock);
}

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    int got = 0;
    pthread_mutex_lock(&q_lock);
    if (q_head != q_tail) {
        *ev = queue[q_tail];
        q_tail = (q_tail + 1) % QUEUE;
        got = 1;
    }
    pthread_mutex_unlock(&q_lock);
    if (!got && wait_ms && wait_ms != 0xFFFFFFFFu)
        host_sleep_ms(wait_ms);
    return got;
}

/* ---- controllers ---- */
static struct { int on; unsigned buttons; float x, y, hx, hy; } pad;

static int pad_bit(int key)
{
    switch (key) {
    case AKEYCODE_BUTTON_A: return 0;
    case AKEYCODE_BUTTON_B: return 1;
    case AKEYCODE_BUTTON_X: return 2;
    case AKEYCODE_BUTTON_Y: return 3;
    case AKEYCODE_BUTTON_L1: return 4;
    case AKEYCODE_BUTTON_R1: return 5;
    case AKEYCODE_BUTTON_SELECT: return 6;
    case AKEYCODE_BUTTON_START: return 7;
    case AKEYCODE_BUTTON_THUMBL: return 8;
    case AKEYCODE_BUTTON_THUMBR: return 9;
    case AKEYCODE_BUTTON_L2: return 10;
    case AKEYCODE_BUTTON_R2: return 11;
    case AKEYCODE_DPAD_UP: return 12;
    case AKEYCODE_DPAD_RIGHT: return 13;
    case AKEYCODE_DPAD_DOWN: return 14;
    case AKEYCODE_DPAD_LEFT: return 15;
    }
    return -1;
}

int host_pad_read(int i, host_pad *p)
{
    memset(p, 0, sizeof *p);
    if (i != 0 || !pad.on)
        return 0;
    p->x = pad.x;
    p->y = pad.y;
    p->buttons = pad.buttons;
    if (pad.hy < -0.5f) p->buttons |= 1u << 12;
    if (pad.hx > 0.5f) p->buttons |= 1u << 13;
    if (pad.hy > 0.5f) p->buttons |= 1u << 14;
    if (pad.hx < -0.5f) p->buttons |= 1u << 15;
    p->style = HOST_PAD_GENERIC;
    snprintf(p->name, sizeof p->name, "Controller");
    return 1;
}

static int vk_of(int k)
{
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z)
        return 'A' + (k - AKEYCODE_A);
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9)
        return '0' + (k - AKEYCODE_0);
    switch (k) {
    case AKEYCODE_SPACE: return 0x20;
    case AKEYCODE_ENTER: return 0x0D;
    case AKEYCODE_DEL: return 0x08;
    case AKEYCODE_TAB: return 0x09;
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return 0x10;
    case AKEYCODE_DPAD_LEFT: return 0x25;
    case AKEYCODE_DPAD_UP: return 0x26;
    case AKEYCODE_DPAD_RIGHT: return 0x27;
    case AKEYCODE_DPAD_DOWN: return 0x28;
    }
    return 0;
}

static int32_t on_input(struct android_app *a, AInputEvent *e)
{
    int type = AInputEvent_getType(e), source = AInputEvent_getSource(e);
    (void)a;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int key = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN, bit = pad_bit(key);
        if ((source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK)) && bit >= 0) {
            pad.on = 1;
            if (down)
                pad.buttons |= 1u << bit;
            else
                pad.buttons &= ~(1u << bit);
            return 1;
        }
        {
            host_event h;
            memset(&h, 0, sizeof h);
            h.type = HOST_EV_KEY;
            h.vk = vk_of(key);
            h.down = down;
            if (h.vk) {
                qpush(&h);
                return 1;
            }
        }
        return 0;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        if (source & AINPUT_SOURCE_JOYSTICK) {
            pad.on = 1;
            pad.x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
            pad.y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
            pad.hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            pad.hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            return 1;
        }
        {
            int act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
            int which = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            size_t i, n = AMotionEvent_getPointerCount(e);
            for (i = 0; i < n; i++) {
                host_event h;
                int phase = 1;
                if ((int)i == which) {
                    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN)
                        phase = 0;
                    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP || kind == AMOTION_EVENT_ACTION_CANCEL)
                        phase = 2;
                }
                memset(&h, 0, sizeof h);
                h.type = HOST_EV_TOUCH;
                h.id = AMotionEvent_getPointerId(e, i);
                h.phase = phase;
                h.x = AMotionEvent_getX(e, i);
                h.y = AMotionEvent_getY(e, i);
                qpush(&h);
            }
            return 1;
        }
    }
    return 0;
}

static void focus(int down)
{
    host_event h;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_FOCUS;
    h.down = down;
    qpush(&h);
}

static void on_cmd(struct android_app *a, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        window = a->window;
        surface_ready = 1;
        break;
    case APP_CMD_TERM_WINDOW:
        window = NULL;
        break;
    case APP_CMD_GAINED_FOCUS:
        focus(1);
        break;
    case APP_CMD_LOST_FOCUS:
        focus(0);
        break;
    }
}

/* the game has returned: the process ends once the activity has finished, by
   _exit, because exit's destructors would run under the system's own threads */
static atomic_int game_done;
static int game_code;

static void *game_thread(void *arg)
{
    static char name[] = "game";
    char *argv[] = { name, NULL };
    int code;
    (void)arg;
    code = nshvc_game_main(1, argv);
    fflush(NULL);
    game_code = code;
    atomic_store(&game_done, 1);
    ANativeActivity_finish(app->activity);
    return NULL;
}

void android_main(struct android_app *a)
{
    pthread_t t;
    int started = 0;
    app = a;
    a->onAppCmd = on_cmd;
    a->onInputEvent = on_input;
    for (;;) {
        int events;
        struct android_poll_source *src;
        if (a->destroyRequested && atomic_load(&game_done))
            _exit(game_code);
        while (ALooper_pollOnce(started ? 16 : -1, NULL, &events, (void **)&src) >= 0) {
            if (src)
                src->process(a, src);
            if (a->destroyRequested) {
                host_event h;
                memset(&h, 0, sizeof h);
                h.type = HOST_EV_CLOSE;
                qpush(&h);
            }
            if (surface_ready && !started) {
                started = 1;
                pthread_create(&t, NULL, game_thread, NULL);
            }
        }
    }
}

/* ---- the rest of host.h ---- */
void host_set_app_name(const char *dir, const char *title) { (void)dir; (void)title; }

void host_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    snprintf(save_dir, sizeof save_dir, "%s", app->activity->internalDataPath);
    host_mkdir(save_dir);
}

void host_shutdown(void) {}
const char *host_save_dir(void) { return save_dir; }
const char *host_resource_dir(void) { return ""; }
int host_window_open(int width, int height, const char *title) { (void)width; (void)height; (void)title; return 1; }
void host_window_close(void) {}
int host_window_visible(void) { return window != NULL; }
void *host_window_handle(void) { return window; }

void host_window_pixels(int *w, int *h)
{
    ANativeWindow *n = window;
    *w = n ? ANativeWindow_getWidth(n) : 0;
    *h = n ? ANativeWindow_getHeight(n) : 0;
}

void host_window_points(float *w, float *h)
{
    int a, b;
    host_window_pixels(&a, &b);
    *w = (float)a;
    *h = (float)b;
}

void host_present(const uint32_t *a, int w, int h, float s) { (void)a; (void)w; (void)h; (void)s; }
void host_message_box(const char *text, const char *caption) { __android_log_print(ANDROID_LOG_ERROR, "nativeshvc", "%s: %s", caption ? caption : "", text ? text : ""); }
void host_safe_area(float *l, float *t, float *r, float *b) { *l = *t = *r = *b = 0; }
void host_text_input(int on)
{
    if (on)
        ANativeActivity_showSoftInput(app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    else
        ANativeActivity_hideSoftInput(app->activity, 0);
}
int host_keyboard_attached(void) { return (AConfiguration_getKeyboard(app->config) == ACONFIGURATION_KEYBOARD_QWERTY); }

/* ---- sound ---- */
static AAudioStream *stream;
static host_audio_fn afn;
static void *auser;

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)s;
    (void)user;
    afn((float *)data, frames, auser);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
        return 0;
    afn = fn;
    auser = user;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &stream) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(b);
        return 0;
    }
    AAudioStreamBuilder_delete(b);
    return AAudioStream_requestStart(stream) == AAUDIO_OK;
}

void host_audio_close(void)
{
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream = NULL;
    }
}

void *host_vulkan_loader(void)
{
    void *h = dlopen("libvulkan.so", RTLD_NOW);
    return h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
}

/* ---- the package's assets ---- */
void *host_asset_open(const char *path) { return AAssetManager_open(app->activity->assetManager, path, AASSET_MODE_STREAMING); }
uint32_t host_asset_read(void *a, void *data, uint32_t size) { int n = AAsset_read((AAsset *)a, data, size); return n < 0 ? 0u : (uint32_t)n; }
void host_asset_close(void *a) { AAsset_close((AAsset *)a); }
