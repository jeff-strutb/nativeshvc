/* host_windows.c: the Windows host -- a window, the keyboard and game
 * controllers, sound out. The counterpart of host/macos/host_macos.m;
 * host/win32 under it supplies time and files.
 *
 * The game keeps its own loop on the main thread, so this host never runs a
 * message loop of its own: host_poll_event drains the real queue. Frames
 * are drawn on the window by the renderer (render/vulkan or render/d3d12,
 * host_window_handle).
 *
 *   keys      a key message's virtual key; Alt+Enter and F11 switch the
 *             window between full screen and a window (the game never
 *             sees them)
 *   sound     WASAPI, shared mode, 32-bit float, on its own thread
 *   pads      XInput's controllers
 *
 * PORT_SAVEDIR names the save folder (default %APPDATA%\<app>). A GUI exe
 * has no console: what the game reports goes to log.txt in the save folder
 * (PORT_LOGFILE names another file; 0 keeps stderr).
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shlobj.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static char s_save[2 * MAX_PATH];
static const char *s_app_dir = "Game", *s_app_title = "Game";

void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        s_app_dir = dir;
    if (title)
        s_app_title = title;
}

/* ---- process -------------------------------------------------------------------- */
static void exe_dir(char *out, size_t n)
{
    char *s;
    GetModuleFileNameA(NULL, out, (DWORD)n);
    s = strrchr(out, '\\');
    if (s)
        *s = 0;
}

static void log_open(void)
{
    char path[2 * MAX_PATH + 16];
    const char *e = getenv("PORT_LOGFILE");
    if (e && e[0] == '0')
        return;
    if (e)
        snprintf(path, sizeof path, "%s", e);
    else
        snprintf(path, sizeof path, "%s\\log.txt", s_save);
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

void host_init(int argc, char **argv)
{
    const char *e;
    char here[MAX_PATH];
    (void)argc;
    (void)argv;
    exe_dir(here, sizeof here);
    e = getenv("PORT_SAVEDIR");
    if (e) {
        snprintf(s_save, sizeof s_save, "%s", e);
    } else {
        char app[MAX_PATH];
        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, app) == S_OK)
            snprintf(s_save, sizeof s_save, "%s\\%s", app, s_app_dir);
        else
            snprintf(s_save, sizeof s_save, "%s\\save", here);
    }
    host_mkdir(s_save);
    log_open();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
}

void host_shutdown(void)
{
    host_audio_close();
    host_window_close();
}

const char *host_save_dir(void) { return s_save; }

const char *host_resource_dir(void)
{
    static char here[MAX_PATH];
    const char *e = getenv("PORT_RESDIR");
    if (e)
        return e;
    if (!here[0])
        exe_dir(here, sizeof here);
    return here;
}

/* ---- the event queue the window fills and host_poll_event drains ------------------- */
#define QMAX 256
static host_event s_q[QMAX];
static int s_qh, s_qt;
static uint8_t s_held[256];          /* by virtual key: held */

static void qpush(const host_event *e)
{
    int n = (s_qt + 1) % QMAX;
    if (n != s_qh) {
        s_q[s_qt] = *e;
        s_qt = n;
    }
}

static void key_event(WPARAM vk, int down)
{
    host_event e;
    if (vk >= 256 || s_held[vk] == down)
        return;                       /* auto-repeat */
    s_held[vk] = (uint8_t)down;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_KEY;
    e.vk = (int)vk;
    e.down = down;
    qpush(&e);
}

/* ---- the window ----------------------------------------------------------------- */
static HWND s_win;
static void (*s_watch)(void);  /* told when the window changes size, minimised or back (the window's thread) */

/* Each monitor's own pixels: a process aware only of the system's DPI is drawn at
   the primary monitor's scale and stretched by Windows on the others. The calls
   looked up: Windows 10 1703's and 1607's, the system's scale before them. */
static void dpi_aware(void)
{
    typedef BOOL (WINAPI *SetContext)(HANDLE);
    HMODULE u = GetModuleHandleA("user32.dll");
    SetContext set = u ? (SetContext)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!set || !set((HANDLE)(INT_PTR)-4))    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        SetProcessDPIAware();
}

/* the dots per inch of the monitor window h is on */
static int window_dpi(HWND h)
{
    typedef UINT (WINAPI *GetDpi)(HWND);
    static GetDpi get;
    static int looked;
    int dpi = 0;
    HDC dc;
    if (!looked) {
        HMODULE u = GetModuleHandleA("user32.dll");
        get = u ? (GetDpi)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (get)
        dpi = (int)get(h);
    if (dpi <= 0 && (dc = GetDC(h)) != NULL) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(h, dc);
    }
    return dpi > 0 ? dpi : 96;
}

/* full screen: the window, borderless, covering its monitor; the window's
   place before it, to go back to */
static WINDOWPLACEMENT s_place;

static void toggle_full_screen(void)
{
    DWORD style = (DWORD)GetWindowLongA(s_win, GWL_STYLE);
    MONITORINFO mi;
    if (style & WS_OVERLAPPEDWINDOW) {
        s_place.length = sizeof s_place;
        mi.cbSize = sizeof mi;
        if (!GetWindowPlacement(s_win, &s_place) ||
            !GetMonitorInfoA(MonitorFromWindow(s_win, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongA(s_win, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(s_win, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(s_win, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(s_win, &s_place);
        SetWindowPos(s_win, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* the mouse: where the pointer is (points, from the client area's top
   left), how far it moved since the last, the buttons held */
static void pointer_event(HWND h, WPARAM wp, LPARAM lp)
{
    static int have;
    static float lx, ly;
    host_event e;
    float s = 96.0f / (float)window_dpi(h);
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_MOUSE;
    e.x = (float)(short)LOWORD(lp) * s;
    e.y = (float)(short)HIWORD(lp) * s;
    if (have) {
        e.dx = e.x - lx;
        e.dy = e.y - ly;
    }
    have = 1;
    lx = e.x;
    ly = e.y;
    e.buttons = ((wp & MK_LBUTTON) ? 1 : 0) | ((wp & MK_RBUTTON) ? 2 : 0) | ((wp & MK_MBUTTON) ? 4 : 0);
    qpush(&e);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    host_event e;
    memset(&e, 0, sizeof e);
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* Alt+Enter (bit 29: Alt held) and F11; bit 30: a repeat */
        if (wp == VK_F11 || (wp == VK_RETURN && m == WM_SYSKEYDOWN && (lp & (1 << 29)))) {
            if (!(lp & (1 << 30)))
                toggle_full_screen();
            return 0;
        }
        key_event(wp, 1);
        if (m == WM_SYSKEYDOWN && wp != VK_F4)
            return 0;                 /* Alt alone must not open the window menu */
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key_event(wp, 0);
        return 0;
    case WM_SYSCHAR:
        return 0;                     /* Alt+key: no beep for a missing menu */
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        int i;
        if (m == WM_KILLFOCUS)        /* keys held when focus leaves go up */
            for (i = 0; i < 256; i++)
                if (s_held[i])
                    key_event((WPARAM)i, 0);
        e.type = HOST_EV_FOCUS;
        e.down = m == WM_SETFOCUS;
        qpush(&e);
        return 0;
    }
    case WM_SIZE:                     /* a new size, minimised or back: the renderer told */
        if (s_watch)
            s_watch();
        break;
    case WM_CLOSE:
        e.type = HOST_EV_CLOSE;
        qpush(&e);
        return 0;                     /* the game decides */
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {             /* on a monitor of another scale: a window at the size Windows suggests, full screen its monitor */
        const RECT *to = (const RECT *)lp;
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!((DWORD)GetWindowLongA(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW) &&
            GetMonitorInfoA(MonitorFromRect(to, MONITOR_DEFAULTTONEAREST), &mi))
            to = &mi.rcMonitor;
        SetWindowPos(h, NULL, to->left, to->top, to->right - to->left, to->bottom - to->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        pointer_event(h, wp, lp);
        return 0;
    case WM_MOUSEWHEEL:               /* in lines (WHEEL_DELTA a notch), positive up and right */
    case 0x020E:                      /* WM_MOUSEHWHEEL */
        e.type = HOST_EV_WHEEL;
        if (m == WM_MOUSEWHEEL)
            e.dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        else
            e.dx = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        qpush(&e);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int host_window_open(int width, int height, const char *title)
{
    WNDCLASSA wc;
    RECT r;
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (s_win)
        return 1;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;              /* (an OpenGL context keeps its DC) */
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "PortGameWindow";
    RegisterClassA(&wc);
    dpi_aware();
    r.left = r.top = 0;
    r.right = width * 3;
    r.bottom = height * 3;
    AdjustWindowRect(&r, style, FALSE);
    s_win = CreateWindowA("PortGameWindow", title ? title : s_app_title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!s_win)
        return 0;
    ShowWindow(s_win, SW_SHOW);
    SetForegroundWindow(s_win);
    return 1;
}

void host_window_close(void)
{
    if (s_win) {
        DestroyWindow(s_win);
        s_win = NULL;
    }
}

/* vkGetInstanceProcAddr, the Vulkan library being loaded here (render/vulkan): NULL
   on a computer without it */
void *host_vulkan_loader(void)
{
    HMODULE v = LoadLibraryA("vulkan-1.dll");
    return v ? (void *)GetProcAddress(v, "vkGetInstanceProcAddr") : NULL;
}

int host_close_pending(void);

typedef struct {
    void (*fn)(void *);
    void *arg;
} PumpedRun;

static DWORD WINAPI pumped_thread(LPVOID p)
{
    PumpedRun *r = (PumpedRun *)p;
    r->fn(r->arg);
    return 0;
}

/* fn(arg) on a thread with as much stack as the main one could want (a driver's
   compiler may go deep), the window serviced until it returns; on this thread if
   none can be had. A close asked for meanwhile ends the program: nothing has begun. */
void host_run_pumped(void (*fn)(void *), void *arg)
{
    PumpedRun r;
    HANDLE t;
    r.fn = fn;
    r.arg = arg;
    t = s_win ? CreateThread(NULL, 16u << 20, pumped_thread, &r, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (!t) {
        fn(arg);
        return;
    }
    for (;;) {
        MSG m;
        if (MsgWaitForMultipleObjects(1, &t, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0)
            break;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (host_close_pending())
            TerminateProcess(GetCurrentProcess(), 0);
    }
    CloseHandle(t);
}

int host_window_visible(void)
{
    return s_win && IsWindowVisible(s_win) && !IsIconic(s_win);
}

/* the window for a renderer that draws on it (render/gl, render/vulkan):
   its HWND, its client area in pixels, the module */
void *host_window_handle(void) { return (void *)s_win; }
void host_window_pixels(int *w, int *h);
int host_windowed(void) { return s_win != NULL; }
/* the window lasts until it is closed, after the renderer: nothing to wait for */
void *host_window_take(int *w, int *h)
{
    int a, b;
    host_window_pixels(&a, &b);
    if (w)
        *w = a;
    if (h)
        *h = b;
    return (void *)s_win;
}
void host_window_give(void *kept) { (void)kept; }
void host_window_watch(void (*changed)(void)) { s_watch = changed; }
void *host_win32_instance(void) { return (void *)GetModuleHandleA(NULL); }

/* both from one reading of the window: points are pixels at its dots per inch over 96 */
void host_window_sizes(int *pw, int *ph, float *tw, float *th)
{
    int dpi = s_win ? window_dpi(s_win) : 96;
    host_window_pixels(pw, ph);
    *tw = (float)*pw * 96.0f / (float)dpi;
    *th = (float)*ph * 96.0f / (float)dpi;
}

/* points: pixels at the display's dots per inch over 96 */
void host_window_points(float *w, float *h)
{
    int pw, ph, dpi = s_win ? window_dpi(s_win) : 96;
    host_window_pixels(&pw, &ph);
    *w = (float)pw * 96.0f / (float)dpi;
    *h = (float)ph * 96.0f / (float)dpi;
}

void host_window_pixels(int *w, int *h)
{
    RECT r;
    *w = *h = 0;
    if (s_win && GetClientRect(s_win, &r)) {
        *w = r.right - r.left;
        *h = r.bottom - r.top;
    }
}

/* a close waiting in the queue (render/gl's wait while its shaders are made) */
int host_close_pending(void)
{
    int i;
    for (i = s_qh; i != s_qt; i = (i + 1) % QMAX)
        if (s_q[i].type == HOST_EV_CLOSE)
            return 1;
    return 0;
}

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    MSG m;
    while (s_qh == s_qt) {
        if (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
            continue;
        }
        if (wait_ms == 0)
            return 0;
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, wait_ms == 0xFFFFFFFFu ? INFINITE : wait_ms, QS_ALLINPUT) == WAIT_TIMEOUT)
            return 0;
        wait_ms = 0;
    }
    *ev = s_q[s_qh];
    s_qh = (s_qh + 1) % QMAX;
    return 1;
}

void host_message_box(const char *text, const char *caption)
{
    MessageBoxA(s_win, text ? text : "", caption ? caption : s_app_title, MB_OK);
}

/* ---- audio out: WASAPI ------------------------------------------------------------ */
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ksmedia.h), spelled here */
static const GUID k_subtype_float = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static IAudioClient        *s_ac;
static IAudioRenderClient  *s_rc;
static HANDLE               s_aev, s_athread;
static volatile LONG        s_astop;
static host_audio_fn        s_afn;
static void                *s_auser;
static UINT32               s_abuf;
static int                  s_ach;

static DWORD WINAPI audio_main(LPVOID p)
{
    float tmp[2 * 4096];
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!s_astop) {
        UINT32 pad = 0, n, k;
        BYTE *buf;
        WaitForSingleObject(s_aev, 100);
        if (FAILED(IAudioClient_GetCurrentPadding(s_ac, &pad)))
            continue;
        n = s_abuf - pad;
        while (n > 0) {
            UINT32 m = n > 4096 ? 4096 : n;
            if (FAILED(IAudioRenderClient_GetBuffer(s_rc, m, &buf)))
                break;
            s_afn(tmp, (int)m, s_auser);
            /* the mixer is stereo; the device may have more channels */
            for (k = 0; k < m; k++) {
                float *o = (float *)buf + (size_t)k * (size_t)s_ach;
                int c;
                o[0] = tmp[2 * k];
                if (s_ach > 1)
                    o[1] = tmp[2 * k + 1];
                for (c = 2; c < s_ach; c++)
                    o[c] = 0;
            }
            IAudioRenderClient_ReleaseBuffer(s_rc, m, 0);
            n -= m;
        }
    }
    return 0;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEXTENSIBLE wf;
    WAVEFORMATEX *mix = NULL;
    if (s_ac)
        return 1;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)))
        return 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        IMMDeviceEnumerator_Release(en);
        return 0;
    }
    IMMDeviceEnumerator_Release(en);
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&s_ac))) {
        IMMDevice_Release(dev);
        return 0;
    }
    IMMDevice_Release(dev);
    IAudioClient_GetMixFormat(s_ac, &mix);
    /* float at the mixer's rate; the shared-mode engine converts */
    memset(&wf, 0, sizeof wf);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = (WORD)(mix && mix->nChannels ? mix->nChannels : 2);
    wf.Format.nSamplesPerSec = (DWORD)rate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = (WORD)(wf.Format.nChannels * 4);
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = wf.Format.nChannels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                       : mix && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask : 0;
    wf.SubFormat = k_subtype_float;
    if (mix)
        CoTaskMemFree(mix);
    s_ach = wf.Format.nChannels;
    if (FAILED(IAudioClient_Initialize(s_ac, AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       500000, 0, (WAVEFORMATEX *)&wf, NULL)) ||
        FAILED(IAudioClient_GetBufferSize(s_ac, &s_abuf)) ||
        FAILED(IAudioClient_GetService(s_ac, &IID_IAudioRenderClient, (void **)&s_rc))) {
        IAudioClient_Release(s_ac);
        s_ac = NULL;
        return 0;
    }
    s_aev = CreateEventA(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(s_ac, s_aev);
    s_afn = fn;
    s_auser = user;
    s_astop = 0;
    IAudioClient_Start(s_ac);
    s_athread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
    return 1;
}

void host_audio_close(void)
{
    if (!s_ac)
        return;
    InterlockedExchange(&s_astop, 1);
    SetEvent(s_aev);
    WaitForSingleObject(s_athread, INFINITE);
    CloseHandle(s_athread);
    IAudioClient_Stop(s_ac);
    IAudioRenderClient_Release(s_rc);
    IAudioClient_Release(s_ac);
    CloseHandle(s_aev);
    s_ac = NULL;
    s_rc = NULL;
}

/* ---- game controllers: XInput -------------------------------------------------- */
int host_pad_read(int index, host_pad *o)
{
    XINPUT_STATE st;
    const XINPUT_GAMEPAD *g;
    unsigned b = 0;
    DWORD i;
    int n = 0;
    memset(o, 0, sizeof *o);
    for (i = 0; i < XUSER_MAX_COUNT; i++)
        if (XInputGetState(i, &st) == ERROR_SUCCESS && n++ == index)
            break;
    if (i == XUSER_MAX_COUNT)
        return 0;
    g = &st.Gamepad;
    /* XInput's controllers are Xbox controllers (their names: the front
       end's, by the style) */
    o->style = HOST_PAD_XBOX;
    snprintf(o->name, sizeof o->name, "Xbox Controller");
    o->x = g->sThumbLX / 32767.0f;
    o->y = -g->sThumbLY / 32767.0f;
    if (o->x < -1) o->x = -1;
    if (o->y < -1) o->y = -1;
    o->rx = g->sThumbRX / 32767.0f;
    o->ry = -g->sThumbRY / 32767.0f;
    if (o->rx < -1) o->rx = -1;
    if (o->ry > 1) o->ry = 1;
    o->lt = g->bLeftTrigger / 255.0f;
    o->rt = g->bRightTrigger / 255.0f;
    if (g->wButtons & XINPUT_GAMEPAD_A) b |= 1u << 0;
    if (g->wButtons & XINPUT_GAMEPAD_B) b |= 1u << 1;
    if (g->wButtons & XINPUT_GAMEPAD_X) b |= 1u << 2;
    if (g->wButtons & XINPUT_GAMEPAD_Y) b |= 1u << 3;
    if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= 1u << 4;
    if (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= 1u << 5;
    if (g->wButtons & XINPUT_GAMEPAD_BACK) b |= 1u << 6;
    if (g->wButtons & XINPUT_GAMEPAD_START) b |= 1u << 7;
    if (g->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) b |= 1u << 8;
    if (g->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) b |= 1u << 9;
    if (g->bLeftTrigger > 127) b |= 1u << 10;
    if (g->bRightTrigger > 127) b |= 1u << 11;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= 1u << 12;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= 1u << 13;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= 1u << 14;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= 1u << 15;
    o->buttons = b;
    return 1;
}

void host_safe_area(float *l, float *t, float *r, float *b) { *l = *t = *r = *b = 0; }

void host_text_input(int on) { (void)on; }

int host_keyboard_attached(void) { return 1; }
