// libsm64 audio -> waveOut. Mirrors libsm64's own test/audio.cpp loop: 32 kHz stereo s16, ticked ~30 Hz.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "libsm64.h"

#define NUM_BUFS   16
#define BUF_FRAMES (544 * 2)

static HWAVEOUT s_wave;
static WAVEHDR s_hdr[NUM_BUFS];
static int16_t s_data[NUM_BUFS][BUF_FRAMES * 2];
static volatile LONG s_running;
static HANDLE s_thread;

static uint32_t queued_frames(void)
{
    uint32_t n = 0;
    for (int i = 0; i < NUM_BUFS; i++)
        if ((s_hdr[i].dwFlags & WHDR_PREPARED) && !(s_hdr[i].dwFlags & WHDR_DONE))
            n += s_hdr[i].dwBufferLength / 4;
    return n;
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    int next = 0;
    while (s_running) {
        int16_t buf[BUF_FRAMES * 2];
        uint32_t queued = queued_frames();
        uint32_t n = sm64_audio_tick(queued, 1100, buf);
        {
            // output level, logged every ~5 s: shows whether anything (music, sounds) is actually being played
            static double sum;
            static uint32_t count, ticks;
            for (uint32_t i = 0; i < n * 4 && i < BUF_FRAMES * 2; i++) { sum += (double)buf[i] * buf[i]; count++; }
            if (++ticks == 150) {
                extern void ng64_audio_level(double rms);
                ng64_audio_level(count ? sqrt(sum / count) : 0);
                sum = 0; count = 0; ticks = 0;
            }
        }
        WAVEHDR *h = &s_hdr[next];
        if (queued < 6000 && (!(h->dwFlags & WHDR_PREPARED) || (h->dwFlags & WHDR_DONE))) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s_wave, h, sizeof(*h));
            uint32_t bytes = n * 2 * 4;
            if (bytes > sizeof(s_data[next])) bytes = sizeof(s_data[next]);
            memcpy(s_data[next], buf, bytes);
            memset(h, 0, sizeof(*h));
            h->lpData = (LPSTR)s_data[next];
            h->dwBufferLength = bytes;
            waveOutPrepareHeader(s_wave, h, sizeof(*h));
            waveOutWrite(s_wave, h, sizeof(*h));
            next = (next + 1) % NUM_BUFS;
        }
        Sleep(33);
    }
    return 0;
}

int ng64_audio_start(const uint8_t *rom)
{
    WAVEFORMATEX fmt = { 0 };
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 32000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 32000 * 4;
    if (waveOutOpen(&s_wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;
    sm64_audio_init(rom);
    s_running = 1;
    s_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    return 1;
}

void ng64_audio_stop(void)
{
    if (!s_running) return;
    s_running = 0;
    WaitForSingleObject(s_thread, 1000);
    waveOutReset(s_wave);
    for (int i = 0; i < NUM_BUFS; i++)
        if (s_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s_wave, &s_hdr[i], sizeof(s_hdr[i]));
    waveOutClose(s_wave);
}
