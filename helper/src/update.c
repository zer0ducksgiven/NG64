// Update check: asks GitHub (a plain HTTPS GET of the latest release of the project, through WinHTTP) whether a newer
// NG64 has been published, so the game can say so. Nothing is sent but the request itself; the answer is a release tag.
// Off if no-update-check.txt sits next to the helper, or with --no-update-check.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "version.h"
#include "update.h"

#define RELEASES_HOST L"api.github.com"
#define RELEASES_PATH L"/repos/zer0ducksgiven/NG64/releases/latest"
#define RECHECK_MS    (12ul * 60 * 60 * 1000)

static volatile LONG s_state;          // NG64_UPDATE_*
static char s_tag[40];                 // the newest release's tag, as published ("v0.1.5")
static char s_current[40] = NG64_VERSION;

// "v0.1.5" / "0.1.5" -> {0, 1, 5}; false if it isn't a version
static int parse_version(const char *s, int v[3])
{
    if (*s == 'v' || *s == 'V') s++;
    v[0] = v[1] = v[2] = 0;
    int got = 0;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return got > 0;
        v[i] = atoi(s);
        got++;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') s++; else break;
    }
    return got > 0;
}

int ng64_version_newer(const char *tag, const char *current)
{
    int a[3], b[3];
    if (!parse_version(tag, a) || !parse_version(current, b)) return 0;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 0;
}

// the value of "tag_name" in a release's JSON; false if it isn't there
static int find_tag(const char *json, char *out, size_t outSize)
{
    const char *k = strstr(json, "\"tag_name\"");
    if (!k) return 0;
    k = strchr(k + 10, '"');
    if (!k) return 0;
    const char *e = strchr(k + 1, '"');
    if (!e || (size_t)(e - k - 1) >= outSize) return 0;
    memcpy(out, k + 1, (size_t)(e - k - 1));
    out[e - k - 1] = 0;
    return 1;
}

static int fetch_latest_tag(char *tag, size_t tagSize)
{
    int ok = 0;
    char ua[80];
    snprintf(ua, sizeof(ua), "NG64-helper/%s", NG64_VERSION);
    wchar_t wua[80];
    MultiByteToWideChar(CP_ACP, 0, ua, -1, wua, 80);
    HINTERNET ses = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 8000, 8000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, RELEASES_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", RELEASES_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            static char body[64 * 1024];
            DWORD total = 0, n = 0;
            while (total < sizeof(body) - 1 && WinHttpReadData(req, body + total, (DWORD)(sizeof(body) - 1 - total), &n) && n) total += n;
            body[total] = 0;
            ok = find_tag(body, tag, tagSize);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static DWORD WINAPI check_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        char tag[40];
        if (fetch_latest_tag(tag, sizeof(tag))) {
            if (ng64_version_newer(tag, s_current)) {
                snprintf(s_tag, sizeof(s_tag), "%s", tag);
                s_state = NG64_UPDATE_NEWER;
            } else if (s_state != NG64_UPDATE_NEWER) {
                s_state = NG64_UPDATE_CURRENT;
            }
        }
        Sleep(RECHECK_MS);
    }
    return 0;
}

void ng64_update_start(const char *exeDir)
{
    char flag[MAX_PATH];
    snprintf(flag, sizeof(flag), "%s\\no-update-check.txt", exeDir);
    if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) return;
    // a test hook: pretend to be another version, to see the notice
    const char *fake = getenv("NG64_UPDATE_TEST_VERSION");
    if (fake && *fake) snprintf(s_current, sizeof(s_current), "%s", fake);
    HANDLE t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

int ng64_update_state(char *tag, size_t tagSize, char *current, size_t currentSize)
{
    LONG st = s_state;
    if (st == NG64_UPDATE_NEWER) {
        snprintf(tag, tagSize, "%s", s_tag);
        snprintf(current, currentSize, "%s", s_current);
    }
    return (int)st;
}
