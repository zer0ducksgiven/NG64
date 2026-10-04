// libsm64 audio -> waveOut. One libsm64 audio tick is a 30 Hz game tick of audio: 2 x 528/544 frames of 32 kHz stereo
// s16. The device is kept about TARGET_MS ahead, so a late wake-up (the game is hogging the CPU) doesn't run it dry.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "libsm64.h"

#define NUM_BUFS   32
#define BUF_FRAMES (544 * 2)
#define SAMPLE_RATE 32000
#define DEFAULT_TARGET_MS 80    // NG64_AUDIO_TARGET_MS overrides it (for tuning)

static HWAVEOUT s_wave;
static HANDLE s_event;   // the device signals it each time it finishes a buffer
static WAVEHDR s_hdr[NUM_BUFS];
static int16_t s_data[NUM_BUFS][BUF_FRAMES * 2];
static volatile LONG s_running;
static HANDLE s_thread;
static uint32_t s_targetFrames;

// stats, read from the main thread (logged there: the audio thread never touches the log file)
static volatile LONG s_underruns, s_maxGapMs, s_level;

// libsm64's audio tick shares state with the calls that start sounds and music on the main thread; they take this
static CRITICAL_SECTION s_lock;
static volatile LONG s_lockReady;

void ng64_sm64_lock(void) { if (s_lockReady) EnterCriticalSection(&s_lock); }
void ng64_sm64_unlock(void) { if (s_lockReady) LeaveCriticalSection(&s_lock); }

static uint32_t queued_frames(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NUM_BUFS; i++)
        if ((s_hdr[i].dwFlags & WHDR_PREPARED) && !(s_hdr[i].dwFlags & WHDR_DONE))
            n += s_hdr[i].dwBufferLength / 4;
    return n;
}

// Windows treats a windowless background process as low priority work (power throttling / efficiency mode, coarse
// timers), which is exactly when the game is busiest. Opt out of both.
static void opt_out_of_throttling(void)
{
    typedef BOOL (WINAPI *SetProcessInformationFn)(HANDLE, int, LPVOID, DWORD);
    struct { ULONG version, controlMask, stateMask; } state = { 1, 0x1 /* execution speed */ | 0x4 /* timer resolution */, 0 };
    HMODULE k = GetModuleHandleA("kernel32.dll");
    SetProcessInformationFn fn = k ? (SetProcessInformationFn)GetProcAddress(k, "SetProcessInformation") : NULL;
    if (fn) fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof(state));
}

static void prioritise_audio_thread(void)
{
    typedef HANDLE (WINAPI *AvSetMmThreadCharacteristicsWFn)(LPCWSTR, LPDWORD);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMODULE av = LoadLibraryA("avrt.dll");   // MMCSS: the scheduler's own class for audio threads
    AvSetMmThreadCharacteristicsWFn fn = av ? (AvSetMmThreadCharacteristicsWFn)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
    if (fn) { DWORD idx = 0; fn(L"Pro Audio", &idx); }
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    prioritise_audio_thread();
    int next = 0, primed = 0, dry = 0;
    DWORD lastLoop = 0;
    double sum = 0;
    uint32_t count = 0, ticks = 0;
    while (s_running) {
        DWORD t = GetTickCount();
        if (lastLoop && (LONG)(t - lastLoop) > s_maxGapMs) s_maxGapMs = (LONG)(t - lastLoop);
        lastLoop = t;

        // top up to the target; an empty device while we're running is an audible gap
        for (;;) {
            uint32_t queued = queued_frames();
            if (primed && queued == 0) { if (!dry) { s_underruns++; dry = 1; } } else if (queued) dry = 0;
            if (queued >= s_targetFrames) break;
            WAVEHDR *h = &s_hdr[next];
            if ((h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE)) break;   // the device still has this one

            int16_t buf[BUF_FRAMES * 2];
            ng64_sm64_lock();
            uint32_t n = sm64_audio_tick(queued, s_targetFrames, buf);
            ng64_sm64_unlock();

            // output level, over ~5 s: shows whether anything (music, sounds) is actually being played
            for (uint32_t i = 0; i < n * 4 && i < BUF_FRAMES * 2; i++) { sum += (double)buf[i] * buf[i]; count++; }
            if (++ticks == 150) { s_level = (LONG)(count ? sqrt(sum / count) : 0); sum = 0; count = 0; ticks = 0; }

            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s_wave, h, sizeof(*h));
            uint32_t bytes = n * 2 * 4;
            if (bytes > sizeof(s_data[next])) bytes = sizeof(s_data[next]);
            memcpy(s_data[next], buf, bytes);
            memset(h, 0, sizeof(*h));
            h->lpData = (LPSTR)s_data[next];
            h->dwBufferLength = bytes;
            waveOutPrepareHeader(s_wave, h, sizeof(*h));
            waveOutWrite(s_wave, h, sizeof(*h));
            primed = 1;
            next = (next + 1) % NUM_BUFS;
        }
        WaitForSingleObject(s_event, 10);   // woken the moment the device finishes a buffer
    }
    return 0;
}

int ng64_audio_start(const uint8_t *rom)
{
    opt_out_of_throttling();
    timeBeginPeriod(1);
    int targetMs = DEFAULT_TARGET_MS;
    const char *env = getenv("NG64_AUDIO_TARGET_MS");
    if (env && atoi(env) >= 20 && atoi(env) <= 500) targetMs = atoi(env);
    s_targetFrames = (uint32_t)targetMs * SAMPLE_RATE / 1000;

    WAVEFORMATEX fmt = { 0 };
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * 4;
    s_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&s_wave, WAVE_MAPPER, &fmt, (DWORD_PTR)s_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return 0;
    sm64_audio_init(rom);
    InitializeCriticalSection(&s_lock);
    s_lockReady = 1;
    s_running = 1;
    s_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    return 1;
}

void ng64_audio_stop(void)
{
    if (!s_running) return;
    s_running = 0;
    SetEvent(s_event);
    WaitForSingleObject(s_thread, 1000);
    s_lockReady = 0;
    waveOutReset(s_wave);
    for (int i = 0; i < NUM_BUFS; i++)
        if (s_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s_wave, &s_hdr[i], sizeof(s_hdr[i]));
    waveOutClose(s_wave);
    CloseHandle(s_event);
}

void ng64_audio_stats(long *underruns, long *maxGapMs, long *level)
{
    *underruns = s_underruns;
    *maxGapMs = s_maxGapMs;
    s_maxGapMs = 0;
    *level = s_level;
}
