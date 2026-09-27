// Mario's model, part 1 of 2 (display lists, vertices, lights; see mario_rom_geo.c for the rest) - his skeleton (geo layout) and body parts (display lists, vertices, lights) - read from the
// player's own ROM at startup, the way his textures already are. libsm64 normally compiles this in from the SM64
// decompilation's actors/mario/*.inc.c; NG64 builds libsm64 without those files (patches/libsm64-rommodel.patch), so
// nothing of Nintendo's ships with the helper. Only where things are in the US ROM is written down here.
//
// The ROM holds them as the N64 does: the geo layout as big-endian geo commands (segment 0x17, uncompressed) and the
// display lists as Fast3D commands in the MIO0-compressed Mario bank (segment 0x04). They're translated into the
// forms libsm64 builds from the C sources: GeoLayout words (geo_commands.h) and its own GFXCMD display lists
// (gfx_macros.h), with segment addresses turned into pointers.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// libsm64's display-list types; these clash with the decomp's N64 headers, hence two files (as libsm64 does it)
#include "gfx_macros.h"
#include "load_tex_data.h"
#include "mario_rom.h"

uint8_t *g_marioBank;            // decompressed segment 0x04; lights are used in place (their layout is all bytes)
uint32_t g_marioBankSize;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int16_t be16(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

// ---- small address -> converted-thing maps ---------------------------------------------------------------------

typedef struct { uint32_t key; void *ptr; } Entry;
typedef struct { Entry *e; int n, cap; } Map;
static Map s_dls, s_vtx;

static void *map_get(Map *m, uint32_t key)
{
    for (int i = 0; i < m->n; i++) if (m->e[i].key == key) return m->e[i].ptr;
    return NULL;
}
static void map_put(Map *m, uint32_t key, void *ptr)
{
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 64; m->e = realloc(m->e, sizeof(Entry) * m->cap); }
    m->e[m->n].key = key; m->e[m->n].ptr = ptr; m->n++;
}

// growable word list for building one display list / geo layout
typedef struct { intptr_t *w; int n, cap; } Words;
static void emit(Words *o, intptr_t v)
{
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 64; o->w = realloc(o->w, sizeof(intptr_t) * o->cap); }
    o->w[o->n++] = v;
}

// ---- segment 0x04: vertices, lights, display lists -------------------------------------------------------------

static const uint8_t *bank_at(uint32_t segAddr, uint32_t len)
{
    uint32_t off = segAddr & 0x00FFFFFF;
    if ((segAddr >> 24) != 0x04 || off + len > g_marioBankSize) { ng64_rom_fail("address outside Mario's bank", segAddr); return NULL; }
    return g_marioBank + off;
}

static Vtx *convert_vertices(uint32_t segAddr, int count)
{
    uint32_t key = segAddr ^ ((uint32_t)count << 24);
    Vtx *v = map_get(&s_vtx, key);
    if (v) return v;
    const uint8_t *p = bank_at(segAddr, (uint32_t)count * 16);
    if (!p) return NULL;
    v = calloc(count, sizeof(Vtx));
    for (int i = 0; i < count; i++, p += 16) {
        // N64 Vtx: s16 x, y, z; u16 flag; s16 s, t; then normal (or colour) and alpha bytes
        v[i].n.ob[0] = be16(p); v[i].n.ob[1] = be16(p + 2); v[i].n.ob[2] = be16(p + 4);
        v[i].n.flag = (unsigned short)be16(p + 6);
        v[i].n.tc[0] = be16(p + 8); v[i].n.tc[1] = be16(p + 10);
        v[i].n.n[0] = (signed char)p[12]; v[i].n.n[1] = (signed char)p[13]; v[i].n.n[2] = (signed char)p[14];
        v[i].n.a = p[15];
    }
    map_put(&s_vtx, key, v);
    return v;
}

static int texture_index(uint32_t segAddr)
{
    uint32_t off = segAddr & 0x00FFFFFF;
    for (int i = 0; i < NUM_USED_TEXTURES; i++) if ((uint32_t)mario_tex_offsets[i] == off) return i;
    // textures libsm64 doesn't draw (metal wings, unused eye directions) keep the placeholder numbers its sources give them
    static const struct { uint32_t off; int index; } unused[] = {
        { 0xA090, mario_texture_metal_wings_half_1 }, { 0xB090, mario_texture_metal_wings_half_2 },
        { 0x4890, mario_texture_eyes_closed_unused1 }, { 0x5090, mario_texture_eyes_closed_unused2 },
        { 0x5890, mario_texture_eyes_right }, { 0x6090, mario_texture_eyes_left },
        { 0x6890, mario_texture_eyes_up }, { 0x7090, mario_texture_eyes_down },
    };
    for (size_t i = 0; i < sizeof(unused) / sizeof(unused[0]); i++) if (unused[i].off == off) return unused[i].index;
    ng64_rom_fail("unknown Mario texture", segAddr);
    return 0;
}

void *ng64_rom_convert_dl(uint32_t segAddr)
{
    Gfx *done = map_get(&s_dls, segAddr);
    if (done) return done;
    Words o = { 0 };
    int inLoadTextureBlock = 0;
    for (uint32_t a = segAddr;; a += 8) {
        const uint8_t *c = bank_at(a, 8);
        if (!c) return NULL;
        uint32_t w0 = be32(c), w1 = be32(c + 4);
        switch (w0 >> 24) {
        case 0x04: {   // G_VTX: n-1 in bits 20-23, v0 in 16-19
            int n = ((w0 >> 20) & 0xF) + 1, v0 = (w0 >> 16) & 0xF;
            Vtx *v = convert_vertices(w1, n);
            emit(&o, GFXCMD_VertexData); emit(&o, (intptr_t)v); emit(&o, n); emit(&o, v0);
            break;
        }
        case 0xBF:     // G_TRI1: vertex indices * 10
            emit(&o, GFXCMD_Triangle);
            emit(&o, ((w1 >> 16) & 0xFF) / 10); emit(&o, ((w1 >> 8) & 0xFF) / 10); emit(&o, (w1 & 0xFF) / 10);
            emit(&o, w1 >> 24);
            break;
        case 0x03: {   // G_MOVEMEM: light 1 (0x86) is the diffuse colour, light 2 (0x88) the ambient
            uint32_t which = (w0 >> 16) & 0xFF;
            if (which == 0x86 || which == 0x88) {
                const uint8_t *l = bank_at(w1, 8);
                emit(&o, GFXCMD_Light); emit(&o, (intptr_t)l); emit(&o, which == 0x86 ? 1 : 2);
            }
            break;
        }
        case 0xBB:     // G_TEXTURE
            emit(&o, GFXCMD_Texture); emit(&o, w1 >> 16); emit(&o, w1 & 0xFFFF); emit(&o, w0 & 0xFF);
            break;
        case 0xFD:     // G_SETTIMG
            // gsDPLoadTextureBlock expands to SETTIMG, SETTILE, ..., SETTILESIZE; libsm64 drops that macro entirely
            // (only the metal texture is loaded that way), so its image and tile size are dropped here too
            if (bank_at(a + 8, 1) && bank_at(a + 8, 1)[0] == 0xF5) { inLoadTextureBlock = 1; break; }
            emit(&o, GFXCMD_SetTextureImage); emit(&o, texture_index(w1));
            break;
        case 0xF2:     // G_SETTILESIZE
            if (inLoadTextureBlock) { inLoadTextureBlock = 0; break; }
            emit(&o, GFXCMD_SetTileSize);
            emit(&o, (w0 >> 12) & 0xFFF); emit(&o, w0 & 0xFFF); emit(&o, (w1 >> 12) & 0xFFF); emit(&o, w1 & 0xFFF);
            break;
        case 0x06: {   // G_DL: call, or branch (no return)
            Gfx *sub = ng64_rom_convert_dl(w1);
            emit(&o, GFXCMD_SubDisplayList); emit(&o, (intptr_t)sub);
            if (((w0 >> 16) & 0xFF) == 1) { emit(&o, GFXCMD_EndDisplayList); goto end; }
            break;
        }
        case 0xB8:     // G_ENDDL
            emit(&o, GFXCMD_EndDisplayList);
            goto end;
        default:       // render modes, tiles, syncs, combiner: libsm64 has no use for them either
            break;
        }
        if (g_marioRomFailed) return NULL;
    }
end:
    map_put(&s_dls, segAddr, o.w);
    return (Gfx *)o.w;
}


// tests: the converted display list for a segment address (NULL if never converted)
void *ng64_mario_rom_dl(uint32_t segAddr) { return map_get(&s_dls, segAddr); }
