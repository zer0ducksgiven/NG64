// Mario's model, part 2 of 2 (the skeleton, and loading; display lists are in mario_rom_dl.c) - his skeleton (geo layout) and body parts (display lists, vertices, lights) - read from the
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

#include "decomp/include/types.h"
#include "decomp/include/sm64.h"
#include "decomp/include/geo_commands.h"
#include "decomp/game/rendering_graph_node.h"
#include "decomp/shim.h"
#include "decomp/game/object_stuff.h"
#include "decomp/game/behavior_actions.h"
#include "decomp/game/mario_misc.h"
#include "decomp/tools/libmio0.h"
#include "mario_rom.h"

#define MARIO_BANK_ROM    0x114750   // MIO0: segment 0x04 (Mario's textures, vertices, lights, display lists)
#define MARIO_GEO_ROM     0x1279B0   // segment 0x17 (group 0 geo layouts), uncompressed
#define MARIO_GEO_SIZE    0x2E40
#define GEO_LOAD_BODY     0x17002CE0 // mario_geo_load_body: what libsm64's own root layout branches to

// libsm64's root layout pointer (defined in libsm64.c by patches/libsm64-rommodel.patch)
extern void *mario_geo_ptr;

// N64 addresses of the geo layout's callbacks -> libsm64's versions of them
static const struct { uint32_t addr; void *fn; } s_funcs[] = {
    { 0x802770A4, (void *)geo_mirror_mario_set_alpha },
    { 0x80277150, (void *)geo_switch_mario_stand_run },
    { 0x802771BC, (void *)geo_switch_mario_eyes },
    { 0x80277294, (void *)geo_mario_tilt_torso },
    { 0x802773A4, (void *)geo_mario_head_rotation },
    { 0x802774F4, (void *)geo_switch_mario_hand },
    { 0x802775CC, (void *)geo_mario_hand_foot_scaler },
    { 0x802776D8, (void *)geo_switch_mario_cap_effect },
    { 0x80277740, (void *)geo_switch_mario_cap_on_off },
    { 0x80277824, (void *)geo_mario_rotate_wing_cap_wings },
    { 0x8027795C, (void *)geo_switch_mario_hand_grab_pos },
    { 0x80277D6C, (void *)geo_mirror_mario_backface_culling },
    { 0x802B1BB0, (void *)geo_move_mario_part_from_parent },
};

static const uint8_t *s_rom;
int g_marioRomFailed;
static char s_error[160];

void ng64_rom_fail(const char *what, uint32_t addr)
{
    if (!g_marioRomFailed) snprintf(s_error, sizeof(s_error), "%s at %08X", what, addr);
    g_marioRomFailed = 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// ---- small address -> converted-thing maps ---------------------------------------------------------------------

typedef struct { uint32_t key; void *ptr; } Entry;
typedef struct { Entry *e; int n, cap; } Map;
static Map s_geos;

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

// ---- segment 0x17: geo layout -----------------------------------------------------------------------------------

static intptr_t convert_pointer(uint32_t p, uint32_t at);
static GeoLayout *convert_geo(uint32_t segAddr);

// how long each geo command is in the ROM, in bytes
static int geo_len(const uint8_t *b)
{
    int dl = (b[1] & 0x80) ? 4 : 0;
    switch (b[0]) {
    case 0x01: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0B: case 0x0C:
    case 0x17: case 0x1B: case 0x20: return 4;
    case 0x00: case 0x02: case 0x0D: case 0x0E: case 0x15: case 0x16: case 0x18: case 0x19: case 0x1A: case 0x1E:
        return 8;
    case 0x08: case 0x13: case 0x1C: return 12;
    case 0x0A: return b[1] ? 12 : 8;
    case 0x0F: return 20;
    case 0x1F: return 16;
    case 0x10: { static const int base[4] = { 16, 8, 8, 4 }; return base[(b[1] >> 4) & 3] + dl; }
    case 0x11: case 0x12: case 0x14: case 0x1D: return 8 + dl;
    }
    return 0;
}

static uint32_t geo_rom(uint32_t segAddr)
{
    uint32_t off = segAddr & 0x00FFFFFF;
    if ((segAddr >> 24) != 0x17 || off >= MARIO_GEO_SIZE) { ng64_rom_fail("geo address outside Mario's geo", segAddr); return 0; }
    return MARIO_GEO_ROM + off;
}

static intptr_t convert_pointer(uint32_t p, uint32_t at)
{
    if (!p) return 0;
    switch (p >> 24) {
    case 0x04: return (intptr_t)ng64_rom_convert_dl(p);
    case 0x17: return (intptr_t)convert_geo(p);
    case 0x80:
        for (size_t i = 0; i < sizeof(s_funcs) / sizeof(s_funcs[0]); i++)
            if (s_funcs[i].addr == p) return (intptr_t)s_funcs[i].fn;
        break;
    }
    ng64_rom_fail("unknown pointer in Mario's geo", at);
    return 0;
}

static GeoLayout *convert_geo(uint32_t segAddr)
{
    GeoLayout *done = map_get(&s_geos, segAddr);
    if (done) return done;
    Words o = { 0 };
    uint32_t r = geo_rom(segAddr);
    if (!r) return NULL;
    for (;;) {
        const uint8_t *b = s_rom + r;
        int len = geo_len(b);
        if (!len) { ng64_rom_fail("unknown geo command", r); return NULL; }
        uint8_t cmd = b[0];
        // first word of every command: CMD_BBH(command, byte, halfword)
        emit(&o, (intptr_t)CMD_BBH(b[0], b[1], (uint16_t)(b[2] << 8 | b[3])));
        // the rest: halfword pairs, a plain word (scale) or pointers, depending on the command
        for (int k = 4; k < len; k += 4) {
            const uint8_t *w = b + k;
            int isPtr = 0;
            switch (cmd) {
            case 0x00: case 0x02: case 0x0E: case 0x15: case 0x18: case 0x19: isPtr = 1; break;
            case 0x0A: isPtr = k == 8; break;
            case 0x0F: isPtr = k == 16; break;
            case 0x13: case 0x1C: isPtr = k == 8; break;
            case 0x10: case 0x11: case 0x12: case 0x14: case 0x1D: isPtr = (b[1] & 0x80) && k == len - 4; break;
            }
            if (isPtr) emit(&o, convert_pointer(be32(w), r + k));
            else if (cmd == 0x1D) emit(&o, (intptr_t)CMD_W(be32(w)));
            else emit(&o, (intptr_t)CMD_HH((uint16_t)(w[0] << 8 | w[1]), (uint16_t)(w[2] << 8 | w[3])));
            if (g_marioRomFailed) return NULL;
        }
        if (cmd == 0x01 || cmd == 0x03) break;   // end / return
        r += len;
    }
    map_put(&s_geos, segAddr, o.w);
    return (GeoLayout *)o.w;
}

// ---- entry point ---------------------------------------------------------------------------------------------------

#define SHADOW_CIRCLE_PLAYER 99

// libsm64's own root layout (from its geo.inc.c, CC0), around the body loaded from the ROM
static GeoLayout s_root[] = {
    GEO_SHADOW(SHADOW_CIRCLE_PLAYER, 0xB4, 100),
    GEO_OPEN_NODE(),
       GEO_ZBUFFER(1),
       GEO_OPEN_NODE(),
          GEO_SCALE(0x00, 16384),
          GEO_OPEN_NODE(),
             GEO_ASM(0, geo_mirror_mario_backface_culling),
             GEO_ASM(0, geo_mirror_mario_set_alpha),
             GEO_BRANCH(1, 0),                  // -> mario_geo_load_body, filled in at load
             GEO_ASM(1, geo_mirror_mario_backface_culling),
          GEO_CLOSE_NODE(),
       GEO_CLOSE_NODE(),
    GEO_CLOSE_NODE(),
    GEO_END(),
};
#define ROOT_BRANCH_TARGET 13   // index of GEO_BRANCH's pointer word in s_root

// 1 on success; otherwise 0 and a reason in ng64_mario_rom_error(). Call before sm64_mario_create.
int ng64_load_mario_from_rom(const uint8_t *rom, size_t romLen)
{
    s_rom = rom;
    g_marioRomFailed = 0;
    if (romLen < MARIO_GEO_ROM + MARIO_GEO_SIZE || memcmp(rom + MARIO_BANK_ROM, "MIO0", 4)) {
        snprintf(s_error, sizeof(s_error), "Mario's model isn't where a US ROM has it");
        return 0;
    }
    mio0_header_t head;
    if (mio0_decode_header(rom + MARIO_BANK_ROM, &head) != 1) { snprintf(s_error, sizeof(s_error), "bad Mario bank"); return 0; }
    g_marioBank = malloc(head.dest_size);
    g_marioBankSize = head.dest_size;
    mio0_decode(rom + MARIO_BANK_ROM, g_marioBank, NULL);

    GeoLayout *body = convert_geo(GEO_LOAD_BODY);
    if (g_marioRomFailed || !body) return 0;
    if (s_root[ROOT_BRANCH_TARGET - 1] != (GeoLayout)CMD_BBH(0x02, 1, 0)) {
        snprintf(s_error, sizeof(s_error), "root layout index is wrong");
        return 0;
    }
    s_root[ROOT_BRANCH_TARGET] = (GeoLayout)body;
    mario_geo_ptr = s_root;
    return 1;
}

const char *ng64_mario_rom_error(void) { return s_error; }

// tests: the converted geo layout for a segment address (NULL if never converted)
void *ng64_mario_rom_geo(uint32_t segAddr) { return map_get(&s_geos, segAddr); }
