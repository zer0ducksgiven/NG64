// Minimal uncompressed (stored-deflate) RGBA PNG writer. Output is big but needs no zlib.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put_be32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
    fwrite(b, 1, 4, f);
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    put_be32(f, len);
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    fwrite(type, 1, 4, f);
    if (len) {
        fwrite(data, 1, len, f);
        crc = crc_update(crc, data, len);
    }
    put_be32(f, crc ^ 0xFFFFFFFFu);
}

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h)
{
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0 };
    write_chunk(f, "IHDR", ihdr, 13);

    // raw scanlines, each prefixed with filter byte 0
    size_t rowLen = (size_t)w * 4 + 1;
    size_t rawLen = rowLen * h;
    uint8_t *raw = malloc(rawLen);
    for (int y = 0; y < h; y++) {
        raw[y * rowLen] = 0;
        memcpy(raw + y * rowLen + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    // zlib stream of stored blocks (max 65535 bytes each)
    size_t nBlocks = (rawLen + 65534) / 65535;
    size_t zLen = 2 + rawLen + nBlocks * 5 + 4;
    uint8_t *z = malloc(zLen), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < rawLen; off += 65535) {
        uint16_t n = (uint16_t)((rawLen - off) > 65535 ? 65535 : (rawLen - off));
        *p++ = (off + n >= rawLen) ? 1 : 0;
        *p++ = n & 0xff; *p++ = n >> 8;
        *p++ = ~n & 0xff; *p++ = (~n >> 8) & 0xff;
        memcpy(p, raw + off, n);
        p += n;
    }
    for (size_t i = 0; i < rawLen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    *p++ = adler >> 24; *p++ = adler >> 16; *p++ = adler >> 8; *p++ = adler;

    write_chunk(f, "IDAT", z, (uint32_t)(p - z));
    write_chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    fclose(f);
    return 1;
}
