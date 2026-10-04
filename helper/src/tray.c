// A notification-area icon while the helper runs: Mario's head from SM64's lives counter (taken from the player's ROM
// like every other graphic), tooltip "NG64 helper". No window, no menu - it's only there so you can see it's running.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int ng64_hud_image(const char *name, const uint8_t **rgba, int *w, int *h);

#define TRAY_ID 1

static HWND s_win;
static HICON s_icon;
static NOTIFYICONDATAA s_nid;
static UINT s_taskbarCreated;
static int s_added;

// the 16x16 head scaled (nearest, to stay crisp) to the size the notification area wants
static HICON make_icon(void)
{
    const uint8_t *px;
    int w, h;
    if (!ng64_hud_image("mario", &px, &w, &h)) return LoadIconA(NULL, IDI_APPLICATION);
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    uint32_t *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color) return LoadIconA(NULL, IDI_APPLICATION);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const uint8_t *p = px + ((size_t)(y * h / size) * w + (x * w / size)) * 4;
            bits[y * size + x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconA(NULL, IDI_APPLICATION);
}

static void add_icon(void)
{
    memset(&s_nid, 0, sizeof(s_nid));
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_win;
    s_nid.uID = TRAY_ID;
    s_nid.uFlags = NIF_ICON | NIF_TIP;
    s_nid.hIcon = s_icon;
    snprintf(s_nid.szTip, sizeof(s_nid.szTip), "NG64 helper - running");
    s_added = Shell_NotifyIconA(NIM_ADD, &s_nid) != 0;
}

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == s_taskbarCreated && s_taskbarCreated) add_icon();   // explorer restarted: its icons are gone
    return DefWindowProcA(h, msg, wp, lp);
}

void ng64_tray_stop(void)
{
    if (s_added) Shell_NotifyIconA(NIM_DELETE, &s_nid);
    s_added = 0;
}

int ng64_tray_start(void)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NG64HelperTray";
    RegisterClassA(&wc);
    s_taskbarCreated = RegisterWindowMessageA("TaskbarCreated");
    s_win = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "NG64 helper", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!s_win) return 0;
    s_icon = make_icon();
    add_icon();
    atexit(ng64_tray_stop);   // however the helper ends, the icon goes with it
    return s_added;
}

// called from the main loop: the window needs its messages answered (explorer restarts, shutdown)
void ng64_tray_pump(void)
{
    MSG m;
    while (s_win && PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
