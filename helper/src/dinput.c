// DirectInput controllers (PlayStation, Switch Pro, generic pads) read in the background and mapped to the XInput layout
// by pad_map.c. XInput-compatible devices are left to XInput, which already reads them; the two are merged in
// read_pad (main.c), so a PlayStation pad and an Xbox pad both work, whichever is touched.
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "pad.h"

#define MAX_DEVS 4
#define SCAN_MS  3000   // plugging a pad in is noticed within this

typedef struct {
    IDirectInputDevice8A *dev;
    GUID inst;
    unsigned axisMask;
    unsigned char prev[32];   // buttons last poll, to log presses
    NgPadMap map;
} Dev;

static IDirectInput8A *s_di;
static Dev s_dev[MAX_DEVS];
static int s_n;
static void (*s_log)(const char *);
static char s_ini[4096];
static int s_haveIni;

static void say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log) s_log(buf);
}

static const DWORD s_axisOffsets[8] = {
    DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)
};

// XInput-compatible controllers carry "IG_" in their device path: XInput has them
static int is_xinput_device(IDirectInputDevice8A *dev)
{
    DIPROPGUIDANDPATH gp = { { sizeof(gp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE } };
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph))) return 0;
    _wcslwr(gp.wszPath);
    return wcsstr(gp.wszPath, L"ig_") != NULL;
}

static void release_all(void)
{
    for (int i = 0; i < s_n; i++)
        if (s_dev[i].dev) { IDirectInputDevice8_Unacquire(s_dev[i].dev); IDirectInputDevice8_Release(s_dev[i].dev); }
    s_n = 0;
}

typedef struct { GUID guids[16]; DIDEVICEINSTANCEA inst[16]; int n; } Found;

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    Found *f = ctx;
    BYTE type = GET_DIDEVICE_TYPE(inst->dwDevType);
    // a DualSense reports itself as a "first person" device (type 24), not a gamepad; wheels and flight sticks stay out
    if ((type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK || type == DI8DEVTYPE_1STPERSON) && f->n < 16) {
        f->inst[f->n] = *inst;
        f->guids[f->n++] = inst->guidInstance;
    }
    return DIENUM_CONTINUE;
}

static void open_device(const DIDEVICEINSTANCEA *inst)
{
    IDirectInputDevice8A *dev;
    if (FAILED(IDirectInput8_CreateDevice(s_di, &inst->guidInstance, &dev, NULL))) return;
    if (is_xinput_device(dev)) { IDirectInputDevice8_Release(dev); return; }
    HWND win = FindWindowA("NG64HelperTray", NULL);
    if (!win) win = GetDesktopWindow();
    if (FAILED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2)) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(dev, win, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        IDirectInputDevice8_Release(dev);
        return;
    }
    Dev *d = &s_dev[s_n];
    memset(d, 0, sizeof(*d));
    d->dev = dev;
    d->inst = inst->guidInstance;
    // every axis spans the full signed range; the ones that refuse aren't there
    DIPROPDWORD dz = { { sizeof(dz), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, 0 };
    IDirectInputDevice8_SetProperty(dev, DIPROP_DEADZONE, &dz.diph);
    for (int i = 0; i < 8; i++) {
        DIPROPRANGE r = { { sizeof(r), sizeof(DIPROPHEADER), s_axisOffsets[i], DIPH_BYOFFSET }, -32768, 32767 };
        if (SUCCEEDED(IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &r.diph))) d->axisMask |= 1u << i;
    }
    DIPROPDWORD vp = { { sizeof(vp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, 0 };
    unsigned vid = 0, pid = 0;
    if (SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_VIDPID, &vp.diph))) { vid = vp.dwData & 0xFFFF; pid = vp.dwData >> 16; }
    int layout = ng_pad_layout_for_vendor(vid);
    ng_pad_default_map(layout, vid, &d->map);
    int bad = s_haveIni ? ng_pad_apply_ini(s_ini, vid, &d->map) : 0;
    IDirectInputDevice8_Acquire(dev);
    say("controller (DirectInput): %s, vendor %04x product %04x, %s layout%s%s", inst->tszProductName, vid, pid,
        layout == NG_LAYOUT_SWITCH ? "Switch" : "PlayStation-style", s_haveIni ? ", controller.ini applied" : "",
        bad ? " (some of its lines weren't understood)" : "");
    s_n++;
}

// the connected set changed (or this is the first look): reopen the lot
static void scan(void)
{
    Found f = { 0 };
    if (FAILED(IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_cb, &f, DIEDFL_ATTACHEDONLY))) return;
    static GUID known[16];
    static int knownN = -1;
    int same = knownN == f.n;
    for (int i = 0; same && i < f.n; i++) same = IsEqualGUID(&known[i], &f.guids[i]);
    if (same) return;
    release_all();
    for (int i = 0; i < f.n && s_n < MAX_DEVS; i++) open_device(&f.inst[i]);
    memcpy(known, f.guids, sizeof(GUID) * f.n);
    knownN = f.n;
    if (!s_n && f.n) say("DirectInput controllers found but none usable (XInput ones are read through XInput)");
}

static void merge(Pad *to, const Pad *from)
{
    if (fabsf(from->lx) > fabsf(to->lx)) to->lx = from->lx;
    if (fabsf(from->ly) > fabsf(to->ly)) to->ly = from->ly;
    if (fabsf(from->rx) > fabsf(to->rx)) to->rx = from->rx;
    if (fabsf(from->ry) > fabsf(to->ry)) to->ry = from->ry;
    to->a |= from->a; to->b |= from->b; to->z |= from->z; to->zoomIn |= from->zoomIn; to->zoomOut |= from->zoomOut;
    to->y |= from->y; to->music |= from->music; to->songNext |= from->songNext; to->songPrev |= from->songPrev;
}

// Everything DirectInput does - looking for pads (which can take 100+ ms with a few HID devices around), opening
// them, polling them - happens on this thread, so the 30 Hz simulation never waits on it. It publishes the latest
// state for ng64_dinput_read to copy.
static CRITICAL_SECTION s_stateLock;
static Pad s_state;
static volatile LONG s_connected;

static void poll_all(Pad *out)
{
    memset(out, 0, sizeof(*out));
    int any = 0;
    for (int i = 0; i < s_n; i++) {
        Dev *d = &s_dev[i];
        IDirectInputDevice8_Poll(d->dev);
        DIJOYSTATE2 st;
        HRESULT hr = IDirectInputDevice8_GetDeviceState(d->dev, sizeof(st), &st);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            IDirectInputDevice8_Acquire(d->dev);
            hr = IDirectInputDevice8_GetDeviceState(d->dev, sizeof(st), &st);
        }
        if (FAILED(hr)) continue;
        NgDiState s;
        memset(&s, 0, sizeof(s));
        s.axisMask = d->axisMask;
        s.axis[0] = st.lX; s.axis[1] = st.lY; s.axis[2] = st.lZ; s.axis[3] = st.lRx; s.axis[4] = st.lRy; s.axis[5] = st.lRz;
        s.axis[6] = st.rglSlider[0]; s.axis[7] = st.rglSlider[1];
        for (int b = 0; b < 32; b++) {
            s.btn[b] = (st.rgbButtons[b] & 0x80) != 0;
            // the first presses are logged with their DirectInput number (the one controller.ini uses), so a
            // button that does the wrong thing can be told apart from the log
            static int logged;
            if (s.btn[b] && !d->prev[b] && logged < 60) { logged++; say("controller button %d pressed", b + 1); }
            d->prev[b] = s.btn[b];
        }
        Pad p;
        ng_pad_map(&d->map, &s, &p);
        merge(out, &p);
        any = 1;
    }
    s_connected = any;
}

static DWORD WINAPI input_thread(LPVOID arg)
{
    (void)arg;
    DWORD lastScan = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (!lastScan || now - lastScan >= SCAN_MS) { lastScan = now; scan(); }
        Pad p;
        poll_all(&p);
        EnterCriticalSection(&s_stateLock);
        s_state = p;
        LeaveCriticalSection(&s_stateLock);
        Sleep(8);
    }
    return 0;
}

void ng64_dinput_init(void (*logfn)(const char *), const char *iniPath)
{
    s_log = logfn;
    if (iniPath) {
        FILE *f = fopen(iniPath, "rb");
        if (f) {
            size_t n = fread(s_ini, 1, sizeof(s_ini) - 1, f);
            s_ini[n] = 0;
            s_haveIni = 1;
            fclose(f);
        }
    }
    InitializeCriticalSection(&s_stateLock);
    if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&s_di, NULL))) {
        s_di = NULL;
        say("DirectInput isn't available - only XInput controllers will work");
        return;
    }
    HANDLE t = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
    }
}

// adds the DirectInput pads' latest state into pad (the larger stick, buttons or'd); returns 1 when a pad is connected
int ng64_dinput_read(Pad *pad)
{
    if (!s_di) return 0;
    Pad p;
    EnterCriticalSection(&s_stateLock);
    p = s_state;
    LeaveCriticalSection(&s_stateLock);
    merge(pad, &p);
    return s_connected != 0;
}
