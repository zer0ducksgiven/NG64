// SM64 objects read from the player's own ROM (see objrom.h). The ROM holds them as the N64 does: Fast3D display lists,
// vertices and textures in MIO0-compressed banks (segments 3, 4, 6, 8) and geo layouts as big-endian geo commands in
// raw segments (0x0D, 0x0F, 0x16). Where things are in the US ROM is written down here; nothing of Nintendo's is built in.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "objrom.h"
#include "decomp/tools/libmio0.h"

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);

#define SM64_SCALE 0.0085f   // metres per SM64 unit (as NG64_SCALE)

// ---- where things are in the US ROM ------------------------------------------------------------------------------
static const struct { int seg; uint32_t rom; } k_banks[] = {   // MIO0 banks
    { 3, 0x201410 },    // common1: coin, star, caps, explosion
    { 4, 0x114750 },    // Mario
    { 6, 0x1C4230 },    // group 14: koopa
    { 8, 0x1F2200 },    // common0: goomba, bob-omb, koopa shell
};
static const struct { int seg; uint32_t rom, size; } k_raws[] = {   // geo layouts
    { 0x0D, 0x1D7C90, 1664 },   // group 14 geo
    { 0x0F, 0x2008D0, 2880 },   // common0 geo
    { 0x16, 0x218DA0, 4192 },   // common1 geo
};
static const struct { int model; uint32_t geo; } k_models[OM_COUNT] = {
    [OM_COIN_YELLOW] = { OM_COIN_YELLOW, 0x1600013C }, [OM_COIN_RED] = { OM_COIN_RED, 0x160002C4 },
    [OM_COIN_BLUE] = { OM_COIN_BLUE, 0x16000200 }, [OM_STAR] = { OM_STAR, 0x16000EA0 },
    [OM_STAR_TRANSPARENT] = { OM_STAR_TRANSPARENT, 0x16000F6C },
    [OM_CAP_METAL] = { OM_CAP_METAL, 0x16000CF0 }, [OM_CAP_WING] = { OM_CAP_WING, 0x16000D3C },
    [OM_GOOMBA] = { OM_GOOMBA, 0x0F0006E4 }, [OM_BOBOMB] = { OM_BOBOMB, 0x0F0007B8 },
    [OM_KOOPA] = { OM_KOOPA, 0x0D000214 }, [OM_KOOPA_SHELL] = { OM_KOOPA_SHELL, 0x0F000AB0 },
    [OM_EXPLOSION] = { OM_EXPLOSION, 0x16000040 }, [OM_KOOPA_NOSHELL] = { OM_KOOPA_NOSHELL, 0x0D0000D0 },
};

static const uint8_t *s_rom;
static size_t s_romLen;
static uint8_t *s_bank[32];
static uint32_t s_bankSize[32];
static char s_error[200];
static int s_failed;

static void fail(const char *what, uint32_t addr)
{
    if (!s_failed) snprintf(s_error, sizeof(s_error), "%s at %08X", what, addr);
    s_failed = 1;
}
const char *objrom_error(void) { return s_error; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int16_t be16(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

static const uint8_t *segp(uint32_t a, uint32_t len)
{
    int seg = a >> 24;
    uint32_t off = a & 0xFFFFFF;
    if (seg < 32 && s_bank[seg]) {
        if (off + len > s_bankSize[seg]) { fail("address outside its bank", a); return NULL; }
        return s_bank[seg] + off;
    }
    for (size_t i = 0; i < sizeof(k_raws) / sizeof(k_raws[0]); i++)
        if (k_raws[i].seg == seg) {
            if (off + len > k_raws[i].size) { fail("address outside its geo segment", a); return NULL; }
            return s_rom + k_raws[i].rom + off;
        }
    fail("unmapped segment", a);
    return NULL;
}

// ---- atlas ------------------------------------------------------------------------------------------------------------
#define ATLAS_W 1024
#define TEX_MARGIN 6    // texels of replicated border around each texture: clamped (and slightly wrapped) coordinates land in it
static uint8_t *s_atlas;      // RGBA
static int s_atlasH = 1024, s_shelfY, s_shelfX, s_shelfH;

typedef struct { uint64_t key; int x, y, w, h; } Rect;
static Rect s_rects[1500];
static int s_nRects;

static int atlas_alloc(int w, int h, int *x, int *y)   // w,h include padding
{
    if (s_shelfX + w > ATLAS_W) { s_shelfY += s_shelfH; s_shelfX = 0; s_shelfH = 0; }
    if (s_shelfY + h > s_atlasH) return 0;
    *x = s_shelfX; *y = s_shelfY;
    s_shelfX += w;
    if (h > s_shelfH) s_shelfH = h;
    return 1;
}

static void atlas_put(int x, int y, int w, int h, const uint8_t *rgba, int pad)   // w*h image at (x+pad, y+pad), edges replicated
{
    for (int j = -pad; j < h + pad; j++)
        for (int i = -pad; i < w + pad; i++) {
            int si = i < 0 ? 0 : i >= w ? w - 1 : i, sj = j < 0 ? 0 : j >= h ? h - 1 : j;
            memcpy(s_atlas + ((size_t)(y + pad + j) * ATLAS_W + (x + pad + i)) * 4, rgba + ((size_t)sj * w + si) * 4, 4);
        }
}

static Rect *rect_find(uint64_t key)
{
    for (int i = 0; i < s_nRects; i++) if (s_rects[i].key == key) return &s_rects[i];
    return NULL;
}

// a solid colour: a 2x2 cell
static Rect *atlas_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint64_t key = (1ull << 62) | (uint64_t)r << 24 | (uint64_t)g << 16 | (uint64_t)b << 8 | a;
    Rect *e = rect_find(key);
    if (e) return e;
    int x, y;
    if (s_nRects >= 1500 || !atlas_alloc(4, 4, &x, &y)) { fail("atlas full", 0); return NULL; }
    uint8_t one[4] = { r, g, b, a };
    atlas_put(x, y, 1, 1, one, 1);
    atlas_put(x + 1, y + 1, 1, 1, one, 1);
    e = &s_rects[s_nRects++];
    e->key = key; e->x = x + 1; e->y = y + 1; e->w = 1; e->h = 1;
    return e;
}

// ---- texture decoding ---------------------------------------------------------------------------------------------------
enum { FMT_RGBA = 0, FMT_YUV = 1, FMT_CI = 2, FMT_IA = 3, FMT_I = 4 };

static uint8_t expand5(int v) { return (uint8_t)((v << 3) | (v >> 2)); }

static int decode_texture(uint32_t addr, int fmt, int siz, int w, int h, uint8_t *out)   // RGBA8, w*h
{
    int bpp = siz == 0 ? 4 : siz == 1 ? 8 : siz == 2 ? 16 : 32;
    const uint8_t *src = segp(addr, (uint32_t)((w * h * bpp + 7) / 8));
    if (!src) return 0;
    for (int i = 0; i < w * h; i++) {
        uint8_t *o = out + i * 4;
        if (fmt == FMT_RGBA && siz == 2) {
            uint16_t c = (uint16_t)(src[i * 2] << 8 | src[i * 2 + 1]);
            o[0] = expand5(c >> 11 & 31); o[1] = expand5(c >> 6 & 31); o[2] = expand5(c >> 1 & 31); o[3] = (c & 1) ? 255 : 0;
        } else if (fmt == FMT_RGBA && siz == 3) {
            memcpy(o, src + i * 4, 4);
        } else if (fmt == FMT_IA && siz == 2) {
            o[0] = o[1] = o[2] = src[i * 2]; o[3] = src[i * 2 + 1];
        } else if (fmt == FMT_IA && siz == 1) {
            int v = src[i], in = v >> 4, a = v & 15;
            o[0] = o[1] = o[2] = (uint8_t)(in * 17); o[3] = (uint8_t)(a * 17);
        } else if (fmt == FMT_IA && siz == 0) {
            int v = (src[i / 2] >> (i & 1 ? 0 : 4)) & 15, in = v >> 1, a = v & 1;
            o[0] = o[1] = o[2] = (uint8_t)(in * 255 / 7); o[3] = a ? 255 : 0;
        } else if (fmt == FMT_I && siz == 1) {
            o[0] = o[1] = o[2] = o[3] = src[i];
        } else if (fmt == FMT_I && siz == 0) {
            int v = (src[i / 2] >> (i & 1 ? 0 : 4)) & 15;
            o[0] = o[1] = o[2] = o[3] = (uint8_t)(v * 17);
        } else {
            fail("texture format not handled", addr);
            return 0;
        }
    }
    return 1;
}

// a texture, tinted by a colour (lit surfaces take their light's colour), in the atlas; with 1 px of replicated border
static Rect *atlas_texture(uint32_t addr, int fmt, int siz, int w, int h, const uint8_t tint[3], int opaque)
{
    uint64_t key = (uint64_t)addr << 30 ^ ((uint64_t)fmt << 27) ^ ((uint64_t)siz << 25) ^ ((uint64_t)w << 17) ^ ((uint64_t)h << 9)
                 ^ ((uint64_t)tint[0] << 40) ^ ((uint64_t)tint[1] << 48) ^ ((uint64_t)tint[2] << 56) ^ (opaque ? 1 : 0);
    key &= ~(1ull << 62);
    Rect *e = rect_find(key);
    if (e) return e;
    int x, y;
    if (s_nRects >= 1500 || !atlas_alloc(w + 2 * TEX_MARGIN, h + 2 * TEX_MARGIN, &x, &y)) { fail("atlas full", addr); return NULL; }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!decode_texture(addr, fmt, siz, w, h, px)) { free(px); return NULL; }
    for (int i = 0; i < w * h; i++) {
        px[i * 4] = (uint8_t)(px[i * 4] * tint[0] / 255);
        px[i * 4 + 1] = (uint8_t)(px[i * 4 + 1] * tint[1] / 255);
        px[i * 4 + 2] = (uint8_t)(px[i * 4 + 2] * tint[2] / 255);
        if (opaque) px[i * 4 + 3] = 255;
    }
    atlas_put(x, y, w, h, px, TEX_MARGIN);
    free(px);
    e = &s_rects[s_nRects++];
    e->key = key; e->x = x + TEX_MARGIN; e->y = y + TEX_MARGIN; e->w = w; e->h = h;
    return e;
}

// ---- display list -> piece ----------------------------------------------------------------------------------------------
#define MAX_PIECES 160
static ObjPiece s_pieces[MAX_PIECES];
static int s_nPieces;
static struct { uint32_t dl; int alpha; int piece; } s_pieceKeys[MAX_PIECES];

typedef struct { int16_t p[3]; uint8_t c[3]; uint8_t a; int16_t s, t; } SVtx;

typedef struct {
    uint32_t timg; int timgFmt, timgSiz;
    uint32_t loaded;           // image address of the last LOADBLOCK
    struct { int fmt, siz, cms, cmt, maskS, maskT, w, h; } tile[8];
    int texOn; uint32_t scaleS, scaleT;
    uint32_t geom;
    uint8_t diffuse[3], ambient[3];
    SVtx vtx[16];
    ObjPiece *piece;
    int opaque;
    int cap, capi;
    uint32_t root;
} GfxState;

static void piece_add_vertex(GfxState *g, const SVtx *v, Rect *rc, float s, float t, int lit, int *outIdx)
{
    ObjPiece *pc = g->piece;
    ObjVert o;
    memset(&o, 0, sizeof(o));
    o.p[0] = v->p[0]; o.p[1] = v->p[1]; o.p[2] = v->p[2];
    if (lit) { o.n[0] = (int8_t)v->c[0]; o.n[1] = (int8_t)v->c[1]; o.n[2] = (int8_t)v->c[2]; }
    else { o.n[0] = 0; o.n[1] = 0; o.n[2] = 127; }
    float u, w;
    if (rc->w == 1 && rc->h == 1) { u = rc->x + 0.5f; w = rc->y + 0.5f; }
    else {
        // clamp inside the texture (half a texel in from the edges)
        float lo = 0.5f - TEX_MARGIN, hiS = rc->w + TEX_MARGIN - 0.5f, hiT = rc->h + TEX_MARGIN - 0.5f;
        float ss = s < lo ? lo : s > hiS ? hiS : s;
        float tt = t < lo ? lo : t > hiT ? hiT : t;
        u = rc->x + ss; w = rc->y + tt;
    }
    o.uv[0] = (uint16_t)(u / ATLAS_W * 65535.0f + 0.5f);
    o.uv[1] = (uint16_t)(w / s_atlasH * 65535.0f + 0.5f);
    for (int i = 0; i < pc->nv; i++)
        if (!memcmp(&pc->v[i], &o, sizeof(o))) { *outIdx = i; return; }
    pc->v = realloc(pc->v, sizeof(ObjVert) * (pc->nv + 1));
    pc->v[pc->nv] = o;
    *outIdx = pc->nv++;
}

static int s_oorTris;

static void emit_tri(GfxState *g, int a, int b, int c)
{
    int idx[3], ids[3] = { a, b, c };
    int lit = (g->geom & 0x20000) != 0;
    SVtx *vs[3] = { &g->vtx[a], &g->vtx[b], &g->vtx[c] };
    Rect *rc;
    float st[3][2] = { { 0 } };
    uint8_t white[3] = { 255, 255, 255 };
    int textured = g->texOn && g->loaded;
    if (textured) {
        int t = 0;
        int w = g->tile[t].w, h = g->tile[t].h;
        if (w <= 0) w = g->tile[t].maskS ? 1 << g->tile[t].maskS : 32;
        if (h <= 0) h = g->tile[t].maskT ? 1 << g->tile[t].maskT : 32;
        int texgen = (g->geom & 0x40000) != 0;
        // lit: the light's colour tints the texture; unlit: the vertex colour does (taken from the first vertex)
        const uint8_t *tint = lit ? g->diffuse : texgen ? white : vs[0]->c;
        rc = atlas_texture(g->loaded, g->tile[t].fmt, g->tile[t].siz, w, h, tint, g->opaque);
        if (!rc) return;
        for (int i = 0; i < 3; i++) {
            if (texgen) {
                // environment mapping: the texture coordinates come from the normal (here in model space, the view isn't known)
                st[i][0] = ((int8_t)vs[i]->c[0] / 127.0f * 0.5f + 0.5f) * w;
                st[i][1] = (0.5f - (int8_t)vs[i]->c[1] / 127.0f * 0.5f) * h;
                continue;
            }
            st[i][0] = vs[i]->s * (g->scaleS / 65536.0f) / 32.0f;
            st[i][1] = vs[i]->t * (g->scaleT / 65536.0f) / 32.0f;
            if (st[i][0] < -0.01f || st[i][0] > w + 0.01f || st[i][1] < -0.01f || st[i][1] > h + 0.01f) { s_oorTris++;
#ifdef OBJROM_TEST
                if (s_oorTris < 40) printf("oor: dl %08X scale %X/%X raw %d,%d tex %08X %dx%d fmt%d/%d cms%d cmt%d s=%.1f t=%.1f\n", g->root, g->scaleS, g->scaleT, vs[i]->s, vs[i]->t, g->loaded, w, h, g->tile[0].fmt, g->tile[0].siz, g->tile[0].cms, g->tile[0].cmt, st[i][0], st[i][1]);
#endif
                break; }
        }
    } else if (lit) {
        rc = atlas_color(g->diffuse[0], g->diffuse[1], g->diffuse[2], 255);
        if (!rc) return;
    } else {
        // unlit and untextured: the vertex colour (the first vertex's, for the whole triangle)
        rc = atlas_color(vs[0]->c[0], vs[0]->c[1], vs[0]->c[2], vs[0]->a ? vs[0]->a : 255);
        if (!rc) return;
    }
    (void)ids;
    if (lit) g->piece->litTris++; else g->piece->unlitTris++;
    for (int i = 0; i < 3; i++) piece_add_vertex(g, vs[i], rc, st[i][0], st[i][1], lit, &idx[i]);
    ObjPiece *pc = g->piece;
    pc->idx = realloc(pc->idx, sizeof(uint16_t) * (pc->ni + 3));
    pc->idx[pc->ni++] = (uint16_t)idx[0]; pc->idx[pc->ni++] = (uint16_t)idx[1]; pc->idx[pc->ni++] = (uint16_t)idx[2];
}

static void run_dl(uint32_t addr, GfxState *g, int depth)
{
    if (depth > 8) { fail("display list nesting", addr); return; }
    for (uint32_t a = addr;; a += 8) {
        const uint8_t *c = segp(a, 8);
        if (!c) return;
        uint32_t w0 = be32(c), w1 = be32(c + 4);
        switch (w0 >> 24) {
        case 0x04: {   // G_VTX
            int n = ((w0 >> 20) & 0xF) + 1, v0 = (w0 >> 16) & 0xF;
            const uint8_t *p = segp(w1, (uint32_t)n * 16);
            if (!p) return;
            for (int i = 0; i < n && v0 + i < 16; i++, p += 16) {
                SVtx *v = &g->vtx[v0 + i];
                v->p[0] = be16(p); v->p[1] = be16(p + 2); v->p[2] = be16(p + 4);
                v->s = be16(p + 8); v->t = be16(p + 10);
                v->c[0] = p[12]; v->c[1] = p[13]; v->c[2] = p[14]; v->a = p[15];
            }
            break;
        }
        case 0xBF:     // G_TRI1: indices * 10
            emit_tri(g, ((w1 >> 16) & 0xFF) / 10, ((w1 >> 8) & 0xFF) / 10, (w1 & 0xFF) / 10);
            break;
        case 0xB5:     // G_QUAD (a quad as two triangles)
            { int i0 = ((w1 >> 24) & 0xFF) / 10, i1 = ((w1 >> 16) & 0xFF) / 10, i2 = ((w1 >> 8) & 0xFF) / 10, i3 = (w1 & 0xFF) / 10;
              emit_tri(g, i0, i1, i2); emit_tri(g, i0, i2, i3); }
            break;
        case 0x03: {   // G_MOVEMEM: light 0 (0x86) is the diffuse colour, light 1 (0x88) the ambient
            uint32_t which = (w0 >> 16) & 0xFF;
            if (which == 0x86 || which == 0x88) {
                const uint8_t *l = segp(w1, 16);
                if (!l) return;
                memcpy(which == 0x86 ? g->diffuse : g->ambient, l, 3);
            }
            break;
        }
        case 0xB7: g->geom |= w1; break;                 // G_SETGEOMETRYMODE
        case 0xB6: g->geom &= ~w1; break;                // G_CLEARGEOMETRYMODE
        case 0xBB:     // G_TEXTURE
            g->texOn = (w0 & 0xFF) != 0; g->scaleS = w1 >> 16; g->scaleT = w1 & 0xFFFF;
            break;
        case 0xFD:     // G_SETTIMG
            g->timg = w1; g->timgFmt = (w0 >> 21) & 7; g->timgSiz = (w0 >> 19) & 3;
            break;
        case 0xF3:     // G_LOADBLOCK: the image just pointed at is what the render tile shows
            g->loaded = g->timg;
            break;
        case 0xF5: {   // G_SETTILE
            int t = (w1 >> 24) & 7;
            g->tile[t].fmt = (w0 >> 21) & 7; g->tile[t].siz = (w0 >> 19) & 3;
            g->tile[t].cmt = (w1 >> 18) & 3; g->tile[t].maskT = (w1 >> 14) & 15;
            g->tile[t].cms = (w1 >> 8) & 3; g->tile[t].maskS = (w1 >> 4) & 15;
            break;
        }
        case 0xF2: {   // G_SETTILESIZE
            int t = (w1 >> 24) & 7;
            int uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
            g->tile[t].w = ((lrs - uls) >> 2) + 1; g->tile[t].h = ((lrt - ult) >> 2) + 1;
            break;
        }
        case 0x06: {   // G_DL: call, or branch (no return)
            run_dl(w1, g, depth + 1);
            if (((w0 >> 16) & 0xFF) == 1) return;
            break;
        }
        case 0xB8: return;   // G_ENDDL
        default: break;      // render modes, syncs, combiner, colours: not needed to rebuild the shape and texture
        }
        if (s_failed) return;
    }
}

static int piece_for(uint32_t dl, int alpha, int opaque)
{
    for (int i = 0; i < s_nPieces; i++) if (s_pieceKeys[i].dl == dl && s_pieceKeys[i].alpha == alpha) return s_pieceKeys[i].piece;
    if (s_nPieces >= MAX_PIECES) { fail("too many pieces", dl); return -1; }
    ObjPiece *pc = &s_pieces[s_nPieces];
    memset(pc, 0, sizeof(*pc));
    pc->id = s_nPieces; pc->alpha = alpha;
    GfxState g;
    memset(&g, 0, sizeof(g));
    g.piece = pc; g.opaque = opaque; g.root = dl;
    g.geom = 0x1 | 0x4 | 0x200 | 0x2000 | 0x20000;   // what SM64 sets before drawing an object: z-buffer, shade, smooth, cull back, lighting
    g.diffuse[0] = g.diffuse[1] = g.diffuse[2] = 255;
    run_dl(dl, &g, 0);
    s_pieceKeys[s_nPieces].dl = dl; s_pieceKeys[s_nPieces].alpha = alpha; s_pieceKeys[s_nPieces].piece = s_nPieces;
    return s_nPieces++;
}

int objrom_piece_count(void) { return s_nPieces; }
const ObjPiece *objrom_piece(int id) { return id >= 0 && id < s_nPieces ? &s_pieces[id] : NULL; }

// ---- geo layout -> tree ------------------------------------------------------------------------------------------------
enum { N_GROUP, N_SWITCH, N_TRANSLATE, N_ROTATE, N_TRANSROT, N_ANIMATED, N_SCALE, N_BILLBOARD, N_DL };
typedef struct {
    uint8_t type, layer;
    int16_t t[3], r[3];
    float scale;
    uint32_t dl;
    int cases;
    int child, next, lastChild;
} GNode;
typedef struct { GNode *n; int count, cap; int root; } GeoTree;

static int gt_new(GeoTree *t, int parent, int type)
{
    if (t->count == t->cap) { t->cap = t->cap ? t->cap * 2 : 64; t->n = realloc(t->n, sizeof(GNode) * t->cap); }
    int i = t->count++;
    GNode *n = &t->n[i];
    memset(n, 0, sizeof(*n));
    n->type = (uint8_t)type; n->child = n->next = n->lastChild = -1; n->scale = 1;
    if (parent < 0) { if (t->root < 0) t->root = i; else { int k = t->root; while (t->n[k].next >= 0) k = t->n[k].next; t->n[k].next = i; } }
    else if (t->n[parent].lastChild < 0) t->n[parent].child = t->n[parent].lastChild = i;
    else { t->n[t->n[parent].lastChild].next = i; t->n[parent].lastChild = i; }
    return i;
}

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

// parent stack: the current list's parent, and "the node an OPEN would descend into"
typedef struct { int parent[16]; int last[16]; int depth; } GeoStack;

static int parse_cmds(GeoTree *t, uint32_t addr, GeoStack *st, int depthGuard)
{
    if (depthGuard > 8) { fail("geo branch nesting", addr); return 0; }
    for (uint32_t a = addr;;) {
        const uint8_t *b = segp(a, 4);
        if (!b) return 0;
        int len = geo_len(b);
        if (!len) { fail("unknown geo command", a); return 0; }
        b = segp(a, (uint32_t)len);
        if (!b) return 0;
        uint8_t cmd = b[0];
        int par = st->parent[st->depth];
        int node = -1;
        switch (cmd) {
        case 0x01: return 1;                         // END
        case 0x03: return 1;                         // RETURN
        case 0x04:                                   // OPEN_NODE: descend into the last node made
            if (st->depth >= 15) { fail("geo nesting", a); return 0; }
            st->depth++; st->parent[st->depth] = st->last[st->depth - 1]; st->last[st->depth] = -1;
            break;
        case 0x05: if (st->depth > 0) st->depth--; break;   // CLOSE_NODE
        case 0x00: case 0x02: {                     // BRANCH_AND_LINK / BRANCH
            uint32_t target = be32(b + 4);
            int link = cmd == 0x00 || b[1] == 1;
            if (!parse_cmds(t, target, st, depthGuard + 1)) return 0;
            if (!link) return 1;
            break;
        }
        case 0x08: case 0x09: case 0x0A: case 0x0F: case 0x0B: case 0x0C: case 0x0D: case 0x16: case 0x17: case 0x18:
        case 0x19: case 0x20: case 0x1C:
            node = gt_new(t, par, N_GROUP);
            break;
        case 0x0E:
            node = gt_new(t, par, N_SWITCH); t->n[node].cases = be16(b + 2);
            break;
        case 0x10: {   // TRANSLATE_ROTATE
            int mode = (b[1] >> 4) & 7;
            node = gt_new(t, par, N_TRANSROT);
            GNode *n = &t->n[node];
            const uint8_t *q = b + 4;
            if (mode == 0) { for (int i = 0; i < 3; i++) n->t[i] = be16(q + i * 2); for (int i = 0; i < 3; i++) n->r[i] = (int16_t)((be16(q + 6 + i * 2) * 32768) / 180); q += 12; }
            else if (mode == 1) { for (int i = 0; i < 3; i++) n->t[i] = be16(q + i * 2); q += 6; }
            else if (mode == 2) { for (int i = 0; i < 3; i++) n->r[i] = (int16_t)((be16(q + i * 2) * 32768) / 180); q += 6; }
            else { n->r[1] = (int16_t)((be16(b + 2) * 32768) / 180); }
            if (b[1] & 0x80) {
                n->layer = b[1] & 15; n->dl = be32(b + len - 4);
                int ch = gt_new(t, node, N_DL); t->n[ch].layer = n->layer; t->n[ch].dl = n->dl; n->dl = 0;
            }
            break;
        }
        case 0x11: {   // TRANSLATE
            node = gt_new(t, par, N_TRANSLATE);
            GNode *n = &t->n[node];
            for (int i = 0; i < 3; i++) n->t[i] = be16(b + 2 + i * 2);
            if (b[1] & 0x80) { int ch = gt_new(t, node, N_DL); t->n[ch].layer = b[1] & 15; t->n[ch].dl = be32(b + 8); }
            break;
        }
        case 0x12: {   // ROTATE
            node = gt_new(t, par, N_ROTATE);
            GNode *n = &t->n[node];
            for (int i = 0; i < 3; i++) n->r[i] = (int16_t)((be16(b + 2 + i * 2) * 32768) / 180);
            if (b[1] & 0x80) { int ch = gt_new(t, node, N_DL); t->n[ch].layer = b[1] & 15; t->n[ch].dl = be32(b + 8); }
            break;
        }
        case 0x13: {   // ANIMATED_PART
            node = gt_new(t, par, N_ANIMATED);
            GNode *n = &t->n[node];
            n->layer = b[1];
            for (int i = 0; i < 3; i++) n->t[i] = be16(b + 2 + i * 2);
            n->dl = be32(b + 8);
            break;
        }
        case 0x14: {   // BILLBOARD
            node = gt_new(t, par, N_BILLBOARD);
            GNode *n = &t->n[node];
            for (int i = 0; i < 3; i++) n->t[i] = be16(b + 2 + i * 2);
            if (b[1] & 0x80) { int ch = gt_new(t, node, N_DL); t->n[ch].layer = b[1] & 15; t->n[ch].dl = be32(b + 8); }
            break;
        }
        case 0x15:     // DISPLAY_LIST
            node = gt_new(t, par, N_DL);
            t->n[node].layer = b[1]; t->n[node].dl = be32(b + 4);
            break;
        case 0x1D: {   // SCALE
            node = gt_new(t, par, N_SCALE);
            GNode *n = &t->n[node];
            n->scale = be32(b + 4) / 65536.0f;
            if (b[1] & 0x80) { int ch = gt_new(t, node, N_DL); t->n[ch].layer = b[1] & 15; t->n[ch].dl = be32(b + 8); }
            break;
        }
        case 0x06: case 0x07: case 0x1A: case 0x1B: case 0x1E: case 0x1F: break;   // no node
        default: break;
        }
        if (node >= 0) st->last[st->depth] = node;
        a += len;
    }
}

// ---- models and posing ----------------------------------------------------------------------------------------------------
static GeoTree s_trees[OM_COUNT];

typedef float Mat4[4][4];

static float sins16(int16_t a) { return sinf((float)a * (6.28318530718f / 65536.0f)); }
static float coss16(int16_t a) { return cosf((float)a * (6.28318530718f / 65536.0f)); }

static void mtxf_mul(Mat4 dest, Mat4 a, Mat4 b)
{
    Mat4 t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    for (int j = 0; j < 3; j++) t[3][j] = a[3][0] * b[0][j] + a[3][1] * b[1][j] + a[3][2] * b[2][j] + b[3][j];
    t[0][3] = t[1][3] = t[2][3] = 0; t[3][3] = 1;
    memcpy(dest, t, sizeof(t));
}

static void mtxf_rotate_zxy_and_translate(Mat4 d, const float *tr, const int16_t *r)
{
    float sx = sins16(r[0]), cx = coss16(r[0]), sy = sins16(r[1]), cy = coss16(r[1]), sz = sins16(r[2]), cz = coss16(r[2]);
    d[0][0] = cy * cz + sx * sy * sz; d[1][0] = -cy * sz + sx * sy * cz; d[2][0] = cx * sy; d[3][0] = tr[0];
    d[0][1] = cx * sz; d[1][1] = cx * cz; d[2][1] = -sx; d[3][1] = tr[1];
    d[0][2] = -sy * cz + sx * cy * sz; d[1][2] = sy * sz + sx * cy * cz; d[2][2] = cx * cy; d[3][2] = tr[2];
    d[0][3] = d[1][3] = d[2][3] = 0; d[3][3] = 1;
}

static void mtxf_rotate_xyz_and_translate(Mat4 d, const float *b, const int16_t *c)
{
    float sx = sins16(c[0]), cx = coss16(c[0]), sy = sins16(c[1]), cy = coss16(c[1]), sz = sins16(c[2]), cz = coss16(c[2]);
    d[0][0] = cy * cz; d[0][1] = cy * sz; d[0][2] = -sy; d[0][3] = 0;
    d[1][0] = sx * sy * cz - cx * sz; d[1][1] = sx * sy * sz + cx * cz; d[1][2] = sx * cy; d[1][3] = 0;
    d[2][0] = cx * sy * cz + sx * sz; d[2][1] = cx * sy * sz - sx * cz; d[2][2] = cx * cy; d[2][3] = 0;
    d[3][0] = b[0]; d[3][1] = b[1]; d[3][2] = b[2]; d[3][3] = 1;
}

typedef struct {
    const GeoTree *tree;
    const ObjPose *pose;
    // current animation (the decomp's gCurrAnim* globals)
    int animType;       // 0 none, 1 translation, 2 vertical, 3 lateral, 4 none-translation, 5 rotation
    int animFrame;
    const uint8_t *animIndex;    // pairs of big-endian u16
    const uint8_t *animValues;   // big-endian s16
    uint32_t animValuesLen;
    float animMul;
    ObjPart *out; int nOut, maxOut;
} Eval;

static int anim_next(Eval *e)   // retrieve_animation_index + read the value
{
    int maxFrame = (e->animIndex[0] << 8) | e->animIndex[1];
    int off = (e->animIndex[2] << 8) | e->animIndex[3];
    int result = e->animFrame < maxFrame ? off + e->animFrame : off + maxFrame - 1;
    e->animIndex += 4;
    if ((uint32_t)(result * 2 + 2) > e->animValuesLen) return 0;
    return be16(e->animValues + result * 2);
}

static void bng_part(Mat4 m, int piece, int billboard, Eval *e)
{
    if (e->nOut >= e->maxOut) return;
    ObjPart *p = &e->out[e->nOut++];
    p->piece = piece; p->billboard = billboard;
    // scale = length of the first row (uniform scales only)
    float sc = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
    if (sc < 1e-6f) sc = 1e-6f;
    // row-vector rotation Rr (v' = v Rr); column form Rc = Rr^T; BeamNG: Rb = P Rc P^T with x->x, y_bng = -z, z_bng = y
    float R[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[j][i] = m[i][j] / sc;   // Rc
    static const float P[3][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };
    float T[3][3], Rb[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { T[i][j] = 0; for (int k = 0; k < 3; k++) T[i][j] += P[i][k] * R[k][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Rb[i][j] = 0; for (int k = 0; k < 3; k++) Rb[i][j] += T[i][k] * P[j][k]; }
    float tr = Rb[0][0] + Rb[1][1] + Rb[2][2], x, y, z, w;
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; w = 0.25f * s; x = (Rb[2][1] - Rb[1][2]) / s; y = (Rb[0][2] - Rb[2][0]) / s; z = (Rb[1][0] - Rb[0][1]) / s; }
    else if (Rb[0][0] > Rb[1][1] && Rb[0][0] > Rb[2][2]) { float s = sqrtf(1 + Rb[0][0] - Rb[1][1] - Rb[2][2]) * 2; w = (Rb[2][1] - Rb[1][2]) / s; x = 0.25f * s; y = (Rb[0][1] + Rb[1][0]) / s; z = (Rb[0][2] + Rb[2][0]) / s; }
    else if (Rb[1][1] > Rb[2][2]) { float s = sqrtf(1 + Rb[1][1] - Rb[0][0] - Rb[2][2]) * 2; w = (Rb[0][2] - Rb[2][0]) / s; x = (Rb[0][1] + Rb[1][0]) / s; y = 0.25f * s; z = (Rb[1][2] + Rb[2][1]) / s; }
    else { float s = sqrtf(1 + Rb[2][2] - Rb[0][0] - Rb[1][1]) * 2; w = (Rb[1][0] - Rb[0][1]) / s; x = (Rb[0][2] + Rb[2][0]) / s; y = (Rb[1][2] + Rb[2][1]) / s; z = 0.25f * s; }
    p->quat[0] = x; p->quat[1] = y; p->quat[2] = z; p->quat[3] = w;
    p->pos[0] = m[3][0] * SM64_SCALE; p->pos[1] = -m[3][2] * SM64_SCALE; p->pos[2] = m[3][1] * SM64_SCALE;
    p->scale = sc;
}

static void eval_list(Eval *e, int first, Mat4 parent, int switchChild);

static void eval_node(Eval *e, int i, Mat4 parent)
{
    const GNode *n = &e->tree->n[i];
    Mat4 m;
    switch (n->type) {
    case N_GROUP: eval_list(e, n->child, parent, 0); return;
    case N_SWITCH: {
        int sel = e->pose->animState;
        if (sel < 0 || sel >= n->cases) sel = 0;
        int c = n->child;
        for (int k = 0; k < sel && c >= 0; k++) c = e->tree->n[c].next;
        if (c >= 0) eval_node(e, c, parent);   // exactly one child
        return;
    }
    case N_TRANSLATE: {
        float t[3] = { n->t[0], n->t[1], n->t[2] };
        int16_t z[3] = { 0, 0, 0 };
        mtxf_rotate_xyz_and_translate(m, t, z);
        mtxf_mul(m, m, parent);
        eval_list(e, n->child, m, 0);
        return;
    }
    case N_TRANSROT: {
        float t[3] = { n->t[0], n->t[1], n->t[2] };
        mtxf_rotate_xyz_and_translate(m, t, n->r);
        mtxf_mul(m, m, parent);
        eval_list(e, n->child, m, 0);
        return;
    }
    case N_ROTATE: {
        float t[3] = { 0, 0, 0 };
        mtxf_rotate_xyz_and_translate(m, t, n->r);
        mtxf_mul(m, m, parent);
        eval_list(e, n->child, m, 0);
        return;
    }
    case N_ANIMATED: {
        int16_t rot[3] = { 0, 0, 0 };
        float tr[3] = { n->t[0], n->t[1], n->t[2] };
        if (e->animType == 1) {
            tr[0] += anim_next(e) * e->animMul; tr[1] += anim_next(e) * e->animMul; tr[2] += anim_next(e) * e->animMul;
            e->animType = 5;
        } else if (e->animType == 3) {
            tr[0] += anim_next(e) * e->animMul;
            e->animIndex += 4;
            tr[2] += anim_next(e) * e->animMul;
            e->animType = 5;
        } else if (e->animType == 2) {
            e->animIndex += 4;
            tr[1] += anim_next(e) * e->animMul;
            e->animIndex += 4;
            e->animType = 5;
        } else if (e->animType == 4) {
            e->animIndex += 12;
            e->animType = 5;
        }
        if (e->animType == 5) { rot[0] = (int16_t)anim_next(e); rot[1] = (int16_t)anim_next(e); rot[2] = (int16_t)anim_next(e); }
        mtxf_rotate_xyz_and_translate(m, tr, rot);
        mtxf_mul(m, m, parent);
        if (n->dl) { int pc = piece_for(n->dl, n->layer >= 4, n->layer < 4); if (pc >= 0) bng_part(m, pc, 0, e); }
        eval_list(e, n->child, m, 0);
        return;
    }
    case N_SCALE: {
        memcpy(m, parent, sizeof(m));
        for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c] = parent[r][c] * n->scale;
        eval_list(e, n->child, m, 0);
        return;
    }
    case N_BILLBOARD: {
        float t[3] = { n->t[0], n->t[1], n->t[2] };
        int16_t z[3] = { 0, 0, 0 };
        mtxf_rotate_xyz_and_translate(m, t, z);
        mtxf_mul(m, m, parent);
        // children are drawn facing the camera; the pose is marked and the game turns it
        for (int c = n->child; c >= 0; c = e->tree->n[c].next) {
            const GNode *ch = &e->tree->n[c];
            if (ch->type == N_DL) { int pc = piece_for(ch->dl, ch->layer >= 4, ch->layer < 4); if (pc >= 0) bng_part(m, pc, 1, e); }
        }
        return;
    }
    case N_DL: {
        int pc = piece_for(n->dl, n->layer >= 4, n->layer < 4);
        if (pc >= 0) bng_part(parent, pc, 0, e);
        return;
    }
    }
}

static void eval_list(Eval *e, int first, Mat4 parent, int unused)
{
    (void)unused;
    for (int i = first; i >= 0; i = e->tree->n[i].next) eval_node(e, i, parent);
}

static const uint8_t *anim_struct(uint32_t anim)
{
    if (!anim) return NULL;
    return segp(anim, 24);
}

int objrom_anim_info(uint32_t anim, int *startFrame, int *loopStart, int *loopEnd, int *flags)
{
    const uint8_t *a = anim_struct(anim);
    if (!a) { s_failed = 0; return 0; }
    if (flags) *flags = be16(a);
    if (startFrame) *startFrame = be16(a + 4);
    if (loopStart) *loopStart = be16(a + 6);
    if (loopEnd) *loopEnd = be16(a + 8);
    return 1;
}

uint32_t objrom_anim_from_table(uint32_t table, int index)
{
    const uint8_t *t = segp(table + index * 4, 4);
    if (!t) { s_failed = 0; return 0; }
    return be32(t);
}

int objrom_pose(int model, const ObjPose *pose, ObjPart *parts, int max)
{
    if (model < 0 || model >= OM_COUNT || s_trees[model].root < 0) return 0;
    Eval e;
    memset(&e, 0, sizeof(e));
    e.tree = &s_trees[model]; e.pose = pose; e.out = parts; e.maxOut = max;
    const uint8_t *an = anim_struct(pose->anim);
    if (an) {
        int flags = be16(an), div = be16(an + 2);
        e.animType = (flags & 0x08) ? 2 : (flags & 0x10) ? 3 : (flags & 0x40) ? 4 : 1;
        e.animFrame = pose->animFrame;
        uint32_t idx = be32(an + 16), val = be32(an + 12), len = be32(an + 20);
        e.animIndex = segp(idx, 4);
        e.animValues = segp(val, 2);
        e.animValuesLen = (val & 0xFFFFFF) < s_bankSize[(val >> 24) & 31] ? s_bankSize[(val >> 24) & 31] - (val & 0xFFFFFF) : 0;
        (void)len;
        e.animMul = div == 0 ? 1.0f : pose->animYTrans / (float)div;
        if (!e.animIndex || !e.animValues) { s_failed = 0; e.animType = 0; }
    }
    Mat4 m, start;
    mtxf_rotate_zxy_and_translate(m, pose->pos, pose->angle);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) start[r][c] = m[r][c] * pose->scale[r];
    memcpy(start[3], m[3], sizeof(start[3]));
    eval_list(&e, e.tree->root, start, 0);
    return e.nOut;
}

// ---- loading -----------------------------------------------------------------------------------------------------------------
int objrom_load(const uint8_t *rom, size_t romLen, const char *atlasPath)
{
    s_rom = rom; s_romLen = romLen; s_failed = 0; s_error[0] = 0;
    for (size_t i = 0; i < sizeof(k_banks) / sizeof(k_banks[0]); i++) {
        if (k_banks[i].rom + 16 > romLen || memcmp(rom + k_banks[i].rom, "MIO0", 4)) { snprintf(s_error, sizeof(s_error), "bank %d isn't where a US ROM has it", k_banks[i].seg); return 0; }
        mio0_header_t head;
        if (mio0_decode_header(rom + k_banks[i].rom, &head) != 1) { snprintf(s_error, sizeof(s_error), "bad bank %d", k_banks[i].seg); return 0; }
        s_bank[k_banks[i].seg] = malloc(head.dest_size);
        s_bankSize[k_banks[i].seg] = head.dest_size;
        mio0_decode(rom + k_banks[i].rom, s_bank[k_banks[i].seg], NULL);
    }
    for (size_t i = 0; i < sizeof(k_raws) / sizeof(k_raws[0]); i++)
        if (k_raws[i].rom + k_raws[i].size > romLen) { snprintf(s_error, sizeof(s_error), "geo segment outside the ROM"); return 0; }
    s_atlas = calloc((size_t)ATLAS_W * s_atlasH, 4);
    s_shelfX = s_shelfY = s_shelfH = 0;
    // a magenta cell first, for anything that fails to resolve
    atlas_color(255, 0, 255, 255);
    for (int m = 0; m < OM_COUNT; m++) {
        GeoTree *t = &s_trees[m];
        memset(t, 0, sizeof(*t));
        t->root = -1;
        GeoStack st;
        memset(&st, 0, sizeof(st));
        st.parent[0] = -1; st.last[0] = -1;
        if (!parse_cmds(t, k_models[m].geo, &st, 0) || s_failed) return 0;
        // build every piece the model can show now, so the atlas is complete and failures show up at startup
        for (int i = 0; i < t->count; i++)
            if (t->n[i].dl && t->n[i].type != N_BILLBOARD) piece_for(t->n[i].dl, t->n[i].layer >= 4, t->n[i].layer < 4);
        if (s_failed) return 0;
    }
    (void)atlasPath;
    return 1;
}

// the atlas of every texture and colour the models use, as a PNG (the game's material points at it)
int objrom_write_atlas(const char *path)
{
    if (!s_atlas) return 0;
    return png_write_rgba(path, s_atlas, ATLAS_W, s_atlasH);
}

#ifdef OBJROM_TEST
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(n); fread(rom, 1, n, f); fclose(f);
    if (!objrom_load(rom, n, NULL) || !objrom_write_atlas(argv[2])) { printf("FAILED: %s\n", objrom_error()); return 1; }
    printf("pieces %d, atlas rects %d, out-of-range-uv triangles %d\n", s_nPieces, s_nRects, s_oorTris);
    for (int i = 0; i < s_nPieces; i++) printf("piece %2d: %3d verts %3d idx lit %d unlit %d%s\n", i, s_pieces[i].nv, s_pieces[i].ni, s_pieces[i].litTris, s_pieces[i].unlitTris, s_pieces[i].alpha ? " (alpha)" : "");
    for (int m = 0; m < OM_COUNT; m++) {
        ObjPose pose; memset(&pose, 0, sizeof(pose)); pose.scale[0] = pose.scale[1] = pose.scale[2] = 1;
        ObjPart parts[32];
        int np = objrom_pose(m, &pose, parts, 32);
        printf("model %d: %d parts:", m, np);
        for (int i = 0; i < np; i++) printf(" %d@(%.2f,%.2f,%.2f)x%.2f%s", parts[i].piece, parts[i].pos[0], parts[i].pos[1], parts[i].pos[2], parts[i].scale, parts[i].billboard ? "B" : "");
        printf("\n");
    }
    return 0;
}
#endif
