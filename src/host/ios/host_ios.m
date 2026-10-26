/* host_ios.m: the iOS host -- the app, a full-screen view that shows the
 * frames, touch controls, the keyboard and game controllers, sound out.
 * The counterpart of host/macos/host_macos.m; host/posix under it supplies
 * time and files.
 *
 * UIKit keeps the main thread, so the game's main (nshvc_game_main: the
 * game writes NSHVC_MAIN) runs its own loop on a thread of its own, started
 * when the scene first becomes active. What UIKit reports reaches that
 * thread through a locked event queue that host_poll_event drains. When the
 * app leaves the foreground, the game thread is held inside
 * host_poll_event until it comes back: iOS allows no GPU work in the
 * background.
 *
 * Frames are drawn by the renderer (render/metal) into the view's
 * CAMetalLayer (host_metal_layer), one drawable pixel per screen pixel, or
 * arrive as ARGB pixels through host_present (the software renderer).
 *
 *   touch     every touch, as it lands, moves and lifts (HOST_EV_TOUCH):
 *             what it means is the game's; the view's safe area
 *             (host_safe_area)
 *   keys      a hardware keyboard's HID usage, as a Win32 virtual key
 *   sound     a RemoteIO output unit pulling the game's mixer
 *   pads      GameController.framework, as on macOS
 *
 * PORT_SAVEDIR names the save folder (default the app's Application
 * Support/<app>); PORT_INPUTLOG=1 reports each key event on stderr (input
 * checks).
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <GameController/GameController.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "host.h"

/* the game's main (nshvc.h NSHVC_MAIN: on iOS the host keeps main and runs
   the game's on a thread of its own) */
int nshvc_game_main(int argc, char **argv);
static void host_audio_resume(void);

static char s_save[1024];
static const char *s_app_dir = "Game", *s_app_title = "Game";

void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        s_app_dir = dir;
    if (title)
        s_app_title = title;
}

/* ---- process -------------------------------------------------------------------- */
void host_init(int argc, char **argv)
{
    const char *e;
    (void)argc;
    (void)argv;
    @autoreleasepool {
        e = getenv("PORT_SAVEDIR");
        if (e) {
            snprintf(s_save, sizeof s_save, "%s", e);
        } else {
            NSString *base = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                                 NSUserDomainMask, YES) firstObject];
            snprintf(s_save, sizeof s_save, "%s/%s", base ? [base fileSystemRepresentation] : ".", s_app_dir);
        }
        /* (a new app's container has no Application Support yet) */
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:s_save]
                                  withIntermediateDirectories:YES attributes:nil error:NULL];
    }
}

const char *host_resource_dir(void)
{
    static char res[1024];
    const char *e = getenv("PORT_RESDIR");
    if (e)
        return e;
    if (!res[0]) {
        @autoreleasepool {
            NSString *r = [[NSBundle mainBundle] resourcePath];
            snprintf(res, sizeof res, "%s", r ? [r fileSystemRepresentation] : ".");
        }
    }
    return res;
}

void host_shutdown(void)
{
    host_audio_close();
    host_window_close();
}

const char *host_save_dir(void) { return s_save; }

/* ---- the event queue UIKit fills and host_poll_event drains ----------------------- */
#define QMAX 256
static host_event s_q[QMAX];
static int s_qh, s_qt;
static os_unfair_lock s_qlock = OS_UNFAIR_LOCK_INIT;
/* by virtual key: how many sources hold it, so one letting go of a key
   another holds sends nothing */
static uint8_t s_held[256];

static void qpush(const host_event *e)
{
    int n;
    os_unfair_lock_lock(&s_qlock);
    n = (s_qt + 1) % QMAX;
    if (n != s_qh) {
        s_q[s_qt] = *e;
        s_qt = n;
    }
    os_unfair_lock_unlock(&s_qlock);
}

/* (main thread) */
static void key_event(int vk, int down)
{
    host_event e;
    if (vk <= 0 || vk >= 256)
        return;
    if (down ? s_held[vk]++ != 0 : s_held[vk] == 0 || --s_held[vk] != 0)
        return;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_KEY;
    e.vk = vk;
    e.down = down;
    qpush(&e);
    if (getenv("PORT_INPUTLOG"))
        fprintf(stderr, "input: key %02X %s\n", vk, down ? "down" : "up");
}

/* ---- keys: a hardware keyboard's HID usage -> Win32 virtual key ------------------- */
static int hid_vk(long u)
{
    static const struct { uint8_t hid, vk; } k[] = {
        { 0x28, 0x0D }, { 0x58, 0x0D }, { 0x29, 0x1B }, { 0x2A, 0x08 }, { 0x2B, 0x09 }, { 0x2C, 0x20 },
        { 0x4F, 0x27 }, { 0x50, 0x25 }, { 0x51, 0x28 }, { 0x52, 0x26 },
        { 0xE1, 0x10 }, { 0xE5, 0x10 }, { 0xE0, 0x11 }, { 0xE4, 0x11 }, { 0xE2, 0x12 }, { 0xE6, 0x12 },
        { 0x35, 0xC0 }, { 0x2D, 0xBD }, { 0x2E, 0xBB }, { 0x2F, 0xDB }, { 0x30, 0xDD }, { 0x31, 0xDC },
        { 0x33, 0xBA }, { 0x34, 0xDE }, { 0x36, 0xBC }, { 0x37, 0xBE }, { 0x38, 0xBF },
        { 0x4A, 0x24 }, { 0x4B, 0x21 }, { 0x4D, 0x23 }, { 0x4E, 0x22 }, { 0x49, 0x2D }, { 0x4C, 0x2E },
    };
    size_t i;
    if (u >= 0x04 && u <= 0x1D)
        return 'A' + (int)(u - 0x04);
    if (u >= 0x1E && u <= 0x26)
        return '1' + (int)(u - 0x1E);
    if (u == 0x27)
        return '0';
    if (u >= 0x3A && u <= 0x45)
        return 0x70 + (int)(u - 0x3A);
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].hid == u)
            return k[i].vk;
    return 0;
}

static uint8_t s_kb[256];            /* by HID usage: held */

static void kb_event(long u, int down)
{
    if (u < 0 || u >= 256 || s_kb[u] == down)
        return;
    s_kb[u] = (uint8_t)down;
    key_event(hid_vk(u), down);
}

/* ---- touch: every touch to the game (HOST_EV_TOUCH), in points ------------------- */
/* What a touch means is the game's (nshvc_hooks.touch): the host only says
   where each finger is, as it lands, moves and lifts. A touch's id is
   small and stays its own while it lasts. */
static _Atomic float s_safe[4];             /* the view's safe area: left, top, right, bottom */

void host_safe_area(float *l, float *t, float *r, float *b)
{
    *l = atomic_load(&s_safe[0]);
    *t = atomic_load(&s_safe[1]);
    *r = atomic_load(&s_safe[2]);
    *b = atomic_load(&s_safe[3]);
}

@interface PortTouch : UIView
- (void)reset;
@end

@implementation PortTouch {
    NSMutableDictionary<NSValue *, NSNumber *> *_ids;   /* touch -> its id */
}

- (instancetype)initWithFrame:(CGRect)f
{
    if (!(self = [super initWithFrame:f]))
        return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    _ids = [NSMutableDictionary new];
    return self;
}

- (void)layoutSubviews
{
    UIEdgeInsets s = self.safeAreaInsets;
    [super layoutSubviews];
    atomic_store(&s_safe[0], (float)s.left);
    atomic_store(&s_safe[1], (float)s.top);
    atomic_store(&s_safe[2], (float)s.right);
    atomic_store(&s_safe[3], (float)s.bottom);
}

- (int)idFor:(UITouch *)t
{
    NSValue *k = [NSValue valueWithNonretainedObject:t];
    NSNumber *n = _ids[k];
    int i = 0;
    if (n)
        return n.intValue;
    for (;;) {
        BOOL used = NO;
        for (NSNumber *v in _ids.allValues)
            if (v.intValue == i)
                used = YES;
        if (!used)
            break;
        i++;
    }
    _ids[k] = @(i);
    return i;
}

- (void)send:(NSSet<UITouch *> *)touches phase:(int)phase
{
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        host_event e;
        memset(&e, 0, sizeof e);
        e.type = HOST_EV_TOUCH;
        e.id = [self idFor:t];
        e.phase = phase;
        e.x = (float)p.x;
        e.y = (float)p.y;
        qpush(&e);
        if (phase == 2)
            [_ids removeObjectForKey:[NSValue valueWithNonretainedObject:t]];
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)ev { (void)ev; [self send:t phase:0]; }
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)ev { (void)ev; [self send:t phase:1]; }
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)ev { (void)ev; [self send:t phase:2]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)ev { (void)ev; [self send:t phase:2]; }

/* the app leaves the foreground: every touch lifted */
- (void)reset
{
    for (NSValue *k in _ids.allKeys) {
        host_event e;
        memset(&e, 0, sizeof e);
        e.type = HOST_EV_TOUCH;
        e.id = _ids[k].intValue;
        e.phase = 2;
        qpush(&e);
    }
    [_ids removeAllObjects];
}
@end

/* ---- the view ------------------------------------------------------------------------ */
@interface PortView : UIView
@end

static PortView *s_view;
static PortTouch *s_controls;
static CAMetalLayer *s_layer;
static CALayer *s_pic;
static _Atomic int s_pw, s_ph;        /* the drawable's size in pixels */
static _Atomic float s_ptw, s_pth;    /* the view's in points */

@implementation PortView
+ (Class)layerClass { return [CAMetalLayer class]; }

- (void)layoutSubviews
{
    CGFloat sc = self.window.windowScene.screen.nativeScale ?: 1;
    CGSize b = self.bounds.size, want = CGSizeMake(floor(b.width * sc + 0.5), floor(b.height * sc + 0.5));
    [super layoutSubviews];
    self.contentScaleFactor = sc;
    if (want.width >= 1 && want.height >= 1 && !CGSizeEqualToSize(s_layer.drawableSize, want))
        s_layer.drawableSize = want;
    atomic_store(&s_pw, (int)want.width);
    atomic_store(&s_ph, (int)want.height);
    atomic_store(&s_ptw, (float)b.width);
    atomic_store(&s_pth, (float)b.height);
}
@end

@interface PortViewController : UIViewController
@end

@implementation PortViewController
- (void)loadView
{
    s_view = [[PortView alloc] initWithFrame:CGRectZero];
    s_view.backgroundColor = [UIColor blackColor];
    s_layer = (CAMetalLayer *)s_view.layer;
    s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = NO;     /* the renderer copies its frame in */
    s_layer.magnificationFilter = kCAFilterNearest;
    s_layer.opaque = YES;
    s_controls = [[PortTouch alloc] initWithFrame:CGRectZero];
    s_controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [s_view addSubview:s_controls];
    self.view = s_view;
}
- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    s_controls.frame = s_view.bounds;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)ev
{
    int any = 0;
    for (UIPress *p in presses)
        if (p.key) {
            kb_event((long)p.key.keyCode, 1);
            any = 1;
        }
    if (!any)
        [super pressesBegan:presses withEvent:ev];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)ev
{
    int any = 0;
    for (UIPress *p in presses)
        if (p.key) {
            kb_event((long)p.key.keyCode, 0);
            any = 1;
        }
    if (!any)
        [super pressesEnded:presses withEvent:ev];
}
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)ev
{
    [self pressesEnded:presses withEvent:ev];
}
@end

/* ---- the foreground: the game thread held while the app is away ------------------- */
static _Atomic int s_active;
static pthread_mutex_t s_pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_pc = PTHREAD_COND_INITIALIZER;
static int s_parked, s_started;

static void focus_event(int down)
{
    host_event e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_FOCUS;
    e.down = down;
    qpush(&e);
}

static void *game_thread(void *p)
{
    static char name[] = "game";
    char *argv[] = { name, NULL };
    (void)p;
    exit(nshvc_game_main(1, argv));
}

static void app_active(void)
{
    pthread_mutex_lock(&s_pm);
    atomic_store(&s_active, 1);
    pthread_cond_broadcast(&s_pc);
    pthread_mutex_unlock(&s_pm);
    focus_event(1);
    if (!s_started) {
        pthread_attr_t a;
        pthread_t t;
        s_started = 1;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 16u << 20);   /* the main thread's room, and more */
        pthread_create(&t, &a, game_thread, NULL);
        pthread_attr_destroy(&a);
    }
}

static void app_inactive(void)
{
    struct timespec until;
    int i;
    /* keys held go up, or they stay down for good */
    [s_controls reset];
    for (i = 0; i < 256; i++)
        kb_event(i, 0);
    focus_event(0);
    pthread_mutex_lock(&s_pm);
    atomic_store(&s_active, 0);
    /* the game thread parks at its next poll, within a frame; a stuck
       one is not waited for long */
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 500000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    while (s_started && !s_parked)
        if (pthread_cond_timedwait(&s_pc, &s_pm, &until))
            break;
    pthread_mutex_unlock(&s_pm);
}

/* ---- the app and its scene ------------------------------------------------------------ */
@interface PortSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation PortSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opt
{
    (void)session;
    (void)opt;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [PortViewController new];
    self.window.backgroundColor = [UIColor blackColor];
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    (void)scene;
    app_active();
    host_audio_resume();
}
- (void)sceneWillResignActive:(UIScene *)scene
{
    (void)scene;
    app_inactive();
}
@end

@interface PortAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation PortAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)opt
{
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    (void)app;
    (void)opt;
    c.delegateClass = [PortSceneDelegate class];
    return c;
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([PortAppDelegate class]));
    }
}

/* ---- the window, from the game thread ------------------------------------------------ */
/* the scene made the view before the game thread started */
int host_window_open(int width, int height, const char *title)
{
    (void)width;
    (void)height;
    (void)title;
    return s_layer != nil;
}

void host_window_close(void) {}

int host_window_visible(void) { return s_layer && atomic_load(&s_active); }

/* the view's CAMetalLayer, for render/metal; its drawable is kept at the
   view's size in pixels on the main thread (PortView's layoutSubviews) */
CAMetalLayer *host_metal_layer(void) { return s_layer; }

void host_metal_layer_fit(int *w, int *h)
{
    *w = atomic_load(&s_pw);
    *h = atomic_load(&s_ph);
}

void *host_window_handle(void) { return (__bridge void *)s_layer; }
void host_window_pixels(int *w, int *h) { host_metal_layer_fit(w, h); }
void host_window_points(float *w, float *h)
{
    *w = atomic_load(&s_ptw);
    *h = atomic_load(&s_pth);
}

/* a picture of ARGB pixels (0xAARRGGBB, top row first) onto the view, at
   the shape `aspect` (width over height), centred in the largest
   rectangle of that shape the view holds */
void host_present(const uint32_t *argb, int w, int h, float aspect)
{
    NSData *d;
    if (!s_view || !argb)
        return;
    if (aspect <= 0)
        aspect = (float)w / (float)h;
    d = [NSData dataWithBytes:argb length:(NSUInteger)w * (NSUInteger)h * 4];
    dispatch_async(dispatch_get_main_queue(), ^{
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)d);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, rgb,
                                       kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                                       kCGRenderingIntentDefault);
        CGRect b = s_view.bounds;
        CGFloat fw = b.size.width, fh = b.size.height;
        if (fh > 0 && fw / fh > aspect)
            fw = floor(fh * aspect + 0.5);
        else
            fh = floor(fw / aspect + 0.5);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (!s_pic) {
            s_pic = [CALayer layer];
            s_pic.magnificationFilter = kCAFilterNearest;
            s_pic.contentsGravity = kCAGravityResize;
            [s_view.layer insertSublayer:s_pic atIndex:0];
        }
        s_pic.frame = CGRectMake(floor((b.size.width - fw) / 2), floor((b.size.height - fh) / 2), fw, fh);
        s_pic.contents = (__bridge id)img;
        [CATransaction commit];
        CGImageRelease(img);
        CGDataProviderRelease(dp);
        CGColorSpaceRelease(rgb);
    });
}

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    uint64_t until = host_ticks_ns() + (uint64_t)wait_ms * 1000000ull;
    int got;
    /* away from the foreground: held here until the app is back */
    if (!atomic_load(&s_active)) {
        pthread_mutex_lock(&s_pm);
        s_parked = 1;
        pthread_cond_broadcast(&s_pc);
        while (!atomic_load(&s_active))
            pthread_cond_wait(&s_pc, &s_pm);
        s_parked = 0;
        pthread_mutex_unlock(&s_pm);
    }
    for (;;) {
        os_unfair_lock_lock(&s_qlock);
        got = s_qh != s_qt;
        if (got) {
            *ev = s_q[s_qh];
            s_qh = (s_qh + 1) % QMAX;
        }
        os_unfair_lock_unlock(&s_qlock);
        if (got || wait_ms == 0 || (wait_ms != 0xFFFFFFFFu && host_ticks_ns() >= until))
            return got;
        host_sleep_ms(1);
    }
}

void host_message_box(const char *text, const char *caption)
{
    NSString *t = [NSString stringWithUTF8String:text ? text : ""];
    NSString *c = [NSString stringWithUTF8String:caption ? caption : s_app_title];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    fprintf(stderr, "%s: %s\n", [c UTF8String], [t UTF8String]);
    if ([NSThread isMainThread] || !s_view)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *a = [UIAlertController alertControllerWithTitle:c message:t
                                                            preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *x) { (void)x; dispatch_semaphore_signal(done); }]];
        [s_view.window.rootViewController presentViewController:a animated:YES completion:nil];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

/* ---- audio out: a RemoteIO unit pulling the game's mixer --------------------------- */
static AudioComponentInstance s_unit;
static host_audio_fn s_afn;
static void *s_auser;

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    (void)ref; (void)flags; (void)ts; (void)bus;
    if (s_afn)
        s_afn((float *)io->mBuffers[0].mData, (int)frames, s_auser);
    else
        memset(io->mBuffers[0].mData, 0, io->mBuffers[0].mDataByteSize);
    return noErr;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_RemoteIO,
                                       kAudioUnitManufacturer_Apple, 0, 0 };
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb = { render, NULL };
    AudioComponent comp;
    if (s_unit)
        return 1;
    @autoreleasepool {
        AVAudioSession *as = [AVAudioSession sharedInstance];
        /* the game's sound mixes with what else plays, and the silent
           switch silences it */
        [as setCategory:AVAudioSessionCategoryAmbient error:NULL];
        [as setPreferredSampleRate:rate error:NULL];
        [as setActive:YES error:NULL];
    }
    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &s_unit) != noErr)
        return 0;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 8;
    fmt.mBytesPerPacket = 8;
    s_afn = fn;
    s_auser = user;
    if (AudioUnitSetProperty(s_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &fmt, sizeof fmt) != noErr ||
        AudioUnitSetProperty(s_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(s_unit) != noErr || AudioOutputUnitStart(s_unit) != noErr) {
        AudioComponentInstanceDispose(s_unit);
        s_unit = NULL;
        return 0;
    }
    return 1;
}

/* (main thread) back in the foreground: an interruption (a call, Siri)
   may have stopped the unit */
static void host_audio_resume(void)
{
    if (s_unit) {
        [[AVAudioSession sharedInstance] setActive:YES error:NULL];
        AudioOutputUnitStart(s_unit);
    }
}

void host_audio_close(void)
{
    if (s_unit) {
        AudioOutputUnitStop(s_unit);
        AudioUnitUninitialize(s_unit);
        AudioComponentInstanceDispose(s_unit);
        s_unit = NULL;
    }
}

/* ---- game controllers ------------------------------------------------------------- */
/* any controller iOS knows (Xbox, PlayStation, MFi, Switch Pro), in the
 * order GameController lists those with a full gamepad profile */
int host_pad_read(int index, host_pad *o)
{
    @autoreleasepool {
        GCController *c = nil;
        GCExtendedGamepad *g;
        unsigned b = 0;
        int n = 0;
        memset(o, 0, sizeof *o);
        for (GCController *k in GCController.controllers)
            if (k.extendedGamepad && n++ == index) {
                c = k;
                break;
            }
        if (!c || !(g = c.extendedGamepad))
            return 0;
        o->x = g.leftThumbstick.xAxis.value;
        o->y = -g.leftThumbstick.yAxis.value;
        if (g.buttonA.pressed) b |= 1u << 0;
        if (g.buttonB.pressed) b |= 1u << 1;
        if (g.buttonX.pressed) b |= 1u << 2;
        if (g.buttonY.pressed) b |= 1u << 3;
        if (g.leftShoulder.pressed) b |= 1u << 4;
        if (g.rightShoulder.pressed) b |= 1u << 5;
        if (g.buttonOptions.pressed) b |= 1u << 6;
        if (g.buttonMenu.pressed) b |= 1u << 7;
        if (g.leftThumbstickButton.pressed) b |= 1u << 8;
        if (g.rightThumbstickButton.pressed) b |= 1u << 9;
        if (g.leftTrigger.value > 0.5f) b |= 1u << 10;
        if (g.rightTrigger.value > 0.5f) b |= 1u << 11;
        if (g.dpad.up.pressed) b |= 1u << 12;
        if (g.dpad.right.pressed) b |= 1u << 13;
        if (g.dpad.down.pressed) b |= 1u << 14;
        if (g.dpad.left.pressed) b |= 1u << 15;
        o->buttons = b;
        return 1;
    }
}
