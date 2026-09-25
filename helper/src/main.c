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
#include "protocol.h"

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
int ng64_audio_start(const uint8_t *rom);
void ng64_audio_stop(void);

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

// Height jump (m) between neighbouring samples above which the ground is treated as a ledge (flat tiles + wall)
// rather than a slope. Anything smaller is triangulated smoothly and Mario just walks/steps over it.
#define STEP_M 0.35f

static void push_quad_up(const float *b0, const float *b1, const float *b2, const float *b3)
{
    float s0[3], s1[3], s2[3], s3[3], up[3] = { 0, 1, 0 };
    bng2sm(b0, s0); bng2sm(b1, s1); bng2sm(b2, s2); bng2sm(b3, s3);
    surf_push(s0, s1, s2, up);
    surf_push(s0, s2, s3, up);
}

static void load_terrain(const uint8_t *p, int len)
{
    if (len < 14) return;
    float cx, cy, sp;
    uint16_t n;
    memcpy(&cx, p, 4); memcpy(&cy, p + 4, 4); memcpy(&sp, p + 8, 4); memcpy(&n, p + 12, 2);
    if (n < 2 || len < 14 + (int)n * n * 4) return;
    const float *h = (const float *)(p + 14);
    s_surfCount = 0;

    float half = (n - 1) * sp * 0.5f;
#define H(i, j) h[(j) * n + (i)]
#define X(i) (cx - half + (i) * sp)
#define Y(j) (cy - half + (j) * sp)
#define STEP(a, b) (!isnan(a) && !isnan(b) && fabsf((a) - (b)) > STEP_M)

    // 1) cells whose four edges are all gentle become two smooth triangles; the rest are "ledge" cells
    uint8_t *needTile = calloc((size_t)n * n, 1);
    for (int j = 0; j < n - 1; j++) {
        for (int i = 0; i < n - 1; i++) {
            float a = H(i, j), b = H(i + 1, j), c = H(i + 1, j + 1), d = H(i, j + 1);
            int ledge = STEP(a, b) || STEP(b, c) || STEP(c, d) || STEP(d, a);
            if (!ledge) {
                float up[3] = { 0, 1, 0 };
                float pa[3] = { X(i), Y(j), a }, pb[3] = { X(i + 1), Y(j), b }, pc[3] = { X(i + 1), Y(j + 1), c }, pd[3] = { X(i), Y(j + 1), d };
                float sa[3], sb[3], sc[3], sd[3];
                bng2sm(pa, sa); bng2sm(pb, sb); bng2sm(pc, sc); bng2sm(pd, sd);
                if (!isnan(a) && !isnan(b) && !isnan(c)) surf_push(sa, sb, sc, up);
                if (!isnan(a) && !isnan(c) && !isnan(d)) surf_push(sa, sc, sd, up);
            } else {
                needTile[j * n + i] = needTile[j * n + i + 1] = needTile[(j + 1) * n + i] = needTile[(j + 1) * n + i + 1] = 1;
            }
        }
    }

    // 2) every corner of a ledge cell gets a flat tile of its own height (half a cell each way), so the top of a
    //    ledge reaches right up to the wall and Mario has something to land on / grab
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            float z = H(i, j);
            if (!needTile[j * n + i] || isnan(z)) continue;
            float x0 = X(i) - sp * 0.5f, x1 = X(i) + sp * 0.5f, y0 = Y(j) - sp * 0.5f, y1 = Y(j) + sp * 0.5f;
            float q0[3] = { x0, y0, z }, q1[3] = { x1, y0, z }, q2[3] = { x1, y1, z }, q3[3] = { x0, y1, z };
            push_quad_up(q0, q1, q2, q3);
        }
    }

    // 3) vertical walls on the tile boundary between neighbours with a real step, facing the low side
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            for (int d = 0; d < 2; d++) {
                int i2 = i + (d == 0), j2 = j + (d == 1);
                if (i2 >= n || j2 >= n) continue;
                float h1 = H(i, j), h2 = H(i2, j2);
                if (!STEP(h1, h2)) continue;
                float lo = fminf(h1, h2), hi = fmaxf(h1, h2);
                float mx = (X(i) + X(i2)) * 0.5f, my = (Y(j) + Y(j2)) * 0.5f;
                float ex = (d == 0) ? 0 : sp * 0.5f, ey = (d == 0) ? sp * 0.5f : 0;
                float b0[3] = { mx - ex, my - ey, lo }, b1[3] = { mx + ex, my + ey, lo };
                float b2[3] = { mx + ex, my + ey, hi }, b3[3] = { mx - ex, my - ey, hi };
                float s0[3], s1[3], s2[3], s3[3];
                bng2sm(b0, s0); bng2sm(b1, s1); bng2sm(b2, s2); bng2sm(b3, s3);
                float dirB[3] = { (float)(i2 - i), (float)(j2 - j), 0 };
                if (h2 > h1) { dirB[0] = -dirB[0]; dirB[1] = -dirB[1]; }
                float hint[3];
                bng2sm(dirB, hint);
                surf_push(s0, s1, s2, hint);
                surf_push(s0, s2, s3, hint);
            }
        }
    }
    free(needTile);
#undef H
#undef X
#undef Y
#undef STEP
    sm64_static_surfaces_load(s_surfBuf, s_surfCount);
}

#define MAX_VEH 64
typedef struct {
    int used;
    uint32_t vehId;
    uint32_t objId;
    int isHull;        // objId is the node-derived hull rather than the bounding box
    float half[3];     // sm64 half extents (bounding box; attacks use it either way)
    float center[3];   // sm64
    float axes[3][3];  // sm64 unit axes (rows)
    float framePos[3]; // sm64, vehicle reference frame origin
    float frameRows[3][3];
    DWORD lastSeen;
    int hitCooldown;
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
    } else {
        memcpy(t.position, veh->center, 12);
        axes_to_euler(veh->axes, t.eulerRotation);
    }
    sm64_surface_object_move(veh->objId, &t);
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
        int rebuild = !veh;
        if (veh && !veh->isHull)
            for (int a = 0; a < 3; a++) if (fabsf(veh->half[a] - half[a]) > 8.0f) rebuild = 1;
        if (!veh && !freeSlot) continue;
        if (!veh) { veh = freeSlot; memset(veh, 0, sizeof(*veh)); }
        memcpy(veh->center, center, 12);
        memcpy(veh->axes, axes, sizeof(axes));
        memcpy(veh->half, half, 12);
        bng2sm(org, veh->framePos);
        frame_rows(fwdB, upB, veh->frameRows);
        if (rebuild) {
            // bounding box until the vehicle sends its hull
            if (veh->used) sm64_surface_object_delete(veh->objId);
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
            sm64_surface_object_delete(s_veh[i].objId);
            s_veh[i].used = 0;
        }
    }
}

// Hull from the vehicle's own nodes: a grid (vehicle frame) of the highest node in each cell. Each cell is a flat
// tile with walls down to its lower neighbour (or the underside), so roofs, beds, hoods and bumpers sit where they
// really are instead of inside one big box.
static void load_hull(const uint8_t *p, int len)
{
    if (len < 24) return;
    uint32_t id;
    float cell, x0, y0, bottom;
    uint16_t nx, ny;
    memcpy(&id, p, 4); memcpy(&cell, p + 4, 4); memcpy(&x0, p + 8, 4); memcpy(&y0, p + 12, 4); memcpy(&bottom, p + 16, 4);
    memcpy(&nx, p + 20, 2); memcpy(&ny, p + 22, 2);
    if (nx < 1 || ny < 1 || nx > 64 || ny > 64 || len < 24 + nx * ny * 4) return;
    const float *top = (const float *)(p + 24);
    Vehicle *veh = vehicle_find(id);
    if (!veh) return;

    static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    float upH[3] = { 0, 1, 0 };
    int saved = s_surfCount;
#define T(i, j) (((i) < 0 || (j) < 0 || (i) >= nx || (j) >= ny) ? NAN : top[(j) * nx + (i)])
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float t = T(i, j);
            if (isnan(t)) continue;
            float xa = x0 + i * cell, xb = xa + cell, ya = y0 + j * cell, yb = ya + cell;
            float q0[3] = { xa, ya, t }, q1[3] = { xb, ya, t }, q2[3] = { xb, yb, t }, q3[3] = { xa, yb, t };
            float s0[3], s1[3], s2[3], s3[3];
            bng2sm(q0, s0); bng2sm(q1, s1); bng2sm(q2, s2); bng2sm(q3, s3);
            surf_push(s0, s1, s2, upH);
            surf_push(s0, s2, s3, upH);
            for (int d = 0; d < 4; d++) {
                float tn = T(i + dirs[d][0], j + dirs[d][1]);
                float lo = isnan(tn) ? bottom : fmaxf(tn, bottom);
                if (t - lo < 0.02f) continue;
                float e0x, e0y, e1x, e1y;
                if (dirs[d][0] == 1)       { e0x = xb; e0y = ya; e1x = xb; e1y = yb; }
                else if (dirs[d][0] == -1) { e0x = xa; e0y = ya; e1x = xa; e1y = yb; }
                else if (dirs[d][1] == 1)  { e0x = xa; e0y = yb; e1x = xb; e1y = yb; }
                else                       { e0x = xa; e0y = ya; e1x = xb; e1y = ya; }
                float w0[3] = { e0x, e0y, lo }, w1[3] = { e1x, e1y, lo }, w2[3] = { e1x, e1y, t }, w3[3] = { e0x, e0y, t };
                float hb[3] = { (float)dirs[d][0], (float)dirs[d][1], 0 }, hs[3];
                bng2sm(hb, hs);
                bng2sm(w0, s0); bng2sm(w1, s1); bng2sm(w2, s2); bng2sm(w3, s3);
                surf_push(s0, s1, s2, hs);
                surf_push(s0, s2, s3, hs);
            }
        }
    }
#undef T
    int n = s_surfCount - saved;
    if (n == 0) return;
    struct SM64SurfaceObject obj = { 0 };
    obj.surfaceCount = n;
    obj.surfaces = malloc(sizeof(struct SM64Surface) * n);
    memcpy(obj.surfaces, s_surfBuf + saved, sizeof(struct SM64Surface) * n);
    s_surfCount = saved;
    memcpy(obj.transform.position, veh->framePos, 12);
    axes_to_euler(veh->frameRows, obj.transform.eulerRotation);
    sm64_surface_object_delete(veh->objId);
    veh->objId = sm64_surface_object_create(&obj);
    veh->isHull = 1;
    free(obj.surfaces);
    logf_("vehicle %u hull: %dx%d cells, %d surfaces", id, nx, ny, n);
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

typedef struct { float lx, ly, rx, ry; int a, b, z, zoomIn, zoomOut; } Pad;

static float deadzone(SHORT v, SHORT dz)
{
    if (v > dz) return (v - dz) / (32767.0f - dz);
    if (v < -dz) return (v + dz) / (32768.0f - dz);
    return 0;
}

static int beamng_focused(void)
{
    HWND w = GetForegroundWindow();
    char title[256] = { 0 };
    if (!w) return 0;
    GetWindowTextA(w, title, sizeof(title));
    return strstr(title, "BeamNG") != NULL;
}

static int s_ignoreFocus;

static void read_pad(Pad *p)
{
    memset(p, 0, sizeof(*p));
    if (!s_ignoreFocus && !beamng_focused()) return;
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
            break;
        }
    }
#define KEY(k) ((GetAsyncKeyState(k) & 0x8000) != 0)
    if (KEY('W')) p->ly = 1;
    if (KEY('S')) p->ly = -1;
    if (KEY('A')) p->lx = -1;
    if (KEY('D')) p->lx = 1;
    if (KEY(VK_SPACE)) p->a = 1;
    if (KEY('J')) p->b = 1;
    if (KEY('K')) p->z = 1;
    if (KEY(VK_LEFT)) p->rx = -1;
    if (KEY(VK_RIGHT)) p->rx = 1;
    if (KEY(VK_UP)) p->ry = 1;
    if (KEY(VK_DOWN)) p->ry = -1;
#undef KEY
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
    s_camPitch += pad->ry * 1.2f * dt;
    if (s_camPitch < -0.3f) s_camPitch = -0.3f;
    if (s_camPitch > 1.2f) s_camPitch = 1.2f;
    if (pad->zoomIn) s_camDist -= 600 * dt;
    if (pad->zoomOut) s_camDist += 600 * dt;
    if (s_camDist < 300) s_camDist = 300;
    if (s_camDist > 2000) s_camDist = 2000;

    if (fabsf(pad->rx) < 0.1f && st->forwardVelocity > 8.0f) {
        float behind = st->faceAngle + PI;
        s_camYaw += angdiff(behind, s_camYaw) * fminf(1.0f, 0.9f * dt * st->forwardVelocity / 32.0f);
    }
    s_camTarget[0] = st->position[0];
    s_camTarget[1] = st->position[1] + 100;
    s_camTarget[2] = st->position[2];
    s_camPos[0] = s_camTarget[0] + sinf(s_camYaw) * cosf(s_camPitch) * s_camDist;
    s_camPos[1] = s_camTarget[1] + sinf(s_camPitch) * s_camDist;
    s_camPos[2] = s_camTarget[2] + cosf(s_camYaw) * cosf(s_camPitch) * s_camDist;
}

// ---------------------------------------------------------------------------------------------------------------

static PackedVert s_vertBuf[SM64_GEO_MAX_TRIANGLES * 3];
static uint32_t s_seq;

static void send_frame(Mario *m)
{
    FrameHeader h;
    const struct SM64MarioState *st = &m->state;
    float origin[3];
    if (m->key == 0) memcpy(origin, st->position, 12);
    else memcpy(origin, m->rPos, 12);

    memset(&h, 0, sizeof(h));
    h.type = MSG_FRAME;
    h.key = m->key;
    h.seq = ++s_seq;
    sm2bng(origin, h.pos);
    float vel[3] = { st->velocity[0] * 30, st->velocity[1] * 30, st->velocity[2] * 30 };
    sm2bng(vel, h.vel);
    h.faceAngle = st->faceAngle;
    h.health = st->health;
    h.action = st->action;
    h.animId = (int16_t)st->animID;
    h.animFrame = st->animFrame;
    h.flags = st->flags;
    if (m->key == 0) {
        sm2bng(s_camPos, h.camPos);
        sm2bng(s_camTarget, h.camTarget);
    }

    int nv = m->geo.numTrianglesUsed * 3;
    PackedVert *verts = s_vertBuf;
    for (int i = 0; i < nv; i++) {
        float rel[3] = { m->geo.position[i * 3] - origin[0], m->geo.position[i * 3 + 1] - origin[1], m->geo.position[i * 3 + 2] - origin[2] };
        float b[3];
        sm2bng(rel, b);
        for (int k = 0; k < 3; k++) {
            float mm = b[k] * 1000.0f;
            if (mm > 32767) mm = 32767;
            if (mm < -32768) mm = -32768;
            verts[i].p[k] = (int16_t)lroundf(mm);
        }
        const float *sn = &m->geo.normal[i * 3];
        float bn[3] = { sn[0], -sn[2], sn[1] };
        for (int k = 0; k < 3; k++) verts[i].n[k] = (int8_t)lroundf(fmaxf(-1, fminf(1, bn[k])) * 127);

        int band = band_for_color(&m->geo.color[i * 3]);
        float u = m->geo.uv[i * 2], v = m->geo.uv[i * 2 + 1];
        float au, av;
        if (u >= 1.0f && v >= 1.0f) {   // untextured triangle: solid patch
            au = (SOLID_X0 + 8) / (float)ATLAS_W;
            av = (band * BAND_H + BAND_H * 0.5f) / (float)ATLAS_H;
        } else {
            au = u * SM64_TEXTURE_WIDTH / (float)ATLAS_W;
            av = (band * BAND_H + 0.5f + v * (BAND_H - 1)) / (float)ATLAS_H;   // inset so filtering can't bleed into the next band
        }
        verts[i].uv[0] = (uint16_t)lroundf(fmaxf(0, fminf(1, au)) * 65535);
        verts[i].uv[1] = (uint16_t)lroundf(fmaxf(0, fminf(1, av)) * 65535);
    }
    h.numVerts = (uint16_t)nv;
    send_raw(&h, sizeof(h));

    static uint8_t chunk[sizeof(ChunkHeader) + NG64_CHUNK_VERTS * sizeof(PackedVert)];
    for (int start = 0; start < nv; start += NG64_CHUNK_VERTS) {
        int count = nv - start < NG64_CHUNK_VERTS ? nv - start : NG64_CHUNK_VERTS;
        ChunkHeader *c = (ChunkHeader *)chunk;
        c->type = MSG_CHUNK;
        c->key = m->key;
        c->seq = h.seq;
        c->start = (uint16_t)start;
        c->count = (uint16_t)count;
        memcpy(chunk + sizeof(ChunkHeader), verts + start, count * sizeof(PackedVert));
        send_raw(chunk, (int)(sizeof(ChunkHeader) + count * sizeof(PackedVert)));
    }
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
        send_welcome(1, "ok");
        logf_("hello from mod, user path %s", s_userPath);
        break;
    }
    case MSG_TERRAIN: load_terrain(p, len); break;
    case MSG_SPAWN: if (len >= 12) spawn_local((const float *)p); break;
    case MSG_DESPAWN: { Mario *m = mario_find(0); if (m) mario_delete(m); break; }
    case MSG_TELEPORT: {
        Mario *m = mario_find(0);
        if (m && len >= 12) { float sp[3]; bng2sm((const float *)p, sp); sm64_set_mario_position(m->id, sp[0], sp[1], sp[2]); sm64_set_mario_velocity(m->id, 0, 0, 0); }
        break;
    }
    case MSG_VEHICLES: update_vehicles(p, len); break;
    case MSG_HULL: load_hull(p, len); break;
    case MSG_HURT: {
        Mario *m = mario_find(0);
        if (m && len >= 14) {
            float sp[3];
            bng2sm((const float *)p, sp);
            sm64_mario_take_damage(m->id, p[12], p[13] ? 0x00000008 /* INT_SUBTYPE_BIG_KNOCKBACK */ : 0, sp[0], sp[1], sp[2]);
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
    case MSG_PING: { char pong = MSG_PING; send_raw(&pong, 1); break; }
    }
}

int main(int argc, char **argv)
{
    const char *romPath = NULL;
    int audio = 1, port = NG64_PORT, verbose = 0;
    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    char *slash = strrchr(exeDir, '\\');
    if (slash) *slash = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) romPath = argv[++i];
        else if (!strcmp(argv[i], "--no-audio")) audio = 0;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ignore-focus")) s_ignoreFocus = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    }

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\ng64helper.log", exeDir);
    s_log = fopen(logPath, "w");

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
    }
    size_t romLen = 0;
    uint8_t *rom = load_file(romPath, &romLen);
    if (!rom) {
        logf_("ERROR: could not read Super Mario 64 ROM at '%s'. Put your own US .z64 ROM there, pass --rom <path>, or write its path into rom.txt next to ng64helper.exe.", romPath);
        MessageBoxA(NULL, "NG64 helper could not find your Super Mario 64 (US) ROM.\n\nPlace it next to ng64helper.exe as sm64.us.z64, or put its full path in rom.txt.", "NG64", MB_ICONERROR);
        return 1;
    }
    if (romLen != 8388608 || rom[0] != 0x80 || rom[1] != 0x37) {
        logf_("ERROR: '%s' is not a big-endian (.z64) 8 MB SM64 ROM", romPath);
        MessageBoxA(NULL, "That file is not a Super Mario 64 US .z64 ROM (8 MB, big-endian).", "NG64", MB_ICONERROR);
        return 1;
    }

    if (verbose) sm64_register_debug_print_function(debug_print);
    s_marioTex = malloc(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    sm64_global_init(rom, s_marioTex);
    if (audio && !ng64_audio_start(rom)) logf_("audio unavailable - continuing without sound");
    logf_("libsm64 initialised from %s", romPath);

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
        for (;;) {
            struct sockaddr_in from;
            int fromLen = sizeof(from);
            int n = recvfrom(s_sock, (char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromLen);
            if (n <= 0) break;
            s_client = from;
            s_haveClient = 1;
            lastPacket = GetTickCount();
            handle_packet(pkt, n);
        }

        QueryPerformanceCounter(&now);
        acc += (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;
        if (acc < tickSec) { Sleep(1); continue; }
        if (acc > 0.25) acc = tickSec;
        acc -= tickSec;

        Pad pad;
        read_pad(&pad);
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
                sm64_set_mario_action(m->id, m->rAction);
                sm64_set_mario_animation(m->id, m->rAnim);
                sm64_set_mario_anim_frame(m->id, m->rFrame);
                in.camLookZ = -1;
            }
            sm64_mario_tick(m->id, &in, &m->state, &m->geo);
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
                check_attacks(m);
            }
            send_frame(m);
        }
        if (s_atlasDirty && GetTickCount() - lastPacket < 2000) {
            write_atlas();
            send_welcome(1, "atlas");
        }
    }
}
