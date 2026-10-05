// SM64 objects (pickups, enemies) read from the player's own ROM at startup: geo layouts, Fast3D display lists, textures
// and animations, the way mario_rom_*.c does for Mario. Nothing from the game is built in. The models are drawn by the
// game as rigid pieces (one mesh per display list); the helper poses them every tick from the geo layout and the
// current animation, as SM64 does.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {   // models
    OM_COIN_YELLOW, OM_COIN_RED, OM_COIN_BLUE, OM_STAR, OM_STAR_TRANSPARENT,
    OM_CAP_METAL, OM_CAP_WING, OM_GOOMBA, OM_BOBOMB, OM_KOOPA, OM_KOOPA_SHELL, OM_EXPLOSION, OM_KOOPA_NOSHELL, OM_SPARKLES, OM_MIST, OM_SMOKE, OM_NUMBER,
    OM_COUNT
};

int objrom_texture_rgba(uint32_t addr, int fmt, int siz, int w, int h, uint8_t *out);

#pragma pack(push, 1)
typedef struct { int16_t p[3]; int8_t n[3]; int8_t pad; uint16_t uv[2]; } ObjVert;   // p in SM64 units; uv in the atlas (0..65535)
#pragma pack(pop)

typedef struct {
    int id;
    int nv, ni;
    ObjVert *v;
    uint16_t *idx;
    int alpha;          // drawn with alpha cut-out (the display list was in an alpha layer)
    int litTris, unlitTris;   // how many of its triangles were lit (shaded by their normals) or not (flat, unlit)
} ObjPiece;

typedef struct {
    int piece;
    float pos[3];       // BeamNG metres, relative to nothing: world
    float quat[4];      // x y z w, BeamNG axes
    float scale;
    int billboard;
} ObjPart;

typedef struct {
    float pos[3];       // SM64 world units (x, y up, z)
    int16_t angle[3];   // pitch, yaw, roll (SM64 s16 angles)
    float scale[3];
    int animState;      // switch-case selector (oAnimState)
    uint32_t anim;      // segmented address of an Animation, 0 = none
    int animFrame;
    float animYTrans;   // oAnimYTrans multiplier source (0 for most)
} ObjPose;

// 1 on success; else 0 and the reason in objrom_error(). The ROM must stay in memory.
int objrom_load(const uint8_t *rom, size_t romLen, const char *unused);
int objrom_write_atlas(const char *path);   // the PNG of every texture and colour the models use
const char *objrom_error(void);
int objrom_piece_count(void);
const ObjPiece *objrom_piece(int id);
// poses a model: fills parts[] (up to max) and returns how many
int objrom_pose(int model, const ObjPose *pose, ObjPart *parts, int max);
// animation info for the behaviour code: frame count (loopEnd), flags; 0 if the address isn't an animation
int objrom_anim_info(uint32_t anim, int *startFrame, int *loopStart, int *loopEnd, int *flags);
// segmented address of animation #index of an actor's animation table (table also a segmented address), 0 if bad
uint32_t objrom_anim_from_table(uint32_t table, int index);
