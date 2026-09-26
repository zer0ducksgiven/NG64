// NG64 helper <-> BeamNG GE-Lua wire protocol. UDP on 127.0.0.1, little-endian, packed.
// Every datagram starts with a 1-byte message type. Keep in sync with mod/lua/ge/extensions/ng64/proto.lua.
#pragma once
#include <stdint.h>

#define NG64_PORT          47064
#define NG64_PROTO_VERSION 6

// BeamNG metres per SM64 unit (same scale sm64-san-andreas uses for GTA).
#define NG64_SCALE 0.0085f

// client -> helper
#define MSG_HELLO     'H'  // u16 version, str userPath\0
#define MSG_MESH      'O'  // u32 region; u16 chunk, chunks, tris; f32 tris[tris*9] (bng world) - map objects' collision triangles
#define MSG_FLOOR_QUERY 'Q'  // tests: u32 id; u16 n; f32 points[n*3] (bng) -> MSG_FLOOR_REPLY
#define MSG_FLOOR_REPLY 'q'  // u32 id; u16 n; f32 floorZ[n] (bng, NaN = none)
#define MSG_TERRAIN   'T'  // f32 cx, cy, spacing; u16 n; f32 heights[n*n] (bng z, NaN = no ground)
#define MSG_SPAWN     'S'  // f32 x, y, z (bng)
#define MSG_DESPAWN   'D'
#define MSG_VEHICLES  'V'  // u16 count; {u32 id; f32 oobbCenter[3]; f32 oobbHalfAxis[3][3]; f32 origin[3]; f32 fwd[3]; f32 up[3]} (bng)
#define MSG_HULL      'U'  // u32 id; f32 cell, x0, y0, bottom; u16 nx, ny; u8 piece, pieceCount; f32 top[nx*ny] (vehicle frame x right, y fwd, z up; NaN = empty)
#define MSG_HURT      'K'  // f32 src[3] (bng); u8 damage; u8 bigKnockback; f32 vehicleVel[3] (bng m/s)
#define MSG_REMOTE    'R'  // u32 key; f32 pos[3] (bng); f32 faceAngle; u32 action; i16 animId; i16 animFrame; u32 flags  (key 0 = remove all)
#define MSG_REMOTE_DEL 'X' // u32 key
#define MSG_INPUT     'I'  // scripted input for UAT: f32 stickX, stickY; u8 a, b, z; u16 frames
#define MSG_PING      'P'
#define MSG_CONTROL   'N'  // u8: 1 = the player controls Mario, 0 = another vehicle (Mario stays, input off)
#define MSG_TELEPORT  'M'  // f32 x, y, z (bng); optional u8 reset (full health, freefall)

// helper -> client
#define MSG_WELCOME   'W'  // u8 ok; str message\0; str atlasPath\0 (game-virtual path)
#define MSG_FRAME     'F'  // FrameHeader only; the mesh follows in MSG_CHUNK packets (LuaSocket caps UDP reads at 8 KB)
#define MSG_CHUNK     'G'  // ChunkHeader then count * PackedVert (unique vertices)
#define MSG_INDEX     'J'  // ChunkHeader then count * u16 (corner -> unique vertex)
#define MSG_HIT       'A'  // u32 vehId; f32 point[3]; f32 dir[3]; f32 strength (bng)
#define MSG_LOG       'L'  // str\0
#define MSG_CARRY     'C'  // u8 kind (1 start, 2 hold, 3 release); u32 vehId; u8 piece; u8 heavy; f32 point[3]; f32 yaw; f32 vel[3] (bng)

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;          // 'F'
    uint32_t key;           // 0 = local Mario, otherwise remote key
    uint32_t seq;           // frame number, matches the chunks
    float    pos[3];        // bng world, mesh origin
    float    vel[3];        // bng m/s
    float    faceAngle;     // radians, sm64 convention
    int16_t  health;
    uint32_t action;
    int16_t  animId;
    int16_t  animFrame;
    uint32_t flags;
    float    camPos[3];     // bng, local only
    float    camTarget[3];  // bng, local only
    uint16_t numVerts;      // unique vertices (MSG_CHUNK)
    uint16_t numIndices;    // triangle corners (MSG_INDEX), 3 per triangle
    uint32_t indexHash;     // FNV-1a of the index list: same hash = same topology, so frames can be blended
    uint32_t tick;          // simulation tick (30 Hz) this pose belongs to - the mod times blending by this, not arrival
} FrameHeader;

typedef struct {
    uint8_t  type;   // 'G'
    uint32_t key;
    uint32_t seq;
    uint16_t start;
    uint16_t count;
} ChunkHeader;

#define NG64_CHUNK_VERTS 600     // 600 * 13 + 13 bytes stays under 8 KB
#define NG64_CHUNK_INDICES 3000  // 3000 * 2 + 13 bytes

typedef struct {
    int16_t  p[3];   // millimetres relative to FrameHeader.pos, bng axes
    int8_t   n[3];   // normal * 127
    uint16_t uv[2];  // atlas uv * 65535
} PackedVert;
#pragma pack(pop)
