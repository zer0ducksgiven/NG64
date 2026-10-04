// NG64 helper: runs libsm64 for the BeamNG mod and talks to it over localhost UDP (see protocol.h).
//
// Usage: ng64helper.exe [--rom <path>] [--no-audio] [--port <n>]
// The ROM is only ever read from the user's own disk; nothing derived from it is written except the
// Mario texture atlas, which goes into the user's BeamNG user folder (never into the mod).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libsm64.h"
#include "pad.h"
#include "protocol.h"

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
int ng64_audio_start(const uint8_t *rom);
int ng64_hud_extract(const uint8_t *rom, size_t romLen);
int ng64_load_mario_from_rom(const uint8_t *rom, size_t romLen);
int ng64_rom_normalize(uint8_t *rom, size_t len);
int ng64_single_instance(int port);
void ng64_attach_game(DWORD pid);
int ng64_game_closed(void);
DWORD ng64_game_pid(void);
int ng64_run_watcher(const char *exePath, const char *exeDir);
const char *ng64_mario_rom_error(void);
int ng64_hud_write(const char *userPath);
static int s_hudOk;
void ng64_audio_stop(void);
void ng64_audio_stats(long *underruns, long *maxGapMs, long *level);
uint8_t *ng64_preview_render(const struct SM64MarioGeometryBuffers *geo, const uint8_t *tex, int w, int h);
int ng64_preview_write(const char *userPath, const uint8_t *img, int w, int h);
int ng64_tray_start(void);
void ng64_tray_pump(void);
void ng64_tray_stop(void);
void ng64_sm64_lock(void);
void ng64_sm64_unlock(void);
// libsm64's audio runs on its own thread (audio.c) and shares state with the calls that start sounds and music: those
// run under this lock, one at a time with the audio tick
#define SM64_AUDIO_SAFE(call) do { ng64_sm64_lock(); call; ng64_sm64_unlock(); } while (0)

extern uint32_t s_tick;   // simulation tick, defined with the frame sender

#define PI 3.14159265f
#define S  NG64_SCALE

// sm64 action/flag bits we care about (decomp include/sm64.h)
#define MARIO_PUNCHING        0x00100000
#define MARIO_KICKING         0x00200000
#define MARIO_TRIPPING        0x00400000
#define ACT_GROUND_POUND      0x008008A9
#define ACT_GROUND_POUND_LAND 0x0080023C
#define ACT_FLAG_AIR          0x00000800
#define ACT_SLIDE_KICK        0x018008AA
#define ACT_SLIDE_KICK_SLIDE  0x0080045A
#define ACT_DIVE              0x0188088A
#define ACT_DIVE_SLIDE        0x00880456
#define ACT_THROWN_BACKWARD   0x010208BE
#define ACT_FREEFALL          0x0100088C

// ---------------------------------------------------------------------------------------------------------------
// coordinates: sm64 is Y-up, BeamNG is Z-up. sm = (x, z, -y) / S
static void bng2sm(const float *b, float *s) { s[0] = b[0] / S; s[1] = b[2] / S; s[2] = -b[1] / S; }
static void sm2bng(const float *s, float *b) { b[0] = s[0] * S; b[1] = -s[2] * S; b[2] = s[1] * S; }

static FILE *s_log;
static SOCKET s_sock = INVALID_SOCKET;
static struct sockaddr_in s_client;
static int s_haveClient;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (s_log) { fprintf(s_log, "%s\n", buf); fflush(s_log); }
}

static void debug_print(const char *msg) { logf_("[libsm64] %s", msg); }

static void send_raw(const void *data, int len)
{
    if (!s_haveClient) return;
    sendto(s_sock, (const char *)data, len, 0, (struct sockaddr *)&s_client, sizeof(s_client));
}

static void send_log(const char *msg)
{
    char buf[512];
    buf[0] = MSG_LOG;
    snprintf(buf + 1, sizeof(buf) - 1, "%s", msg);
    send_raw(buf, (int)strlen(buf + 1) + 2);
}

// ---------------------------------------------------------------------------------------------------------------
// texture atlas: libsm64's texture is decals over per-part vertex colours (mix(color, tex, tex.a)). BeamNG's
// ProceduralMesh has no vertex colours, so bake one band per distinct part colour and remap uvs into it.
#define ATLAS_W     1024
#define ATLAS_H     1024
#define BAND_H      64
#define MAX_BANDS   (ATLAS_H / BAND_H)
#define SOLID_X0    (SM64_TEXTURE_WIDTH + 8)   // solid-colour patch right of the decals

static uint8_t *s_marioTex;       // SM64_TEXTURE_WIDTH x SM64_TEXTURE_HEIGHT RGBA
static float s_bandColor[MAX_BANDS][3];
static int s_numBands;
static int s_atlasDirty;
static char s_atlasFsPath[MAX_PATH];     // real path on disk
static char s_atlasGamePath[MAX_PATH];   // BeamNG virtual path
static char s_userPath[MAX_PATH];
static char s_exePath[MAX_PATH], s_romPathUsed[MAX_PATH];
static void ensure_preview(const char *userPath);
static void di_log(const char *msg) { logf_("%s", msg); }

static int band_for_color(const float *c)
{
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i < s_numBands; i++) {
        float dr = s_bandColor[i][0] - c[0], dg = s_bandColor[i][1] - c[1], db = s_bandColor[i][2] - c[2];
        float d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (bestD < 0.0004f || s_numBands >= MAX_BANDS) return best;
    memcpy(s_bandColor[s_numBands], c, sizeof(float) * 3);
    s_atlasDirty = 1;
    return s_numBands++;
}

static void write_atlas(void)
{
    if (!s_userPath[0]) return;
    uint8_t *img = calloc(ATLAS_W * ATLAS_H, 4);
    for (int b = 0; b < s_numBands; b++) {
        uint8_t cr = (uint8_t)(s_bandColor[b][0] * 255), cg = (uint8_t)(s_bandColor[b][1] * 255), cb = (uint8_t)(s_bandColor[b][2] * 255);
        for (int y = 0; y < BAND_H; y++) {
            uint8_t *row = img + ((size_t)(b * BAND_H + y) * ATLAS_W) * 4;
            for (int x = 0; x < ATLAS_W; x++) {
                uint8_t *o = row + x * 4;
                if (x < SM64_TEXTURE_WIDTH) {
                    const uint8_t *t = s_marioTex + ((size_t)y * SM64_TEXTURE_WIDTH + x) * 4;
                    float a = t[3] / 255.0f;
                    o[0] = (uint8_t)(cr + (t[0] - cr) * a);
                    o[1] = (uint8_t)(cg + (t[1] - cg) * a);
                    o[2] = (uint8_t)(cb + (t[2] - cb) * a);
                } else {
                    o[0] = cr; o[1] = cg; o[2] = cb;
                }
                o[3] = 255;
            }
        }
    }
    // versioned file name so BeamNG's texture cache never serves a stale atlas
    static int version;
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\ng64_cache", s_userPath);
    CreateDirectoryA(dir, NULL);
    version++;
    snprintf(s_atlasFsPath, sizeof(s_atlasFsPath), "%s\\mario_atlas_%d_%lu.png", dir, version, (unsigned long)GetCurrentProcessId());
    snprintf(s_atlasGamePath, sizeof(s_atlasGamePath), "/ng64_cache/mario_atlas_%d_%lu.png", version, (unsigned long)GetCurrentProcessId());
    png_write_rgba(s_atlasFsPath, img, ATLAS_W, ATLAS_H);
    free(img);
    s_atlasDirty = 0;
    logf_("atlas written: %s (%d colour bands)", s_atlasFsPath, s_numBands);
}

// ---------------------------------------------------------------------------------------------------------------
// Marios
#define MAX_MARIOS 16

typedef struct {
    int used;
    uint32_t key;          // 0 = local
    int32_t id;
    struct SM64MarioState state;
    struct SM64MarioGeometryBuffers geo;
    // remote target state
    float rPos[3];         // sm64
    float rFace;
    uint32_t rAction;
    int16_t rAnim, rFrame;
    DWORD lastSeen;
    // last sent geometry per body part, kept as the ready-to-send MSG_PART packet (answers MSG_PART_REQ)
    int partCount;
    uint32_t partHash[NG64_MAX_PARTS];
    uint8_t *partPkt[NG64_MAX_PARTS];
    int partLen[NG64_MAX_PARTS];
} Mario;

static Mario s_marios[MAX_MARIOS];

static Mario *mario_find(uint32_t key)
{
    for (int i = 0; i < MAX_MARIOS; i++) if (s_marios[i].used && s_marios[i].key == key) return &s_marios[i];
    return NULL;
}

static Mario *mario_create(uint32_t key, const float *smPos)
{
    for (int i = 0; i < MAX_MARIOS; i++) {
        Mario *m = &s_marios[i];
        if (m->used) continue;
        int32_t id = sm64_mario_create(smPos[0], smPos[1], smPos[2]);
        if (id < 0) return NULL;
        memset(m, 0, sizeof(*m));
        m->used = 1;
        m->key = key;
        m->id = id;
        m->geo.position = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        m->geo.normal = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        m->geo.color = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        m->geo.uv = malloc(sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES);
        memcpy(m->rPos, smPos, sizeof(float) * 3);
        m->lastSeen = GetTickCount();
        return m;
    }
    return NULL;
}

static void mario_delete(Mario *m)
{
    if (!m->used) return;
    sm64_mario_delete(m->id);
    free(m->geo.position); free(m->geo.normal); free(m->geo.color); free(m->geo.uv);
    for (int p = 0; p < NG64_MAX_PARTS; p++) free(m->partPkt[p]);
    m->used = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// collision: terrain heightfield -> static surfaces; vehicles -> moving box surface objects

static struct SM64Surface *s_surfBuf;
static int s_surfCount, s_surfCap;

static void surf_push(const float *a, const float *b, const float *c, const float *hint)
{
    // sm64 normal = (b-a) x (c-b); flip winding if it faces away from hint
    float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float v[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
    float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-3f) return;
    if (s_surfCount >= s_surfCap) {
        s_surfCap = s_surfCap ? s_surfCap * 2 : 4096;
        s_surfBuf = realloc(s_surfBuf, sizeof(struct SM64Surface) * s_surfCap);
    }
    int flip = (n[0] * hint[0] + n[1] * hint[1] + n[2] * hint[2]) < 0;
    struct SM64Surface *s = &s_surfBuf[s_surfCount++];
    memset(s, 0, sizeof(*s));
    s->type = 0;      // SURFACE_DEFAULT
    s->terrain = 0;   // TERRAIN_GRASS
    const float *vs[3] = { a, flip ? c : b, flip ? b : c };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) s->vertices[i][k] = (int32_t)lroundf(vs[i][k]);
}

// Height jump (m) between neighbouring terrain samples above which the ground is a ledge (flat tiles + a wall)
// rather than a slope. Anything smaller is triangulated smoothly and Mario walks/steps over it.
#define STEP_M 0.35f

// Surfaces for a grid of heights in bng-style coords (x, y horizontal, z up), sample (i, j) at (ox + i*sp, oy + j*sp).
// Gentle 2x2 groups become smooth triangles. Where neighbours differ by more than `step`, every affected sample
// gets a flat tile of its own height (half a sample each way) and a vertical wall sits exactly on the tile edge
// between them, so a ledge's top reaches right up to its wall. With `outer`, the grid is a closed solid: samples
// on the edge get tiles too and walls run from `bottom` up to them (vehicle hulls). Everything goes through
// bng2sm, so pass world coords for terrain and vehicle-frame coords for hulls.
static void heightfield_surfaces(const float *h, int nx, int ny, float ox, float oy, float sp, float step, int outer, float bottom)
{
    float up[3] = { 0, 1, 0 };
#define H(i, j) (((i) < 0 || (j) < 0 || (i) >= nx || (j) >= ny) ? NAN : h[(j) * nx + (i)])
#define X(i) (ox + (i) * sp)
#define Y(j) (oy + (j) * sp)
#define BIG(a, b) (!isnan(a) && !isnan(b) && fabsf((a) - (b)) > step)
    uint8_t *needTile = calloc((size_t)nx * ny, 1);

    for (int j = 0; j < ny - 1; j++) {
        for (int i = 0; i < nx - 1; i++) {
            float a = H(i, j), b = H(i + 1, j), c = H(i + 1, j + 1), d = H(i, j + 1);
            if (!(BIG(a, b) || BIG(b, c) || BIG(c, d) || BIG(d, a))) {
                float pa[3] = { X(i), Y(j), a }, pb[3] = { X(i + 1), Y(j), b }, pc[3] = { X(i + 1), Y(j + 1), c }, pd[3] = { X(i), Y(j + 1), d };
                float sa[3], sb[3], sc[3], sd[3];
                bng2sm(pa, sa); bng2sm(pb, sb); bng2sm(pc, sc); bng2sm(pd, sd);
                if (!isnan(a) && !isnan(b) && !isnan(c)) surf_push(sa, sb, sc, up);
                if (!isnan(a) && !isnan(c) && !isnan(d)) surf_push(sa, sc, sd, up);
                // a group with a missing corner still needs its valid corners covered
                if (isnan(a) || isnan(b) || isnan(c) || isnan(d))
                    needTile[j * nx + i] = needTile[j * nx + i + 1] = needTile[(j + 1) * nx + i] = needTile[(j + 1) * nx + i + 1] = 1;
            } else {
                needTile[j * nx + i] = needTile[j * nx + i + 1] = needTile[(j + 1) * nx + i] = needTile[(j + 1) * nx + i + 1] = 1;
            }
        }
    }
    static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    if (outer || nx == 1 || ny == 1)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                for (int d = 0; d < 4; d++)
                    if (isnan(H(i + dirs[d][0], j + dirs[d][1]))) needTile[j * nx + i] = 1;

    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float z = H(i, j);
            if (!needTile[j * nx + i] || isnan(z)) continue;
            float x0 = X(i) - sp * 0.5f, x1 = X(i) + sp * 0.5f, y0 = Y(j) - sp * 0.5f, y1 = Y(j) + sp * 0.5f;
            float q0[3] = { x0, y0, z }, q1[3] = { x1, y0, z }, q2[3] = { x1, y1, z }, q3[3] = { x0, y1, z };
            float s0[3], s1[3], s2[3], s3[3];
            bng2sm(q0, s0); bng2sm(q1, s1); bng2sm(q2, s2); bng2sm(q3, s3);
            surf_push(s0, s1, s2, up);
            surf_push(s0, s2, s3, up);
        }
    }

    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float h1 = H(i, j);
            if (isnan(h1)) continue;
            for (int d = 0; d < 4; d++) {
                int i2 = i + dirs[d][0], j2 = j + dirs[d][1];
                float h2 = H(i2, j2), lo;
                if (isnan(h2)) {
                    if (!outer) continue;
                    lo = bottom;                       // outside of a solid: wall down to the underside
                } else {
                    if (!BIG(h1, h2) || h2 > h1) continue;   // each step once, from its high side
                    lo = h2;
                }
                if (h1 - lo < 0.02f) continue;
                // wall on the shared tile edge, facing the low / outside neighbour
                float mx = X(i) + dirs[d][0] * sp * 0.5f, my = Y(j) + dirs[d][1] * sp * 0.5f;
                float ex = dirs[d][1] ? sp * 0.5f : 0, ey = dirs[d][0] ? sp * 0.5f : 0;
                float b0[3] = { mx - ex, my - ey, lo }, b1[3] = { mx + ex, my + ey, lo };
                float b2[3] = { mx + ex, my + ey, h1 }, b3[3] = { mx - ex, my - ey, h1 };
                float s0[3], s1[3], s2[3], s3[3], hb[3] = { (float)dirs[d][0], (float)dirs[d][1], 0 }, hint[3];
                bng2sm(b0, s0); bng2sm(b1, s1); bng2sm(b2, s2); bng2sm(b3, s3);
                bng2sm(hb, hint);
                surf_push(s0, s1, s2, hint);
                surf_push(s0, s2, s3, hint);
            }
        }
    }
    free(needTile);
#undef H
#undef X
#undef Y
#undef BIG
}

// The static world is two parts: terrain (a heightfield, sampled) and the map's objects (their real collision
// triangles, streamed in by the mod). Either one changing reloads both.
static struct SM64Surface *s_terrainSurf, *s_meshSurf, *s_levelSurf;
static int s_terrainCount, s_meshCount, s_levelCount;

static void reload_static(void)
{
    int total = s_terrainCount + s_meshCount + s_levelCount;
    struct SM64Surface *all = malloc(sizeof(struct SM64Surface) * (total ? total : 1));
    if (s_terrainCount) memcpy(all, s_terrainSurf, sizeof(struct SM64Surface) * s_terrainCount);
    if (s_meshCount) memcpy(all + s_terrainCount, s_meshSurf, sizeof(struct SM64Surface) * s_meshCount);
    if (s_levelCount) memcpy(all + s_terrainCount + s_meshCount, s_levelSurf, sizeof(struct SM64Surface) * s_levelCount);
    sm64_static_surfaces_load(all, total);
    free(all);
}

// MSG_SURFACES: a level built from SM64 (sm64_surfaces.json) sends its original surfaces, types and all, so Mario
// slides on the slippery roof and gets pushed by currents. They stay until the next set (or an empty one) arrives.
static struct SM64Surface *s_levelPend;
static int s_levelPendCount, s_levelPendCap;

static void load_level_surfaces(const uint8_t *p, int len)
{
    if (len < 6) return;
    uint16_t chunk, chunks, count;
    memcpy(&chunk, p, 2); memcpy(&chunks, p + 2, 2); memcpy(&count, p + 4, 2);
    if (chunks == 0) {
        free(s_levelSurf); s_levelSurf = NULL; s_levelCount = 0;
        reload_static();
        return;
    }
    if (len < 6 + (int)count * 40) return;
    if (chunk == 0) s_levelPendCount = 0;
    if (s_levelPendCount + count > s_levelPendCap) {
        s_levelPendCap = (s_levelPendCount + count) * 2;
        s_levelPend = realloc(s_levelPend, sizeof(struct SM64Surface) * s_levelPendCap);
    }
    for (int i = 0; i < count; i++) {
        const uint8_t *t = p + 6 + i * 40;
        struct SM64Surface *s = &s_levelPend[s_levelPendCount++];
        memset(s, 0, sizeof(*s));
        uint16_t type; int16_t force;
        memcpy(&type, t, 2); memcpy(&force, t + 2, 2);
        s->type = (int16_t)type;
        s->force = force;
        float v[9], sv[3];
        memcpy(v, t + 4, 36);
        for (int k = 0; k < 3; k++) {       // the original winding: bng2sm is a rotation, so it keeps facing
            bng2sm(v + k * 3, sv);
            for (int c = 0; c < 3; c++) s->vertices[k][c] = (int32_t)lroundf(sv[c]);
        }
    }
    if (chunk + 1 == chunks) {
        free(s_levelSurf);
        s_levelSurf = malloc(sizeof(struct SM64Surface) * (s_levelPendCount ? s_levelPendCount : 1));
        memcpy(s_levelSurf, s_levelPend, sizeof(struct SM64Surface) * s_levelPendCount);
        s_levelCount = s_levelPendCount;
        reload_static();
        logf_("level surfaces: %d with their SM64 types", s_levelCount);
    }
}

// MSG_WATER: the level's water boxes; Mario swims below the top of the box he's in
#define MAX_WATER 32
static float s_water[MAX_WATER][5];   // x0, y0, x1, y1, z (bng)
static int s_waterCount;

static void load_water(const uint8_t *p, int len)
{
    if (len < 1) return;
    int n = p[0];
    if (n > MAX_WATER) n = MAX_WATER;
    if (len < 1 + n * 20) return;
    memcpy(s_water, p + 1, (size_t)n * 20);
    s_waterCount = n;
    logf_("water boxes: %d", n);
}

// water level (sm64 units) at a position in sm64 units; SM64's "no water" below everything otherwise
static int water_level_at(const float *sp)
{
    float bx = sp[0] * S, by = -sp[2] * S;
    float best = -10000.0f;
    for (int i = 0; i < s_waterCount; i++) {
        const float *w = s_water[i];
        if (bx >= w[0] && bx <= w[2] && by >= w[1] && by <= w[3] && w[4] / S > best) best = w[4] / S;
    }
    return (int)lroundf(best);
}

static void load_terrain(const uint8_t *p, int len)
{
    if (len < 14) return;
    float cx, cy, sp;
    uint16_t n;
    memcpy(&cx, p, 4); memcpy(&cy, p + 4, 4); memcpy(&sp, p + 8, 4); memcpy(&n, p + 12, 2);
    if (n < 2 || len < 14 + (int)n * n * 4) return;
    float half = (n - 1) * sp * 0.5f;
    s_surfCount = 0;
    heightfield_surfaces((const float *)(p + 14), n, n, cx - half, cy - half, sp, STEP_M, 0, 0);
    free(s_terrainSurf);
    s_terrainSurf = malloc(sizeof(struct SM64Surface) * (s_surfCount ? s_surfCount : 1));
    memcpy(s_terrainSurf, s_surfBuf, sizeof(struct SM64Surface) * s_surfCount);
    s_terrainCount = s_surfCount;
    reload_static();
}

// MSG_MESH: u32 cell; u16 chunk; u16 chunks; u16 tris; f32 tris[tris * 9] (bng world). The map's objects arrive as
// 16 m cells around Mario, each sent once while he's near (MSG_MESH_DROP removes one). A cell's chunks are
// collected and swapped in together, so Mario never stands on half a cell. Rebuilding sm64's static collision is
// deferred to the tick loop and done at most every MESH_REBUILD_MS, however many cells change in between.
#define MAX_CELLS 256
#define MESH_REBUILD_MS 500   // new cells are at the edge of what Mario can reach: no hurry
typedef struct { uint32_t id; struct SM64Surface *surf; int count; } MeshCell;
static MeshCell s_cells[MAX_CELLS];
static int s_numCells;
static int s_meshDirty;
static DWORD s_meshBuiltAt;
static struct { uint32_t cell; int chunks, got; float *tris; int n, cap; uint8_t *seen; } s_meshIn;

static void cell_remove(uint32_t id)
{
    for (int i = 0; i < s_numCells; i++) {
        if (s_cells[i].id != id) continue;
        free(s_cells[i].surf);
        s_cells[i] = s_cells[--s_numCells];
        s_meshDirty = 1;
        return;
    }
}

static void cells_clear(void)
{
    for (int i = 0; i < s_numCells; i++) free(s_cells[i].surf);
    s_numCells = 0;
    s_meshDirty = 1;
}

static void load_mesh_chunk(const uint8_t *p, int len)
{
    if (len < 10) return;
    uint32_t cell;
    uint16_t chunk, chunks, nt;
    memcpy(&cell, p, 4); memcpy(&chunk, p + 4, 2); memcpy(&chunks, p + 6, 2); memcpy(&nt, p + 8, 2);
    if (chunks == 0 || chunk >= chunks || len < 10 + nt * 36) return;
    if (s_meshIn.cell != cell || !s_meshIn.seen || chunk == 0) {
        free(s_meshIn.seen);
        s_meshIn.cell = cell; s_meshIn.chunks = chunks; s_meshIn.got = 0; s_meshIn.n = 0;
        s_meshIn.seen = calloc(chunks, 1);
    }
    if (chunks != s_meshIn.chunks || s_meshIn.seen[chunk]) return;
    s_meshIn.seen[chunk] = 1;
    if (s_meshIn.n + nt > s_meshIn.cap) {
        s_meshIn.cap = (s_meshIn.n + nt) * 2;
        s_meshIn.tris = realloc(s_meshIn.tris, sizeof(float) * 9 * s_meshIn.cap);
    }
    memcpy(s_meshIn.tris + s_meshIn.n * 9, p + 10, nt * 36);
    s_meshIn.n += nt;
    if (++s_meshIn.got < s_meshIn.chunks) return;

    // complete: to sm64 surfaces, facing the way the mesh says (its winding), not a guess
    s_surfCount = 0;
    for (int t = 0; t < s_meshIn.n; t++) {
        const float *w = s_meshIn.tris + t * 9;
        float a[3], b[3], c[3];
        bng2sm(w, a); bng2sm(w + 3, b); bng2sm(w + 6, c);
        float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
        float nrm[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        surf_push(a, b, c, nrm);
    }
    free(s_meshIn.seen);
    s_meshIn.seen = NULL;
    cell_remove(cell);
    if (s_numCells >= MAX_CELLS) { logf_("too many map cells, dropping cell %08x", cell); return; }
    MeshCell *mc = &s_cells[s_numCells++];
    mc->id = cell;
    mc->count = s_surfCount;
    mc->surf = malloc(sizeof(struct SM64Surface) * (s_surfCount ? s_surfCount : 1));
    memcpy(mc->surf, s_surfBuf, sizeof(struct SM64Surface) * s_surfCount);
    s_meshDirty = 1;
}

static void drop_mesh_cell(const uint8_t *p, int len)
{
    if (len < 4) return;
    uint32_t cell;
    memcpy(&cell, p, 4);
    if (cell == 0xFFFFFFFFu) cells_clear();
    else cell_remove(cell);
}

// all cells' surfaces, without the duplicates of triangles that span several cells
static void rebuild_mesh_static(void)
{
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    int total = 0;
    for (int i = 0; i < s_numCells; i++) total += s_cells[i].count;
    free(s_meshSurf);
    s_meshSurf = malloc(sizeof(struct SM64Surface) * (total ? total : 1));
    int cap = 1;
    while (cap < total * 2) cap <<= 1;
    int32_t *table = malloc(sizeof(int32_t) * cap);
    for (int i = 0; i < cap; i++) table[i] = -1;
    int n = 0;
    for (int i = 0; i < s_numCells; i++) {
        for (int k = 0; k < s_cells[i].count; k++) {
            const struct SM64Surface *s = &s_cells[i].surf[k];
            const uint8_t *bytes = (const uint8_t *)s->vertices;
            uint32_t h = 2166136261u;
            for (size_t b = 0; b < sizeof(s->vertices); b++) h = (h ^ bytes[b]) * 16777619u;
            uint32_t slot = h & (cap - 1);
            int dup = 0;
            while (table[slot] >= 0) {
                if (!memcmp(s_meshSurf[table[slot]].vertices, s->vertices, sizeof(s->vertices))) { dup = 1; break; }
                slot = (slot + 1) & (cap - 1);
            }
            if (dup) continue;
            table[slot] = n;
            s_meshSurf[n++] = *s;
        }
    }
    free(table);
    s_meshCount = n;
    reload_static();
    s_meshDirty = 0;
    s_meshBuiltAt = GetTickCount();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    if (ms > 10) logf_("map collision rebuilt: %d cells, %d surfaces (%d duplicates dropped) in %.0f ms", s_numCells, n, total - n, ms);
}

static void mesh_update(void)
{
    if (s_meshDirty && GetTickCount() - s_meshBuiltAt >= MESH_REBUILD_MS) rebuild_mesh_static();
}

#define MAX_VEH 64
#define MAX_HULLS 6
typedef struct {
    uint32_t objId;
    float *top;        // vehicle-frame height grid
    float topMax;      // its highest cell (the hull's box, for reaching it)
    int nx, ny;
    float cell, x0, y0, bottom;
} Hull;

typedef struct {
    int used;
    uint32_t vehId;
    uint32_t objId;    // bounding-box surface object, until the vehicle sends its hull
    int isHull;        // hulls[] are live and objId is gone
    float half[3];     // sm64 half extents (bounding box; attacks use it either way)
    float center[3];   // sm64
    float axes[3][3];  // sm64 unit axes (rows)
    float framePos[3]; // sm64, vehicle reference frame origin
    float frameRows[3][3];
    float orgB[3], fwdB[3], upB[3], rightB[3];   // same frame in bng, for push-out
    Hull hulls[MAX_HULLS];   // one per piece still held together (a wrecked car falls apart into several)
    int numHulls;
    DWORD lastSeen;
    int hitCooldown;
    int held;              // being carried: no collision surfaces at all
    int collideAfterPending; // surfaces already released for the carry
    uint32_t collideAfter; // tick when collision comes back after a carry
} Vehicle;
static Vehicle s_veh[MAX_VEH];

static void box_surfaces(const float *he, struct SM64Surface *out, int *count)
{
    // 12 triangles of an axis-aligned box in local sm64 space, outward normals
    static const int faces[6][4][3] = {
        { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } },
        { { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, -1 } },
        { { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 } },
        { { -1, -1, 1 }, { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 } },
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
        { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
    };
    static const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    int saved = s_surfCount;
    for (int f = 0; f < 6; f++) {
        float q[4][3];
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 3; a++) q[k][a] = faces[f][k][a] * he[a];
        surf_push(q[0], q[1], q[2], normals[f]);
        surf_push(q[0], q[2], q[3], normals[f]);
    }
    *count = s_surfCount - saved;
    memcpy(out, s_surfBuf + saved, sizeof(struct SM64Surface) * *count);
    s_surfCount = saved;
}

static void axes_to_euler(float r[3][3], float *eulerDeg)
{
    // inverse of the decomp's mtxf_rotate_zxy_and_translate (row i = local axis i in world); libsm64's
    // CONVERT_ANGLE negates whatever we pass, so hand it the negated angles
    float sx = -r[2][1];
    if (sx > 1) sx = 1;
    if (sx < -1) sx = -1;
    float x = asinf(sx);
    float y = atan2f(r[2][0], r[2][2]);
    float z = atan2f(r[0][1], r[1][1]);
    eulerDeg[0] = -x * 180.0f / PI;
    eulerDeg[1] = -y * 180.0f / PI;
    eulerDeg[2] = -z * 180.0f / PI;
}

static void unit_sm(const float *b, float *out)
{
    float t[3];
    bng2sm(b, t);
    float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (int k = 0; k < 3; k++) out[k] = l > 1e-6f ? t[k] / l : 0;
}

// Vehicle frame (bng origin, forward, up) -> sm64 object rows. Hull coords are bng-style (x right, y forward,
// z up) run through bng2sm, so the rows are the sm64 images of right, up and -forward.
static void frame_rows(const float *fwdB, const float *upB, float rows[3][3])
{
    float rightB[3] = { fwdB[1] * upB[2] - fwdB[2] * upB[1], fwdB[2] * upB[0] - fwdB[0] * upB[2], fwdB[0] * upB[1] - fwdB[1] * upB[0] };
    float f[3];
    unit_sm(rightB, rows[0]);
    unit_sm(upB, rows[1]);
    unit_sm(fwdB, f);
    for (int k = 0; k < 3; k++) rows[2][k] = -f[k];
}

static void vehicle_move(Vehicle *veh)
{
    struct SM64ObjectTransform t;
    if (veh->isHull) {
        memcpy(t.position, veh->framePos, 12);
        axes_to_euler(veh->frameRows, t.eulerRotation);
        for (int h = 0; h < veh->numHulls; h++) sm64_surface_object_move(veh->hulls[h].objId, &t);
    } else {
        memcpy(t.position, veh->center, 12);
        axes_to_euler(veh->axes, t.eulerRotation);
        sm64_surface_object_move(veh->objId, &t);
    }
}

static void hull_free(Hull *h)
{
    if (h->top) sm64_surface_object_delete(h->objId);
    free(h->top);
    memset(h, 0, sizeof(*h));
}

static void vehicle_release(Vehicle *veh)
{
    if (veh->isHull) for (int h = 0; h < MAX_HULLS; h++) hull_free(&veh->hulls[h]);
    else if (veh->used) sm64_surface_object_delete(veh->objId);
    veh->numHulls = 0;
    veh->isHull = 0;
}

static Vehicle *vehicle_find(uint32_t id)
{
    for (int i = 0; i < MAX_VEH; i++) if (s_veh[i].used && s_veh[i].vehId == id) return &s_veh[i];
    return NULL;
}

#define VEH_REC 88   // u32 id; f32 oobbCenter[3], oobbHalfAxes[3][3], origin[3], fwd[3], up[3]

static void update_vehicles(const uint8_t *p, int len)
{
    if (len < 2) return;
    uint16_t count;
    memcpy(&count, p, 2);
    p += 2; len -= 2;
    DWORD now = GetTickCount();
    for (int v = 0; v < count && len >= VEH_REC; v++, p += VEH_REC, len -= VEH_REC) {
        uint32_t id;
        float c[3], ha[3][3], org[3], fwdB[3], upB[3];
        memcpy(&id, p, 4);
        memcpy(c, p + 4, 12);
        memcpy(ha, p + 16, 36);
        memcpy(org, p + 52, 12);
        memcpy(fwdB, p + 64, 12);
        memcpy(upB, p + 76, 12);

        float center[3], axes[3][3], half[3];
        bng2sm(c, center);
        for (int a = 0; a < 3; a++) {
            float sa[3];
            bng2sm(ha[a], sa);   // already divides by S -> sm64 units
            float l = sqrtf(sa[0] * sa[0] + sa[1] * sa[1] + sa[2] * sa[2]);
            half[a] = l;
            for (int k = 0; k < 3; k++) axes[a][k] = l > 1e-4f ? sa[k] / l : (a == k);
        }
        // keep it right-handed so it is a pure rotation
        float cr[3] = { axes[0][1] * axes[1][2] - axes[0][2] * axes[1][1], axes[0][2] * axes[1][0] - axes[0][0] * axes[1][2], axes[0][0] * axes[1][1] - axes[0][1] * axes[1][0] };
        if (cr[0] * axes[2][0] + cr[1] * axes[2][1] + cr[2] * axes[2][2] < 0)
            for (int k = 0; k < 3; k++) axes[2][k] = -axes[2][k];

        Vehicle *veh = NULL, *freeSlot = NULL;
        for (int i = 0; i < MAX_VEH; i++) {
            if (s_veh[i].used && s_veh[i].vehId == id) { veh = &s_veh[i]; break; }
            if (!s_veh[i].used && !freeSlot) freeSlot = &s_veh[i];
        }
        extern uint32_t s_tick;
        if (veh && (veh->held || s_tick < veh->collideAfter)) {
            // carried (or just thrown): keep the frame current for when collision comes back, but no surfaces
            memcpy(veh->center, center, 12); memcpy(veh->axes, axes, sizeof(axes)); memcpy(veh->half, half, 12);
            bng2sm(org, veh->framePos); frame_rows(fwdB, upB, veh->frameRows);
            memcpy(veh->orgB, org, 12); memcpy(veh->fwdB, fwdB, 12); memcpy(veh->upB, upB, 12);
            veh->rightB[0] = fwdB[1] * upB[2] - fwdB[2] * upB[1];
            veh->rightB[1] = fwdB[2] * upB[0] - fwdB[0] * upB[2];
            veh->rightB[2] = fwdB[0] * upB[1] - fwdB[1] * upB[0];
            veh->lastSeen = now;
            continue;
        }
        int rebuild = !veh || veh->collideAfter;   // collideAfter set = its surfaces were removed for a carry
        if (veh) veh->collideAfter = 0;
        if (veh && !veh->isHull)
            for (int a = 0; a < 3; a++) if (fabsf(veh->half[a] - half[a]) > 8.0f) rebuild = 1;
        if (!veh && !freeSlot) continue;
        if (!veh) { veh = freeSlot; memset(veh, 0, sizeof(*veh)); }
        memcpy(veh->center, center, 12);
        memcpy(veh->axes, axes, sizeof(axes));
        memcpy(veh->half, half, 12);
        bng2sm(org, veh->framePos);
        frame_rows(fwdB, upB, veh->frameRows);
        memcpy(veh->orgB, org, 12);
        memcpy(veh->fwdB, fwdB, 12);
        memcpy(veh->upB, upB, 12);
        veh->rightB[0] = fwdB[1] * upB[2] - fwdB[2] * upB[1];
        veh->rightB[1] = fwdB[2] * upB[0] - fwdB[0] * upB[2];
        veh->rightB[2] = fwdB[0] * upB[1] - fwdB[1] * upB[0];
        if (rebuild) {
            // bounding box until the vehicle sends its hull
            if (veh->used && !veh->collideAfterPending) vehicle_release(veh);
            veh->collideAfterPending = 0;
            struct SM64Surface surfs[12];
            int n;
            box_surfaces(half, surfs, &n);
            struct SM64SurfaceObject obj = { 0 };
            obj.surfaceCount = n;
            obj.surfaces = surfs;
            memcpy(obj.transform.position, center, 12);
            axes_to_euler(axes, obj.transform.eulerRotation);
            veh->objId = sm64_surface_object_create(&obj);
            veh->isHull = 0;
            veh->used = 1;
            veh->vehId = id;
        } else {
            vehicle_move(veh);
        }
        veh->lastSeen = now;
    }
    for (int i = 0; i < MAX_VEH; i++) {
        if (s_veh[i].used && now - s_veh[i].lastSeen > 500) {
            vehicle_release(&s_veh[i]);
            s_veh[i].used = 0;
        }
    }
}

#define HULL_STEP 0.25f   // smaller steps across a car's top (hood -> windscreen -> roof) are smooth slopes

// Hull from the vehicle's own nodes: a grid (vehicle frame) of the highest node in each cell, built as a closed
// solid - smooth across gentle changes, flat tiles + walls at real steps, sealed walls down to the underside. Only
// real edges (roof, bed rails, tailgate) are ledges Mario can grab; the old one-tile-per-cell hull was a staircase.
static void load_hull(const uint8_t *p, int len)
{
    if (len < 26) return;
    uint32_t id;
    float cell, x0, y0, bottom;
    uint16_t nx, ny;
    memcpy(&id, p, 4); memcpy(&cell, p + 4, 4); memcpy(&x0, p + 8, 4); memcpy(&y0, p + 12, 4); memcpy(&bottom, p + 16, 4);
    memcpy(&nx, p + 20, 2); memcpy(&ny, p + 22, 2);
    int index = p[24], count = p[25];
    if (nx < 1 || ny < 1 || nx > 64 || ny > 64 || index >= MAX_HULLS || count > MAX_HULLS || index >= count || len < 26 + nx * ny * 4) return;
    Vehicle *veh = vehicle_find(id);
    extern uint32_t s_tick;
    if (!veh || veh->held || s_tick < veh->collideAfter) return;

    if (!veh->isHull) {
        // first hull replaces the bounding box
        sm64_surface_object_delete(veh->objId);
        veh->isHull = 1;
    }
    // pieces the vehicle no longer has (count went down, or it was reset back into one piece)
    for (int h = count; h < MAX_HULLS; h++) hull_free(&veh->hulls[h]);
    veh->numHulls = count;

    Hull *hl = &veh->hulls[index];
    hull_free(hl);
    hl->top = malloc(sizeof(float) * nx * ny);
    memcpy(hl->top, p + 26, sizeof(float) * nx * ny);
    hl->nx = nx; hl->ny = ny;
    hl->cell = cell; hl->x0 = x0; hl->y0 = y0; hl->bottom = bottom;
    hl->topMax = bottom;
    for (int k = 0; k < nx * ny; k++) if (!isnan(hl->top[k]) && hl->top[k] > hl->topMax) hl->topMax = hl->top[k];

    int saved = s_surfCount;
    heightfield_surfaces(hl->top, nx, ny, x0 + cell * 0.5f, y0 + cell * 0.5f, cell, HULL_STEP, 1, bottom);
    int n = s_surfCount - saved;
    struct SM64SurfaceObject obj = { 0 };
    obj.surfaceCount = n;
    obj.surfaces = malloc(sizeof(struct SM64Surface) * (n ? n : 1));
    memcpy(obj.surfaces, s_surfBuf + saved, sizeof(struct SM64Surface) * n);
    s_surfCount = saved;
    memcpy(obj.transform.position, veh->framePos, 12);
    axes_to_euler(veh->frameRows, obj.transform.eulerRotation);
    hl->objId = sm64_surface_object_create(&obj);
    free(obj.surfaces);
    if (index == 0) logf_("vehicle %u hull: %d piece(s), piece 0 %dx%d cells, %d surfaces", id, count, nx, ny, n);
}

// SM64 walls only push Mario out sideways by his radius, so a car moving into him (or him landing badly on one)
// can leave him inside the body. Each tick: if he's under the hull's top somewhere inside it, pop him onto the top
// when that's a short hop, otherwise out through the nearest side.
#define MARIO_RADIUS_M 0.35f
static int vehicle_push_out(Mario *m)
{
    float b[3];
    sm2bng(m->state.position, b);
    for (int v = 0; v < MAX_VEH; v++) {
        Vehicle *veh = &s_veh[v];
        if (!veh->used || !veh->isHull || veh->held) continue;
        float d[3] = { b[0] - veh->orgB[0], b[1] - veh->orgB[1], b[2] - veh->orgB[2] };
        float lx = d[0] * veh->rightB[0] + d[1] * veh->rightB[1] + d[2] * veh->rightB[2];
        float ly = d[0] * veh->fwdB[0] + d[1] * veh->fwdB[1] + d[2] * veh->fwdB[2];
        float lz = d[0] * veh->upB[0] + d[1] * veh->upB[1] + d[2] * veh->upB[2];
        for (int h = 0; h < veh->numHulls; h++) {
            const Hull *hl = &veh->hulls[h];
            if (!hl->top) continue;
            float c = hl->cell;
            int i = (int)floorf((lx - hl->x0) / c), j = (int)floorf((ly - hl->y0) / c);
            int nx = hl->nx, ny = hl->ny;
            if (i < 0 || j < 0 || i >= nx || j >= ny) continue;
            float top = hl->top[j * nx + i];
            if (isnan(top) || lz < hl->bottom - 0.3f) continue;
            // the smoothed surface between cells can sit below a cell's own (highest-node) top, so only count him as
            // inside when he's under the lowest top around him too - standing on a slope must never trigger this
            float tmin = top;
            for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++) {
                    int ii = i + di, jj = j + dj;
                    if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
                    float t = hl->top[jj * nx + ii];
                    if (!isnan(t) && t < tmin) tmin = t;
                }
            if (lz > tmin - 0.1f) continue;

            float nlz = lz, nlx = lx, nly = ly;
            if (top - lz < 0.45f) {
                nlz = top + 0.02f;
            } else {
                // nearest way out: walk cells in each direction until one Mario fits on top of (or off the hull)
                static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                float best = 1e9f;
                for (int k = 0; k < 4; k++) {
                    int ii = i, jj = j;
                    for (;;) {
                        ii += dirs[k][0]; jj += dirs[k][1];
                        if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) break;
                        float t = hl->top[jj * nx + ii];
                        if (isnan(t) || t < lz + 0.3f) break;
                    }
                    // edge of the last blocking cell, plus Mario's radius
                    float ex = dirs[k][0] > 0 ? hl->x0 + ii * c + MARIO_RADIUS_M : dirs[k][0] < 0 ? hl->x0 + (ii + 1) * c - MARIO_RADIUS_M : lx;
                    float ey = dirs[k][1] > 0 ? hl->y0 + jj * c + MARIO_RADIUS_M : dirs[k][1] < 0 ? hl->y0 + (jj + 1) * c - MARIO_RADIUS_M : ly;
                    float dist = fabsf(ex - lx) + fabsf(ey - ly);
                    if (dist < best) { best = dist; nlx = ex; nly = ey; }
                }
            }
            float nb[3], ns[3];
            for (int k = 0; k < 3; k++) nb[k] = veh->orgB[k] + nlx * veh->rightB[k] + nly * veh->fwdB[k] + nlz * veh->upB[k];
            bng2sm(nb, ns);
            sm64_set_mario_position(m->id, ns[0], ns[1], ns[2]);
            return 1;
        }
    }
    return 0;
}

// point inside vehicle box (sm64 space), with margin; returns closest point on box too
static int point_near_box(const Vehicle *v, const float *pt, float margin, float *closest)
{
    float d[3] = { pt[0] - v->center[0], pt[1] - v->center[1], pt[2] - v->center[2] };
    int inside = 1;
    for (int k = 0; k < 3; k++) closest[k] = v->center[k];
    for (int a = 0; a < 3; a++) {
        float t = d[0] * v->axes[a][0] + d[1] * v->axes[a][1] + d[2] * v->axes[a][2];
        if (fabsf(t) > v->half[a] + margin) inside = 0;
        if (t > v->half[a]) t = v->half[a];
        if (t < -v->half[a]) t = -v->half[a];
        for (int k = 0; k < 3; k++) closest[k] += v->axes[a][k] * t;
    }
    return inside;
}

static void send_hit(const Vehicle *v, const float *smPoint, const float *smDir, float strength)
{
    uint8_t buf[1 + 4 + 12 + 12 + 4];
    float bp[3], bd[3];
    sm2bng(smPoint, bp);
    float dirScaled[3] = { smDir[0] / S, smDir[1] / S, smDir[2] / S };  // sm2bng multiplies by S; keep unit length
    sm2bng(dirScaled, bd);
    buf[0] = MSG_HIT;
    memcpy(buf + 1, &v->vehId, 4);
    memcpy(buf + 5, bp, 12);
    memcpy(buf + 17, bd, 12);
    memcpy(buf + 29, &strength, 4);
    send_raw(buf, sizeof(buf));
    logf_("hit vehicle %u strength %.2f", v->vehId, strength);
}

// Landing on a car from a jump or a fall dents it where he lands, harder the faster he came down (vertical speed on
// the frame before touchdown: a hop does nothing, a big fall leaves a real dent). Ground pounds have their own hit.
static float s_prevVy;
static int s_prevAir;

static void check_landing(Mario *m)
{
    const struct SM64MarioState *st = &m->state;
    int air = (st->action & ACT_FLAG_AIR) != 0;
    if (s_prevAir && !air && st->action != ACT_GROUND_POUND_LAND && s_prevVy < -25.0f) {
        float strength = fminf(0.5f, (-s_prevVy - 25.0f) / 50.0f);
        float feet[3] = { st->position[0], st->position[1] - 10, st->position[2] };
        for (int i = 0; i < MAX_VEH; i++) {
            Vehicle *v = &s_veh[i];
            float closest[3];
            if (!v->used || v->held || !point_near_box(v, feet, 20, closest)) continue;
            float down[3] = { 0, -1, 0 };
            send_hit(v, closest, down, strength);
            break;
        }
    }
    s_prevAir = air;
    s_prevVy = st->velocity[1];
}

static void check_attacks(Mario *m)
{
    const struct SM64MarioState *st = &m->state;
    uint32_t act = st->action;
    // strength per attack; the vehicle side scales both the dent and the shove by it
    float strength = 0, height = 60;
    if (act == ACT_SLIDE_KICK || act == ACT_SLIDE_KICK_SLIDE) { strength = 1.6f; height = 25; }
    else if (act == ACT_DIVE || act == ACT_DIVE_SLIDE) { strength = 1.2f; height = 30; }
    else if (st->flags & MARIO_KICKING) strength = 1.4f;
    else if (st->flags & MARIO_TRIPPING) { strength = 1.2f; height = 20; }   // sweep kick
    else if (st->flags & MARIO_PUNCHING) strength = 1.0f;
    int pound = act == ACT_GROUND_POUND_LAND || (act == ACT_GROUND_POUND && st->velocity[1] < -20.0f);

    float fwd[3] = { sinf(st->faceAngle), 0, cosf(st->faceAngle) };
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s_veh[i];
        if (!v->used) continue;
        if (v->held) continue;
        if (v->hitCooldown > 0) { v->hitCooldown--; continue; }
        float closest[3];
        if (strength > 0) {
            float pt[3] = { st->position[0] + fwd[0] * 70, st->position[1] + height, st->position[2] + fwd[2] * 70 };
            if (point_near_box(v, pt, 30, closest)) {
                send_hit(v, closest, fwd, strength);
                v->hitCooldown = 12;
            }
        } else if (pound) {
            float pt[3] = { st->position[0], st->position[1] - 20, st->position[2] };
            if (point_near_box(v, pt, 30, closest)) {
                float down[3] = { 0, -1, 0 };
                send_hit(v, closest, down, 2.2f);
                v->hitCooldown = 20;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// input: XInput pad (N64 layout) + keyboard fallback, only while BeamNG has focus

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static XInputGetStateFn s_xinputGetState;


static float deadzone(SHORT v, SHORT dz)
{
    if (v > dz) return (v - dz) / (32767.0f - dz);
    if (v < -dz) return (v + dz) / (32768.0f - dz);
    return 0;
}

// Whether the game window has focus, as the mod reports it (MSG_FOCUS). The helper can't reliably tell for itself:
// under Wine/Proton it can't see the game's window at all, so looking at the foreground window's title left the
// controller dead there.
static int s_gameFocused = 1;

static int s_ignoreFocus;

static void read_pad(Pad *p)
{
    memset(p, 0, sizeof(*p));
    if (!s_ignoreFocus && !s_gameFocused) return;
    if (s_xinputGetState) {
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE xs;
            if (s_xinputGetState(i, &xs) != ERROR_SUCCESS) continue;
            XINPUT_GAMEPAD *g = &xs.Gamepad;
            p->lx = deadzone(g->sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            p->ly = deadzone(g->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            p->rx = deadzone(g->sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            p->ry = deadzone(g->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            p->a = (g->wButtons & XINPUT_GAMEPAD_A) != 0;
            p->b = (g->wButtons & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X)) != 0;
            p->z = g->bRightTrigger > 64 || g->bLeftTrigger > 64;
            p->zoomIn = (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            p->zoomOut = (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            p->y = (g->wButtons & XINPUT_GAMEPAD_Y) != 0;
            p->music = (g->wButtons & XINPUT_GAMEPAD_BACK) != 0;
            break;
        }
    }
    ng64_dinput_read(p);   // PlayStation / Switch / generic pads, merged with whatever XInput gave
#define KEY(k) ((GetAsyncKeyState(k) & 0x8000) != 0)
    if (KEY('W')) p->ly = 1;
    if (KEY('S')) p->ly = -1;
    if (KEY('A')) p->lx = -1;
    if (KEY('D')) p->lx = 1;
    if (KEY(VK_SPACE)) p->a = 1;
    if (KEY('J')) p->b = 1;
    if (KEY('K')) p->z = 1;
    if (KEY('E')) p->y = 1;
    if (KEY('M')) p->music = 1;
    if (KEY(VK_OEM_6)) p->songNext = 1;   // ]
    if (KEY(VK_OEM_4)) p->songPrev = 1;   // [
    if (KEY(VK_LEFT)) p->rx = -1;
    if (KEY(VK_RIGHT)) p->rx = 1;
    if (KEY(VK_UP)) p->ry = 1;
    if (KEY(VK_DOWN)) p->ry = -1;
#undef KEY
}

// ---------------------------------------------------------------------------------------------------------------
// carrying cars and wreck pieces (Y / E). SM64 does the lift, carry, heavy walk, throw (B) and put-down (Z); the
// vehicle's own Lua holds the piece at the point sent here and applies the throw.
#define ACT_FLAG_THROWING_BIT 0x80000000u
#define CARRY_REACH_M 1.5f   // from Mario (chest height) to the nearest point of a piece he faces (as far as the old 0.4 m-ahead point + 1 m reached)

static struct { int active; uint32_t vehId; int piece, heavy; } s_carry;
static int s_injectB, s_prevY, s_scriptY, s_prevZ, s_effZ;
static int s_inputEnabled = 1;

// ---- music: SM64's own, from the ROM, while you're playing as Mario (Back / M toggles) -------------------------------
// SM64's music, in its sequence order (seq_ids.h), for cycling through; starts on Bob-omb Battlefield
static const struct { uint8_t seq; const char *name; } s_songs[] = {
    { 0x03, "Bob-omb Battlefield" }, { 0x02, "Title Theme" }, { 0x04, "Inside the Castle Walls" },
    { 0x05, "Dire, Dire Docks" }, { 0x06, "Lethal Lava Land" }, { 0x07, "Koopa's Theme" },
    { 0x08, "Snow Mountain" }, { 0x09, "Slider" }, { 0x0A, "Haunted House" }, { 0x0B, "Piranha Plant's Lullaby" },
    { 0x0C, "Cave Dungeon" }, { 0x0D, "Star Select" }, { 0x0E, "Powerful Mario" }, { 0x0F, "Metallic Mario" },
    { 0x10, "Koopa's Message" }, { 0x11, "Koopa's Road" }, { 0x12, "High Score" }, { 0x13, "Merry-Go-Round" },
    { 0x14, "Race Fanfare" }, { 0x15, "Star Appears" }, { 0x16, "Stage Boss" }, { 0x17, "Key Get" },
    { 0x18, "Endless Stairs" }, { 0x19, "Ultimate Koopa" }, { 0x1A, "Staff Roll" }, { 0x1B, "Puzzle Solved" },
    { 0x1C, "Toad's Message" }, { 0x1D, "Peach's Message" }, { 0x1E, "Opening" }, { 0x1F, "Ultimate Victory" },
    { 0x20, "Ending" }, { 0x21, "File Select" }, { 0x22, "Lakitu" }, { 0x01, "Star Get" },
};
#define NUM_SONGS ((int)(sizeof(s_songs) / sizeof(s_songs[0])))
static int s_song;
static int s_audioOk, s_musicOn = 1, s_musicPlaying, s_prevMusic, s_scriptMusic, s_scriptSong;
static int s_musicCombo, s_prevNext, s_prevPrev;

static void send_toast(const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "toast:%s", msg);
    send_log(buf);
}


// Back (M) toggles the music, on release; held with RB / LB (or ] / [ on the keyboard) it changes song instead.
// While Back is held the bumpers don't zoom the camera.
static void music_update(Pad *pad)
{
    int held = pad->music || s_scriptMusic;
    int next = (held && pad->zoomIn) || pad->songNext || s_scriptSong > 0;
    int prev = (held && pad->zoomOut) || pad->songPrev || s_scriptSong < 0;
    int step = (next && !s_prevNext) ? 1 : (prev && !s_prevPrev) ? -1 : 0;
    s_prevNext = next; s_prevPrev = prev;
    s_scriptSong = 0;
    if (held) { pad->zoomIn = pad->zoomOut = 0; if (step) s_musicCombo = 1; }
    if (step && s_inputEnabled) {
        s_song = (s_song + step + NUM_SONGS) % NUM_SONGS;
        s_musicOn = 1;
        s_musicPlaying = 0;   // start the new song below
        char msg[96];
        snprintf(msg, sizeof(msg), "Music %d/%d: %s", s_song + 1, NUM_SONGS, s_songs[s_song].name);
        send_toast(msg);
        logf_("song %d: %s", s_song + 1, s_songs[s_song].name);
    }
    int released = !held && s_prevMusic;
    if (released && !s_musicCombo && s_inputEnabled) {
        s_musicOn = !s_musicOn;
        send_toast(s_musicOn ? "Music on" : "Music off");
        logf_("music %s", s_musicOn ? "on" : "off");
    }
    if (!held) s_musicCombo = 0;
    s_prevMusic = held;
    s_scriptMusic = 0;
    int want = s_audioOk && s_musicOn && s_inputEnabled && mario_find(0) != NULL;
    if (want && !s_musicPlaying) {
        ng64_sm64_lock();
        sm64_ng64_music_play(s_songs[s_song].seq);
        uint32_t ms = sm64_ng64_music_status();
        ng64_sm64_unlock();
        s_musicPlaying = 1;
        logf_("music playing (sequence 0x%02x; player has 0x%02x, queue %u)", s_songs[s_song].seq, ms & 0xFF, ms >> 16);
    } else if (!want && s_musicPlaying) {
        SM64_AUDIO_SAFE(sm64_ng64_music_stop(30));   // one-second fade
        s_musicPlaying = 0;
        logf_("music stopped");
    }
}   // off while the player controls another vehicle (Mario stays, standing idle)   // s_effZ: Z as fed to SM64 this tick (pad or script)
#define ACT_IDLE_NG64 0x0C400201

static void send_carry(int kind, const Vehicle *v, int piece, int heavy, const float *pointB, float yaw, const float *velB)
{
    uint8_t buf[1 + 1 + 4 + 1 + 1 + 12 + 4 + 12];
    float zero[3] = { 0, 0, 0 };
    buf[0] = MSG_CARRY;
    buf[1] = (uint8_t)kind;
    memcpy(buf + 2, &v->vehId, 4);
    buf[6] = (uint8_t)piece;
    buf[7] = (uint8_t)heavy;
    memcpy(buf + 8, pointB ? pointB : zero, 12);
    memcpy(buf + 20, &yaw, 4);
    memcpy(buf + 24, velB ? velB : zero, 12);
    send_raw(buf, sizeof(buf));
}

// nearest carryable piece in front of Mario: distance from a point just ahead of him to each hull piece's footprint
static char s_carryMiss[256];   // why the last carry_target found nothing (log)
// What Y picks up: the nearest car or wreck piece he's facing, measured from Mario himself (chest height) to each
// piece's whole hull box - its footprint and its height, in the car's own frame. Used to be a single point 0.4 m in
// front of him against the footprint only: pinned against a crumpled corner at an angle, or under a bent panel,
// that point could miss for seconds while he ran on the spot. Anything he's touching always counts.
#define CARRY_TOUCH_M 0.35f
static Vehicle *carry_target(const Mario *m, int *pieceOut)
{
    s_carryMiss[0] = 0;
    const struct SM64MarioState *st = &m->state;
    float chest[3] = { st->position[0], st->position[1] + 50, st->position[2] };
    float b[3];
    sm2bng(chest, b);
    float face[2] = { sinf(st->faceAngle), -cosf(st->faceAngle) };   // his facing, bng x/y
    Vehicle *best = NULL;
    float bestD = CARRY_REACH_M;
    for (int v = 0; v < MAX_VEH; v++) {
        Vehicle *veh = &s_veh[v];
        if (!veh->used || veh->held) continue;
        float d[3] = { b[0] - veh->orgB[0], b[1] - veh->orgB[1], b[2] - veh->orgB[2] };
        float lx = d[0] * veh->rightB[0] + d[1] * veh->rightB[1] + d[2] * veh->rightB[2];
        float ly = d[0] * veh->fwdB[0] + d[1] * veh->fwdB[1] + d[2] * veh->fwdB[2];
        float lz = d[0] * veh->upB[0] + d[1] * veh->upB[1] + d[2] * veh->upB[2];
        int pieces = veh->isHull ? veh->numHulls : 1;
        for (int h = 0; h < pieces; h++) {
            float c[3];   // nearest point of the piece's box, world (bng)
            if (veh->isHull) {
                const Hull *hl = &veh->hulls[h];
                if (!hl->top) continue;
                float x1 = hl->x0 + hl->nx * hl->cell, y1 = hl->y0 + hl->ny * hl->cell;
                float nxl = lx < hl->x0 ? hl->x0 : lx > x1 ? x1 : lx;
                float nyl = ly < hl->y0 ? hl->y0 : ly > y1 ? y1 : ly;
                float nzl = lz < hl->bottom ? hl->bottom : lz > hl->topMax ? hl->topMax : lz;
                for (int k = 0; k < 3; k++)
                    c[k] = veh->orgB[k] + veh->rightB[k] * nxl + veh->fwdB[k] * nyl + veh->upB[k] * nzl;
            } else {
                float sp[3], cs[3];
                bng2sm(b, sp);
                point_near_box(veh, sp, 0, cs);
                sm2bng(cs, c);
            }
            float dx = c[0] - b[0], dy = c[1] - b[1], dz = c[2] - b[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            float horiz = sqrtf(dx * dx + dy * dy);
            float facing = horiz > 1e-3f ? (dx * face[0] + dy * face[1]) / horiz : 1;
            if (dist < 4 && strlen(s_carryMiss) < sizeof(s_carryMiss) - 64)
                snprintf(s_carryMiss + strlen(s_carryMiss), sizeof(s_carryMiss) - strlen(s_carryMiss),
                         "veh %u piece %d: %.2f m, facing %.2f; ", veh->vehId, h, dist, facing);
            if (dist >= bestD) continue;
            if (dist > CARRY_TOUCH_M && facing < 0.3f) continue;   // near but behind / beside him
            bestD = dist; best = veh; *pieceOut = h;
        }
    }
    return best;
}
static void carry_update(Mario *m, const Pad *pad)
{
    const struct SM64MarioState *st = &m->state;
    int yEdge = (pad->y || s_scriptY) && !s_prevY;
    s_prevY = pad->y || s_scriptY;
    s_scriptY = 0;
    Vehicle *v = s_carry.active ? vehicle_find(s_carry.vehId) : NULL;
    if (s_carry.active && !v) { s_carry.active = 0; sm64_mario_drop_held(m->id); return; }   // the car was removed

    if (!s_carry.active) {
        if (!yEdge || (st->action & ACT_FLAG_AIR)) return;
        int piece = 0;
        v = carry_target(m, &piece);
        if (!v) {
            logf_("Y: nothing in reach (%s)", s_carryMiss);
            return;
        }
        // the main body lifts overhead (heavy); a piece that came off lifts like a crate
        int heavy = piece == 0;
        SM64_AUDIO_SAFE(sm64_mario_pick_up(m->id, heavy != 0));
        s_carry.active = 1; s_carry.vehId = v->vehId; s_carry.piece = piece; s_carry.heavy = heavy;
        vehicle_release(v);            // no collision while it's in his hands
        v->held = 1;
        v->collideAfterPending = 1;
        send_carry(1, v, piece, heavy, NULL, st->faceAngle, NULL);
        logf_("picked up vehicle %u piece %d (%s)", v->vehId, piece, heavy ? "heavy" : "light");
        return;
    }

    if (sm64_mario_is_holding(m->id)) {
        {
            // trace: once a second while carrying, where he is and what he's doing, so a stuck carry shows up in the
            // log even when it doesn't match the "not moving" check below
            static DWORD traceAt;
            DWORD t = GetTickCount();
            if (t - traceAt >= 1000) {
                traceAt = t;
                logf_("carry: pos %.0f %.0f %.0f, action %08x, forward speed %.1f, vel %.1f %.1f %.1f, stick %.2f %.2f",
                      st->position[0], st->position[1], st->position[2], (unsigned)st->action, st->forwardVelocity,
                      st->velocity[0], st->velocity[1], st->velocity[2], pad->lx, pad->ly);
            }
        }
        {
            // diagnostics: a walking action (ACT_FLAG_MOVING) with next to no movement for a second is logged with
            // what's around him, to find out what he's walking into (reported: "walks on the spot" while carrying)
            static float lastPos[3];
            static DWORD since, loggedAt;
            DWORD now = GetTickCount();
            float dx = st->position[0] - lastPos[0], dz = st->position[2] - lastPos[2];
            if (!(st->action & 0x400) || dx * dx + dz * dz > 18 * 18) {   // not walking, or moved > ~15 cm
                memcpy(lastPos, st->position, 12);
                since = now;
            } else if (now - since > 1000 && now - loggedAt > 3000) {
                loggedAt = now;
                float wx = st->position[0], wy = st->position[1], wz = st->position[2];
                int walls = sm64_surface_find_wall_collision(&wx, &wy, &wz, 60, 50);
                struct SM64SurfaceCollisionData *floor = NULL;
                float fy = sm64_surface_find_floor(st->position[0], st->position[1] + 50, st->position[2], &floor);
                float nearest = 1e9f;
                uint32_t nearId = 0;
                for (int i = 0; i < MAX_VEH; i++) {
                    if (!s_veh[i].used) continue;
                    float p[3];
                    sm2bng(st->position, p);
                    float ddx = p[0] - s_veh[i].orgB[0], ddy = p[1] - s_veh[i].orgB[1];
                    float d = sqrtf(ddx * ddx + ddy * ddy);
                    if (d < nearest) { nearest = d; nearId = s_veh[i].vehId; }
                }
                logf_("carrying but not moving for %.1f s: action %08x, forward speed %.1f, %d wall(s) against him, "
                      "floor %.0f below (normal y %.2f), nearest vehicle %u at %.1f m (held %u, %s)",
                      (now - since) / 1000.0f, (unsigned)st->action, st->forwardVelocity, walls,
                      st->position[1] - fy, floor ? floor->normal.y : -1.0f, nearId, nearest, s_carry.vehId,
                      s_carry.heavy ? "heavy" : "light");
            }
        }
        if (yEdge) s_injectB = 1;   // Y again throws, same as B
        // SM64 has no put-down for heavy things (B throws, Z does nothing); Z lets go of a car here. Light pieces
        // keep SM64's own put-down.
        int zEdge = s_effZ && !s_prevZ;
        s_prevZ = s_effZ;
        if (s_carry.heavy && zEdge && st->action == 0x08000208 /* ACT_HOLD_HEAVY_IDLE */) {
            sm64_mario_drop_held(m->id);
            SM64_AUDIO_SAFE(sm64_set_mario_action(m->id, ACT_IDLE_NG64));
        }
        // where the piece's underside should rest: on his gloves, wherever this frame's animation has put them (the
        // gloves are the only pure-white part of the model) - so it sits in his hands and bobs with them, the way
        // King Bob-omb does. Fixed points above/in front of him are the fallback.
        float fx = sinf(st->faceAngle), fz = cosf(st->faceAngle);
        float hold[3];
        float gx = 0, gz = 0, gTop = -1e9f, gBot = 1e9f;
        int gn = 0;
        for (int i = 0; i < m->geo.numTrianglesUsed * 3; i++) {
            const float *c = &m->geo.color[i * 3];
            if (c[0] < 0.9f || c[1] < 0.9f || c[2] < 0.9f) continue;
            const float *vp = &m->geo.position[i * 3];
            gx += vp[0]; gz += vp[2]; gn++;
            if (vp[1] > gTop) gTop = vp[1];
            if (vp[1] < gBot) gBot = vp[1];
        }
        if (gn > 0) {
            hold[0] = gx / gn; hold[2] = gz / gn;
            if (s_carry.heavy) {
                // his raised hands sit either side of his head (gloves top out ~18 units below his cap), so a car
                // resting on them would pass through his head: it sits just on top of him instead, like King Bob-omb
                float top = gTop;
                for (int i = 0; i < m->geo.numTrianglesUsed * 3; i++) if (m->geo.position[i * 3 + 1] > top) top = m->geo.position[i * 3 + 1];
                hold[1] = top + 3;
            } else {
                hold[1] = gBot;                     // light: held between his hands
            }
        } else if (s_carry.heavy) { hold[0] = st->position[0] + fx * 10; hold[1] = st->position[1] + 175; hold[2] = st->position[2] + fz * 10; }
        else { hold[0] = st->position[0] + fx * 60; hold[1] = st->position[1] + 60; hold[2] = st->position[2] + fz * 60; }
        float holdB[3];
        sm2bng(hold, holdB);
        send_carry(2, v, s_carry.piece, s_carry.heavy, holdB, st->faceAngle, NULL);
        return;
    }

    // let go: a throw sends it flying the way he faces, anything else just leaves it with his own speed
    float velS[3] = { st->velocity[0] * 30, st->velocity[1] * 30, st->velocity[2] * 30 };
    int thrown = (st->action & ACT_FLAG_THROWING_BIT) != 0;
    if (thrown) {
        float speed = s_carry.heavy ? 1500.0f : 1900.0f;   // units/s: ~13 / ~16 m/s
        velS[0] += sinf(st->faceAngle) * speed;
        velS[1] += s_carry.heavy ? 600.0f : 700.0f;
        velS[2] += cosf(st->faceAngle) * speed;
    }
    float velB[3] = { velS[0] * S, -velS[2] * S, velS[1] * S };
    send_carry(3, v, s_carry.piece, s_carry.heavy, NULL, st->faceAngle, velB);
    logf_("%s vehicle %u", thrown ? "threw" : "dropped", v->vehId);
    v->held = 0;
    v->collideAfter = s_tick + 45;   // collision back 1.5 s later, once it has flown clear of him
    s_carry.active = 0;
}

// scripted input (UAT)
static struct { float sx, sy; uint8_t a, b, z; int frames; int absDir; float lookX, lookZ; } s_script;

// ---------------------------------------------------------------------------------------------------------------
// camera (sm64 space): orbit on right stick, lazily swings behind Mario while he runs

static float s_camYaw = PI, s_camPitch = 0.25f, s_camDist = 750;
static float s_camPos[3], s_camTarget[3];

static float angdiff(float a, float b)
{
    float d = fmodf(a - b + PI, 2 * PI);
    if (d < 0) d += 2 * PI;
    return d - PI;
}

static void update_camera(const Mario *m, const Pad *pad, float dt)
{
    const struct SM64MarioState *st = &m->state;
    s_camYaw -= pad->rx * 2.5f * dt;
    s_camPitch -= pad->ry * 1.2f * dt;   // stick up looks up, like BeamNG's camera
    if (s_camPitch < -0.3f) s_camPitch = -0.3f;
    if (s_camPitch > 1.2f) s_camPitch = 1.2f;
    if (pad->zoomIn) s_camDist -= 600 * dt;
    if (pad->zoomOut) s_camDist += 600 * dt;
    if (s_camDist < 300) s_camDist = 300;
    if (s_camDist > 2000) s_camDist = 2000;

    if (fabsf(pad->rx) < 0.1f && st->forwardVelocity > 8.0f) {
        // swing round behind him as he runs away from / across the view - but not when he runs at the camera: there
        // "behind him" is ~180 degrees away, the shortest way round flips left/right with every wobble (the camera
        // lurched about), and turning at all bends his camera-relative stick direction so he curves. Like SM64's
        // camera, it fades out towards 100 degrees and just backs up in front of him.
        float behind = st->faceAngle + PI;
        float diff = angdiff(behind, s_camYaw);
        const float limit = 1.75f;
        if (fabsf(diff) < limit) {
            float fade = 1.0f - fabsf(diff) / limit;
            s_camYaw += diff * fade * fminf(1.0f, 0.9f * dt * st->forwardVelocity / 32.0f);
        }
    }
    s_camTarget[0] = st->position[0];
    s_camTarget[1] = st->position[1] + 100;
    s_camTarget[2] = st->position[2];
    s_camPos[0] = s_camTarget[0] + sinf(s_camYaw) * cosf(s_camPitch) * s_camDist;
    s_camPos[1] = s_camTarget[1] + sinf(s_camPitch) * s_camDist;
    s_camPos[2] = s_camTarget[2] + cosf(s_camYaw) * cosf(s_camPitch) * s_camDist;
}

// ---------------------------------------------------------------------------------------------------------------

static uint32_t s_seq;
uint32_t s_tick;   // advances once per 30 Hz simulation step

// libsm64 (patched, libsm64-parts.patch): per triangle, the body part (matrix) it was drawn with and its vertices
// before that matrix; the matrices themselves. Filled by each sm64_mario_tick, so read straight after it.
extern int g_ng64PartCount;
extern float g_ng64PartMtx[NG64_MAX_PARTS][4][4];
extern unsigned char g_ng64TriPart[SM64_GEO_MAX_TRIANGLES];
extern float g_ng64LocalPos[SM64_GEO_MAX_TRIANGLES * 9];
extern float g_ng64LocalNrm[SM64_GEO_MAX_TRIANGLES * 9];

static void pack_vert(PackedVert *out, const float *smLocal, const float *smNormal, const float *color, const float *uv)
{
    float b[3] = { smLocal[0] * S, -smLocal[2] * S, smLocal[1] * S };   // sm2bng, as a direction (no origin)
    for (int k = 0; k < 3; k++) {
        float mm = b[k] * 1000.0f;
        if (mm > 32767) mm = 32767;
        if (mm < -32768) mm = -32768;
        out->p[k] = (int16_t)lroundf(mm);
    }
    float bn[3] = { smNormal[0], -smNormal[2], smNormal[1] };
    float len = sqrtf(bn[0] * bn[0] + bn[1] * bn[1] + bn[2] * bn[2]);
    if (len < 1e-6f) len = 1;
    for (int k = 0; k < 3; k++) out->n[k] = (int8_t)lroundf(fmaxf(-1, fminf(1, bn[k] / len)) * 127);

    int band = band_for_color(color);
    float u = uv[0], v = uv[1];
    float au, av;
    if (u >= 1.0f && v >= 1.0f) {   // untextured triangle: solid patch
        au = (SOLID_X0 + 8) / (float)ATLAS_W;
        av = (band * BAND_H + BAND_H * 0.5f) / (float)ATLAS_H;
    } else {
        au = u * SM64_TEXTURE_WIDTH / (float)ATLAS_W;
        av = (band * BAND_H + 0.5f + v * (BAND_H - 1)) / (float)ATLAS_H;   // inset so filtering can't bleed into the next band
    }
    out->uv[0] = (uint16_t)lroundf(fmaxf(0, fminf(1, au)) * 65535);
    out->uv[1] = (uint16_t)lroundf(fmaxf(0, fminf(1, av)) * 65535);
}

static void send_part(const Mario *m, int part, uint32_t hash)
{
    if (part < 0 || part >= m->partCount || !m->partPkt[part] || m->partHash[part] != hash) return;
    send_raw(m->partPkt[part], m->partLen[part]);
}

// BeamNG redraws a rebuilt ProceduralMesh at a cost that never goes away: rebuilding Mario every frame made the game
// slower and slower (unplayable after ~30 min). SM64's Mario is rigid body parts, one matrix each, so each part's
// geometry is sent once (in its own frame) and a frame only carries where every part is. A part's geometry changes
// only with its look (blinking eyes, hand pose, cap), and the mod keeps every look it has seen.
static void send_frame(Mario *m)
{
    static uint8_t pkt[sizeof(FrameHeader) + NG64_MAX_PARTS * sizeof(PartPose)];
    FrameHeader *h = (FrameHeader *)pkt;
    PartPose *poses = (PartPose *)(pkt + sizeof(FrameHeader));
    const struct SM64MarioState *st = &m->state;
    float origin[3];
    if (m->key == 0) memcpy(origin, st->position, 12);
    else memcpy(origin, m->rPos, 12);

    memset(h, 0, sizeof(*h));
    h->type = MSG_FRAME;
    h->key = m->key;
    h->seq = ++s_seq;
    sm2bng(origin, h->pos);
    float vel[3] = { st->velocity[0] * 30, st->velocity[1] * 30, st->velocity[2] * 30 };
    sm2bng(vel, h->vel);
    h->faceAngle = st->faceAngle;
    h->health = st->health;
    h->action = st->action;
    h->animId = (int16_t)st->animID;
    h->animFrame = st->animFrame;
    h->flags = st->flags;
    if (m->key == 0) {
        sm2bng(s_camPos, h->camPos);
        sm2bng(s_camTarget, h->camTarget);
    }

    int np = g_ng64PartCount;
    if (np > NG64_MAX_PARTS) np = NG64_MAX_PARTS;
    int ntri = m->geo.numTrianglesUsed;

    // geometry per part, deduplicated into unique vertices + corner indices
    static PackedVert verts[SM64_GEO_MAX_TRIANGLES * 3];
    static uint16_t idx[SM64_GEO_MAX_TRIANGLES * 3];
    static int32_t table[4096];
    static uint8_t partBuf[NG64_MAX_PART_BYTES + 64];
    int totalVerts = 0;
    for (int p = 0; p < np; p++) {
        for (int i = 0; i < 4096; i++) table[i] = -1;
        int nu = 0, ni = 0;
        uint32_t hash = 2166136261u;
        for (int t = 0; t < ntri; t++) {
            if (g_ng64TriPart[t] != p) continue;
            for (int c = 0; c < 3; c++) {
                int corner = t * 3 + c;
                PackedVert pv;
                pack_vert(&pv, &g_ng64LocalPos[corner * 3], &g_ng64LocalNrm[corner * 3], &m->geo.color[corner * 3], &m->geo.uv[corner * 2]);
                const uint8_t *bytes = (const uint8_t *)&pv;
                uint32_t hsh = 2166136261u;
                for (size_t k = 0; k < sizeof(PackedVert); k++) hsh = (hsh ^ bytes[k]) * 16777619u;
                uint32_t slot = hsh & 4095;
                int id;
                for (;;) {
                    if (table[slot] < 0) { table[slot] = nu; verts[nu] = pv; id = nu++; hash = (hash ^ hsh) * 16777619u; break; }
                    if (!memcmp(&verts[table[slot]], &pv, sizeof(PackedVert))) { id = table[slot]; break; }
                    slot = (slot + 1) & 4095;
                }
                idx[ni++] = (uint16_t)id;
                hash = (hash ^ (uint32_t)id) * 16777619u;
            }
        }
        if (ni == 0) hash = 0;
        if (hash == 0 && ni) hash = 1;
        totalVerts += nu;

        PartPose *pp = &poses[p];
        pp->hash = hash;
        // the part's matrix (sm64 row vectors: world = v * M) in bng terms: where the part's own bng x, y, z axes
        // point, with its scale. Geometry is already in bng axes and metres, so only this and the position move it.
        float (*mx)[4] = g_ng64PartMtx[p];
        for (int k = 0; k < 3; k++) {
            float e[3] = { 0, 0, 0 };
            e[k] = 1;
            float s3[3] = { e[0], e[2], -e[1] };   // bng direction -> sm64 direction
            float w[3];
            for (int j = 0; j < 3; j++) w[j] = s3[0] * mx[0][j] + s3[1] * mx[1][j] + s3[2] * mx[2][j];
            pp->axes[k][0] = w[0]; pp->axes[k][1] = -w[2]; pp->axes[k][2] = w[1];
        }
        float t3[3] = { mx[3][0], mx[3][1], mx[3][2] };
        sm2bng(t3, pp->pos);

        if (p >= m->partCount || hash != m->partHash[p]) {
            int len = (int)(sizeof(PartHeader) + nu * sizeof(PackedVert) + ni * 2);
            if (len > NG64_MAX_PART_BYTES) {
                static int warned;
                if (!warned) { warned = 1; logf_("body part %d too big to send (%d verts, %d corners)", p, nu, ni); }
                pp->hash = 0;
                continue;
            }
            PartHeader *ph = (PartHeader *)partBuf;
            ph->type = MSG_PART;
            ph->key = m->key;
            ph->part = (uint8_t)p;
            ph->hash = hash;
            ph->nv = (uint16_t)nu;
            ph->ni = (uint16_t)ni;
            memcpy(partBuf + sizeof(PartHeader), verts, nu * sizeof(PackedVert));
            memcpy(partBuf + sizeof(PartHeader) + nu * sizeof(PackedVert), idx, ni * 2);
            m->partPkt[p] = realloc(m->partPkt[p], len);
            memcpy(m->partPkt[p], partBuf, len);
            m->partLen[p] = len;
            m->partHash[p] = hash;
            if (hash) send_raw(partBuf, len);
        }
    }
    m->partCount = np;

    h->numVerts = (uint16_t)totalVerts;
    h->numParts = (uint16_t)np;
    h->tick = s_tick;
    send_raw(pkt, (int)(sizeof(FrameHeader) + np * sizeof(PartPose)));
}

static void send_welcome(int ok, const char *msg)
{
    char buf[1200];
    buf[0] = MSG_WELCOME;
    buf[1] = (char)ok;
    int n = 2;
    n += snprintf(buf + n, sizeof(buf) - n, "%s", msg) + 1;
    n += snprintf(buf + n, sizeof(buf) - n, "%s", s_atlasGamePath) + 1;
    send_raw(buf, n);
}

// 8 MB big-endian (.z64) N64 image whose header says Super Mario 64, US region
static int is_sm64_us(const uint8_t *rom, size_t len)
{
    return len == 8388608 && rom[0] == 0x80 && rom[1] == 0x37 && !memcmp(rom + 0x20, "SUPER MARIO 64", 14) && rom[0x3E] == 'E';
}

static uint8_t *load_file(const char *path, size_t *outLen);

static int find_sm64_rom(const char *dir, char *out, size_t outSize)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.*64", dir);   // .z64, .v64, .n64
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        size_t len = 0;
        uint8_t *rom = load_file(path, &len);
        if (rom) ng64_rom_normalize(rom, len);   // .v64 / .n64 byte orders too
        if (rom && is_sm64_us(rom, len)) { snprintf(out, outSize, "%s", path); ok = 1; }
        free(rom);
    } while (!ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static uint8_t *load_file(const char *path, size_t *outLen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *outLen = len;
    return buf;
}

static void spawn_local(const float *bng)
{
    Mario *m = mario_find(0);
    if (m) mario_delete(m);
    float sp[3];
    bng2sm(bng, sp);
    m = mario_create(0, sp);
    if (!m) {
        logf_("spawn failed at %.2f %.2f %.2f (no floor under spawn point?)", bng[0], bng[1], bng[2]);
        send_log("spawn failed: no floor under Mario");
        return;
    }
    s_camYaw = PI;
    logf_("spawned local mario id %d at %.2f %.2f %.2f", m->id, bng[0], bng[1], bng[2]);
}

static void handle_packet(const uint8_t *p, int len)
{
    if (len < 1) return;
    uint8_t type = p[0];
    p++; len--;
    switch (type) {
    case MSG_HELLO: {
        uint16_t ver = 0;
        if (len >= 2) memcpy(&ver, p, 2);
        if (len > 2) {
            snprintf(s_userPath, sizeof(s_userPath), "%.*s", len - 2, (const char *)p + 2);
            size_t l = strlen(s_userPath);
            while (l && (s_userPath[l - 1] == '\\' || s_userPath[l - 1] == '/')) s_userPath[--l] = 0;
        }
        if (ver != NG64_PROTO_VERSION) { send_welcome(0, "protocol version mismatch - update the NG64 helper or mod"); break; }
        if (!s_atlasGamePath[0] || s_atlasDirty) write_atlas();
        if (s_hudOk && !ng64_hud_write(s_userPath)) logf_("could not write the HUD graphics into %s\\ng64_cache\\hud", s_userPath);
        send_welcome(1, s_hudOk ? "ok hud" : "ok");
        logf_("hello from mod, user path %s", s_userPath);
        ensure_preview(s_userPath);
        break;
    }
    case MSG_TERRAIN: load_terrain(p, len); break;
    case MSG_MESH: load_mesh_chunk(p, len); break;
    case MSG_MESH_DROP: drop_mesh_cell(p, len); break;
    case MSG_SURFACES: load_level_surfaces(p, len); break;
    case MSG_WATER: load_water(p, len); break;
    case MSG_FLOOR_QUERY: {
        // tests: SM64's floor height under each point, to compare with BeamNG's own raycasts
        if (len < 6) break;
        uint32_t qid; uint16_t n;
        memcpy(&qid, p, 4); memcpy(&n, p + 4, 2);
        if (n > 1000 || len < 6 + n * 12) break;
        static uint8_t out[1 + 4 + 2 + 1000 * 4];
        out[0] = MSG_FLOOR_REPLY;
        memcpy(out + 1, &qid, 4); memcpy(out + 5, &n, 2);
        for (int i = 0; i < n; i++) {
            float b[3], sp[3];
            memcpy(b, p + 6 + i * 12, 12);
            bng2sm(b, sp);
            struct SM64SurfaceCollisionData *fl = NULL;
            float h = sm64_surface_find_floor(sp[0], sp[1], sp[2], &fl);
            float z = fl ? h * S : NAN;
            memcpy(out + 7 + i * 4, &z, 4);
        }
        send_raw(out, 7 + n * 4);
        break;
    }
    case MSG_SPAWN: if (s_meshDirty) rebuild_mesh_static(); if (len >= 12) spawn_local((const float *)p); break;
    case MSG_DESPAWN: { Mario *m = mario_find(0); if (m) mario_delete(m); break; }
    case MSG_TELEPORT: {
        if (s_meshDirty) rebuild_mesh_static();   // the destination's cells were just sent: floors before he lands
        Mario *m = mario_find(0);
        if (m && len >= 12) {
            float sp[3];
            bng2sm((const float *)p, sp);
            sm64_set_mario_position(m->id, sp[0], sp[1], sp[2]);
            sm64_set_mario_velocity(m->id, 0, 0, 0);
            sm64_set_mario_forward_velocity(m->id, 0);
            if (len >= 13 && p[12]) {
                // vehicle reset: back on his feet at full health, like a reset repairs a car
                sm64_set_mario_health(m->id, 0x880);
                SM64_AUDIO_SAFE(sm64_set_mario_action(m->id, ACT_FREEFALL));
                logf_("mario reset to %.2f %.2f %.2f", ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
            }
        }
        break;
    }
    case MSG_VEHICLES: update_vehicles(p, len); break;
    case MSG_HULL: load_hull(p, len); break;
    case MSG_HURT: {
        Mario *m = mario_find(0);
        if (m && len >= 14) {
            float sp[3];
            bng2sm((const float *)p, sp);
            SM64_AUDIO_SAFE(sm64_mario_take_damage(m->id, p[12], p[13] ? 0x00000008 /* INT_SUBTYPE_BIG_KNOCKBACK */ : 0, sp[0], sp[1], sp[2]));
            if (len >= 26) {
                // thrown clear of the car (SM64's own tumble-through-the-air knockback, as from an explosion):
                // face the car and fly backwards along its direction of travel, faster the faster it was going
                float vb[3], vs[3];
                memcpy(vb, p + 14, 12);
                bng2sm(vb, vs);                                  // sm64 units per second
                for (int k = 0; k < 3; k++) vs[k] /= 30.0f;      // per frame
                float hs = sqrtf(vs[0] * vs[0] + vs[2] * vs[2]);
                const float *mp = m->state.position;
                float ax = hs > 1 ? -vs[0] : sp[0] - mp[0], az = hs > 1 ? -vs[2] : sp[2] - mp[2];   // toward the car
                float face = atan2f(ax, az);
                float speed = fminf(70.0f, fmaxf(24.0f, hs * 1.1f));
                SM64_AUDIO_SAFE(sm64_set_mario_action(m->id, ACT_THROWN_BACKWARD));
                sm64_set_mario_faceangle(m->id, face);
                sm64_set_mario_forward_velocity(m->id, -speed);
                sm64_set_mario_velocity(m->id, -sinf(face) * speed, fminf(55.0f, fmaxf(28.0f, speed * 0.6f)), -cosf(face) * speed);
                logf_("thrown by vehicle at %.0f units/frame", speed);
                // the car can already be pressed against him when the hit is noticed; the throw's first step would hit
                // it and turn into a bonk, leaving him on (or in) the car. Its collision sits out for 0.4 s so he clears it.
                Vehicle *hitter = NULL;
                float bestD = 1e18f;
                for (int i = 0; i < MAX_VEH; i++) {
                    Vehicle *v = &s_veh[i];
                    if (!v->used || v->held) continue;
                    float dx = v->center[0] - sp[0], dy = v->center[1] - sp[1], dz = v->center[2] - sp[2];
                    float d = dx * dx + dy * dy + dz * dz;
                    if (d < bestD) { bestD = d; hitter = v; }
                }
                if (hitter && bestD < 400.0f * 400.0f) {   // the car the hit came from (its centre is the source point)
                    if (!hitter->collideAfterPending) vehicle_release(hitter);
                    hitter->collideAfterPending = 1;
                    hitter->collideAfter = s_tick + 12;
                }
            }
        }
        break;
    }
    case MSG_REMOTE: {
        if (len < 32) break;
        uint32_t key;
        memcpy(&key, p, 4);
        if (key == 0) break;
        float bp[3], sp[3];
        memcpy(bp, p + 4, 12);
        bng2sm(bp, sp);
        Mario *m = mario_find(key);
        if (!m) m = mario_create(key, sp);
        if (!m) break;
        memcpy(m->rPos, sp, 12);
        memcpy(&m->rFace, p + 16, 4);
        memcpy(&m->rAction, p + 20, 4);
        memcpy(&m->rAnim, p + 24, 2);
        memcpy(&m->rFrame, p + 26, 2);
        m->lastSeen = GetTickCount();
        break;
    }
    case MSG_REMOTE_DEL: {
        uint32_t key;
        if (len < 4) break;
        memcpy(&key, p, 4);
        Mario *m = mario_find(key);
        if (m && key) mario_delete(m);
        break;
    }
    case MSG_INPUT:
        if (len >= 13) {
            memcpy(&s_script.sx, p, 4);
            memcpy(&s_script.sy, p + 4, 4);
            s_script.a = p[8]; s_script.b = p[9]; s_script.z = p[10];
            uint16_t fr;
            memcpy(&fr, p + 11, 2);
            s_script.frames = fr;
            s_script.absDir = 0;
            if (len >= 22 && (p[21] & 1)) s_scriptY = 1;           // optional trailing u8 flags (tests): 1 = Y,
            if (len >= 22 && (p[21] & 2)) s_scriptMusic = 1;       // 2 = music toggle,
            if (len >= 22 && (p[21] & 4)) s_scriptSong = 1;        // 4 = next song, 8 = previous
            if (len >= 22 && (p[21] & 8)) s_scriptSong = -1;
            if (len >= 21) {   // optional world direction (bng x, y) that "stick up" walks along
                float d[3] = { 0, 0, 0 }, sd[3];
                memcpy(d, p + 13, 8);
                bng2sm(d, sd);
                s_script.absDir = 1;
                s_script.lookX = sd[0];
                s_script.lookZ = sd[2];
            }
        }
        break;
    case MSG_CONTROL:
        if (len >= 1) {
            s_inputEnabled = p[0] != 0;
            Mario *m = mario_find(0);
            if (!s_inputEnabled && m && s_carry.active) sm64_mario_drop_held(m->id);   // he puts it down when you leave
            logf_("player %s mario", s_inputEnabled ? "controls" : "left");
        }
        break;
    case MSG_PING: { char pong = MSG_PING; send_raw(&pong, 1); break; }
    case MSG_HEAL: { Mario *m = mario_find(0); if (m && len >= 1) sm64_mario_heal(m->id, p[0]); break; }
    case MSG_FOCUS:
        if (len >= 1 && s_gameFocused != (p[0] != 0)) {
            s_gameFocused = p[0] != 0;
            logf_("game window %s", s_gameFocused ? "focused: reading the controller" : "in the background: controller ignored");
        }
        break;
    case MSG_PART_REQ: {
        if (len < 9) break;
        uint32_t key, hash;
        memcpy(&key, p, 4);
        memcpy(&hash, p + 5, 4);
        Mario *m = mario_find(key);
        if (m) send_part(m, p[4], hash);
        break;
    }
    }
}

// BeamNG's vehicle selector looks for <user>\vehicles\ng64_mario\default.png. The installer makes it; when it's
// missing (a by-hand install) a second helper process draws it, so this one's frames don't stall on the render
static void ensure_preview(const char *userPath)
{
    char png[MAX_PATH];
    snprintf(png, sizeof(png), "%s\\vehicles\\ng64_mario\\default.png", userPath);
    if (!userPath[0] || !s_exePath[0] || !s_romPathUsed[0] || GetFileAttributesA(png) != INVALID_FILE_ATTRIBUTES) return;
    char cmd[3 * MAX_PATH];
    snprintf(cmd, sizeof(cmd), "\"%s\" --write-preview \"%s\" --rom \"%s\"", s_exePath, userPath, s_romPathUsed);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logf_("drawing the vehicle selector picture into %s\\vehicles\\ng64_mario", userPath);
    }
}

int main(int argc, char **argv)
{
    const char *romPath = NULL;
    int audio = 1, port = NG64_PORT, verbose = 0;
    char exeDir[MAX_PATH], exePath[MAX_PATH];
    int watch = 0;
    const char *previewDir = NULL;   // --write-preview <user folder>: draw the vehicle selector picture and exit
    DWORD parentPid = 0;
    GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    snprintf(exePath, sizeof(exePath), "%s", exeDir);
    snprintf(s_exePath, sizeof(s_exePath), "%s", exeDir);
    char *slash = strrchr(exeDir, '\\');
    if (slash) *slash = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) romPath = argv[++i];
        else if (!strcmp(argv[i], "--no-audio")) audio = 0;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ignore-focus")) s_ignoreFocus = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--write-preview") && i + 1 < argc) previewDir = argv[++i];
        else if (!strcmp(argv[i], "--watch")) watch = 1;                  // standby: start a helper whenever BeamNG runs
        else if (!strcmp(argv[i], "--parent") && i + 1 < argc) parentPid = (DWORD)strtoul(argv[++i], NULL, 10);
    }
    if (watch) return ng64_run_watcher(exePath, exeDir);
    // one helper per port (another would only fight it for the socket), checked before its log is touched
    if (!previewDir && !ng64_single_instance(port)) return 0;
    if (!previewDir) ng64_attach_game(parentPid);   // exits with this game; started by hand, it follows whichever game runs first

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\ng64helper.log", exeDir);
    if (!previewDir) {
        // keep the previous run's log: the watcher starts a new helper every time the game starts
        char oldPath[MAX_PATH];
        snprintf(oldPath, sizeof(oldPath), "%s\\ng64helper.old.log", exeDir);
        MoveFileExA(logPath, oldPath, MOVEFILE_REPLACE_EXISTING);
    }
    if (!previewDir) s_log = fopen(logPath, "w");

    // ROM: --rom, else sm64.us.z64 next to the exe, else any path in rom.txt next to the exe
    char romBuf[MAX_PATH];
    if (!romPath) {
        snprintf(romBuf, sizeof(romBuf), "%s\\rom.txt", exeDir);
        FILE *f = fopen(romBuf, "r");
        if (f) {
            if (fgets(romBuf, sizeof(romBuf), f)) {
                romBuf[strcspn(romBuf, "\r\n")] = 0;
                romPath = romBuf;
            }
            fclose(f);
        }
    }
    if (!romPath) {
        snprintf(romBuf, sizeof(romBuf), "%s\\sm64.us.z64", exeDir);
        romPath = romBuf;
        if (GetFileAttributesA(romBuf) == INVALID_FILE_ATTRIBUTES) {
            // no sm64.us.z64: take any .z64 next to the exe that is actually SM64 (US), whatever it's called
            static char found[MAX_PATH];
            if (find_sm64_rom(exeDir, found, sizeof(found))) {
                romPath = found;
                logf_("using ROM %s", found);
            }
        }
    }
    size_t romLen = 0;
    uint8_t *rom = load_file(romPath, &romLen);
    if (!rom) {
        logf_("ERROR: could not read Super Mario 64 ROM at '%s'. Put your own US .z64 ROM next to ng64helper.exe (any name), pass --rom <path>, or write its path into rom.txt.", romPath);
        MessageBoxA(NULL, "NG64 helper could not find your Super Mario 64 (US) ROM.\n\nPut your .z64 ROM next to ng64helper.exe (any file name), or put its full path in rom.txt.", "NG64", MB_ICONERROR);
        return 1;
    }
    ng64_rom_normalize(rom, romLen);   // a .v64 or .n64 dump works too
    if (romLen != 8388608 || rom[0] != 0x80 || rom[1] != 0x37) {
        logf_("ERROR: '%s' is not a big-endian (.z64) 8 MB SM64 ROM", romPath);
        MessageBoxA(NULL, "That file is not a Super Mario 64 US .z64 ROM (8 MB, big-endian).", "NG64", MB_ICONERROR);
        return 1;
    }

    if (verbose) sm64_register_debug_print_function(debug_print);
    s_marioTex = malloc(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    sm64_global_init(rom, s_marioTex);
    // Mario's model (skeleton, body parts) comes from the ROM too: none of it is built into the helper
    if (!ng64_load_mario_from_rom(rom, romLen)) {
        logf_("ERROR: could not read Mario's model from '%s' (%s)", romPath, ng64_mario_rom_error());
        MessageBoxA(NULL, "NG64 helper could not read Mario's model from your ROM.\n\nIt needs the US version of Super Mario 64 (.z64).", "NG64", MB_ICONERROR);
        return 1;
    }
    snprintf(s_romPathUsed, sizeof(s_romPathUsed), "%s", romPath);
    if (previewDir) {
        // a standing Mario on a flat floor, drawn to <user folder>\vehicles\ng64_mario\{default,mario}.png
        struct SM64Surface floor[2] = { { 0, 0, 0, { { -2000, 0, -2000 }, { -2000, 0, 2000 }, { 2000, 0, 2000 } } },
                                        { 0, 0, 0, { { -2000, 0, -2000 }, { 2000, 0, 2000 }, { 2000, 0, -2000 } } } };
        sm64_static_surfaces_load(floor, 2);
        int32_t id = sm64_mario_create(0, 0, 0);
        struct SM64MarioGeometryBuffers geo = { 0 };
        geo.position = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        geo.normal = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        geo.color = malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        geo.uv = malloc(sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES);
        struct SM64MarioInputs in = { 0 };
        struct SM64MarioState st;
        for (int i = 0; i < 20 && id >= 0; i++) sm64_mario_tick(id, &in, &st, &geo);
        int ok = 0;
        if (id >= 0 && geo.numTrianglesUsed) {
            uint8_t *img = ng64_preview_render(&geo, s_marioTex, 960, 540);
            ok = img && ng64_preview_write(previewDir, img, 960, 540);
            free(img);
        }
        return ok ? 0 : 1;
    }
    if (audio) {
        s_audioOk = ng64_audio_start(rom);
        if (!s_audioOk) logf_("audio unavailable - continuing without sound");
    }
    logf_("libsm64 initialised from %s", romPath);
    s_hudOk = ng64_hud_extract(rom, romLen);
    if (!s_hudOk) logf_("HUD graphics not found in this ROM (not a US ROM?) - the HUD will use plain text");
    logf_("notification area icon: %s", ng64_tray_start() ? "shown" : "could not be added");
    {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\controller.ini", exeDir);
        ng64_dinput_init(di_log, ini);
    }

    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (xi) s_xinputGetState = (XInputGetStateFn)GetProcAddress(xi, "XInputGetState");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        logf_("ERROR: could not bind 127.0.0.1:%d (is another helper already running?)", port);
        return 1;
    }
    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);
    int bufSize = 1 << 20;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufSize, sizeof(bufSize));
    logf_("listening on 127.0.0.1:%d", port);

    timeBeginPeriod(1);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    const double tickSec = 1.0 / 30.0;
    double acc = 0;
    DWORD lastPacket = GetTickCount();
    uint8_t pkt[65536];

    for (;;) {
        ng64_tray_pump();
        if (ng64_game_closed()) {
            logf_("BeamNG (pid %lu) closed: helper exiting", (unsigned long)ng64_game_pid());
            ng64_tray_stop();
            if (s_audioOk) ng64_audio_stop();
            return 0;
        }
        LARGE_INTEGER drain0, drain1;
        int drainCount = 0, drainTypes[128] = { 0 };
        QueryPerformanceCounter(&drain0);
        for (;;) {
            struct sockaddr_in from;
            int fromLen = sizeof(from);
            int n = recvfrom(s_sock, (char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromLen);
            if (n <= 0) break;
            s_client = from;
            s_haveClient = 1;
            lastPacket = GetTickCount();
            LARGE_INTEGER p0, p1;
            QueryPerformanceCounter(&p0);
            handle_packet(pkt, n);
            QueryPerformanceCounter(&p1);
            double pms = (double)(p1.QuadPart - p0.QuadPart) * 1000.0 / freq.QuadPart;
            if (pms > 15) logf_("slow packet '%c' (%d bytes): %.1f ms", pkt[0], n, pms);
            drainCount++;
            drainTypes[pkt[0] & 127]++;
        }
        QueryPerformanceCounter(&drain1);
        {
            double dms = (double)(drain1.QuadPart - drain0.QuadPart) * 1000.0 / freq.QuadPart;
            if (dms > 50) {
                char types[256] = "";
                for (int t = 0; t < 128; t++)
                    if (drainTypes[t]) snprintf(types + strlen(types), sizeof(types) - strlen(types), " %c:%d", t, drainTypes[t]);
                logf_("handling %d packets took %.0f ms (%s)", drainCount, dms, types);
            }
        }

        QueryPerformanceCounter(&now);
        acc += (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;
        if (acc < tickSec) { Sleep(1); continue; }
        if (acc > 0.1) logf_("helper fell behind by %.0f ms", acc * 1000);
        if (acc > 0.25) acc = tickSec;
        acc -= tickSec;

        s_tick++;
        LARGE_INTEGER tickStart;
        QueryPerformanceCounter(&tickStart);
        Pad pad;
        read_pad(&pad);
        if (!s_inputEnabled) memset(&pad, 0, sizeof(pad));
        music_update(&pad);
        mesh_update();
        DWORD t = GetTickCount();

        for (int i = 0; i < MAX_MARIOS; i++) {
            Mario *m = &s_marios[i];
            if (!m->used) continue;
            struct SM64MarioInputs in = { 0 };
            if (m->key == 0) {
                float sx = pad.lx, sy = -pad.ly;
                uint8_t a = pad.a, b = pad.b, z = pad.z;
                int absDir = s_script.frames > 0 && s_script.absDir;
                if (s_script.frames > 0) {
                    sx = s_script.sx; sy = s_script.sy; a = s_script.a; b = s_script.b; z = s_script.z;
                    s_script.frames--;
                }
                in.stickX = sx; in.stickY = sy;
                if (s_injectB) { b = 1; s_injectB = 0; }
                s_effZ = z;
                in.buttonA = a; in.buttonB = b; in.buttonZ = z;
                in.camLookX = s_camTarget[0] - s_camPos[0];
                in.camLookZ = s_camTarget[2] - s_camPos[2];
                if (absDir) { in.camLookX = s_script.lookX; in.camLookZ = s_script.lookZ; }
                if (in.camLookX == 0 && in.camLookZ == 0) in.camLookZ = -1;
            } else {
                if (t - m->lastSeen > 3000) { mario_delete(m); continue; }
                sm64_set_mario_position(m->id, m->rPos[0], m->rPos[1], m->rPos[2]);
                sm64_set_mario_faceangle(m->id, m->rFace);
                sm64_set_mario_velocity(m->id, 0, 0, 0);
                SM64_AUDIO_SAFE(sm64_set_mario_action(m->id, m->rAction));
                sm64_set_mario_animation(m->id, m->rAnim);
                sm64_set_mario_anim_frame(m->id, m->rFrame);
                in.camLookZ = -1;
            }
            sm64_set_mario_water_level(m->id, water_level_at(m->state.position));
            SM64_AUDIO_SAFE(sm64_mario_tick(m->id, &in, &m->state, &m->geo));
            if (m->key == 0 && vehicle_push_out(m)) {
                static DWORD lastPushLog;
                if (GetTickCount() - lastPushLog > 1000) { lastPushLog = GetTickCount(); logf_("pushed mario out of a vehicle"); }
            }
            if (m->key == 0) {
                // fell through a gap in the sampled collision (e.g. outran the terrain refresh): with nothing at all
                // below him he would fall forever, so put him back on the nearest surface above
                const float *pos = m->state.position;
                struct SM64SurfaceCollisionData *below = NULL, *over = NULL;
                sm64_surface_find_floor(pos[0], pos[1] + 30, pos[2], &below);
                if (!below) {
                    float above = sm64_surface_find_floor(pos[0], pos[1] + 6000, pos[2], &over);
                    if (over) {
                        sm64_set_mario_position(m->id, pos[0], above + 1, pos[2]);
                        sm64_set_mario_velocity(m->id, 0, 0, 0);
                        logf_("rescued mario from below the collision (%.0f -> %.0f)", pos[1], above);
                    }
                }
            }
            if (m->key == 0) {
                update_camera(m, &pad, (float)tickSec);
                carry_update(m, &pad);
                check_attacks(m);
                check_landing(m);
            }
            send_frame(m);
        }
        {
            // how long simulating a tick takes (budget: 33 ms), logged every 5 s
            static double tickMsSum, tickMsMax;
            static int tickN;
            LARGE_INTEGER tickEnd;
            QueryPerformanceCounter(&tickEnd);
            double ms = (double)(tickEnd.QuadPart - tickStart.QuadPart) * 1000.0 / freq.QuadPart;
            tickMsSum += ms; if (ms > tickMsMax) tickMsMax = ms;
            if (++tickN == 150) {
                logf_("tick: %.2f ms avg, %.2f ms max (%d static surfaces)", tickMsSum / tickN, tickMsMax, s_terrainCount + s_meshCount + s_levelCount);
                if (s_audioOk) {
                    long underruns, gapMs, level;
                    ng64_audio_stats(&underruns, &gapMs, &level);
                    logf_("audio level %ld (rms)", level);
                    logf_("audio: %ld underrun(s) so far, longest wait between audio loops %ld ms", underruns, gapMs);
                }
                tickMsSum = tickMsMax = 0; tickN = 0;
            }
        }
        if (s_atlasDirty && GetTickCount() - lastPacket < 2000) {
            write_atlas();
            send_welcome(1, "atlas");
        }
    }
}
