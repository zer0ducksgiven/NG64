// When the helper runs, without anyone starting it by hand. A BeamNG mod can't start programs (its Lua sandbox stubs
// os.execute / io.popen, and FFI can't reach Windows), so the installer registers "ng64helper.exe --watch" to start
// at sign-in: a small standby process that does nothing until BeamNG starts, then launches the helper proper for
// that game. The helper exits when the game it belongs to closes; the watcher waits for the next time.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define GAME_EXE        "BeamNG.drive.x64.exe"
#define HELPER_MUTEX    "Local\\NG64Helper_%d"   // per port, so a test helper on another port can run alongside
#define WATCHER_MUTEX   "Local\\NG64Watcher"
#define WATCH_POLL_MS   2000

// the running game's process id, 0 if it isn't running
DWORD ng64_find_game(void)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, GAME_EXE)) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

// ---- helper side ------------------------------------------------------------------------------------------------

static HANDLE s_game;           // the game this helper belongs to (NULL until it's seen)
static DWORD s_gamePid;
static DWORD s_lastLook;

// one helper at a time (per port): 0 if another is already running (this one should just quit)
int ng64_single_instance(int port)
{
    char name[64];
    snprintf(name, sizeof(name), HELPER_MUTEX, port);
    CreateMutexA(NULL, TRUE, name);
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

// the game to follow: the one the watcher started us for, or (started by hand) whichever runs first
void ng64_attach_game(DWORD pid)
{
    if (s_game || !pid) return;
    s_game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (s_game) s_gamePid = pid;
}

// polled from the main loop: 1 once the game this helper belongs to has closed
int ng64_game_closed(void)
{
    if (!s_game) {
        DWORD now = GetTickCount();
        if (now - s_lastLook < WATCH_POLL_MS) return 0;
        s_lastLook = now;
        ng64_attach_game(ng64_find_game());
        return 0;
    }
    return WaitForSingleObject(s_game, 0) == WAIT_OBJECT_0;
}

DWORD ng64_game_pid(void) { return s_gamePid; }

// ---- watcher ------------------------------------------------------------------------------------------------------

static FILE *s_wlog;
static void wlog(const char *fmt, ...)
{
    if (!s_wlog) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_wlog, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_wlog, fmt, ap);
    va_end(ap);
    fputc('\n', s_wlog);
    fflush(s_wlog);
}

static int helper_running(void)
{
    char name[64];
    snprintf(name, sizeof(name), HELPER_MUTEX, 47064);   // NG64_PORT: the helper the mod talks to
    HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, name);
    if (!m) return 0;
    CloseHandle(m);
    return 1;
}

// "--watch": runs until sign-out. Nearly free while idle: one process list every couple of seconds.
int ng64_run_watcher(const char *exePath, const char *exeDir)
{
    CreateMutexA(NULL, TRUE, WATCHER_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already watching
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\ng64watch.log", exeDir);
    s_wlog = fopen(logPath, "w");
    wlog("watching for %s", GAME_EXE);

    DWORD startedFor = 0;
    for (;;) {
        DWORD pid = ng64_find_game();
        if (pid && pid != startedFor && !helper_running()) {
            char cmd[MAX_PATH + 64];
            snprintf(cmd, sizeof(cmd), "\"%s\" --parent %lu", exePath, (unsigned long)pid);
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            if (CreateProcessA(exePath, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, exeDir, &si, &pi)) {
                wlog("BeamNG started (pid %lu): started the helper (pid %lu)", (unsigned long)pid, (unsigned long)pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                startedFor = pid;
            } else {
                wlog("BeamNG started (pid %lu) but the helper could not be started (error %lu)", (unsigned long)pid, GetLastError());
                startedFor = pid;   // don't retry every 2 s for the same game
            }
        }
        if (!pid && startedFor) { wlog("BeamNG closed"); startedFor = 0; }
        Sleep(WATCH_POLL_MS);
    }
}
