// SM64's HUD graphics (digits, x, coin, Mario head, star, camera icons, power meter), taken from the player's own ROM
// at runtime - like Mario's textures and the audio - and written as PNGs next to Mario's atlas for the mod's HUD app.
// Nothing from the ROM is part of NG64 itself; only where things are in the US ROM.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "decomp/tools/libmio0.h"
#include "decomp/tools/n64graphics.h"

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);

// US ROM: two MIO0 banks. HUD font / icons are 16x16 RGBA16 tiles, 0x200 bytes each, from the start of the first;
// the power meter is two 32x64 halves and eight 32x32 pies (full .. one segment) in the second.
#define HUD_BANK_ROM   0x108A40
#define METER_BANK_ROM 0x201410
#define METER_LEFT     0x233E0
#define METER_RIGHT    0x243E0
#define METER_PIES     0x253E0   // 8 pies, 0x800 apart: 8 segments first, 1 segment last

typedef struct { const char *name; int tile; } Tile;
static const Tile s_tiles[] = {
    { "digit_0", 0 }, { "digit_1", 1 }, { "digit_2", 2 }, { "digit_3", 3 }, { "digit_4", 4 },
    { "digit_5", 5 }, { "digit_6", 6 }, { "digit_7", 7 }, { "digit_8", 8 }, { "digit_9", 9 },
    { "times", 33 }, { "coin", 34 }, { "mario", 35 }, { "star", 36 }, { "camera", 56 }, { "lakitu", 57 },
};

typedef struct { char name[24]; int w, h; uint8_t *rgba; } Image;
static Image s_images[40];
static int s_numImages;

static void add_image(const char *name, const uint8_t *raw, int w, int h)
{
    rgba *px = raw2rgba(raw, w, h, 16);
    Image *im = &s_images[s_numImages++];
    snprintf(im->name, sizeof(im->name), "%s", name);
    im->w = w; im->h = h;
    im->rgba = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        im->rgba[i * 4] = px[i].red; im->rgba[i * 4 + 1] = px[i].green;
        im->rgba[i * 4 + 2] = px[i].blue; im->rgba[i * 4 + 3] = px[i].alpha;
    }
    free(px);
}

static uint8_t *unpack(const uint8_t *rom, size_t romLen, size_t off, uint32_t *size)
{
    mio0_header_t head;
    if (off + 16 > romLen || memcmp(rom + off, "MIO0", 4) || mio0_decode_header(rom + off, &head) != 1) return NULL;
    uint8_t *out = malloc(head.dest_size);
    mio0_decode(rom + off, out, NULL);
    *size = head.dest_size;
    return out;
}

// decode everything once; 0 if the ROM doesn't have the banks where a US ROM does
int ng64_hud_extract(const uint8_t *rom, size_t romLen)
{
    uint32_t hudSize, meterSize;
    uint8_t *hud = unpack(rom, romLen, HUD_BANK_ROM, &hudSize);
    uint8_t *meter = unpack(rom, romLen, METER_BANK_ROM, &meterSize);
    int ok = hud && meter && hudSize >= 58 * 0x200 && meterSize >= METER_PIES + 8 * 0x800;
    if (ok) {
        for (size_t i = 0; i < sizeof(s_tiles) / sizeof(s_tiles[0]); i++)
            add_image(s_tiles[i].name, hud + s_tiles[i].tile * 0x200, 16, 16);
        add_image("meter_left", meter + METER_LEFT, 32, 64);
        add_image("meter_right", meter + METER_RIGHT, 32, 64);
        for (int k = 0; k < 8; k++) {
            char name[16];
            snprintf(name, sizeof(name), "pie_%d", 8 - k);
            add_image(name, meter + METER_PIES + k * 0x800, 32, 32);
        }
    }
    free(hud);
    free(meter);
    return ok;
}

// <userPath>\ng64_cache\hud\<name>.png, written once per user folder (the files never change for a given ROM)
int ng64_hud_write(const char *userPath)
{
    static char written[MAX_PATH];
    if (!s_numImages || !userPath[0] || !strcmp(written, userPath)) return s_numImages > 0;
    char dir[MAX_PATH], path[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\ng64_cache", userPath);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof(dir), "%s\\ng64_cache\\hud", userPath);
    CreateDirectoryA(dir, NULL);
    for (int i = 0; i < s_numImages; i++) {
        snprintf(path, sizeof(path), "%s\\%s.png", dir, s_images[i].name);
        if (!png_write_rgba(path, s_images[i].rgba, s_images[i].w, s_images[i].h)) return 0;
    }
    snprintf(written, sizeof(written), "%s", userPath);
    return 1;
}
