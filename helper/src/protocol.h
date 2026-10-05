// NG64 helper <-> BeamNG GE-Lua wire protocol. UDP on 127.0.0.1, little-endian, packed.
// Every datagram starts with a 1-byte message type. Keep in sync with mod/lua/ge/extensions/ng64/proto.lua.
#pragma once
#include <stdint.h>

#define NG64_PORT          47064
#define NG64_PROTO_VERSION 14

// BeamNG metres per SM64 unit (same scale sm64-san-andreas uses for GTA).
#define NG64_SCALE 0.0085f

// client -> helper
#define MSG_HELLO     'H'  // u16 version, str userPath\0
#define MSG_MESH      'O'  // u32 cell; u16 chunk, chunks, tris; f32 tris[tris*9] (bng world) - one 16 m map cell's collision triangles
#define MSG_MESH_DROP 'Y'  // u32 cell (0xFFFFFFFF = all): Mario has left it
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
#define MSG_FOCUS     'Z'  // u8: 1 = the game window has focus (input is only read then). Sent on change and every second
#define MSG_HEAL      'h'  // u8 healCounter: heal Mario like SM64 coins do (4 = one wedge). Older helpers ignore it
#define MSG_TELEPORT  'M'  // f32 x, y, z (bng); optional u8 reset (full health, freefall)
#define MSG_SURFACES  'G'  // u16 chunk, chunks, count; count * {u16 type; i16 force; f32 v[9]} (bng): a level's own SM64
                           // surfaces with their original types (chunks = 0 clears them). Replaces nothing else
#define MSG_WATER     'J'  // u8 count; count * {f32 x0, y0, x1, y1, z} (bng): water boxes (count = 0: no water)
#define MSG_WATER_OBB 'j'  // u8 count; count * {f32 cx, cy, halfX, halfY, cosYaw, sinYaw, z, depth} (bng): BeamNG's own water, rotated rectangles with a surface height and depth
#define MSG_SPIN_BREAK 'b' // u32 vehId: the car Mario is spinning hit something, he lets go
#define MSG_FIRE      'k'  // u8 count; count * {f32 x, y, z, radius} (bng): flames near Mario (burning vehicle nodes), refreshed ~10 Hz; none = no fire
#define MSG_ENTITIES  'n'  // u16 count; count * {u16 id; u8 type; u8 state; u8 nparts; u8 pad; f32 x, y, z (bng); nparts * {u16 piece; u8 billboard; u8 pad; f32 pos[3]; i16 quat[4]; f32 scale}}: pickups and enemies, each posed as rigid pieces of its ROM model
#define MSG_OBJ_ATLAS 'z'  // str atlasPath (game-virtual path): the PNG of the textures the object models use
#define MSG_OBJ_SHOW  'u'  // tests: u8 model (255 = clear); f32 x, y, z (bng); i16 yaw (s16 angle); u8 animState; u8 animIdx (255 none); i16 frame - a static showcase object
#define MSG_OBJ_REQ   'y'  // u16 piece: the client has no geometry for this object piece
#define MSG_OBJ_PIECE 'x'  // u16 piece; u8 alpha; u8 pad; u16 nv, ni; nv * ObjVert (14 bytes: s16 p[3] SM64 units; s8 n[3]; pad; u16 uv[2]); ni * u16
#define MSG_ENT_EVENT 'v'  // u8 kind; u16 id; f32 a, b, c, d: something happened to / with them (coin, star, cap, explosion...); see ents.h
#define MSG_DRIVE_FOCUS 'f'  // u8 valid; f32 bng pos[3]: the car the player drives, away from Mario (pickups and enemies round it too)
#define MSG_OPTIONS   'o'  // u8 pickups, enemies (0/1); u8 song (index, 254 = music off, 255 = leave); u8 volume 0..100 (255 = leave)
#define MSG_ENT_KILL  'c'  // u16 id; u8 kind: a car ran this enemy over (0), or a shell hit a car (1)
#define MSG_PART_REQ  'B'  // u32 key; u8 part; u32 hash - the client has no geometry for this part/hash (lost or new)

// helper -> client
#define MSG_WELCOME   'W'  // u8 ok; str message\0; str atlasPath\0 (game-virtual path)
#define MSG_FRAME     'F'  // FrameHeader then numParts * PartPose. Mario is rigid body parts: the frame only moves them.
#define MSG_PART      'E'  // PartHeader then nv * PackedVert, ni * u16: one part's geometry in its own frame. Sent when
                           // a part's hash changes and on MSG_PART_REQ (LuaSocket caps UDP reads at 8 KB: one part fits)
#define MSG_HIT       'A'  // u32 vehId; f32 point[3]; f32 dir[3]; f32 strength (bng)
#define MSG_LOG       'L'  // str\0
#define MSG_CARRY     'C'  // u8 kind (1 start, 2 hold, 3 release); u32 vehId; u8 piece; u8 heavy; f32 point[3]; f32 yaw; f32 vel[3] (bng)

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;          // 'F'
    uint32_t key;           // 0 = local Mario, otherwise remote key
    uint32_t seq;           // frame number
    float    pos[3];        // bng world, Mario's position
    float    vel[3];        // bng m/s
    float    faceAngle;     // radians, sm64 convention
    int16_t  health;
    uint32_t action;
    int16_t  animId;
    int16_t  animFrame;
    uint32_t flags;
    float    camPos[3];     // bng, local only
    float    camTarget[3];  // bng, local only
    uint16_t numVerts;      // total over all parts (status/tests)
    uint16_t numParts;      // PartPose records following this header
    uint32_t tick;          // simulation tick (30 Hz) this pose belongs to - the mod times blending by this, not arrival
} FrameHeader;

typedef struct {
    uint32_t hash;          // FNV-1a of the part's geometry: same hash = same mesh, 0 = part draws nothing
    float    pos[3];        // bng world
    float    axes[3][3];    // bng: where the part's x, y, z axes point, scaled (the part's rotation * scale)
} PartPose;

typedef struct {
    uint8_t  type;   // 'E'
    uint32_t key;
    uint8_t  part;
    uint32_t hash;
    uint16_t nv;
    uint16_t ni;
} PartHeader;

#define NG64_MAX_PARTS 64
#define NG64_MAX_PART_BYTES 8000   // LuaSocket's read limit is 8192

typedef struct {
    int16_t  p[3];   // millimetres in the part's own frame (bng axes)
    int8_t   n[3];   // normal * 127
    uint16_t uv[2];  // atlas uv * 65535
} PackedVert;
#pragma pack(pop)
