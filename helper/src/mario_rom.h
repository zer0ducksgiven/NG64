// Shared between mario_rom_geo.c and mario_rom_dl.c (Mario's model read from the ROM)
#pragma once
#include <stdint.h>

extern uint8_t *g_marioBank;          // decompressed Mario bank (segment 0x04)
extern uint32_t g_marioBankSize;
extern int g_marioRomFailed;

void ng64_rom_fail(const char *what, uint32_t addr);
void *ng64_rom_convert_dl(uint32_t segAddr);   // -> libsm64 GFXCMD display list, NULL on failure
