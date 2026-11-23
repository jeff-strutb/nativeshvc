/*
 * pad_wgi.c: game controllers that are not XInput devices, through Windows.Gaming.Input's
 * RawGameController (DualShock, DualSense, Switch Pro, 8BitDo in D or S mode, other HID pads).
 * XInput pads are read by host_windows.c and left out here (Windows.Gaming.Input also lists
 * them, as Gamepads). A raw controller is a controller only when SDL_GameControllerDB
 * (pad_db_windows.h) lists its vendor and product; its buttons, axes and switches are then read
 * through that entry's layout (pad pad_map.h).
 *
 * combase.dll is loaded when first needed, so the game still runs on a Windows without
 * Windows.Gaming.Input (the XInput pads then work as before). A controller keeps its slot (0-3 here,
 * indices 4-7 to the engine) while it is connected. Windows delivers a raw controller's input only to
 * the window that has the focus.
 */
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "pad_map.h"
#include "pad_db_windows.h"

typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameControllerStatics IStatics;
typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameController IRaw;
typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameController2 IRaw2;
typedef __x_ABI_CWindows_CGaming_CInput_CIGameController IGameCtl;
typedef __x_ABI_CWindows_CGaming_CInput_CIGamepadStatics2 IGamepadStatics2;
typedef __x_ABI_CWindows_CGaming_CInput_CIGamepad IGamepad;
typedef __FIVectorView_1_Windows__CGaming__CInput__CRawGameController IView;
typedef __FIEventHandler_1_Windows__CGaming__CInput__CRawGameController IHandler;
typedef __x_ABI_CWindows_CGaming_CInput_CGameControllerSwitchPosition SwitchPos;

enum { SLOTS = 4, MAX_BUTTONS = 64, MAX_AXES = 16, MAX_SWITCHES = 4 };

static HRESULT (WINAPI *p_init)(RO_INIT_TYPE);
static HRESULT (WINAPI *p_factory)(HSTRING, REFIID, void **);
static HRESULT (WINAPI *p_ref)(PCWSTR, UINT32, HSTRING_HEADER *, HSTRING *);
static PCWSTR (WINAPI *p_raw)(HSTRING, UINT32 *);

static IStatics *statics;
static IGamepadStatics2 *gamepads;
static int state;                       /* 0 not tried, 1 ready, -1 not available */
static ULONGLONG next_list;

static struct slot {
    IRaw    *raw;
    char     key[96];
    char     name[64];
    uint64_t id;
    int      style;
    pad_map  map;
    UINT32   nbuttons, naxes, nswitches;
} slots[SLOTS];

/* the events are only subscribed to: Windows lists a raw controller after a program has asked
   to hear of them. The program reads the list itself, so the handlers do nothing */
static HRESULT STDMETHODCALLTYPE h_query(IHandler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAgileObject) ||
        IsEqualIID(riid, &IID___FIEventHandler_1_Windows__CGaming__CInput__CRawGameController)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_ref(IHandler *This) { (void)This; return 2; }
static HRESULT STDMETHODCALLTYPE h_invoke(IHandler *This, IInspectable *sender, IRaw *args)
{
    (void)This; (void)sender; (void)args;
    return S_OK;
}
static __FIEventHandler_1_Windows__CGaming__CInput__CRawGameControllerVtbl handler_vtbl = { h_query, h_ref, h_ref, h_invoke };
static IHandler handler = { &handler_vtbl };

static HSTRING make_string(const WCHAR *s, HSTRING_HEADER *h)
{
    HSTRING out = NULL;
    p_ref(s, (UINT32)wcslen(s), h, &out);
    return out;
}

static int start(void)
{
    HMODULE m = LoadLibraryA("combase.dll");
    HSTRING_HEADER h1, h2;
    HSTRING c1, c2;
    EventRegistrationToken tok;
    if (!m)
        return -1;
    p_init = (void *)GetProcAddress(m, "RoInitialize");
    p_factory = (void *)GetProcAddress(m, "RoGetActivationFactory");
    p_ref = (void *)GetProcAddress(m, "WindowsCreateStringReference");
    p_raw = (void *)GetProcAddress(m, "WindowsGetStringRawBuffer");
    if (!p_init || !p_factory || !p_ref || !p_raw)
        return -1;
    p_init(RO_INIT_MULTITHREADED);      /* already initialised on this thread is fine */
    c1 = make_string(RuntimeClass_Windows_Gaming_Input_RawGameController, &h1);
    c2 = make_string(RuntimeClass_Windows_Gaming_Input_Gamepad, &h2);
    if (!c1 || !c2 || FAILED(p_factory(c1, &IID___x_ABI_CWindows_CGaming_CInput_CIRawGameControllerStatics, (void **)&statics)) || !statics)
        return -1;
    p_factory(c2, &IID___x_ABI_CWindows_CGaming_CInput_CIGamepadStatics2, (void **)&gamepads);
    statics->lpVtbl->add_RawGameControllerAdded(statics, &handler, &tok);
    statics->lpVtbl->add_RawGameControllerRemoved(statics, &handler, &tok);
    return 1;
}

static void narrow(HSTRING s, char *out, size_t n)
{
    UINT32 len = 0;
    PCWSTR w = s ? p_raw(s, &len) : NULL;
    out[0] = 0;
    if (w && len)
        WideCharToMultiByte(CP_UTF8, 0, w, (int)len, out, (int)n - 1, NULL, NULL);
    out[n - 1] = 0;
}

static void slot_free(struct slot *s)
{
    if (s->raw)
        s->raw->lpVtbl->Release(s->raw);
    memset(s, 0, sizeof *s);
}

/* the controller's key (the system's id, or its vendor and product when it gives none), 0 if it
   is an XInput pad or not in the table */
static int acceptable(IRaw *raw, int ordinal, struct slot *s)
{
    IGameCtl *gc = NULL;
    IRaw2 *raw2 = NULL;
    UINT16 vid = 0, pid = 0;
    INT32 n;
    char id[80] = "", name[64] = "";
    int ok = 0;
    if (SUCCEEDED(raw->lpVtbl->QueryInterface(raw, &IID___x_ABI_CWindows_CGaming_CInput_CIGameController, (void **)&gc)) && gc && gamepads) {
        IGamepad *gp = NULL;
        gamepads->lpVtbl->FromGameController(gamepads, gc, &gp);
        gc->lpVtbl->Release(gc);
        if (gp) {                       /* an XInput pad: host_windows.c has it */
            gp->lpVtbl->Release(gp);
            return 0;
        }
    }
    raw->lpVtbl->get_HardwareVendorId(raw, &vid);
    raw->lpVtbl->get_HardwareProductId(raw, &pid);
    if (!pad_map_find(pad_db_windows, sizeof pad_db_windows / sizeof *pad_db_windows, vid, pid, -1, &s->map))
        return 0;
    if (SUCCEEDED(raw->lpVtbl->QueryInterface(raw, &IID___x_ABI_CWindows_CGaming_CInput_CIRawGameController2, (void **)&raw2)) && raw2) {
        HSTRING hs = NULL;
        if (SUCCEEDED(raw2->lpVtbl->get_NonRoamableId(raw2, &hs)))
            narrow(hs, id, sizeof id);
        hs = NULL;
        if (SUCCEEDED(raw2->lpVtbl->get_DisplayName(raw2, &hs)))
            narrow(hs, name, sizeof name);
        raw2->lpVtbl->Release(raw2);
    }
    if (id[0])
        snprintf(s->key, sizeof s->key, "%04x:%04x:%s", vid, pid, id);
    else
        snprintf(s->key, sizeof s->key, "%04x:%04x:#%d", vid, pid, ordinal);
    snprintf(s->name, sizeof s->name, "%s", name[0] ? name : s->map.name);
    s->style = pad_style_of_vendor(vid);
    s->id = pad_hash(s->key);
    n = 0; raw->lpVtbl->get_ButtonCount(raw, &n); s->nbuttons = n < 0 ? 0 : n > MAX_BUTTONS ? MAX_BUTTONS : (UINT32)n;
    n = 0; raw->lpVtbl->get_AxisCount(raw, &n); s->naxes = n < 0 ? 0 : n > MAX_AXES ? MAX_AXES : (UINT32)n;
    n = 0; raw->lpVtbl->get_SwitchCount(raw, &n); s->nswitches = n < 0 ? 0 : n > MAX_SWITCHES ? MAX_SWITCHES : (UINT32)n;
    ok = 1;
    return ok;
}

/* the system's controllers as slots: a connected one keeps its slot, a new one takes a free slot,
   one gone is freed */
static void refresh(void)
{
    IView *view = NULL;
    UINT32 count = 0, i;
    int seen[SLOTS] = { 0 };
    int ordinals_vid_pid[16][3];
    int nord = 0;
    if (FAILED(statics->lpVtbl->get_RawGameControllers(statics, &view)) || !view)
        return;
    view->lpVtbl->get_Size(view, &count);
    for (i = 0; i < count; i++) {
        IRaw *raw = NULL;
        struct slot cand;
        UINT16 vid = 0, pid = 0;
        int k, ord = 0, found = -1, free_k = -1;
        if (FAILED(view->lpVtbl->GetAt(view, i, &raw)) || !raw)
            continue;
        raw->lpVtbl->get_HardwareVendorId(raw, &vid);
        raw->lpVtbl->get_HardwareProductId(raw, &pid);
        for (k = 0; k < nord; k++)
            if (ordinals_vid_pid[k][0] == vid && ordinals_vid_pid[k][1] == pid)
                ord = ++ordinals_vid_pid[k][2];
        if (!ord && nord < 16) {
            ordinals_vid_pid[nord][0] = vid;
            ordinals_vid_pid[nord][1] = pid;
            ordinals_vid_pid[nord++][2] = 0;
        }
        memset(&cand, 0, sizeof cand);
        if (!acceptable(raw, ord, &cand)) {
            raw->lpVtbl->Release(raw);
            continue;
        }
        for (k = 0; k < SLOTS; k++) {
            if (slots[k].raw && !strcmp(slots[k].key, cand.key))
                found = k;
            else if (!slots[k].raw && free_k < 0)
                free_k = k;
        }
        if (found >= 0) {
            seen[found] = 1;
            raw->lpVtbl->Release(raw);
        } else if (free_k >= 0) {
            cand.raw = raw;
            slots[free_k] = cand;
            seen[free_k] = 1;
        } else
            raw->lpVtbl->Release(raw);
    }
    view->lpVtbl->Release(view);
    for (i = 0; i < SLOTS; i++)
        if (slots[i].raw && !seen[i])
            slot_free(&slots[i]);
}

/* index 0-3: the nth raw controller slot */
int pad_wgi_read(int index, host_pad *o)
{
    struct slot *s;
    boolean buttons[MAX_BUTTONS];
    SwitchPos switches[MAX_SWITCHES];
    DOUBLE axes_d[MAX_AXES];
    float axes[MAX_AXES];
    unsigned char b[MAX_BUTTONS], hats[MAX_SWITCHES];
    UINT64 stamp;
    pad_raw r;
    UINT32 i;
    if (state < 0 || index < 0 || index >= SLOTS)
        return 0;
    if (state == 0)
        state = start();
    if (state < 0)
        return 0;
    if (index == 0 || next_list == 0) {
        ULONGLONG now = GetTickCount64();
        if (now >= next_list) {
            refresh();
            next_list = now + 1000;
        }
    }
    s = &slots[index];
    if (!s->raw)
        return 0;
    memset(buttons, 0, sizeof buttons);
    memset(switches, 0, sizeof switches);
    memset(axes_d, 0, sizeof axes_d);
    if (FAILED(s->raw->lpVtbl->GetCurrentReading(s->raw, s->nbuttons, buttons, s->nswitches, switches, s->naxes, axes_d, &stamp)))
        return 0;
    for (i = 0; i < s->nbuttons; i++)
        b[i] = buttons[i] ? 1 : 0;
    for (i = 0; i < s->naxes; i++)
        axes[i] = (float)(axes_d[i] * 2.0 - 1.0);
    for (i = 0; i < s->nswitches; i++) {
        static const unsigned char mask[9] = { 0, 1, 3, 2, 6, 4, 12, 8, 9 };
        hats[i] = (unsigned)switches[i] <= 8 ? mask[switches[i]] : 0;
    }
    r.buttons = b;
    r.nbuttons = (int)s->nbuttons;
    r.axes = axes;
    r.naxes = (int)s->naxes;
    r.hats = hats;
    r.nhats = (int)s->nswitches;
    memset(o, 0, sizeof *o);
    pad_map_apply(&s->map, &r, o);
    o->style = s->style;
    o->id = s->id;
    snprintf(o->name, sizeof o->name, "%s", s->name);
    return 1;
}
