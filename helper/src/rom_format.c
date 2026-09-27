// N64 ROM dumps come in three byte orders. The helper works in .z64 order (big-endian, as the N64 reads it), so the
// others are turned round in memory when loaded, whatever the file is called.
#include <stdint.h>
#include <stddef.h>

// 1 if it's an N64 ROM (now in .z64 order), 0 if it isn't one in any of the three orders
int ng64_rom_normalize(uint8_t *rom, size_t len)
{
    if (len < 0x40 || len % 4) return 0;
    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40) return 1;   // .z64
    if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {           // .v64: 16-bit words swapped
        for (size_t i = 0; i < len; i += 2) { uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
        return 1;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {           // .n64: 32-bit words reversed
        for (size_t i = 0; i < len; i += 4) {
            uint8_t a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
        return 1;
    }
    return 0;
}
