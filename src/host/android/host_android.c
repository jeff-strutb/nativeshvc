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
#include <time.h>
#include <unistd.h>

#include "host.h"

int nshvc_game_main(int argc, char **argv);

static struct android_app *app;
/* (6.9) DisplayMetrics.density, which is the configuration's density in dots per inch over 160 (kept by the
   activity's thread: at the start and when the configuration changes) */
static _Atomic float s_scale = 1;
static void scale_read(void)
{
    int32_t d = AConfiguration_getDensity(app->config);
    atomic_store(&s_scale, d > 0 && d < ACONFIGURATION_DENSITY_ANY ? (float)d / 160.0f : 1.0f);
}
float host_display_scale(void) { return atomic_load(&s_scale); }
/* the window, set and taken under w_lock: w_taken while the renderer draws a
   frame with it, w_kept the window the renderer keeps a surface on; w_gone a
   window that went while the renderer still kept a surface on it (it did not
   let go in time), given to it as no window until it lets go, so a window
   made again at the same address is never taken for the one that went */
static ANativeWindow *window, *w_kept, *w_gone;
static int w_taken;
/* told when the window comes, goes or changes size */
static void (*_Atomic w_watch)(void);
static void watch_tell(void)
{
    void (*f)(void) = atomic_load(&w_watch);
    if (f)
        f();
}
static pthread_mutex_t w_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_cond = PTHREAD_COND_INITIALIZER;
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
/* one slot per input device (its Android device id); a slot keeps its index
   while the device is connected and is freed when Android no longer lists it */
#define PADS 8
static struct { int on, dev; unsigned buttons; float x, y, rx, ry, lt, rt, hx, hy; char name[64]; long long checked; } pads[PADS];
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* whether InputDevice.getDevice(dev) still finds the device, and its name;
   1 (kept) when the question cannot be asked */
static int device_present(int dev, char *name, size_t n)
{
    JavaVM *vm = app->activity->vm;
    JNIEnv *env;
    jclass cls;
    jmethodID get;
    jobject d;
    int present = 1;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK &&
        (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
        return 1;
    cls = (*env)->FindClass(env, "android/view/InputDevice");
    get = cls ? (*env)->GetStaticMethodID(env, cls, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    if (!get) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    d = (*env)->CallStaticObjectMethod(env, cls, get, (jint)dev);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    if (!d)
        present = 0;
    else {
        if (name) {
            jmethodID gn = (*env)->GetMethodID(env, cls, "getName", "()Ljava/lang/String;");
            jstring js = gn ? (jstring)(*env)->CallObjectMethod(env, d, gn) : NULL;
            if (js) {
                const char *c = (*env)->GetStringUTFChars(env, js, NULL);
                if (c) {
                    snprintf(name, n, "%s", c);
                    (*env)->ReleaseStringUTFChars(env, js, c);
                }
                (*env)->DeleteLocalRef(env, js);
            }
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, d);
    }
    return present;
}

/* the slot of device dev, taking the first free one for a new device; -1 when all are in use. pad_lock held */
static int pad_slot(int dev)
{
    int k, free_k = -1;
    for (k = 0; k < PADS; k++) {
        if (pads[k].on && pads[k].dev == dev)
            return k;
        if (!pads[k].on && free_k < 0)
            free_k = k;
    }
    if (free_k >= 0) {
        memset(&pads[free_k], 0, sizeof pads[free_k]);
        pads[free_k].on = 1;
        pads[free_k].dev = dev;
        pads[free_k].checked = now_ms();
        snprintf(pads[free_k].name, sizeof pads[free_k].name, "Controller");
        device_present(dev, pads[free_k].name, sizeof pads[free_k].name);
    }
    return free_k;
}

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
    if (i < 0 || i >= PADS)
        return 0;
    pthread_mutex_lock(&pad_lock);
    if (pads[i].on && now_ms() - pads[i].checked >= 1000) {
        pads[i].checked = now_ms();
        if (!device_present(pads[i].dev, NULL, 0))
            memset(&pads[i], 0, sizeof pads[i]);
    }
    if (!pads[i].on) {
        pthread_mutex_unlock(&pad_lock);
        return 0;
    }
    p->x = pads[i].x;
    p->y = pads[i].y;
    p->rx = pads[i].rx;
    p->ry = pads[i].ry;
    p->lt = pads[i].lt;
    p->rt = pads[i].rt;
    p->buttons = pads[i].buttons;
    if (pads[i].hy < -0.5f) p->buttons |= 1u << 12;
    if (pads[i].hx > 0.5f) p->buttons |= 1u << 13;
    if (pads[i].hy > 0.5f) p->buttons |= 1u << 14;
    if (pads[i].hx < -0.5f) p->buttons |= 1u << 15;
    snprintf(p->name, sizeof p->name, "%s", pads[i].name);
    pthread_mutex_unlock(&pad_lock);
    p->style = HOST_PAD_GENERIC;
    return 1;
}

static int vk_of(int k)
{
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z)
        return 'A' + (k - AKEYCODE_A);
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9)
        return '0' + (k - AKEYCODE_0);
    if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12)
        return 0x70 + (k - AKEYCODE_F1);
    if (k >= AKEYCODE_NUMPAD_0 && k <= AKEYCODE_NUMPAD_9)
        return 0x60 + (k - AKEYCODE_NUMPAD_0);
    switch (k) {
    case AKEYCODE_ESCAPE: return 0x1B;
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return 0x11;
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return 0x12;
    case AKEYCODE_META_LEFT: return 0x5B;
    case AKEYCODE_META_RIGHT: return 0x5C;
    case AKEYCODE_CAPS_LOCK: return 0x14;
    case AKEYCODE_NUM_LOCK: return 0x90;
    case AKEYCODE_SCROLL_LOCK: return 0x91;
    case AKEYCODE_BREAK: return 0x13;
    case AKEYCODE_SYSRQ: return 0x2C;
    case AKEYCODE_MOVE_HOME: return 0x24;
    case AKEYCODE_MOVE_END: return 0x23;
    case AKEYCODE_PAGE_UP: return 0x21;
    case AKEYCODE_PAGE_DOWN: return 0x22;
    case AKEYCODE_INSERT: return 0x2D;
    case AKEYCODE_FORWARD_DEL: return 0x2E;
    case AKEYCODE_GRAVE: return 0xC0;
    case AKEYCODE_MINUS: return 0xBD;
    case AKEYCODE_EQUALS: return 0xBB;
    case AKEYCODE_LEFT_BRACKET: return 0xDB;
    case AKEYCODE_RIGHT_BRACKET: return 0xDD;
    case AKEYCODE_BACKSLASH: return 0xDC;
    case AKEYCODE_SEMICOLON: return 0xBA;
    case AKEYCODE_APOSTROPHE: return 0xDE;
    case AKEYCODE_COMMA: return 0xBC;
    case AKEYCODE_PERIOD: return 0xBE;
    case AKEYCODE_SLASH: return 0xBF;
    case AKEYCODE_NUMPAD_DIVIDE: return 0x6F;
    case AKEYCODE_NUMPAD_MULTIPLY: return 0x6A;
    case AKEYCODE_NUMPAD_SUBTRACT: return 0x6D;
    case AKEYCODE_NUMPAD_ADD: return 0x6B;
    case AKEYCODE_NUMPAD_DOT: return 0x6E;
    case AKEYCODE_NUMPAD_ENTER: return 0x0D;
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
            int k;
            pthread_mutex_lock(&pad_lock);
            k = pad_slot(AInputEvent_getDeviceId(e));
            if (k >= 0) {
                if (down)
                    pads[k].buttons |= 1u << bit;
                else
                    pads[k].buttons &= ~(1u << bit);
            }
            pthread_mutex_unlock(&pad_lock);
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
            int k;
            pthread_mutex_lock(&pad_lock);
            k = pad_slot(AInputEvent_getDeviceId(e));
            if (k >= 0) {
                pads[k].x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
                pads[k].y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
                pads[k].hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
                pads[k].hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
                pads[k].rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
                pads[k].ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
                /* a controller reports its triggers on one pair of axes or the other */
                {
                    float l = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
                    float r = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                    float b = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
                    float g = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
                    pads[k].lt = l > b ? l : b;
                    pads[k].rt = r > g ? r : g;
                }
            }
            pthread_mutex_unlock(&pad_lock);
            return 1;
        }
        if (source & AINPUT_SOURCE_MOUSE & ~AINPUT_SOURCE_CLASS_POINTER) {
            /* a mouse: the pointer, its buttons and its wheel, never a touch */
            static float lx, ly;
            static int have;
            host_event h;
            int kind = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
            int32_t st = AMotionEvent_getButtonState(e);
            memset(&h, 0, sizeof h);
            if (kind == AMOTION_EVENT_ACTION_SCROLL) {
                h.type = HOST_EV_WHEEL;
                h.dx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0);
                h.dy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0);
                qpush(&h);
                return 1;
            }
            h.type = HOST_EV_MOUSE;
            h.x = AMotionEvent_getX(e, 0);
            h.y = AMotionEvent_getY(e, 0);
            if (have) {
                h.dx = h.x - lx;
                h.dy = h.y - ly;
            }
            have = 1;
            lx = h.x;
            ly = h.y;
            h.buttons = ((st & AMOTION_EVENT_BUTTON_PRIMARY) ? 1 : 0) | ((st & AMOTION_EVENT_BUTTON_SECONDARY) ? 2 : 0)
                        | ((st & AMOTION_EVENT_BUTTON_TERTIARY) ? 4 : 0);
            qpush(&h);
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
    case APP_CMD_CONFIG_CHANGED:
        scale_read();
        break;
    case APP_CMD_INIT_WINDOW:
        pthread_mutex_lock(&w_lock);
        window = a->window;
        pthread_mutex_unlock(&w_lock);
        surface_ready = 1;
        watch_tell();
        break;
    case APP_CMD_WINDOW_RESIZED:
        watch_tell();
        break;
    case APP_CMD_TERM_WINDOW: {
        /* the window goes once this returns: first the renderer finishes the
           frame it is drawing with it and lets go of its surface; one between
           frames is woken to let go at once. A renderer stuck elsewhere is
           waited for 2 seconds, within the time Android allows before it
           calls the app hung. */
        ANativeWindow *old;
        struct timespec until;
        pthread_mutex_lock(&w_lock);
        old = window;
        window = NULL;
        pthread_mutex_unlock(&w_lock);
        watch_tell();
        pthread_mutex_lock(&w_lock);
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 2;
        while (w_taken || (old && w_kept == old))
            if (pthread_cond_timedwait(&w_cond, &w_lock, &until)) {
                __android_log_print(ANDROID_LOG_WARN, "nativeshvc", "the renderer did not let go of the window in time");
                if (old && w_kept == old)
                    w_gone = old;
                break;
            }
        pthread_mutex_unlock(&w_lock);
        break;
    }
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
    scale_read();
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
int host_window_visible(void) { return host_window_handle() != NULL; }
void *host_window_handle(void)
{
    ANativeWindow *n;
    pthread_mutex_lock(&w_lock);
    n = window;
    pthread_mutex_unlock(&w_lock);
    return n;
}

/* the size of window n, asked of it in two calls (under w_lock, so it cannot
   go meanwhile): asked again until a width and height are read together
   with nothing changed between */
static void size_of(ANativeWindow *n, int *w, int *h)
{
    int a = 0, b = 0;
    if (n) {
        a = ANativeWindow_getWidth(n);
        for (;;) {
            b = ANativeWindow_getHeight(n);
            if (ANativeWindow_getWidth(n) == a)
                break;
            a = ANativeWindow_getWidth(n);
        }
    }
    *w = a;
    *h = b;
}

int host_windowed(void) { return 1; }

void *host_window_take(int *w, int *h)
{
    ANativeWindow *n;
    int a, b;
    pthread_mutex_lock(&w_lock);
    if (w_gone && w_kept != w_gone)
        w_gone = NULL;
    n = w_gone ? NULL : window;
    w_taken = 1;
    size_of(n, &a, &b);
    pthread_mutex_unlock(&w_lock);
    if (w)
        *w = a;
    if (h)
        *h = b;
    return n;
}

void host_window_watch(void (*changed)(void)) { atomic_store(&w_watch, changed); }

void host_window_give(void *kept)
{
    pthread_mutex_lock(&w_lock);
    w_taken = 0;
    w_kept = (ANativeWindow *)kept;
    pthread_cond_broadcast(&w_cond);
    pthread_mutex_unlock(&w_lock);
}

/* the window's size, read with the window under the lock */
void host_window_pixels(int *w, int *h)
{
    pthread_mutex_lock(&w_lock);
    size_of(window, w, h);
    pthread_mutex_unlock(&w_lock);
}

/* points are pixels here: both from one reading */
void host_window_sizes(int *pw, int *ph, float *tw, float *th)
{
    host_window_pixels(pw, ph);
    *tw = (float)*pw;
    *th = (float)*ph;
}

void host_window_points(float *w, float *h)
{
    int a, b;
    host_window_pixels(&a, &b);
    *w = (float)a;
    *h = (float)b;
}

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
/* the app's window is the system's to size: full screen, not switched */
int host_full_screen_switch(void) { return 0; }
int host_window_full_screen(void) { return 1; }
void host_window_set_full_screen(int on) { (void)on; }
void host_window_show(void) {}

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
