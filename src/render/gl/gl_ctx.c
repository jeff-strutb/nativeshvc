/* gl_ctx.c: the OpenGL renderer's context (gl_api.h), built with the
 * system's own headers (the Boss Rally port's):
 *
 *   Windows  WGL on the host's window: a pixel format with double buffering,
 *            a legacy context to find wglCreateContextAttribsARB, then a 3.3
 *            core one; offscreen, the same on a hidden window of its own
 *   macOS    CGL, a 3.2+ core profile (4.1 in practice), always offscreen: the
 *            Mac draws with Metal, this is for checking the GL renderer there
 *
 * Anything else: no context (the renderer fails to open). */
#include <stdio.h>
#include <string.h>

#include "gl_api.h"

#define GL_DEF(ret, name, args) PFN_##name name##_p;
GL_FUNCS(GL_DEF)
#undef GL_DEF

int gl_api_load(void)
{
    int ok = 1;
#define GL_LOAD(ret, name, args) \
    if (!(name##_p = (PFN_##name)gl_ctx_proc(#name))) { fprintf(stderr, "rr: gl: no %s\n", #name); ok = 0; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return ok;
}

#if defined(_WIN32)
#include <windows.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *PFNCREATEATTRIBS)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFNSWAPINTERVAL)(int);

static HDC s_dc;
static HGLRC s_rc;
static HMODULE s_gl;
static PFNCREATEATTRIBS s_create;
static int s_pf;
static const int k_attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                               WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
static int s_on_window;

/* a hidden window to hold a context */
static HWND hidden_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RrGlHidden";
    RegisterClassA(&wc);
    return CreateWindowExA(0, "RrGlHidden", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
}

void *gl_ctx_proc(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {   /* the 1.1 ones: opengl32.dll's */
        if (!s_gl)
            s_gl = LoadLibraryA("opengl32.dll");
        p = s_gl ? (void *)GetProcAddress(s_gl, name) : NULL;
    }
    return p;
}

int gl_ctx_open(void *win)
{
    HWND w = (HWND)win;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC legacy;
    PFNSWAPINTERVAL interval;
    int pf;
    s_on_window = w != NULL;
    if (!w) {   /* offscreen: a hidden window to hold the context */
        w = hidden_window();
        if (!w)
            return 0;
    }
    s_dc = GetDC(w);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    if (!(pf = ChoosePixelFormat(s_dc, &pfd)) || !SetPixelFormat(s_dc, pf, &pfd))
        return 0;
    s_pf = pf;
    if (!(legacy = wglCreateContext(s_dc)) || !wglMakeCurrent(s_dc, legacy))
        return 0;
    s_create = (PFNCREATEATTRIBS)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!s_create || !(s_rc = s_create(s_dc, NULL, k_attrs))) {
        fprintf(stderr, "rr: gl: no 3.3 core context\n");
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(legacy);
        return 0;
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!wglMakeCurrent(s_dc, s_rc))
        return 0;
    if ((interval = (PFNSWAPINTERVAL)(void *)wglGetProcAddress("wglSwapIntervalEXT")))
        interval(0);   /* the game paces its own frames */
    return 1;
}

void gl_ctx_swap(void)
{
    if (s_dc)
        SwapBuffers(s_dc);
}

typedef struct {
    HDC dc;
    HGLRC rc;
    void (*fn)(void *);
    void *arg;
    volatile LONG ran;
} GlRun;

static DWORD WINAPI run_thread(LPVOID p)
{
    GlRun *r = (GlRun *)p;
    if (wglMakeCurrent(r->dc, r->rc)) {
        r->fn(r->arg);
        glFinish();                     /* its objects whole for the other context */
        wglMakeCurrent(NULL, NULL);
        r->ran = 1;
    }
    return 0;
}

int host_close_pending(void);

void gl_ctx_run(void (*fn)(void *), void *arg)
{
    GlRun r;
    HWND w;
    HANDLE t = NULL;
    PIXELFORMATDESCRIPTOR pfd;

    memset(&r, 0, sizeof r);
    r.fn = fn;
    r.arg = arg;
    /* a context of its own, on a hidden window of the same pixel format */
    w = s_on_window ? hidden_window() : NULL;
    if (w && (r.dc = GetDC(w)) != NULL) {
        memset(&pfd, 0, sizeof pfd);
        pfd.nSize = sizeof pfd;
        if (DescribePixelFormat(r.dc, s_pf, sizeof pfd, &pfd) && SetPixelFormat(r.dc, s_pf, &pfd))
            r.rc = s_create(r.dc, s_rc, k_attrs);
        /* (a driver's compiler may go deep: as much stack as the main thread could want) */
        if (r.rc)
            t = CreateThread(NULL, 16u << 20, run_thread, &r, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    while (t) {
        MSG m;
        if (MsgWaitForMultipleObjects(1, &t, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0)
            break;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (host_close_pending())       /* nothing has begun, nothing to keep: not waiting for the driver */
            TerminateProcess(GetCurrentProcess(), 0);
    }
    if (t)
        CloseHandle(t);
    if (r.rc)
        wglDeleteContext(r.rc);
    if (w)
        DestroyWindow(w);
    if (!r.ran)                         /* no thread or context to be had: here, as ever */
        fn(arg);
}

#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>

static CGLContextObj s_ctx;

void *gl_ctx_proc(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

int gl_ctx_open(void *win)
{
    CGLPixelFormatAttribute a[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
                                    kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
                                    kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf;
    GLint n;
    (void)win;   /* (the Mac's windows draw with Metal) */
    if (CGLChoosePixelFormat(a, &pf, &n) != kCGLNoError || !pf)
        return 0;
    if (CGLCreateContext(pf, NULL, &s_ctx) != kCGLNoError) {
        CGLDestroyPixelFormat(pf);
        return 0;
    }
    CGLDestroyPixelFormat(pf);
    return CGLSetCurrentContext(s_ctx) == kCGLNoError;
}

void gl_ctx_swap(void) {}
void gl_ctx_run(void (*fn)(void *), void *arg) { fn(arg); }

#else
void *gl_ctx_proc(const char *name) { (void)name; return NULL; }
int gl_ctx_open(void *win) { (void)win; return 0; }
void gl_ctx_swap(void) {}
void gl_ctx_run(void (*fn)(void *), void *arg) { fn(arg); }
#endif
