// SM64's pickups and a few enemies, streamed in round Mario and simulated at 30 Hz like the game's own object code
// (see ents.h). Positions are SM64 units; the packet and events convert to BeamNG. This file ports, from the
// decompilation's behaviour code (written out fresh, with the same numbers and the same order of operations):
//   - the object engine: object_step, cur_obj_move_standard and the floor / wall / water handling under them,
//     angle helpers, animation stepping, hitbox overlap, and how Mario's touch sets an object's interact status;
//   - the behaviours: goomba, bob-omb, koopa (with and without shell), koopa shell, coins, power star, caps, explosion.
// What isn't original: where and when things appear (SM64 places them in each level by hand; here they are streamed in
// at random), the invincibility star (not an SM64 object), and how a car's hit becomes a normal Mario-style attack.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ents.h"
#include "protocol.h"
#include "objrom.h"

#define S NG64_SCALE
#define PI_F 3.14159265f

typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;

#define MAX_OBJ 128
#define MAX_PICKUPS 12
#define MAX_ENEMIES 7
#define TICKS_PER_S 30

// ---- sm64 constants ---------------------------------------------------------------------------------------------
#define FLOOR_LOWER_LIMIT       (-11000.0f)
#define FLOOR_LOWER_LIMIT_MISC  (-10000.0f)

// Mario
#define ACT_FLAG_AIR          0x00000800u
#define ACT_FLAG_SHORT_HITBOX 0x00008000u
#define ACT_FLAG_SWIMMING     0x00002000u
#define ACT_FLAG_THROWING     0x80000000u
#define ACT_DIVE              0x0188088Au
#define ACT_DIVE_SLIDE        0x00880456u
#define ACT_FLAG_METAL_WATER  0x00004000u
#define ACT_STAR_DANCE_WATER  0x00001303u
#define ACT_FALL_AFTER_STAR_GRAB 0x00001904u
#define ACT_STAR_DANCE_NO_EXIT 0x00001307u
#define ACT_FLAG_RIDING_SHELL 0x00010000u
#define ACT_FLAG_INVULNERABLE 0x00020000u
#define ACT_FLAG_ATTACKING    0x00800000u
#define ACT_WALKING           0x04000440u
#define ACT_HOLD_WALKING      0x00000442u
#define ACT_PUNCHING          0x00800380u
#define ACT_MOVE_PUNCHING     0x00800457u
#define ACT_JUMP_KICK         0x018008ACu
#define ACT_GROUND_POUND      0x008008A9u
#define ACT_TWIRLING          0x108008A4u
#define ACT_GROUND_POUND_LAND 0x0080023Cu
#define ACT_TWIRL_LAND        0x18800238u
#define ACT_SLIDE_KICK        0x018008AAu
#define ACT_SLIDE_KICK_SLIDE  0x0080045Au
#define ACT_RIDING_SHELL_GROUND 0x20810446u
#define MARIO_VANISH_CAP      0x00000002u
#define MARIO_METAL_CAP       0x00000004u
#define MARIO_WING_CAP        0x00000008u
#define MARIO_PUNCHING        0x00100000u
#define MARIO_KICKING         0x00200000u
#define MARIO_TRIPPING        0x00400000u

// interaction
#define INTERACT_GRABBABLE   0x00000002u
#define INTERACT_DAMAGE      0x00000008u
#define INTERACT_COIN        0x00000010u
#define INTERACT_CAP         0x00000020u
#define INTERACT_KOOPA       0x00000080u
#define INTERACT_STAR_OR_KEY 0x00001000u
#define INTERACT_BOUNCE_TOP  0x00008000u
#define INTERACT_KOOPA_SHELL 0x00080000u

#define INT_SUBTYPE_DELAY_INVINCIBILITY 0x00000002u
#define INT_SUBTYPE_KICKABLE            0x00000100u

#define INT_GROUND_POUND_OR_TWIRL (1 << 0)
#define INT_PUNCH                 (1 << 1)
#define INT_KICK                  (1 << 2)
#define INT_TRIP                  (1 << 3)
#define INT_SLIDE_KICK            (1 << 4)
#define INT_FAST_ATTACK_OR_SHELL  (1 << 5)
#define INT_HIT_FROM_ABOVE        (1 << 6)
#define INT_HIT_FROM_BELOW        (1 << 7)
#define INT_ATTACK_NOT_FROM_BELOW (INT_GROUND_POUND_OR_TWIRL | INT_PUNCH | INT_KICK | INT_TRIP | INT_SLIDE_KICK | INT_FAST_ATTACK_OR_SHELL | INT_HIT_FROM_ABOVE)

#define ATTACK_PUNCH                 1
#define ATTACK_KICK_OR_TRIP          2
#define ATTACK_FROM_ABOVE            3
#define ATTACK_GROUND_POUND_OR_TWIRL 4
#define ATTACK_FAST_ATTACK           5
#define ATTACK_FROM_BELOW            6
#define INT_STATUS_ATTACK_MASK       0x000000FFu
#define INT_STATUS_ATTACKED_MARIO    (1u << 13)
#define INT_STATUS_WAS_ATTACKED      (1u << 14)
#define INT_STATUS_INTERACTED        (1u << 15)
#define INT_STATUS_TOUCHED_BOB_OMB   (1u << 23)
#define INT_STATUS_MARIO_KNOCKBACK_DMG (1u << 1)

// object move flags
#define OBJ_MOVE_LANDED             (1 << 0)
#define OBJ_MOVE_ON_GROUND          (1 << 1)
#define OBJ_MOVE_LEFT_GROUND        (1 << 2)
#define OBJ_MOVE_ENTERED_WATER      (1 << 3)
#define OBJ_MOVE_AT_WATER_SURFACE   (1 << 4)
#define OBJ_MOVE_UNDERWATER_OFF_GROUND (1 << 5)
#define OBJ_MOVE_UNDERWATER_ON_GROUND  (1 << 6)
#define OBJ_MOVE_IN_AIR             (1 << 7)
#define OBJ_MOVE_HIT_WALL           (1 << 9)
#define OBJ_MOVE_HIT_EDGE           (1 << 10)
#define OBJ_MOVE_ABOVE_LAVA         (1 << 11)
#define OBJ_MOVE_LEAVING_WATER      (1 << 12)
#define OBJ_MOVE_BOUNCE             (1 << 13)
#define OBJ_MOVE_ABOVE_DEATH_BARRIER (1 << 14)
#define OBJ_MOVE_MASK_ON_GROUND (OBJ_MOVE_LANDED | OBJ_MOVE_ON_GROUND)
#define OBJ_MOVE_MASK_IN_WATER (OBJ_MOVE_ENTERED_WATER | OBJ_MOVE_AT_WATER_SURFACE | OBJ_MOVE_UNDERWATER_OFF_GROUND | OBJ_MOVE_UNDERWATER_ON_GROUND)

// object_step's result
#define OBJ_COL_FLAG_GROUNDED   (1 << 0)
#define OBJ_COL_FLAG_HIT_WALL   (1 << 1)
#define OBJ_COL_FLAG_UNDERWATER (1 << 2)
#define OBJ_COL_FLAG_NO_Y_VEL   (1 << 3)

// object actions shared by the enemies
#define OBJ_ACT_LAVA_DEATH        100
#define OBJ_ACT_DEATH_PLANE_DEATH 101
#define OBJ_ACT_HORIZONTAL_KNOCKBACK 100
#define OBJ_ACT_VERTICAL_KNOCKBACK   101
#define OBJ_ACT_SQUISHED             102

#define GOOMBA_ACT_WALK 0
#define GOOMBA_ACT_ATTACKED_MARIO 1
#define GOOMBA_ACT_JUMP 2

#define BOBOMB_ACT_PATROL 0
#define BOBOMB_ACT_LAUNCHED 1
#define BOBOMB_ACT_CHASE_MARIO 2
#define BOBOMB_ACT_EXPLODE 3

#define KOOPA_UNSHELLED_ACT_RUN 0
#define KOOPA_UNSHELLED_ACT_DIVE 1
#define KOOPA_UNSHELLED_ACT_LYING 2
#define KOOPA_SHELLED_ACT_STOPPED 0
#define KOOPA_SHELLED_ACT_WALK 1
#define KOOPA_SHELLED_ACT_RUN_FROM_MARIO 2
#define KOOPA_SHELLED_ACT_LYING 3
#define KOOPA_SHELLED_SUB_ACT_START_WALK 0
#define KOOPA_SHELLED_SUB_ACT_WALK 1
#define KOOPA_SHELLED_SUB_ACT_STOP_WALK 2
#define KOOPA_BP_UNSHELLED 0
#define KOOPA_BP_NORMAL 1

// sounds: SOUND_ARG_LOAD(bank, playFlags, id, priority, flags2), spelled out
#define SND_ARG(bank, play, id, prio, f2) (((u32)(bank) << 28) | ((u32)(play) << 24) | ((u32)(id) << 16) | ((u32)(prio) << 8) | ((u32)(f2) << 4) | 1u)
#define SOUND_GENERAL_COIN         SND_ARG(3, 8, 0x11, 0x80, 8)
#define SOUND_GENERAL_COIN_SPURT   SND_ARG(3, 0, 0x30, 0x00, 8)
#define SOUND_GENERAL_COIN_DROP    SND_ARG(3, 0, 0x36, 0x40, 8)
#define SOUND_MENU_COLLECT_RED_COIN SND_ARG(7, 8, 0x28, 0x90, 8)
#define SOUND_MENU_STAR_SOUND      SND_ARG(7, 0, 0x1E, 0xFF, 8)
#define SOUND_OBJ_GOOMBA_WALK      SND_ARG(5, 0, 0x20, 0x00, 8)
#define SOUND_OBJ_BOBOMB_WALK      SND_ARG(5, 0, 0x27, 0x00, 8)
#define SOUND_OBJ_DEFAULT_DEATH    SND_ARG(5, 0, 0x2C, 0x80, 8)
#define SOUND_OBJ_GOOMBA_ALERT     SND_ARG(5, 0, 0x2F, 0x00, 8)
#define SOUND_OBJ_STOMPED          SND_ARG(5, 0, 0x30, 0x80, 8)
#define SOUND_OBJ_KOOPA_WALK       SND_ARG(5, 0, 0x35, 0x00, 8)
#define SOUND_OBJ_KOOPA_DAMAGE     SND_ARG(5, 0, 0x3E, 0xA0, 8)
#define SOUND_OBJ_ENEMY_DEATH_HIGH SND_ARG(5, 0, 0x60, 0xB0, 8)
#define SOUND_OBJ_ENEMY_DEATH_LOW  SND_ARG(5, 0, 0x61, 0xB0, 8)
#define SOUND_OBJ_KOOPA_FLYGUY_DEATH SND_ARG(5, 0, 0x63, 0xB0, 8)
#define SOUND_AIR_BOBOMB_LIT_FUSE  SND_ARG(6, 0, 0x08, 0x60, 0)
#define SOUND_GENERAL2_BOBOMB_EXPLOSION SND_ARG(8, 0, 0x2E, 0x20, 8)

enum { B_NONE, B_COIN, B_STAR, B_CAP, B_STAR_POWER, B_GOOMBA, B_BOBOMB, B_KOOPA, B_SHELL, B_EXPLOSION, B_SPAWNED_COIN,
       B_MIST, B_SPARKLE, B_CELEB_STAR, B_SMOKE };
enum { L_LEVEL, L_PUSHABLE, L_DESTRUCTIVE, L_GENACTOR };

// ---- math (SM64's angles are s16, 0x10000 to a turn, 0 = +z, sin for x) -------------------------------------------
static float sins(s16 a) { return sinf((float)a * (PI_F / 32768.0f)); }
static float coss(s16 a) { return cosf((float)a * (PI_F / 32768.0f)); }
static s16 atan2s(float z, float x)   // SM64's argument order: the yaw of the vector (x, z)
{
    return (s16)(int)(atan2f(x, z) * (32768.0f / PI_F));
}
static float sqr(float x) { return x * x; }

static s16 approach_s16_symmetric(s16 value, s16 target, s16 inc)
{
    s16 dist = (s16)(target - value);
    if (dist >= 0) { if (dist > inc) value += inc; else value = target; }
    else { if (dist < -inc) value -= inc; else value = target; }
    return value;
}

static s16 abs_angle_diff(s16 x0, s16 x1)
{
    s16 diff = (s16)(x1 - x0);
    if (diff == -0x8000) diff = -0x7FFF;
    if (diff < 0) diff = -diff;
    return diff;
}

static int approach_f32_ptr(float *px, float target, float delta)
{
    if (*px > target) delta = -delta;
    *px += delta;
    if ((*px - target) * delta >= 0) { *px = target; return 1; }
    return 0;
}

static uint32_t s_rng = 0x1234567u;
static float random_float(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (s_rng & 0xFFFFFF) / (float)0x1000000;
}
static u16 random_u16(void) { return (u16)(random_float() * 65536.0f); }
static int random_sign(void) { return random_float() < 0.5f ? -1 : 1; }
static s16 random_linear_offset(s16 base, s16 range) { return (s16)(base + (s16)(range * random_float())); }

// ---- objects ------------------------------------------------------------------------------------------------
typedef struct {
    int valid;
    float n[3];
    int type;
} FloorInfo;

typedef struct Obj {
    int used;
    uint16_t id;
    int bhv, ent, model, list;
    // position and motion
    float pos[3], vel[3], fwd, home[3];
    s16 moveYaw, facePitch, faceYaw, faceRoll;
    float gravity, bounce, buoyancy, drag, friction;
    float wallRadius;
    s16 wallAngle;
    int action, prevAction, subAction, timer;
    u32 moveFlags;
    float floorH;
    FloorInfo floor;         // the floor under it (cur_obj_update_floor)
    FloorInfo stepFloor;     // sObjFloor: the floor object_step found
    float distToMario;
    s16 angleToMario;
    // hit boxes
    u32 interactType, interactSub, interactStatus;
    float hbRadius, hbHeight, hbDown, hurtRadius, hurtHeight;
    int damage, health, numLoot;
    int intangible;
    int hitboxSet;
    int collided[4], nCollided;
    u32 collidedTypes;
    // drawing
    float scale[3];
    float graphYOffset;
    int animState;
    u32 animTable, curAnim;
    int animFrame;
    s32 animAccelAssist, animAccel;
    int billboard, invisible, faceLock;
    int deathSound;
    // a car's hit
    int carHit; s16 carYaw;
    // behaviour specific
    int goombaSize; float goombaScale, goombaRelSpeed; s16 goombaTargetYaw; int goombaWalkTimer, goombaTurning, blinkTimer;
    int koopaType; float koopaAgility; s16 koopaTargetYaw; int koopaCountdown, koopaTurning, koopaTimeUntilTurn;
    float koopaDist; s16 koopaAngle;
    int fuseLit, fuseTimer;
    int heldState;           // HELD_FREE, HELD_HELD (Mario carries it), HELD_THROWN, HELD_DROPPED
    int smokeTimer;
    int capF4, capF8;
    float coinBaseVelY;
    float puffScale; int opacity, opacityStep, puffGrow;
    int celebDiameter;
    int sub;
    int ridden;
    float sndPos[3];     // where its sounds come from, relative to Mario (the audio keeps the pointer)
} Obj;

static Obj s_obj[MAX_OBJ];
static EntHost s_host;
static int s_pickupsOn, s_enemiesOn;
static uint16_t s_nextId = 1;
static u32 s_nextPickupSpawn, s_nextEnemySpawn;
static int s_starTicks;        // the invincibility star
static int s_prevCapFlags;
static uint16_t s_grabOffered;   // the grabbable object last offered to Mario (interact_grabbable)
static uint16_t s_heldId;        // the one he holds
enum { HELD_FREE, HELD_HELD, HELD_THROWN, HELD_DROPPED };

// Mario, as the objects see him
static struct {
    int id;
    float pos[3], vel[3], fwd;
    s16 faceYaw;
    u32 action, prevAction, flags;
    int invinc;
    float hitH;
} M;

static void emit(int kind, int id, const float *pos, float d)
{
    if (s_host.event) s_host.event(kind, id, pos ? pos[0] * S : 0, pos ? -pos[2] * S : 0, pos ? pos[1] * S : 0, d);
}

static void play_sound(u32 bits)
{
    s_host.lock(); sm64_ng64_play_sound(bits); s_host.unlock();
}

// a sound that comes from an object: quieter and panned by how far and which way it is from Mario
static void obj_sound(struct Obj *o, u32 bits);

static void obj_sound(Obj *o, u32 bits)
{
    o->sndPos[0] = o->pos[0] - M.pos[0]; o->sndPos[1] = o->pos[1] - M.pos[1]; o->sndPos[2] = o->pos[2] - M.pos[2];
    s_host.lock(); sm64_play_sound((int32_t)bits, o->sndPos); s_host.unlock();
}

// ---- terrain queries ---------------------------------------------------------------------------------------------
static float find_floor(float x, float y, float z, FloorInfo *fi)
{
    struct SM64SurfaceCollisionData *s = NULL;
    s_host.lock();
    float h = sm64_surface_find_floor(x, y, z, &s);
    s_host.unlock();
    if (!s || h < FLOOR_LOWER_LIMIT + 1) { if (fi) { fi->valid = 0; fi->n[0] = fi->n[2] = 0; fi->n[1] = 1; fi->type = 0; } return FLOOR_LOWER_LIMIT; }
    if (fi) { fi->valid = 1; fi->n[0] = s->normal.x; fi->n[1] = s->normal.y; fi->n[2] = s->normal.z; fi->type = s->type; }
    return h;
}

static float find_water_level(float x, float z)
{
    s_host.lock();
    float w = sm64_surface_find_water_level(x, z);
    s_host.unlock();
    return w;
}

// the walls round a point; the point is pushed out of them. Returns how many, and a wall's normal
static int find_walls(float *x, float *y, float *z, float offsetY, float radius, float *nx, float *nz, int first)
{
    struct SM64WallCollisionData cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = *x; cd.y = *y; cd.z = *z; cd.offsetY = offsetY; cd.radius = radius;
    s_host.lock();
    int n = sm64_surface_find_wall_collisions(&cd);
    s_host.unlock();
    if (n) {
        *x = cd.x; *y = cd.y; *z = cd.z;
        int w = first ? 0 : cd.numWalls - 1;
        if (w < 0) w = 0;
        if (cd.walls[w]) { *nx = cd.walls[w]->normal.x; *nz = cd.walls[w]->normal.z; }
    }
    return n;
}

// ---- animation -----------------------------------------------------------------------------------------------------
static void anim_init(Obj *o, int idx)    // geo_obj_init_animation
{
    u32 a = objrom_anim_from_table(o->animTable, idx);
    if (!a) return;
    if (o->curAnim != a) {
        int st, ls, le, fl;
        if (!objrom_anim_info(a, &st, &ls, &le, &fl)) return;
        o->curAnim = a;
        o->animFrame = st + ((fl & 2) ? 1 : -1);
        o->animAccel = 0;
    }
}

static void anim_init_accel(Obj *o, int idx, float accel)   // geo_obj_init_animation_accel
{
    u32 a = objrom_anim_from_table(o->animTable, idx);
    if (!a) return;
    s32 acc = (s32)(accel * 65536.0f);
    if (o->curAnim != a) {
        int st, ls, le, fl;
        if (!objrom_anim_info(a, &st, &ls, &le, &fl)) return;
        o->curAnim = a;
        o->animAccelAssist = (s32)(((u32)st << 16) + ((fl & 2) ? (u32)acc : (u32)-acc));
        o->animFrame = o->animAccelAssist >> 16;
    }
    o->animAccel = acc;
}

static void anim_update(Obj *o)    // geo_update_animation_frame, once a tick
{
    int st, ls, le, fl;
    if (!o->curAnim || !objrom_anim_info(o->curAnim, &st, &ls, &le, &fl)) return;
    if (fl & 4) return;   // ANIM_FLAG_2: stays on its frame
    s32 result;
    if (fl & 2) {
        result = o->animAccel ? o->animAccelAssist - o->animAccel : (s32)((u32)(o->animFrame - 1) << 16);
        if ((s16)(result >> 16) < ls) result = (fl & 1) ? (s32)((u32)ls << 16) : (s32)((u32)(le - 1) << 16);
    } else {
        result = o->animAccel ? o->animAccelAssist + o->animAccel : (s32)((u32)(o->animFrame + 1) << 16);
        if ((s16)(result >> 16) >= le) result = (fl & 1) ? (s32)((u32)(le - 1) << 16) : (s32)((u32)ls << 16);
    }
    o->animAccelAssist = result;
    o->animFrame = (s16)(result >> 16);
}

static int anim_near_end(Obj *o)       // cur_obj_check_if_near_animation_end
{
    int st, ls, le, fl;
    if (!objrom_anim_info(o->curAnim, &st, &ls, &le, &fl)) return 0;
    int nearEnd = le - 2, is = 0;
    if ((fl & 1) && nearEnd + 1 == o->animFrame) is = 1;
    if (o->animFrame == nearEnd) is = 1;
    return is;
}

static void anim_extend_if_at_end(Obj *o)   // cur_obj_extend_animation_if_at_end
{
    int st, ls, le, fl;
    if (!objrom_anim_info(o->curAnim, &st, &ls, &le, &fl)) return;
    if (o->animFrame == le - 1) o->animFrame--;
}

static int anim_frame_in_range(Obj *o, int start, int len) { return o->animFrame >= start && o->animFrame < start + len; }

// cur_obj_play_sound_at_anim_range
static void anim_sound_at_range(Obj *o, int f1, int f2, u32 sound)
{
    int len = o->animAccel / 0x10000;
    if (len <= 0) len = 1;
    if (anim_frame_in_range(o, f1, len) || anim_frame_in_range(o, f2, len)) obj_sound(o, sound);
}

// ---- hit boxes -------------------------------------------------------------------------------------------------------
typedef struct { u32 type; float down; int damage, health, loot; float radius, height, hurtRadius, hurtHeight; } Hitbox;

static void obj_set_hitbox(Obj *o, const Hitbox *h)
{
    if (!o->hitboxSet) {
        o->hitboxSet = 1;
        o->interactType = h->type; o->damage = h->damage; o->health = h->health; o->numLoot = h->loot;
        o->intangible = 0;
    }
    o->hbRadius = o->scale[0] * h->radius;
    o->hbHeight = o->scale[1] * h->height;
    o->hurtRadius = o->scale[0] * h->hurtRadius;
    o->hurtHeight = o->scale[1] * h->hurtHeight;
    o->hbDown = o->scale[1] * h->down;
}

static void become_intangible(Obj *o) { o->intangible = -1; }
static void become_tangible(Obj *o) { o->intangible = 0; }

// ---- object allocation -----------------------------------------------------------------------------------------------
static Obj *obj_alloc(int bhv, int ent, int model, int list)
{
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *o = &s_obj[i];
        if (o->used) continue;
        memset(o, 0, sizeof(*o));
        o->used = 1;
        o->id = s_nextId++; if (!s_nextId) s_nextId = 1;
        o->bhv = bhv; o->ent = ent; o->model = model; o->list = list;
        o->scale[0] = o->scale[1] = o->scale[2] = 1;
        o->floorH = FLOOR_LOWER_LIMIT;
        return o;
    }
    return NULL;
}

static void obj_delete(Obj *o) { o->used = 0; }

// ---- the engine: stepping, walls, floors ---------------------------------------------------------------------------
static void turn_obj_away_from_surface(float velX, float velZ, float nX, float nZ, float *yawX, float *yawZ)
{
    *yawX = (nZ * nZ - nX * nX) * velX / (nX * nX + nZ * nZ) - 2 * velZ * (nX * nZ) / (nX * nX + nZ * nZ);
    *yawZ = (nX * nX - nZ * nZ) * velZ / (nX * nX + nZ * nZ) - 2 * velX * (nX * nZ) / (nX * nX + nZ * nZ);
}

// object_step's wall test; 0 = it hit one (and was pushed out and turned away)
static int obj_find_wall(Obj *o, float newX, float y, float newZ, float velX, float velZ)
{
    float x = newX, yy = y, z = newZ, nx = 0, nz = 0;
    if (find_walls(&x, &yy, &z, o->hbHeight / 2, o->hbRadius, &nx, &nz, 1)) {
        o->pos[0] = x; o->pos[1] = yy; o->pos[2] = z;
        float yawX, yawZ;
        turn_obj_away_from_surface(velX, velZ, nx, nz, &yawX, &yawZ);
        o->moveYaw = atan2s(yawZ, yawX);
        return 0;
    }
    return 1;
}

static int turn_obj_away_from_steep_floor(Obj *o, const FloorInfo *f, float floorY, float velX, float velZ)
{
    if (!f->valid) {
        o->moveYaw = (s16)(int)((float)o->moveYaw + 32767.9992f);
        return 0;
    }
    if (f->n[1] < 0.5f && floorY > o->pos[1]) {
        float yawX, yawZ;
        turn_obj_away_from_surface(velX, velZ, f->n[0], f->n[2], &yawX, &yawZ);
        o->moveYaw = atan2s(yawZ, yawX);
        return 0;
    }
    return 1;
}

static void calc_obj_friction(Obj *o, float *fr, float floorNY)
{
    if (floorNY < 0.2f && o->friction < 0.9999f) *fr = 0; else *fr = o->friction;
}

static void calc_new_obj_vel_and_pos_y(Obj *o, const FloorInfo *f, float floorY, float velX, float velZ)
{
    float nX = f->n[0], nY = f->n[1], nZ = f->n[2], friction;
    o->vel[1] -= o->gravity;
    if (o->vel[1] > 75.0f) o->vel[1] = 75.0f;
    if (o->vel[1] < -75.0f) o->vel[1] = -75.0f;
    o->pos[1] += o->vel[1];
    if (o->pos[1] < floorY) {
        o->pos[1] = floorY;
        if (o->vel[1] < -17.5f) o->vel[1] = -(o->vel[1] / 2); else o->vel[1] = 0;
    }
    if ((s32)o->pos[1] >= (s32)floorY && (s32)o->pos[1] < (s32)floorY + 37) {
        velX += nX * (nX * nX + nZ * nZ) / (nX * nX + nY * nY + nZ * nZ) * o->gravity * 2;
        velZ += nZ * (nX * nX + nZ * nZ) / (nX * nX + nY * nY + nZ * nZ) * o->gravity * 2;
        if (velX < 0.000001f && velX > -0.000001f) velX = 0;
        if (velZ < 0.000001f && velZ > -0.000001f) velZ = 0;
        if (velX != 0 || velZ != 0) o->moveYaw = atan2s(velZ, velX);
        calc_obj_friction(o, &friction, nY);
        o->fwd = sqrtf(velX * velX + velZ * velZ) * friction;
    }
}

static void calc_new_obj_vel_and_pos_y_underwater(Obj *o, const FloorInfo *f, float floorY, float velX, float velZ, float waterY)
{
    float nX = f->n[0], nY = f->n[1], nZ = f->n[2];
    float netYAccel = (1.0f - o->buoyancy) * (-1.0f * o->gravity);
    o->vel[1] -= netYAccel;
    if (o->vel[1] > 75.0f) o->vel[1] = 75.0f;
    if (o->vel[1] < -75.0f) o->vel[1] = -75.0f;
    o->pos[1] += o->vel[1];
    if (o->pos[1] < floorY) {
        o->pos[1] = floorY;
        if (o->vel[1] < -17.5f) o->vel[1] = -(o->vel[1] / 2); else o->vel[1] = 0;
    }
    if (o->fwd > 12.5f && (waterY + 30.0f) > o->pos[1] && (waterY - 30.0f) < o->pos[1]) o->vel[1] = -o->vel[1];
    if ((s32)o->pos[1] >= (s32)floorY && (s32)o->pos[1] < (s32)floorY + 37) {
        velX += nX * (nX * nX + nZ * nZ) / (nX * nX + nY * nY + nZ * nZ) * netYAccel * 2;
        velZ += nZ * (nX * nX + nZ * nZ) / (nX * nX + nY * nY + nZ * nZ) * netYAccel * 2;
    }
    if (velX < 0.000001f && velX > -0.000001f) velX = 0;
    if (velZ < 0.000001f && velZ > -0.000001f) velZ = 0;
    if (o->vel[1] < 0.000001f && o->vel[1] > -0.000001f) o->vel[1] = 0;
    if (velX != 0 || velZ != 0) o->moveYaw = atan2s(velZ, velX);
    o->fwd = sqrtf(velX * velX + velZ * velZ) * 0.8f;
    o->vel[1] *= 0.8f;
}

static void obj_update_pos_vel_xz(Obj *o)
{
    o->pos[0] += o->fwd * sins(o->moveYaw);
    o->pos[2] += o->fwd * coss(o->moveYaw);
}

// the generic move: walls, floor, water and gravity (used by bob-omb, caps, moving coins)
static int object_step(Obj *o)
{
    float objX = o->pos[0], objY = o->pos[1], objZ = o->pos[2];
    float velX = o->fwd * sins(o->moveYaw), velZ = o->fwd * coss(o->moveYaw);
    int flags = 0;

    if (obj_find_wall(o, objX + velX, objY, objZ + velZ, velX, velZ) == 0) flags += OBJ_COL_FLAG_HIT_WALL;

    float floorY = find_floor(objX + velX, objY, objZ + velZ, &o->stepFloor);
    if (turn_obj_away_from_steep_floor(o, &o->stepFloor, floorY, velX, velZ) == 1) {
        float waterY = find_water_level(objX + velX, objZ + velZ);
        if (waterY > objY) {
            calc_new_obj_vel_and_pos_y_underwater(o, &o->stepFloor, floorY, velX, velZ, waterY);
            flags += OBJ_COL_FLAG_UNDERWATER;
        } else {
            calc_new_obj_vel_and_pos_y(o, &o->stepFloor, floorY, velX, velZ);
        }
    } else {
        flags += ((flags & OBJ_COL_FLAG_HIT_WALL) ^ OBJ_COL_FLAG_HIT_WALL);
    }
    obj_update_pos_vel_xz(o);
    if ((s32)o->pos[1] == (s32)floorY) flags += OBJ_COL_FLAG_GROUNDED;
    if ((s32)o->vel[1] == 0) flags += OBJ_COL_FLAG_NO_Y_VEL;
    return (s16)flags;
}

// ---- cur_obj_move_standard and what it rests on ------------------------------------------------------------------------
static void cur_obj_update_floor(Obj *o)
{
    o->floorH = find_floor(o->pos[0], o->pos[1], o->pos[2], &o->floor);
    if (o->floor.valid) {
        if (o->floor.type == 1) o->moveFlags |= OBJ_MOVE_ABOVE_LAVA;           // SURFACE_BURNING
        else if (o->floor.type == 10) o->moveFlags |= OBJ_MOVE_ABOVE_DEATH_BARRIER;   // SURFACE_DEATH_PLANE
    }
}

static int cur_obj_resolve_wall_collisions(Obj *o)
{
    float radius = o->wallRadius;
    if (radius > 0.1f) {
        float x = (float)(int)o->pos[0], y = (float)(int)o->pos[1], z = (float)(int)o->pos[2], nx = 0, nz = 0;   // (the game truncates to s16: BeamNG maps are bigger)
        if (find_walls(&x, &y, &z, 10.0f, radius, &nx, &nz, 0)) {
            o->pos[0] = x; o->pos[1] = y; o->pos[2] = z;
            o->wallAngle = atan2s(nz, nx);
            return abs_angle_diff(o->wallAngle, o->moveYaw) > 0x4000;
        }
    }
    return 0;
}

static int cur_obj_detect_steep_floor(Obj *o, int steepDeg)
{
    float steepNormalY = coss((s16)(steepDeg * (0x10000 / 360)));
    if (o->fwd != 0.0f) {
        FloorInfo f;
        float ix = o->pos[0] + o->vel[0], iz = o->pos[2] + o->vel[2];
        float ih = find_floor(ix, o->pos[1], iz, &f);
        float delta = ih - o->floorH;
        if (ih < FLOOR_LOWER_LIMIT_MISC) { o->wallAngle = (s16)(o->moveYaw + 0x8000); return 2; }
        else if (f.valid && f.n[1] < steepNormalY && delta > 0 && ih > o->pos[1]) {
            o->wallAngle = atan2s(f.n[2], f.n[0]);
            return 1;
        }
    }
    return 0;
}

static void cur_obj_update_floor_and_walls(Obj *o)
{
    o->moveFlags &= ~(OBJ_MOVE_ABOVE_LAVA | OBJ_MOVE_ABOVE_DEATH_BARRIER);
    o->moveFlags &= ~OBJ_MOVE_HIT_WALL;
    if (cur_obj_resolve_wall_collisions(o)) o->moveFlags |= OBJ_MOVE_HIT_WALL;
    cur_obj_update_floor(o);
    if (o->pos[1] > o->floorH) o->moveFlags |= OBJ_MOVE_IN_AIR;
    if (cur_obj_detect_steep_floor(o, 60)) o->moveFlags |= OBJ_MOVE_HIT_WALL;
}

static void apply_drag_to_value(float *value, float dragStrength)
{
    if (*value != 0) {
        float decel = (*value) * (*value) * (dragStrength * 0.0001f);
        if (*value > 0) { *value -= decel; if (*value < 0.001f) *value = 0; }
        else { *value += decel; if (*value > -0.001f) *value = 0; }
    }
}

static int cur_obj_move_xz(Obj *o, float steepNormalY, int careAboutEdgesAndSteepSlopes)
{
    FloorInfo f;
    float ix = o->pos[0] + o->vel[0], iz = o->pos[2] + o->vel[2];
    float ih = find_floor(ix, o->pos[1], iz, &f);
    float delta = ih - o->floorH;
    o->moveFlags &= ~OBJ_MOVE_HIT_EDGE;
    if (ih < FLOOR_LOWER_LIMIT_MISC) { o->moveFlags |= OBJ_MOVE_HIT_EDGE; return 0; }
    else if (delta < 5.0f) {
        if (!careAboutEdgesAndSteepSlopes) { o->pos[0] = ix; o->pos[2] = iz; return 1; }
        else if (delta < -50.0f && (o->moveFlags & OBJ_MOVE_ON_GROUND)) { o->moveFlags |= OBJ_MOVE_HIT_EDGE; return 0; }
        else if (f.n[1] > steepNormalY) { o->pos[0] = ix; o->pos[2] = iz; return 1; }
        else { o->moveFlags |= OBJ_MOVE_HIT_EDGE; return 0; }
    } else if (f.n[1] > steepNormalY || o->pos[1] > ih) {
        o->pos[0] = ix; o->pos[2] = iz;
    }
    return 0;
}

static int clear_move_flag(u32 *set, u32 flag)
{
    if (*set & flag) { *set &= ~flag; return 1; }
    return 0;
}

static void cur_obj_move_update_underwater_flags(Obj *o)
{
    float decelY = (float)(sqrtf(o->vel[1] * o->vel[1]) * (o->drag * 7.0f)) / 100.0f;
    if (o->vel[1] > 0) o->vel[1] -= decelY; else o->vel[1] += decelY;
    if (o->pos[1] < o->floorH) { o->pos[1] = o->floorH; o->moveFlags |= OBJ_MOVE_UNDERWATER_ON_GROUND; }
    else o->moveFlags |= OBJ_MOVE_UNDERWATER_OFF_GROUND;
}

static void cur_obj_move_update_ground_air_flags(Obj *o, float bounciness)
{
    o->moveFlags &= ~OBJ_MOVE_BOUNCE;
    if (o->pos[1] < o->floorH) {
        if (!(o->moveFlags & OBJ_MOVE_ON_GROUND)) {
            if (clear_move_flag(&o->moveFlags, OBJ_MOVE_LANDED)) o->moveFlags |= OBJ_MOVE_ON_GROUND;
            else o->moveFlags |= OBJ_MOVE_LANDED;
        }
        o->pos[1] = o->floorH;
        if (o->vel[1] < 0.0f) o->vel[1] *= bounciness;
        if (o->vel[1] > 5.0f) o->moveFlags |= OBJ_MOVE_BOUNCE;
    } else {
        o->moveFlags &= ~OBJ_MOVE_LANDED;
        if (clear_move_flag(&o->moveFlags, OBJ_MOVE_ON_GROUND)) o->moveFlags |= OBJ_MOVE_LEFT_GROUND;
    }
    o->moveFlags &= ~OBJ_MOVE_MASK_IN_WATER;
}

static float cur_obj_move_y_and_get_water_level(Obj *o, float gravity, float buoyancy)
{
    o->vel[1] += gravity + buoyancy;
    if (o->vel[1] < -78.0f) o->vel[1] = -78.0f;
    o->pos[1] += o->vel[1];
    return find_water_level(o->pos[0], o->pos[2]);
}

static void cur_obj_move_y(Obj *o, float gravity, float bounciness, float buoyancy)
{
    float waterLevel;
    o->moveFlags &= ~OBJ_MOVE_LEFT_GROUND;
    if (o->moveFlags & OBJ_MOVE_AT_WATER_SURFACE) {
        if (o->vel[1] > 5.0f) { o->moveFlags &= ~OBJ_MOVE_MASK_IN_WATER; o->moveFlags |= OBJ_MOVE_LEAVING_WATER; }
    }
    if (!(o->moveFlags & OBJ_MOVE_MASK_IN_WATER)) {
        waterLevel = cur_obj_move_y_and_get_water_level(o, gravity, 0.0f);
        if (o->pos[1] > waterLevel) cur_obj_move_update_ground_air_flags(o, bounciness);
        else { o->moveFlags |= OBJ_MOVE_ENTERED_WATER; o->moveFlags &= ~OBJ_MOVE_MASK_ON_GROUND; }
    } else {
        o->moveFlags &= ~OBJ_MOVE_ENTERED_WATER;
        waterLevel = cur_obj_move_y_and_get_water_level(o, gravity, buoyancy);
        if (o->pos[1] < waterLevel) cur_obj_move_update_underwater_flags(o);
        else {
            if (o->pos[1] < o->floorH) { o->pos[1] = o->floorH; o->moveFlags &= ~OBJ_MOVE_MASK_IN_WATER; }
            else {
                o->pos[1] = waterLevel; o->vel[1] = 0.0f;
                o->moveFlags &= ~(OBJ_MOVE_UNDERWATER_OFF_GROUND | OBJ_MOVE_UNDERWATER_ON_GROUND);
                o->moveFlags |= OBJ_MOVE_AT_WATER_SURFACE;
            }
        }
    }
    if (o->moveFlags & (OBJ_MOVE_MASK_ON_GROUND | OBJ_MOVE_AT_WATER_SURFACE | OBJ_MOVE_UNDERWATER_OFF_GROUND)) o->moveFlags &= ~OBJ_MOVE_IN_AIR;
    else o->moveFlags |= OBJ_MOVE_IN_AIR;
}

static void cur_obj_compute_vel_xz(Obj *o)
{
    o->vel[0] = o->fwd * sins(o->moveYaw);
    o->vel[2] = o->fwd * coss(o->moveYaw);
}

static void cur_obj_move_standard(Obj *o, int steepSlopeAngleDegrees)
{
    int careAboutEdgesAndSteepSlopes = 0, negativeSpeed = 0;
    if (steepSlopeAngleDegrees < 0) { careAboutEdgesAndSteepSlopes = 1; steepSlopeAngleDegrees = -steepSlopeAngleDegrees; }
    float steepNormalY = coss((s16)(steepSlopeAngleDegrees * (0x10000 / 360)));
    cur_obj_compute_vel_xz(o);
    apply_drag_to_value(&o->vel[0], o->drag);
    apply_drag_to_value(&o->vel[2], o->drag);
    cur_obj_move_xz(o, steepNormalY, careAboutEdgesAndSteepSlopes);
    cur_obj_move_y(o, o->gravity, o->bounce, o->buoyancy);
    if (o->fwd < 0.0f) negativeSpeed = 1;
    o->fwd = sqrtf(sqr(o->vel[0]) + sqr(o->vel[2]));
    if (negativeSpeed) o->fwd = -o->fwd;
}

// ---- movement helpers ------------------------------------------------------------------------------------------------
static int cur_obj_rotate_yaw_toward(Obj *o, s16 target, s16 inc)
{
    s16 start = o->moveYaw;
    o->moveYaw = approach_s16_symmetric(o->moveYaw, target, inc);
    return (s16)(o->moveYaw - start) == 0;
}

static s16 obj_angle_to_mario(Obj *o) { return atan2s(M.pos[2] - o->pos[2], M.pos[0] - o->pos[0]); }

static s16 cur_obj_reflect_move_angle_off_wall(Obj *o)
{
    return (s16)(o->wallAngle - ((s16)o->moveYaw - (s16)o->wallAngle) + 0x8000);
}

static int obj_forward_vel_approach(Obj *o, float target, float delta) { return approach_f32_ptr(&o->fwd, target, delta); }

static s16 obj_random_fixed_turn(Obj *o, s16 delta) { return (s16)(o->moveYaw + (s16)random_sign() * delta); }

static int is_point_within_radius_of_mario(float x, float y, float z, int dist)
{
    return sqr(x - M.pos[0]) + sqr(y - M.pos[1]) + sqr(z - M.pos[2]) < (float)(dist * dist);
}

static int obj_check_if_facing_toward_angle(u32 base, u32 goal, s16 range)
{
    s16 dAngle = (s16)((u16)goal - (u16)base);
    return (sins((s16)-range) < sins(dAngle)) && (sins(dAngle) < sins(range)) && (coss(dAngle) > 0);
}

static int obj_return_home_if_safe(Obj *o, float homeX, float y, float homeZ, int dist)
{
    s16 angleTowardsHome = atan2s(homeZ - o->pos[2], homeX - o->pos[0]);
    if (is_point_within_radius_of_mario(homeX, y, homeZ, dist)) return 1;
    o->moveYaw = approach_s16_symmetric(o->moveYaw, angleTowardsHome, 320);
    return 0;
}

static void treat_far_home_as_mario(Obj *o, float threshold)
{
    float dx = o->home[0] - o->pos[0], dy = o->home[1] - o->pos[1], dz = o->home[2] - o->pos[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance > threshold) {
        o->angleToMario = atan2s(dz, dx);
        o->distToMario = 25000.0f;
    } else {
        dx = o->home[0] - M.pos[0]; dy = o->home[1] - M.pos[1]; dz = o->home[2] - M.pos[2];
        distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (distance > threshold) o->distToMario = 20000.0f;
    }
}

static void obj_update_blinking(Obj *o, int *blinkTimer, s16 baseCycleLength, s16 cycleLengthRange, s16 blinkLength)
{
    if (*blinkTimer != 0) (*blinkTimer)--;
    else *blinkTimer = random_linear_offset(baseCycleLength, cycleLengthRange);
    o->animState = (*blinkTimer > blinkLength) ? 0 : 1;
}

// ---- collisions between objects ----------------------------------------------------------------------------------------
static int detect_object_hitbox_overlap(Obj *a, Obj *b)
{
    float ay = a->pos[1] - a->hbDown, by = b->pos[1] - b->hbDown;
    float dx = a->pos[0] - b->pos[0], dz = a->pos[2] - b->pos[2];
    float r = a->hbRadius + b->hbRadius;
    if (r > sqrtf(dx * dx + dz * dz)) {
        float aTop = a->hbHeight + ay, bTop = b->hbHeight + by;
        if (ay > bTop) return 0;
        if (aTop < by) return 0;
        if (a->nCollided >= 4 || b->nCollided >= 4) return 0;
        return 1;
    }
    return 0;
}

// obj_resolve_object_collisions: pushed apart from the first other (non-Mario) object it overlaps
static int obj_resolve_object_collisions(Obj *o, s16 *targetYaw)
{
    if (o->nCollided != 0) {
        Obj *other = &s_obj[o->collided[0]];
        float dx = other->pos[0] - o->pos[0], dz = other->pos[2] - o->pos[2];
        s16 angle = atan2s(dx, dz);     // sic: the game passes them the wrong way round
        float radius = o->hbRadius, otherRadius = other->hbRadius;
        float rel = radius / (radius + otherRadius);
        float cx = o->pos[0] + dx * rel, cz = o->pos[2] + dz * rel;
        o->pos[0] = cx - radius * coss(angle);
        o->pos[2] = cz - radius * sins(angle);
        other->pos[0] = cx + otherRadius * coss(angle);
        other->pos[2] = cz + otherRadius * sins(angle);
        if (targetYaw && abs_angle_diff(o->moveYaw, angle) < 0x4000) {
            *targetYaw = (s16)(angle - o->moveYaw + angle + 0x8000);
            return 1;
        }
    }
    return 0;
}

static int obj_bounce_off_walls_edges_objects(Obj *o, s16 *targetYaw)
{
    if (o->moveFlags & OBJ_MOVE_HIT_WALL) *targetYaw = cur_obj_reflect_move_angle_off_wall(o);
    else if (o->moveFlags & OBJ_MOVE_HIT_EDGE) *targetYaw = (s16)(o->moveYaw + 0x8000);
    else if (!obj_resolve_object_collisions(o, targetYaw)) return 0;
    return 1;
}

static int obj_resolve_collisions_and_turn(Obj *o, s16 targetYaw, s16 turnSpeed)
{
    obj_resolve_object_collisions(o, NULL);
    return !cur_obj_rotate_yaw_toward(o, targetYaw, turnSpeed);
}

// ---- coins that burst out ------------------------------------------------------------------------------------------------
static const Hitbox k_coinHb = { INTERACT_COIN, 0, 1, 0, 0, 100, 64, 0, 0 };
static const Hitbox k_redCoinHb = { INTERACT_COIN, 0, 2, 0, 0, 100, 64, 0, 0 };
static const Hitbox k_blueCoinHb = { INTERACT_COIN, 0, 5, 0, 0, 100, 64, 0, 0 };

static Obj *spawn_coin_burst(const float *pos, int blue, float baseVelY)
{
    Obj *c = obj_alloc(B_SPAWNED_COIN, blue ? ENT_COIN_BLUE : ENT_COIN_YELLOW, blue ? OM_COIN_BLUE : OM_COIN_YELLOW, L_LEVEL);
    if (!c) return NULL;
    memcpy(c->pos, pos, 12);
    c->billboard = 1;
    c->coinBaseVelY = blue ? 20.0f : baseVelY;
    c->gravity = -4.0f; c->bounce = -0.7f; c->drag = 10.0f; c->friction = 10.0f; c->buoyancy = 2.0f;
    c->wallRadius = 30.0f;
    // bhv_spawned_coin_init
    c->vel[1] = random_float() * 10.0f + 30.0f + c->coinBaseVelY;
    c->fwd = random_float() * 10.0f;
    c->moveYaw = (s16)random_u16();
    c->animState = -1;
    obj_set_hitbox(c, blue ? &k_blueCoinHb : &k_coinHb);
    if (blue) c->hbRadius = 120.0f;
    become_intangible(c);
    return c;
}

// ---- particles -------------------------------------------------------------------------------------------------------
// spawn_mist_particles_variable / cur_obj_spawn_particles with sMistParticles: white puffs (MODEL_MIST,
// bhvWhitePuffExplosion) flung out round a point. count 0 means 20, up to 20 means 4.
static void spawn_mist_particles_at(const float *pos, int count, float offsetY, float size)
{
    float sizeBase = size, sizeRange = size / 20.0f;
    int n = count == 0 ? 20 : count > 20 ? count : 4;
    for (int i = 0; i < n; i++) {
        float sc = random_float() * (sizeRange * 0.1f) + sizeBase * 0.1f;
        Obj *p = obj_alloc(B_MIST, ENT_MIST, OM_MIST, L_LEVEL);
        if (!p) return;
        memcpy(p->pos, pos, 12);
        p->pos[1] += offsetY;
        p->sub = 2;                         // oBhvParams2ndByte
        p->moveYaw = (s16)random_u16();
        p->gravity = -4.0f;                 // 252 as the s8 it is
        p->drag = 30.0f;
        p->fwd = random_float() * 5.0f + 40.0f;
        p->vel[1] = random_float() * 20.0f + 30.0f;
        p->scale[0] = p->scale[1] = p->scale[2] = sc;
        p->billboard = 1;
        become_intangible(p);
    }
}

static void spawn_mist(Obj *o) { spawn_mist_particles_at(o->pos, 0, 0, 46.0f); }   // spawn_mist_particles

static void bhv_white_puff_exploding_loop(Obj *o)
{
    if (o->timer == 0) {
        cur_obj_compute_vel_xz(o);
        o->puffScale = o->scale[0];
        if (o->sub == 2) { o->opacity = 254; o->opacityStep = -21; o->puffGrow = 0; }
        else if (o->sub == 3) { o->opacity = 254; o->opacityStep = -13; o->puffGrow = 1; }
    }
    // cur_obj_move_using_vel_and_gravity, cur_obj_apply_drag_xz
    o->pos[0] += o->vel[0]; o->pos[2] += o->vel[2];
    o->vel[1] += o->gravity; o->pos[1] += o->vel[1];
    apply_drag_to_value(&o->vel[0], o->drag);
    apply_drag_to_value(&o->vel[2], o->drag);
    if (o->vel[1] > 100.0f) o->vel[1] = 100.0f;
    if (o->timer > 20) { obj_delete(o); return; }
    if (o->opacity) {
        o->opacity += o->opacityStep;
        if (o->opacity < 2) { obj_delete(o); return; }
        // the puff fades as it shrinks; BeamNG's cut-out material can't fade, so the shrinking carries it
        float sc = o->puffGrow ? o->puffScale * ((254 - o->opacity) / 254.0f) : o->puffScale * (o->opacity / 254.0f);
        o->scale[0] = o->scale[1] = o->scale[2] = sc;
    }
}

// a star's celebration (bhvCelebrationStar): it rises spinning round Mario as he dances, then faces the camera, grows
// back and goes; sparkles (bhvCelebrationStarSparkle) drop from it as it rises
static void spawn_celebration_star(void)
{
    Obj *o = obj_alloc(B_CELEB_STAR, ENT_CELEB_STAR, OM_STAR, L_LEVEL);
    if (!o) return;
    o->home[0] = M.pos[0]; o->home[2] = M.pos[2];
    o->pos[0] = M.pos[0]; o->pos[1] = M.pos[1] + 30.0f; o->pos[2] = M.pos[2];
    o->moveYaw = (s16)(M.faceYaw + 0x8000);
    o->celebDiameter = 100;
    o->scale[0] = o->scale[1] = o->scale[2] = 0.4f;
    become_intangible(o);
}

static void bhv_celebration_star_loop(Obj *o)
{
    if (o->action == 0) {   // CELEB_STAR_ACT_SPIN_AROUND_MARIO
        o->pos[0] = o->home[0] + sins(o->moveYaw) * (float)(o->celebDiameter / 2);
        o->pos[2] = o->home[2] + coss(o->moveYaw) * (float)(o->celebDiameter / 2);
        o->pos[1] += 5.0f;
        o->faceYaw += 0x1000;
        o->moveYaw += 0x2000;
        if (o->timer == 40) o->action = 1;
        if (o->timer < 35) {
            Obj *sp = obj_alloc(B_SPARKLE, ENT_SPARKLES, OM_SPARKLES, L_LEVEL);
            if (sp) { memcpy(sp->pos, o->pos, 12); sp->graphYOffset = 25; sp->animState = -1; sp->billboard = 1; become_intangible(sp); }
            o->celebDiameter++;
        } else o->celebDiameter -= 20;
    } else {                // CELEB_STAR_ACT_FACE_CAMERA
        if (o->timer < 10) {
            float sc = (float)o->timer / 10.0f;
            o->scale[0] = o->scale[1] = o->scale[2] = sc;
            o->faceYaw += 0x1000;
        } else o->faceYaw = M.faceYaw;
        if (o->timer == 59) obj_delete(o);
    }
}

static void bhv_celebration_star_sparkle_loop(Obj *o)
{
    o->animState++;          // the script's ADD_INT(oAnimState, 1): the sparkle geo's frames
    o->pos[1] -= 15.0f;
    if (o->timer == 12) obj_delete(o);
}

static void obj_spawn_loot_yellow_coins(Obj *o, int n, float baseVelY)
{
    FloorInfo f;
    float spawnHeight = find_floor(o->pos[0], o->pos[1], o->pos[2], &f);
    if (o->pos[1] - spawnHeight > 100.0f) spawnHeight = o->pos[1];
    for (int i = 0; i < n; i++) {
        if (o->numLoot <= 0) break;
        o->numLoot--;
        float p[3] = { o->pos[0], spawnHeight, o->pos[2] };
        spawn_coin_burst(p, 0, baseVelY);
    }
}

// ---- Mario meets an object ---------------------------------------------------------------------------------------------
static u32 determine_interaction(float objX, float objY, float objZ)
{
    u32 interaction = 0;
    u32 action = M.action;
    s16 dYaw = (s16)(atan2s(objZ - M.pos[2], objX - M.pos[0]) - M.faceYaw);
    if (action & ACT_FLAG_ATTACKING) {
        if (action == ACT_PUNCHING || action == ACT_MOVE_PUNCHING || action == ACT_JUMP_KICK) {
            if (M.flags & MARIO_PUNCHING) { if (-0x2AAA <= dYaw && dYaw <= 0x2AAA) interaction = INT_PUNCH; }
            if (M.flags & MARIO_KICKING) { if (-0x2AAA <= dYaw && dYaw <= 0x2AAA) interaction = INT_KICK; }
            if (M.flags & MARIO_TRIPPING) { if (-0x4000 <= dYaw && dYaw <= 0x4000) interaction = INT_TRIP; }
        } else if (action == ACT_GROUND_POUND || action == ACT_TWIRLING) {
            if (M.vel[1] < 0.0f) interaction = INT_GROUND_POUND_OR_TWIRL;
        } else if (action == ACT_GROUND_POUND_LAND || action == ACT_TWIRL_LAND) {
            if (M.vel[1] < 0.0f && (M.prevAction == ACT_GROUND_POUND || M.prevAction == ACT_TWIRLING)) interaction = INT_GROUND_POUND_OR_TWIRL;
        } else if (action == ACT_SLIDE_KICK || action == ACT_SLIDE_KICK_SLIDE) {
            interaction = INT_SLIDE_KICK;
        } else if (action & ACT_FLAG_RIDING_SHELL) {
            interaction = INT_FAST_ATTACK_OR_SHELL;
        } else if (M.fwd <= -26.0f || 26.0f <= M.fwd) {
            interaction = INT_FAST_ATTACK_OR_SHELL;
        }
    }
    if (interaction == 0 && (action & ACT_FLAG_AIR)) {
        if (M.vel[1] < 0.0f) { if (M.pos[1] > objY) interaction = INT_HIT_FROM_ABOVE; }
        else { if (M.pos[1] < objY) interaction = INT_HIT_FROM_BELOW; }
    }
    return interaction;
}

static u32 attack_object(Obj *o, u32 interaction)
{
    u32 attackType = 0;
    switch (interaction) {
    case INT_GROUND_POUND_OR_TWIRL: attackType = ATTACK_GROUND_POUND_OR_TWIRL; break;
    case INT_PUNCH: attackType = ATTACK_PUNCH; break;
    case INT_KICK: case INT_TRIP: attackType = ATTACK_KICK_OR_TRIP; break;
    case INT_SLIDE_KICK: case INT_FAST_ATTACK_OR_SHELL: attackType = ATTACK_FAST_ATTACK; break;
    case INT_HIT_FROM_ABOVE: attackType = ATTACK_FROM_ABOVE; break;
    case INT_HIT_FROM_BELOW: attackType = ATTACK_FROM_BELOW; break;
    }
    o->interactStatus = attackType + (INT_STATUS_INTERACTED | INT_STATUS_WAS_ATTACKED);
    return attackType;
}

static int mario_invulnerable(void) { return (M.action & ACT_FLAG_INVULNERABLE) || M.invinc != 0; }

// take_damage_and_knock_back: libsm64 does Mario's half (hurt counter, knockback action); the object learns it hit him
static int take_damage_and_knock_back(Obj *o)
{
    if (!mario_invulnerable() && !(M.flags & MARIO_VANISH_CAP) && !(o->interactSub & INT_SUBTYPE_DELAY_INVINCIBILITY)) {
        o->interactStatus = INT_STATUS_INTERACTED | INT_STATUS_ATTACKED_MARIO;
        s_host.lock();
        sm64_mario_take_damage(M.id, (u32)o->damage, 0, o->pos[0], o->pos[1], o->pos[2]);
        s_host.unlock();
        return 1;
    }
    return 0;
}

// interact_bounce_top (goombas and koopas)
static void interact_bounce_top(Obj *o)
{
    u32 interaction = (M.flags & MARIO_METAL_CAP) ? INT_FAST_ATTACK_OR_SHELL : determine_interaction(o->pos[0], o->pos[1], o->pos[2]);
    if (interaction & INT_ATTACK_NOT_FROM_BELOW) {
        attack_object(o, interaction);
        // Mario's half (bounce off it, bounce back from a punch): libsm64's own port of the same code
        s_host.lock();
        sm64_mario_attack(M.id, o->pos[0], o->pos[1], o->pos[2], o->hbHeight);
        s_host.unlock();
    } else {
        take_damage_and_knock_back(o);
    }
}

static void interact_damage(Obj *o) { take_damage_and_knock_back(o); }

static void interact_coin(Obj *o)
{
    o->interactStatus = INT_STATUS_INTERACTED;
    int v = o->damage;
    s_host.lock(); sm64_mario_heal(M.id, (uint8_t)(4 * v)); s_host.unlock();
    play_sound(v >= 2 ? SOUND_MENU_COLLECT_RED_COIN : SOUND_GENERAL_COIN);
    emit(EV_COIN, o->id, o->pos, (float)v);
}

static void interact_star(Obj *o)
{
    o->interactStatus = INT_STATUS_INTERACTED;
    play_sound(SOUND_MENU_STAR_SOUND);
    s_host.lock(); sm64_mario_heal(M.id, 31); s_host.unlock();
    if (o->bhv == B_STAR_POWER) { s_starTicks = 20 * TICKS_PER_S; emit(EV_STAR_POWER, o->id, o->pos, 20); return; }
    // a Power Star: interact_star_or_key as for a star that doesn't leave the level (a 100-coin star's): the puff where
    // it was, and Mario's star dance (in water, or falling to the ground first if he caught it in the air); the dance
    // brings the celebration star and the jingle (ents_tick, main.c)
    spawn_mist_particles_at(o->pos, 0, 10, 30.0f);   // bhvStarKeyCollectionPuffSpawner
    u32 grab = ACT_STAR_DANCE_NO_EXIT;
    if (M.action & (ACT_FLAG_SWIMMING | ACT_FLAG_METAL_WATER)) grab = ACT_STAR_DANCE_WATER;
    if (M.action & ACT_FLAG_AIR) grab = ACT_FALL_AFTER_STAR_GRAB;
    s_host.lock(); sm64_set_mario_action_arg(M.id, grab, 1); s_host.unlock();
    emit(EV_POWER_STAR, o->id, o->pos, 1);
}

static void interact_cap(Obj *o)
{
    u32 flag = o->sub == 1 ? MARIO_METAL_CAP : MARIO_WING_CAP;
    o->interactStatus = INT_STATUS_INTERACTED;
    s_host.lock(); sm64_mario_interact_cap(M.id, flag, 0, 0); s_host.unlock();
    emit(flag == MARIO_METAL_CAP ? EV_CAP_METAL : EV_CAP_WING, o->id, o->pos, flag == MARIO_METAL_CAP ? 20 : 60);
}

static void interact_koopa_shell(Obj *o)
{
    if (!(M.action & ACT_FLAG_RIDING_SHELL)) {
        u32 interaction = determine_interaction(o->pos[0], o->pos[1], o->pos[2]);
        if (interaction == INT_HIT_FROM_ABOVE || M.action == ACT_WALKING || M.action == ACT_HOLD_WALKING) {
            attack_object(o, interaction);
            s_host.lock(); sm64_set_mario_action(M.id, ACT_RIDING_SHELL_GROUND); s_host.unlock();
            o->ridden = 1;
        }
    }
}

static void interact_grabbable(Obj *o)
{
    // only the kickable part of the game's handler: a kick or trip hits it (the pick-up and throw aren't done)
    if (o->interactSub & INT_SUBTYPE_KICKABLE) {
        u32 interaction = determine_interaction(o->pos[0], o->pos[1], o->pos[2]);
        if (interaction & (INT_KICK | INT_TRIP)) {
            attack_object(o, interaction);
            s_host.lock();
            sm64_mario_attack(M.id, o->pos[0], o->pos[1], o->pos[2], o->hbHeight);   // bounce back, as bounce_back_from_attack
            s_host.unlock();
            return;
        }
    }
    // able_to_grab_object: a punch (its first part: Mario's own grab check only runs then) or a dive. Mario's code then
    // decides, on his next tick, whether he faces it and picks it up (libsm64 patch: sm64_ng64_offer_grab)
    if (M.action == ACT_PUNCHING || M.action == ACT_MOVE_PUNCHING || M.action == ACT_DIVE || M.action == ACT_DIVE_SLIDE) {
        if (!s_heldId) {
            s_host.lock(); sm64_ng64_offer_grab(M.id, o->pos[0], o->pos[1], o->pos[2]); s_host.unlock();
            s_grabOffered = o->id;
        }
    }
}

static void mario_touches(Obj *o)
{
    switch (o->interactType) {
    case INTERACT_COIN: interact_coin(o); break;
    case INTERACT_STAR_OR_KEY: interact_star(o); break;
    case INTERACT_CAP: interact_cap(o); break;
    case INTERACT_KOOPA_SHELL: interact_koopa_shell(o); break;
    case INTERACT_BOUNCE_TOP: case INTERACT_KOOPA: interact_bounce_top(o); break;
    case INTERACT_DAMAGE: interact_damage(o); break;
    case INTERACT_GRABBABLE: interact_grabbable(o); break;
    }
}

// the order SM64's handler table walks the interaction types in
static const u32 k_order[] = { INTERACT_COIN, INTERACT_STAR_OR_KEY, INTERACT_BOUNCE_TOP, INTERACT_DAMAGE, INTERACT_KOOPA,
                               INTERACT_KOOPA_SHELL, INTERACT_CAP, INTERACT_GRABBABLE };

// detect_object_collisions + mario_process_interactions
static void detect_collisions_and_interact(void)
{
    for (int i = 0; i < MAX_OBJ; i++) { s_obj[i].nCollided = 0; s_obj[i].collidedTypes = 0; if (s_obj[i].used && s_obj[i].intangible > 0) s_obj[i].intangible--; }

    // Mario against everything tangible
    int marioHits[MAX_OBJ], nMario = 0;
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *b = &s_obj[i];
        if (!b->used || b->intangible != 0 || !b->hitboxSet) continue;
        float my = M.pos[1];
        float dx = M.pos[0] - b->pos[0], dz = M.pos[2] - b->pos[2];
        float dist = sqrtf(dx * dx + dz * dz);
        float by = b->pos[1] - b->hbDown, bTop = b->hbHeight + by, mTop = M.hitH + my;
        if (37.0f + b->hbRadius > dist && !(my > bTop) && !(mTop < by)) {
            marioHits[nMario++] = i;
            // detect_object_hurtbox_overlap: only a touch of the hurt boxes hurts
            if (b->hurtRadius != 0.0f) {
                b->interactSub |= INT_SUBTYPE_DELAY_INVINCIBILITY;
                float hTop = b->hurtHeight + by;
                if (b->hurtRadius > dist && !(my > hTop) && !(mTop < by)) b->interactSub &= ~INT_SUBTYPE_DELAY_INVINCIBILITY;
            }
        }
    }
    // the objects against each other: pushables with pushables, destructive things with the actors
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *a = &s_obj[i];
        if (!a->used || a->intangible != 0 || !a->hitboxSet) continue;
        if (a->list != L_PUSHABLE && a->list != L_DESTRUCTIVE) continue;
        for (int j = 0; j < MAX_OBJ; j++) {
            Obj *b = &s_obj[j];
            if (j == i || !b->used || b->intangible != 0 || !b->hitboxSet) continue;
            int pair = (a->list == L_PUSHABLE && b->list == L_PUSHABLE) ||
                       (a->list == L_DESTRUCTIVE && (b->list == L_DESTRUCTIVE || b->list == L_PUSHABLE || b->list == L_GENACTOR));
            if (!pair) continue;
            if (a->list == L_DESTRUCTIVE && a->distToMario >= 2000.0f) continue;
            if (detect_object_hitbox_overlap(a, b)) {
                a->collided[a->nCollided++] = j;
                a->collidedTypes |= b->interactType;
            }
        }
    }
    // Mario's handlers, in SM64's order, one object per type
    for (size_t k = 0; k < sizeof(k_order) / sizeof(k_order[0]); k++) {
        u32 type = k_order[k];
        Obj *hit = NULL;
        for (int h = 0; h < nMario; h++) if (s_obj[marioHits[h]].interactType == type) { hit = &s_obj[marioHits[h]]; break; }
        if (!hit) continue;
        if (!(hit->interactStatus & INT_STATUS_INTERACTED)) mario_touches(hit);
    }
}

// ---- how the enemies die ---------------------------------------------------------------------------------------------
static void obj_die_if_health_non_positive(Obj *o)
{
    if (o->health <= 0) {
        spawn_mist(o);
        obj_sound(o, o->deathSound ? (u32)o->deathSound : SOUND_OBJ_DEFAULT_DEATH);
        if (o->numLoot < 0) { float p[3] = { o->pos[0], o->pos[1], o->pos[2] }; spawn_coin_burst(p, 1, 20.0f); }
        else obj_spawn_loot_yellow_coins(o, o->numLoot, 20.0f);
        emit(EV_ENEMY_DEAD, o->id, o->pos, (float)o->ent);
        obj_delete(o);
    }
}

// where a hit sends it: away from Mario, or the way the car that hit it was going
static s16 knock_yaw(Obj *o)
{
    if (o->carHit) { o->carHit = 0; return o->carYaw; }
    return atan2s(o->pos[2] - M.pos[2], o->pos[0] - M.pos[0]);
}

static void obj_set_knockback_action(Obj *o, u32 attackType)
{
    switch (attackType) {
    case ATTACK_KICK_OR_TRIP: case ATTACK_FAST_ATTACK:
        o->action = OBJ_ACT_VERTICAL_KNOCKBACK; o->fwd = 20.0f; o->vel[1] = 50.0f; break;
    default:
        o->action = OBJ_ACT_HORIZONTAL_KNOCKBACK; o->fwd = 50.0f; o->vel[1] = 30.0f; break;
    }
    o->faceLock = 1;     // OBJ_FLAG_SET_FACE_YAW_TO_MOVE_YAW is cleared: it tumbles without turning to its heading
    o->moveYaw = knock_yaw(o);
}

static void obj_set_squished_action(Obj *o)
{
    obj_sound(o, SOUND_OBJ_STOMPED);
    o->action = OBJ_ACT_SQUISHED;
}

static void obj_act_knockback(Obj *o)
{
    cur_obj_update_floor_and_walls(o);
    if (o->curAnim) anim_extend_if_at_end(o);
    if ((o->moveFlags & (OBJ_MOVE_MASK_ON_GROUND | OBJ_MOVE_MASK_IN_WATER | OBJ_MOVE_HIT_WALL | OBJ_MOVE_ABOVE_LAVA))
        || (o->action == OBJ_ACT_VERTICAL_KNOCKBACK && o->timer >= 9)) obj_die_if_health_non_positive(o);
    if (o->used) cur_obj_move_standard(o, -78);
}

static void obj_act_squished(Obj *o, float baseScale)
{
    float targetScaleY = baseScale * 0.3f;
    cur_obj_update_floor_and_walls(o);
    if (o->curAnim) anim_extend_if_at_end(o);
    if (approach_f32_ptr(&o->scale[1], targetScaleY, baseScale * 0.14f)) {
        o->scale[0] = o->scale[2] = baseScale * 2.0f - o->scale[1];
        if (o->timer > 15) obj_die_if_health_non_positive(o);
    }
    if (!o->used) return;
    o->fwd = 0.0f;
    cur_obj_move_standard(o, -78);
}

// returns 1 if the normal behaviour should run
static int obj_update_standard_actions(Obj *o, float scale)
{
    if (o->action < 100) return 1;
    become_intangible(o);
    switch (o->action) {
    case OBJ_ACT_HORIZONTAL_KNOCKBACK: case OBJ_ACT_VERTICAL_KNOCKBACK: obj_act_knockback(o); break;
    case OBJ_ACT_SQUISHED: obj_act_squished(o, scale); break;
    }
    return 0;
}

static int obj_die_if_above_lava_and_health_non_positive(Obj *o)
{
    if (o->moveFlags & OBJ_MOVE_UNDERWATER_ON_GROUND) {
        if (o->gravity + o->buoyancy > 0.0f || find_water_level(o->pos[0], o->pos[2]) - o->pos[1] < 150.0f) return 0;
    } else if (!(o->moveFlags & OBJ_MOVE_ABOVE_LAVA)) {
        return 0;
    }
    obj_die_if_health_non_positive(o);
    return 1;
}

enum { H_NOP, H_DIE, H_KNOCKBACK, H_SQUISHED, H_KOOPA_LOSE_SHELL, H_SPEED_ZERO, H_HUGE_GOOMBA_WEAK, H_SQUISHED_BLUE };
static void shelled_koopa_attack_handler(Obj *o, u32 attackType);

static int obj_handle_attacks(Obj *o, const Hitbox *hb, int attackedMarioAction, const uint8_t *handlers)
{
    obj_set_hitbox(o, hb);
    if (obj_die_if_above_lava_and_health_non_positive(o)) return 1;
    if (o->interactStatus & INT_STATUS_INTERACTED) {
        if (o->interactStatus & INT_STATUS_ATTACKED_MARIO) {
            if (o->action != attackedMarioAction) { o->action = attackedMarioAction; o->timer = 0; }
        } else {
            u32 attackType = o->interactStatus & INT_STATUS_ATTACK_MASK;
            if (attackType >= 1 && attackType <= 6) {
                switch (handlers[attackType - 1]) {
                case H_DIE: obj_die_if_health_non_positive(o); break;
                case H_KNOCKBACK: obj_set_knockback_action(o, attackType); break;
                case H_SQUISHED: obj_set_squished_action(o); break;
                case H_KOOPA_LOSE_SHELL: shelled_koopa_attack_handler(o, attackType); break;
                case H_HUGE_GOOMBA_WEAK: o->action = GOOMBA_ACT_ATTACKED_MARIO; break;
                case H_SQUISHED_BLUE: o->numLoot = -1; obj_set_squished_action(o); break;
                default: break;
                }
            }
            o->interactStatus = 0;
            return (int)attackType;
        }
    }
    o->interactStatus = 0;
    return 0;
}

// ---- coins --------------------------------------------------------------------------------------------------------------
static void bhv_spawned_coin_loop(Obj *o)
{
    cur_obj_update_floor_and_walls(o);
    if (o->moveFlags & OBJ_MOVE_HIT_WALL) o->moveYaw = o->wallAngle;
    cur_obj_move_standard(o, -62);
    if (o->floor.valid) {
        if (o->moveFlags & OBJ_MOVE_ON_GROUND) o->subAction = 1;
        if (o->subAction == 1) {
            o->bounce = 0;
            if (o->floor.n[1] < 0.9f) cur_obj_rotate_yaw_toward(o, atan2s(o->floor.n[2], o->floor.n[0]), 0x400);
        }
    }
    if (o->timer == 0) obj_sound(o, SOUND_GENERAL_COIN_SPURT);
    if (o->vel[1] < 0.0f) become_tangible(o);
    if (o->moveFlags & OBJ_MOVE_LANDED) {
        if (o->moveFlags & (OBJ_MOVE_ABOVE_DEATH_BARRIER | OBJ_MOVE_ABOVE_LAVA)) { obj_delete(o); return; }
    }
    if (o->moveFlags & OBJ_MOVE_BOUNCE) {
        if (o->capF8 < 5) obj_sound(o, SOUND_GENERAL_COIN_DROP);
        o->capF8++;
    }
    // cur_obj_wait_then_blink(400, 20)
    if (o->timer >= 400) {
        if (o->timer < 420) o->invisible = (o->timer % 2) != 0;
        else { obj_delete(o); return; }
    }
    if (o->interactStatus & INT_STATUS_INTERACTED) { obj_delete(o); return; }
    o->animState++;
}

static void bhv_coin_loop(Obj *o)    // a coin lying about (streamed in): bhv_yellow_coin_loop
{
    if (o->interactStatus & INT_STATUS_INTERACTED) { obj_delete(o); return; }
    o->interactStatus = 0;
    o->animState++;
}

// ---- stars and caps ---------------------------------------------------------------------------------------------------------
static const Hitbox k_starHb = { INTERACT_STAR_OR_KEY, 0, 0, 0, 0, 80, 50, 0, 0 };
static const Hitbox k_capHb = { INTERACT_CAP, 0, 0, 0, 0, 80, 80, 90, 90 };

static void bhv_star_loop(Obj *o)
{
    o->faceYaw += 0x800;
    if (o->interactStatus & INT_STATUS_INTERACTED) { obj_delete(o); return; }
}

static void cap_scale_vertically(Obj *o)
{
    o->capF8 += 0x2000;
    o->scale[1] = coss((s16)o->capF8) * 0.3f + 0.7f;
    if (o->capF8 == 0x10000) { o->capF8 = 0; o->capF4 = 2; }
}

static void bhv_cap_loop(Obj *o)    // metal cap (sub 1) and wing cap (sub 2): bhv_metal_cap_loop / bhv_wing_vanish_cap_loop
{
    o->faceYaw = (s16)(o->faceYaw + (s16)(int)(o->fwd * 128.0f));
    int collisionFlags = object_step(o);
    if (o->sub == 2 && (collisionFlags & OBJ_COL_FLAG_GROUNDED)) {
        if (o->vel[1] != 0.0f) { o->capF4 = 1; o->vel[1] = 0.0f; }
    }
    if (o->sub == 2 && o->capF4 == 1) cap_scale_vertically(o);
    if (o->timer > 20) become_tangible(o);
    // cap_despawn / obj_flicker_and_disappear(300)
    if (o->timer > 300) {
        if (o->timer < 340) o->invisible = (o->timer % 2) != 0;
        else { obj_delete(o); return; }
    }
    // cap_set_hitbox
    obj_set_hitbox(o, &k_capHb);
    if (o->interactStatus & INT_STATUS_INTERACTED) obj_delete(o);
}

// ---- goomba ----------------------------------------------------------------------------------------------------------------------
static const Hitbox k_goombaHb = { INTERACT_BOUNCE_TOP, 0, 1, 0, 1, 72, 50, 42, 40 };
static const uint8_t k_goombaHandlers[2][6] = {
    { H_KNOCKBACK, H_KNOCKBACK, H_SQUISHED, H_SQUISHED, H_KNOCKBACK, H_KNOCKBACK },
    { H_HUGE_GOOMBA_WEAK, H_HUGE_GOOMBA_WEAK, H_SQUISHED, H_SQUISHED_BLUE, H_HUGE_GOOMBA_WEAK, H_HUGE_GOOMBA_WEAK },
};
static const struct { float scale; u32 deathSound; int damage; } k_goombaProps[3] = {
    { 1.5f, SOUND_OBJ_ENEMY_DEATH_HIGH, 1 }, { 3.5f, SOUND_OBJ_ENEMY_DEATH_LOW, 2 }, { 0.5f, SOUND_OBJ_ENEMY_DEATH_HIGH, 0 },
};

static void goomba_init(Obj *o, int size)
{
    o->goombaSize = size;
    o->goombaScale = k_goombaProps[size].scale;
    o->deathSound = (int)k_goombaProps[size].deathSound;
    o->wallRadius = 40.0f; o->bounce = -0.5f; o->drag = 10.0f; o->friction = 10.0f; o->buoyancy = 0;
    o->scale[0] = o->scale[1] = o->scale[2] = o->goombaScale;
    obj_set_hitbox(o, &k_goombaHb);
    o->damage = k_goombaProps[size].damage;
    o->gravity = -8.0f / 3.0f * o->goombaScale;
    o->animTable = 0x0801DA4C;
    anim_init(o, 0);
}

static void goomba_begin_jump(Obj *o)
{
    obj_sound(o, SOUND_OBJ_GOOMBA_ALERT);
    o->action = GOOMBA_ACT_JUMP;
    o->fwd = 0.0f;
    o->vel[1] = 50.0f / 3.0f * o->goombaScale;
}

static void goomba_act_walk(Obj *o)
{
    treat_far_home_as_mario(o, 1000.0f);
    obj_forward_vel_approach(o, o->goombaRelSpeed * o->goombaScale, 0.4f);
    if (o->goombaRelSpeed > 4.0f / 3.0f) anim_sound_at_range(o, 2, 17, SOUND_OBJ_GOOMBA_WALK);
    if (o->goombaTurning) {
        o->goombaTurning = obj_resolve_collisions_and_turn(o, o->goombaTargetYaw, 0x200);
    } else {
        if (o->distToMario >= 25000.0f) {
            o->goombaTargetYaw = o->angleToMario;
            o->goombaWalkTimer = random_linear_offset(20, 30);
        }
        if (!(o->goombaTurning = obj_bounce_off_walls_edges_objects(o, &o->goombaTargetYaw))) {
            if (o->distToMario < 500.0f) {
                if (o->goombaRelSpeed <= 2.0f) goomba_begin_jump(o);
                o->goombaTargetYaw = o->angleToMario;
                o->goombaRelSpeed = 20.0f;
            } else {
                o->goombaRelSpeed = 4.0f / 3.0f;
                if (o->goombaWalkTimer != 0) o->goombaWalkTimer--;
                else {
                    if (random_u16() & 3) {
                        o->goombaTargetYaw = obj_random_fixed_turn(o, 0x2000);
                        o->goombaWalkTimer = random_linear_offset(100, 100);
                    } else {
                        goomba_begin_jump(o);
                        o->goombaTargetYaw = obj_random_fixed_turn(o, 0x6000);
                    }
                }
            }
        }
        cur_obj_rotate_yaw_toward(o, o->goombaTargetYaw, 0x200);
    }
}

static void goomba_act_attacked_mario(Obj *o)
{
    if (o->goombaSize == 2) { o->numLoot = 0; obj_die_if_health_non_positive(o); }
    else {
        goomba_begin_jump(o);
        o->goombaTargetYaw = o->angleToMario;
        o->goombaTurning = 0;
    }
}

static void goomba_act_jump(Obj *o)
{
    obj_resolve_object_collisions(o, NULL);
    if (o->moveFlags & OBJ_MOVE_MASK_ON_GROUND) o->action = GOOMBA_ACT_WALK;
    else cur_obj_rotate_yaw_toward(o, o->goombaTargetYaw, 0x800);
}

static void bhv_goomba_update(Obj *o)
{
    if (obj_update_standard_actions(o, o->goombaScale)) {
        o->scale[0] = o->scale[1] = o->scale[2] = o->goombaScale;
        obj_update_blinking(o, &o->blinkTimer, 30, 50, 5);
        cur_obj_update_floor_and_walls(o);
        float animSpeed = o->fwd / o->goombaScale * 0.4f;
        if (animSpeed < 1.0f) animSpeed = 1.0f;
        anim_init_accel(o, 0, animSpeed);
        switch (o->action) {
        case GOOMBA_ACT_WALK: goomba_act_walk(o); break;
        case GOOMBA_ACT_ATTACKED_MARIO: goomba_act_attacked_mario(o); break;
        case GOOMBA_ACT_JUMP: goomba_act_jump(o); break;
        }
        if (o->used) {
            obj_handle_attacks(o, &k_goombaHb, GOOMBA_ACT_ATTACKED_MARIO, k_goombaHandlers[o->goombaSize & 1]);
            if (o->used) cur_obj_move_standard(o, -78);
        }
    } else {
        o->animState = 1;
    }
    if (o->used && !o->faceLock) o->faceYaw = o->moveYaw;     // OBJ_FLAG_SET_FACE_YAW_TO_MOVE_YAW
}

// ---- bob-omb -----------------------------------------------------------------------------------------------------------------
static const Hitbox k_bobombHb = { INTERACT_GRABBABLE, 0, 0, 0, 0, 65, 113, 0, 0 };

static void bobomb_init(Obj *o)
{
    o->gravity = 2.5f; o->friction = 0.8f; o->buoyancy = 1.3f;
    o->interactSub = INT_SUBTYPE_KICKABLE;
    o->animTable = 0x0802396C;
    anim_init(o, 0);
    obj_set_hitbox(o, &k_bobombHb);
}

static void bobomb_act_explode(Obj *o)
{
    if (o->timer < 5) {
        float s = 1.0f + (float)o->timer / 5.0f;
        o->scale[0] = o->scale[1] = o->scale[2] = s;
    } else {
        Obj *e = obj_alloc(B_EXPLOSION, ENT_EXPLOSION, OM_EXPLOSION, L_DESTRUCTIVE);
        if (e) {
            memcpy(e->pos, o->pos, 12);
            e->graphYOffset = 100.0f;
            e->billboard = 1;
            e->animState = -1;
            e->hbRadius = 150; e->hbHeight = 150; e->hbDown = 150;
            e->interactType = INTERACT_DAMAGE; e->damage = 2; e->hitboxSet = 1;
            obj_sound(o, SOUND_GENERAL2_BOBOMB_EXPLOSION);
            emit(EV_EXPLOSION, e->id, o->pos, 3.5f);
        }
        emit(EV_ENEMY_DEAD, o->id, o->pos, (float)o->ent);
        obj_delete(o);
    }
}

static void bobomb_check_interactions(Obj *o)
{
    obj_set_hitbox(o, &k_bobombHb);
    if (o->interactStatus & INT_STATUS_INTERACTED) {
        if (o->interactStatus & INT_STATUS_MARIO_KNOCKBACK_DMG) {
            o->moveYaw = o->carHit ? (o->carHit = 0, o->carYaw) : M.faceYaw;
            o->fwd = 25.0f;
            o->vel[1] = 30.0f;
            o->action = BOBOMB_ACT_LAUNCHED;
        }
        if (o->interactStatus & INT_STATUS_TOUCHED_BOB_OMB) o->action = BOBOMB_ACT_EXPLODE;
        o->interactStatus = 0;
    }
    // obj_attack_collided_from_other_object: touching anything but Mario sets it off (and knocks the other thing about)
    if (o->nCollided != 0) {
        Obj *other = &s_obj[o->collided[0]];
        other->interactStatus |= ATTACK_PUNCH | INT_STATUS_WAS_ATTACKED | INT_STATUS_INTERACTED | INT_STATUS_TOUCHED_BOB_OMB;
        o->action = BOBOMB_ACT_EXPLODE;
    }
}

static void obj_check_floor_death(Obj *o, int collisionFlags, const FloorInfo *f)
{
    if (!f->valid) return;
    if ((collisionFlags & OBJ_COL_FLAG_GROUNDED) == OBJ_COL_FLAG_GROUNDED) {
        if (f->type == 1) o->action = OBJ_ACT_LAVA_DEATH;
        else if (f->type == 10) o->action = OBJ_ACT_DEATH_PLANE_DEATH;
    }
}

static void bobomb_free_loop(Obj *o)
{
    switch (o->action) {
    case BOBOMB_ACT_PATROL: {
        o->fwd = 5.0f;
        int cf = object_step(o);
        if (obj_return_home_if_safe(o, o->home[0], o->home[1], o->home[2], 400) == 1
            && obj_check_if_facing_toward_angle((u32)(u16)o->moveYaw, (u32)(u16)o->angleToMario, 0x2000) == 1) {
            o->fuseLit = 1;
            o->action = BOBOMB_ACT_CHASE_MARIO;
        }
        obj_check_floor_death(o, cf, &o->stepFloor);
        break;
    }
    case BOBOMB_ACT_LAUNCHED: {
        int cf = object_step(o);
        if ((cf & OBJ_COL_FLAG_GROUNDED) == OBJ_COL_FLAG_GROUNDED) o->action = BOBOMB_ACT_EXPLODE;
        break;
    }
    case BOBOMB_ACT_CHASE_MARIO: {
        o->animFrame++;                                   // (the game steps the frame here as well as when drawing)
        o->animAccelAssist = (s32)((u32)o->animFrame << 16);
        o->fwd = 20.0f;
        int cf = object_step(o);
        if (o->animFrame == 5 || o->animFrame == 16) obj_sound(o, SOUND_OBJ_BOBOMB_WALK);
        o->moveYaw = approach_s16_symmetric(o->moveYaw, atan2s(M.pos[2] - o->pos[2], M.pos[0] - o->pos[0]), 0x800);   // obj_turn_toward_object(.., 16, 0x800)
        obj_check_floor_death(o, cf, &o->stepFloor);
        break;
    }
    case BOBOMB_ACT_EXPLODE: bobomb_act_explode(o); break;
    case OBJ_ACT_LAVA_DEATH: case OBJ_ACT_DEATH_PLANE_DEATH:
        obj_delete(o);
        return;
    }
    if (!o->used) return;
    bobomb_check_interactions(o);
    if (o->fuseTimer > 150) o->action = 3;
}

// where Mario holds a light object: in front of his chest (the game's HOLP, the hand's position as drawn)
static void mario_holp(float *out)
{
    out[0] = M.pos[0] + sins(M.faceYaw) * 50.0f;
    out[1] = M.pos[1] + 45.0f;
    out[2] = M.pos[2] + coss(M.faceYaw) * 50.0f;
}

// INT_STATUS_MARIO_DROP_OBJECT: Mario's hold actions answer it with drop_and_set_mario_action - to standing or walking
// on the ground, falling in the air
static void mario_drop_object_status(void)
{
    u32 next = (M.action & ACT_FLAG_AIR) ? 0x0100088Cu /* ACT_FREEFALL */ : M.fwd > 0 ? 0x04000440u /* ACT_WALKING */ : 0x0C400201u /* ACT_IDLE */;
    s_host.lock(); sm64_mario_drop_held(M.id); sm64_set_mario_action(M.id, next); s_host.unlock();
}

static void bobomb_held_loop(Obj *o)
{
    anim_init(o, 1);
    mario_holp(o->pos);     // drawn in his hands (the game hides it and draws it at the HOLP)
    o->moveYaw = M.faceYaw;
    o->fuseLit = 1;
    if (o->fuseTimer > 150) {
        // the game sets INT_STATUS_MARIO_DROP_OBJECT: he drops it, and it goes off
        mario_drop_object_status();
        o->action = BOBOMB_ACT_EXPLODE;
    }
}

// cur_obj_move_after_thrown_or_dropped
static void obj_move_after_thrown_or_dropped(Obj *o, float fwd, float velY)
{
    o->moveFlags = 0;
    o->floorH = find_floor(o->pos[0], o->pos[1] + 160.0f, o->pos[2], &o->floor);
    if (o->floorH > o->pos[1]) o->pos[1] = o->floorH;
    else if (o->floorH < -10000.0f) { memcpy(o->pos, M.pos, 12); o->floorH = find_floor(o->pos[0], o->pos[1] + 160.0f, o->pos[2], &o->floor); }
    o->fwd = fwd; o->vel[1] = velY;
    if (o->fwd != 0) cur_obj_move_y(o, o->gravity, -0.4f, o->buoyancy);
}

static void bobomb_dropped_loop(Obj *o)
{
    become_tangible(o);                 // cur_obj_get_dropped
    o->heldState = HELD_FREE;
    obj_move_after_thrown_or_dropped(o, 0, 0);
    anim_init(o, 0);
    o->action = BOBOMB_ACT_PATROL;
}

static void bobomb_thrown_loop(Obj *o)
{
    become_tangible(o);
    o->heldState = HELD_FREE;
    o->fwd = 25.0f;
    o->vel[1] = 20.0f;
    o->action = BOBOMB_ACT_LAUNCHED;
}

// Mario's side of holding: picked up (mario_grab_used_object), and let go - thrown (mario_throw_held_object) or put
// down (mario_drop_held_object), from where he held it
static void update_held_object(void)
{
    s_host.lock(); int st = sm64_ng64_grab_status(M.id); s_host.unlock();
    if (st == 2 && !s_heldId && s_grabOffered) {
        for (int i = 0; i < MAX_OBJ; i++)
            if (s_obj[i].used && s_obj[i].id == s_grabOffered) {
                s_obj[i].heldState = HELD_HELD; become_intangible(&s_obj[i]); s_heldId = s_grabOffered;
                break;
            }
        s_grabOffered = 0;
    }
    if (st == 0 && !s_heldId) s_grabOffered = 0;
    if (!s_heldId) return;
    Obj *o = NULL;
    for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && s_obj[i].id == s_heldId) o = &s_obj[i];
    if (!o) {   // it went off in his hands
        if (st == 2) { s_host.lock(); sm64_mario_drop_held(M.id); s_host.unlock(); }
        s_heldId = 0;
        return;
    }
    if (st == 2) return;
    float holp[3];
    mario_holp(holp);
    if (M.action & ACT_FLAG_THROWING) {
        o->pos[0] = holp[0] + 32.0f * sins(M.faceYaw); o->pos[1] = holp[1]; o->pos[2] = holp[2] + 32.0f * coss(M.faceYaw);
        o->heldState = HELD_THROWN;
    } else {
        o->pos[0] = holp[0]; o->pos[1] = M.pos[1]; o->pos[2] = holp[2];   // (dropped at Mario's height, as the game does)
        o->heldState = HELD_DROPPED;
    }
    o->moveYaw = M.faceYaw;
    s_heldId = 0;
}

// the fuse's smoke (bhvBobombFuseSmoke, bhv_dust_smoke_loop)
static void spawn_fuse_smoke(Obj *o)
{
    Obj *sm = obj_alloc(B_SMOKE, ENT_SMOKE, OM_SMOKE, L_LEVEL);
    if (!sm) return;
    memcpy(sm->pos, o->pos, 12);
    sm->pos[0] += (int)(random_float() * 80.0f) - 40;
    sm->pos[1] += (int)(random_float() * 80.0f) + 60;
    sm->pos[2] += (int)(random_float() * 80.0f) - 40;
    sm->scale[0] = sm->scale[1] = sm->scale[2] = 1.2f;
    sm->animState = -1;
    sm->billboard = 1;
    sm->invisible = 1;    // DELAY(1): its loop (and its drawing) starts the tick after
    become_intangible(sm);
}

static void bhv_dust_smoke_loop(Obj *o)
{
    o->invisible = 0;
    o->pos[0] += o->vel[0]; o->pos[1] += o->vel[1]; o->pos[2] += o->vel[2];
    if (o->smokeTimer == 10) { obj_delete(o); return; }
    o->smokeTimer++;
    o->animState++;       // the script's ADD_INT(oAnimState, 1): the smoke's frames
}

static void bobomb_random_blink(Obj *o)
{
    if (o->blinkTimer == 0) {
        if ((s16)(random_float() * 100.0f) == 0) { o->animState = 1; o->blinkTimer = 1; }
    } else {
        o->blinkTimer++;
        if (o->blinkTimer > 5) o->animState = 0;
        if (o->blinkTimer > 10) o->animState = 1;
        if (o->blinkTimer > 15) { o->animState = 0; o->blinkTimer = 0; }
    }
}

static void bhv_bobomb_loop(Obj *o)
{
    if (is_point_within_radius_of_mario(o->pos[0], o->pos[1], o->pos[2], 4000)) {
        switch (o->heldState) {
        case HELD_FREE: bobomb_free_loop(o); break;
        case HELD_HELD: bobomb_held_loop(o); break;
        case HELD_THROWN: bobomb_thrown_loop(o); break;
        case HELD_DROPPED: bobomb_dropped_loop(o); break;
        }
        if (!o->used) return;
        bobomb_random_blink(o);
        if (o->fuseLit == 1) {
            int period = o->fuseTimer > 120 ? 1 : 7;
            if (!(period & o->fuseTimer)) spawn_fuse_smoke(o);
            if (!(o->fuseTimer & 7)) obj_sound(o, SOUND_AIR_BOBOMB_LIT_FUSE);
            o->fuseTimer++;
        }
    }
    o->faceYaw = o->moveYaw;
}

// ---- koopa ----------------------------------------------------------------------------------------------------------------------------
static const Hitbox k_koopaHb = { INTERACT_KOOPA, 0, 0, 0, -1, 60, 40, 40, 30 };
static const uint8_t k_koopaUnshelledHandlers[6] = { H_KNOCKBACK, H_KNOCKBACK, H_SQUISHED, H_SQUISHED, H_KNOCKBACK, H_KNOCKBACK };
static const uint8_t k_koopaShelledHandlers[6] = { H_KOOPA_LOSE_SHELL, H_KOOPA_LOSE_SHELL, H_KOOPA_LOSE_SHELL, H_KOOPA_LOSE_SHELL, H_KOOPA_LOSE_SHELL, H_KOOPA_LOSE_SHELL };

static Obj *find_nearest_shell(Obj *o, float *dist)
{
    Obj *best = NULL; float bd = 99999.0f;
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *s = &s_obj[i];
        if (!s->used || s->bhv != B_SHELL) continue;
        float d = sqrtf(sqr(s->pos[0] - o->pos[0]) + sqr(s->pos[1] - o->pos[1]) + sqr(s->pos[2] - o->pos[2]));
        if (d < bd) { bd = d; best = s; }
    }
    *dist = bd;
    return best;
}

static void koopa_init(Obj *o, int type)
{
    o->koopaType = type;
    o->animTable = 0x06011364;
    o->wallRadius = 50.0f; o->gravity = -4.0f; o->bounce = 0; o->drag = 0; o->friction = 10.0f; o->buoyancy = 2.0f;
    o->scale[0] = o->scale[1] = o->scale[2] = 1.5f;
    o->koopaAgility = 1.0f;
    anim_init(o, 9);
    o->model = type == KOOPA_BP_UNSHELLED ? OM_KOOPA_NOSHELL : OM_KOOPA;
}

static void koopa_play_footstep_sound(Obj *o, int f1, int f2) { anim_sound_at_range(o, f1, f2, SOUND_OBJ_KOOPA_WALK); }

static int koopa_anim_and_check_end(Obj *o, int idx) { anim_init(o, idx); return anim_near_end(o); }

static int koopa_check_run_from_mario(Obj *o)
{
    if (o->koopaDist < 300.0f && abs_angle_diff(o->koopaAngle, o->moveYaw) < 0x3000) { o->action = KOOPA_SHELLED_ACT_RUN_FROM_MARIO; return 1; }
    return 0;
}

static void koopa_shelled_act_stopped(Obj *o)
{
    o->fwd = 0.0f;
    if (koopa_anim_and_check_end(o, 7)) {
        o->action = KOOPA_SHELLED_ACT_WALK;
        o->koopaTargetYaw = (s16)(o->moveYaw + 0x2000 * (s16)random_sign());
    }
}

static void koopa_walk_start(Obj *o)
{
    obj_forward_vel_approach(o, 3.0f * o->koopaAgility, 0.3f * o->koopaAgility);
    if (koopa_anim_and_check_end(o, 11)) { o->subAction++; o->koopaCountdown = random_linear_offset(30, 100); }
}

static void koopa_walk(Obj *o)
{
    anim_init(o, 9);
    koopa_play_footstep_sound(o, 2, 17);
    if (o->koopaCountdown != 0) o->koopaCountdown--;
    else if (anim_near_end(o)) o->subAction++;
}

static void koopa_walk_stop(Obj *o)
{
    obj_forward_vel_approach(o, 0.0f, 1.0f * o->koopaAgility);
    if (koopa_anim_and_check_end(o, 10)) o->action = KOOPA_SHELLED_ACT_STOPPED;
}

static void koopa_shelled_act_walk(Obj *o)
{
    if (o->koopaTurning) o->koopaTurning = obj_resolve_collisions_and_turn(o, o->koopaTargetYaw, 0x200);
    else {
        if (o->distToMario >= 25000.0f) o->koopaTargetYaw = o->angleToMario;
        o->koopaTurning = obj_bounce_off_walls_edges_objects(o, &o->koopaTargetYaw);
        cur_obj_rotate_yaw_toward(o, o->koopaTargetYaw, 0x200);
    }
    switch (o->subAction) {
    case KOOPA_SHELLED_SUB_ACT_START_WALK: koopa_walk_start(o); break;
    case KOOPA_SHELLED_SUB_ACT_WALK: koopa_walk(o); break;
    case KOOPA_SHELLED_SUB_ACT_STOP_WALK: koopa_walk_stop(o); break;
    }
    koopa_check_run_from_mario(o);
}

static void koopa_shelled_act_run_from_mario(Obj *o)
{
    anim_init(o, 1);
    koopa_play_footstep_sound(o, 0, 11);
    if (o->distToMario >= 25000.0f) { o->angleToMario = (s16)(o->angleToMario + 0x8000); o->distToMario = 0.0f; }
    if (o->timer > 30 && o->distToMario > 800.0f) {
        if (obj_forward_vel_approach(o, 0.0f, 1.0f)) o->action = KOOPA_SHELLED_ACT_STOPPED;
    } else {
        cur_obj_rotate_yaw_toward(o, (s16)(o->angleToMario + 0x8000), 0x400);
        obj_forward_vel_approach(o, 17.0f, 1.0f);
    }
}

static void koopa_dive_update_speed(Obj *o, float decel)
{
    if (o->moveFlags & OBJ_MOVE_MASK_ON_GROUND) obj_forward_vel_approach(o, 0.0f, decel);
}

static void koopa_shelled_act_lying(Obj *o)
{
    if (o->fwd != 0.0f) {
        if (o->moveFlags & OBJ_MOVE_HIT_WALL) o->moveYaw = cur_obj_reflect_move_angle_off_wall(o);
        anim_init(o, 5); anim_extend_if_at_end(o);
        koopa_dive_update_speed(o, 0.3f);
    } else if (o->koopaCountdown != 0) {
        o->koopaCountdown--;
        anim_extend_if_at_end(o);
    } else if (koopa_anim_and_check_end(o, 6)) {
        o->action = KOOPA_SHELLED_ACT_STOPPED;
    }
}

// a koopa with his shell on loses it: he drops to the unshelled forms and the shell is left lying
static Obj *spawn_shell_at(const float *pos, s16 yaw)
{
    Obj *s = obj_alloc(B_SHELL, ENT_SHELL, OM_KOOPA_SHELL, L_LEVEL);
    if (!s) return NULL;
    memcpy(s->pos, pos, 12);
    s->moveYaw = yaw; s->faceYaw = yaw;
    s->wallRadius = 30.0f; s->gravity = -4.0f; s->bounce = -0.5f; s->drag = 10.0f; s->friction = 10.0f; s->buoyancy = 2.0f;
    return s;
}

static void shelled_koopa_attack_handler(Obj *o, u32 attackType)
{
    if (o->scale[0] > 0.8f) {
        obj_sound(o, SOUND_OBJ_KOOPA_DAMAGE);
        o->koopaType = KOOPA_BP_UNSHELLED;
        o->action = KOOPA_UNSHELLED_ACT_LYING;
        o->fwd = 20.0f;
        if (attackType != ATTACK_FROM_ABOVE && attackType != ATTACK_GROUND_POUND_OR_TWIRL) o->moveYaw = knock_yaw(o);
        o->carHit = 0;
        o->model = OM_KOOPA_NOSHELL;
        spawn_shell_at(o->pos, 0);
        become_intangible(o);
    } else {
        obj_die_if_health_non_positive(o);
    }
}

static void koopa_shelled_update(Obj *o)
{
    cur_obj_update_floor_and_walls(o);
    obj_update_blinking(o, &o->blinkTimer, 20, 50, 4);
    switch (o->action) {
    case KOOPA_SHELLED_ACT_STOPPED: koopa_shelled_act_stopped(o); koopa_check_run_from_mario(o); break;
    case KOOPA_SHELLED_ACT_WALK: koopa_shelled_act_walk(o); break;
    case KOOPA_SHELLED_ACT_RUN_FROM_MARIO: koopa_shelled_act_run_from_mario(o); break;
    case KOOPA_SHELLED_ACT_LYING: koopa_shelled_act_lying(o); break;
    }
    obj_handle_attacks(o, &k_koopaHb, o->action, k_koopaShelledHandlers);
    if (o->used) cur_obj_move_standard(o, -78);
}

static void koopa_unshelled_act_run(Obj *o)
{
    float distToShell = 99999.0f;
    anim_init(o, 3);
    koopa_play_footstep_sound(o, 0, 6);
    if (o->koopaTurning) o->koopaTurning = obj_resolve_collisions_and_turn(o, o->koopaTargetYaw, 0x600);
    else {
        if (o->distToMario >= 25000.0f) o->koopaTargetYaw = o->angleToMario;
        Obj *shell = find_nearest_shell(o, &distToShell);
        if (shell) o->koopaTargetYaw = atan2s(shell->pos[2] - o->pos[2], shell->pos[0] - o->pos[0]);
        else if (!(o->koopaTurning = obj_bounce_off_walls_edges_objects(o, &o->koopaTargetYaw))) {
            if (o->koopaTimeUntilTurn != 0) o->koopaTimeUntilTurn--;
            else o->koopaTargetYaw = obj_random_fixed_turn(o, 0x2000);
        }
        if (o->distToMario > 800.0f || (shell && abs_angle_diff(o->koopaTargetYaw, (s16)(o->angleToMario + 0x8000)) < 0x2000))
            cur_obj_rotate_yaw_toward(o, o->koopaTargetYaw, 0x600);
        else cur_obj_rotate_yaw_toward(o, (s16)(o->angleToMario + 0x8000), 0x600);
    }
    if (obj_forward_vel_approach(o, 20.0f, 1.0f) && distToShell < 600.0f && abs_angle_diff(o->koopaTargetYaw, o->moveYaw) < 0xC00) {
        o->moveYaw = o->koopaTargetYaw;
        o->action = KOOPA_UNSHELLED_ACT_DIVE;
        o->fwd *= 1.2f;
        o->vel[1] = distToShell / 20.0f;
        o->koopaCountdown = 20;
    }
}

static void koopa_unshelled_act_dive(Obj *o)
{
    if (o->timer > 10) become_tangible(o);
    if (o->timer > 10) {
        float distToShell = 0;
        Obj *shell = find_nearest_shell(o, &distToShell);
        // if he got the shell and Mario didn't, he puts it on
        if (shell && !shell->ridden && sqrtf(sqr(shell->pos[0] - M.pos[0]) + sqr(shell->pos[1] - M.pos[1]) + sqr(shell->pos[2] - M.pos[2])) > 200.0f && distToShell < 50.0f) {
            o->koopaType = KOOPA_BP_NORMAL;
            o->action = KOOPA_SHELLED_ACT_LYING;
            o->fwd *= 0.5f;
            o->model = OM_KOOPA;
            obj_delete(shell);
            return;
        }
    }
    if (o->fwd != 0.0f) {
        if (o->action == KOOPA_UNSHELLED_ACT_LYING) { o->animState = 1; anim_init(o, 2); anim_extend_if_at_end(o); }
        else { anim_init(o, 5); anim_extend_if_at_end(o); }
        koopa_dive_update_speed(o, 0.5f);
    } else if (o->koopaCountdown != 0) {
        o->koopaCountdown--;
        anim_extend_if_at_end(o);
    } else if (koopa_anim_and_check_end(o, 6)) {
        o->action = KOOPA_UNSHELLED_ACT_RUN;
    }
}

static void koopa_unshelled_update(Obj *o)
{
    cur_obj_update_floor_and_walls(o);
    obj_update_blinking(o, &o->blinkTimer, 10, 15, 3);
    switch (o->action) {
    case KOOPA_UNSHELLED_ACT_RUN: koopa_unshelled_act_run(o); break;
    case KOOPA_UNSHELLED_ACT_DIVE: case KOOPA_UNSHELLED_ACT_LYING: koopa_unshelled_act_dive(o); break;
    }
    if (!o->used) return;
    obj_handle_attacks(o, &k_koopaHb, o->action, k_koopaUnshelledHandlers);
    if (o->used) cur_obj_move_standard(o, -78);
}

static void bhv_koopa_update(Obj *o)
{
    o->deathSound = (int)SOUND_OBJ_KOOPA_FLYGUY_DEATH;
    if (obj_update_standard_actions(o, o->koopaAgility * 1.5f)) {
        o->koopaDist = o->distToMario;
        o->koopaAngle = o->angleToMario;
        treat_far_home_as_mario(o, 1000.0f);
        switch (o->koopaType) {
        case KOOPA_BP_UNSHELLED: koopa_unshelled_update(o); break;
        case KOOPA_BP_NORMAL: koopa_shelled_update(o); break;
        }
    } else {
        o->animState = 1;
    }
    if (o->used) o->faceYaw = approach_s16_symmetric(o->faceYaw, o->moveYaw, 0x600);   // obj_face_yaw_approach
}

// ---- koopa shell -------------------------------------------------------------------------------------------------------------------------
static const Hitbox k_shellHb = { INTERACT_KOOPA_SHELL, 0, 4, 1, 1, 50, 50, 50, 50 };

static void bhv_koopa_shell_loop(Obj *o)
{
    obj_set_hitbox(o, &k_shellHb);
    o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
    switch (o->action) {
    case 0:
        cur_obj_update_floor_and_walls(o);
        if (o->moveFlags & OBJ_MOVE_HIT_WALL) o->moveYaw = o->wallAngle;
        if (o->interactStatus & INT_STATUS_INTERACTED) o->action++;
        o->faceYaw += 0x1000;
        cur_obj_move_standard(o, -20);
        break;
    case 1:
        // ridden: it sits under Mario, turned the way he faces; when he steps off it goes
        memcpy(o->pos, M.pos, 12);
        o->faceYaw = M.faceYaw;
        if (!(M.action & ACT_FLAG_RIDING_SHELL) && o->timer > 3) { obj_delete(o); return; }
        break;
    }
    o->interactStatus = 0;
}

// ---- explosion ----------------------------------------------------------------------------------------------------------------------------
static void bhv_explosion_loop(Obj *o)
{
    if (o->timer == 9) {
        // under water the game lets out bubbles; on land, a big puff of smoke (bhvBobombBullyDeathSmoke) below it
        if (find_water_level(o->pos[0], o->pos[2]) <= o->pos[1]) {
            Obj *sm = obj_alloc(B_SMOKE, ENT_SMOKE, OM_SMOKE, L_LEVEL);
            if (sm) {
                memcpy(sm->pos, o->pos, 12);
                sm->pos[1] -= 300.0f;
                sm->scale[0] = sm->scale[1] = sm->scale[2] = 10.0f;
                sm->animState = -1; sm->billboard = 1; sm->invisible = 1;
                become_intangible(sm);
            }
        }
        obj_delete(o);
        return;
    }
    float s = (float)o->timer / 9.0f + 1.0f;
    o->scale[0] = o->scale[1] = o->scale[2] = s;
    o->animState++;
}

// ---- the manager -------------------------------------------------------------------------------------------------------------------------------
static int is_pickup(int ent) { return ent >= ENT_COIN_YELLOW && ent <= ENT_STAR_POWER; }
static int is_enemy(int ent) { return ent >= ENT_GOOMBA && ent <= ENT_SHELL; }

void ents_init(const EntHost *host)
{
    s_host = *host;
    memset(s_obj, 0, sizeof(s_obj));
}

void ents_set_options(int pickups, int enemies)
{
    pickups = pickups != 0; enemies = enemies != 0;
    if (!pickups && s_pickupsOn) for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && (is_pickup(s_obj[i].ent) || s_obj[i].bhv == B_SPAWNED_COIN)) s_obj[i].used = 0;
    if (!enemies && s_enemiesOn) for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && (is_enemy(s_obj[i].ent) || s_obj[i].ent == ENT_EXPLOSION)) s_obj[i].used = 0;
    s_pickupsOn = pickups; s_enemiesOn = enemies;
}

void ents_get_options(int *pickups, int *enemies) { *pickups = s_pickupsOn; *enemies = s_enemiesOn; }
int ents_star_active(void) { return s_starTicks > 0; }
int ents_star_dancing(void) { return M.action == ACT_STAR_DANCE_NO_EXIT || M.action == ACT_STAR_DANCE_WATER || M.action == ACT_FALL_AFTER_STAR_GRAB; }
void ents_clear(void) { memset(s_obj, 0, sizeof(s_obj)); s_starTicks = 0; }

int ents_count(int enemies)
{
    int n = 0;
    for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && (enemies ? is_enemy(s_obj[i].ent) : (is_pickup(s_obj[i].ent) && s_obj[i].bhv != B_SPAWNED_COIN))) n++;
    return n;
}

static int count_ent(int ent)
{
    int n = 0;
    for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && s_obj[i].ent == ent) n++;
    return n;
}

// ---- spawning round Mario ---------------------------------------------------------------------------------------------------------------------------
static int pick_weighted(const int *types, const int *weights, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += weights[i];
    int r = (int)(random_float() * total);
    for (int i = 0; i < n; i++) { if (r < weights[i]) return types[i]; r -= weights[i]; }
    return types[0];
}

// a random spot on flat ground 12 - 50 m from Mario
static int find_spot(float *out)
{
    for (int tries = 0; tries < 8; tries++) {
        float ang = random_float() * 2 * PI_F, dist = (12.0f + random_float() * 38.0f) / S;
        float x = M.pos[0] + sinf(ang) * dist, z = M.pos[2] + cosf(ang) * dist;
        float fy, ny;
        if (!s_host.floor(x, M.pos[1] + 400, z, &fy, &ny)) continue;
        if (ny < 0.93f) continue;
        if (fabsf(fy - M.pos[1]) > 700) continue;
        if (s_host.blocked && s_host.blocked(x, fy + 50, z, fy)) continue;
        out[0] = x; out[1] = fy; out[2] = z;
        return 1;
    }
    return 0;
}

static void set_home(Obj *o) { memcpy(o->home, o->pos, 12); }

static int s_testType;       // tests: spawn this pickup at s_testPos instead of a random one somewhere
static float s_testPos[3];

static void spawn_pickup(void)
{
    static const int types[] = { ENT_COIN_YELLOW, ENT_COIN_RED, ENT_COIN_BLUE, ENT_POWER_STAR, ENT_CAP_METAL, ENT_CAP_WING, ENT_STAR_POWER };
    static const int weights[] = { 55, 8, 4, 4, 8, 8, 5 };
    int type = s_testType ? s_testType : pick_weighted(types, weights, 7);
    if (!s_testType && type >= ENT_POWER_STAR && count_ent(type) >= 1) return;
    float p[3];
    int ok = 1;
    if (s_testType) memcpy(p, s_testPos, 12); else ok = find_spot(p);
    if (!ok) return;
    Obj *o = NULL;
    switch (type) {
    case ENT_COIN_YELLOW: case ENT_COIN_RED: case ENT_COIN_BLUE:
        o = obj_alloc(B_COIN, type, type == ENT_COIN_YELLOW ? OM_COIN_YELLOW : type == ENT_COIN_RED ? OM_COIN_RED : OM_COIN_BLUE, L_LEVEL);
        if (!o) return;
        memcpy(o->pos, p, 12); o->billboard = 1;
        obj_set_hitbox(o, type == ENT_COIN_YELLOW ? &k_coinHb : type == ENT_COIN_RED ? &k_redCoinHb : &k_blueCoinHb);
        o->animState = (int)(random_float() * 8);
        break;
    case ENT_POWER_STAR: case ENT_STAR_POWER:
        o = obj_alloc(type == ENT_POWER_STAR ? B_STAR : B_STAR_POWER, type, type == ENT_POWER_STAR ? OM_STAR : OM_STAR_TRANSPARENT, L_LEVEL);
        if (!o) return;
        memcpy(o->pos, p, 12); o->pos[1] += 90;
        obj_set_hitbox(o, &k_starHb);
        o->faceYaw = (s16)random_u16();
        break;
    case ENT_CAP_METAL: case ENT_CAP_WING:
        // a cap as the game drops it out of a box: a little way up, falling
        o = obj_alloc(B_CAP, type, type == ENT_CAP_METAL ? OM_CAP_METAL : OM_CAP_WING, L_LEVEL);
        if (!o) return;
        memcpy(o->pos, p, 12); o->pos[1] += 120;
        o->sub = type == ENT_CAP_METAL ? 1 : 2;
        if (type == ENT_CAP_METAL) { o->gravity = 2.4f; o->friction = 0.999f; o->buoyancy = 1.5f; }
        else { o->gravity = 1.2f; o->friction = 0.999f; o->buoyancy = 0.9f; }
        obj_set_hitbox(o, &k_capHb);
        become_intangible(o);          // (tangible after 20 frames)
        o->moveYaw = (s16)random_u16();
        break;
    }
    if (o) set_home(o);
}

static void spawn_enemy(void)
{
    static const int types[] = { ENT_GOOMBA, ENT_BOBOMB, ENT_KOOPA };
    static const int weights[] = { 60, 28, 12 };
    int type = s_testType ? s_testType : pick_weighted(types, weights, 3);
    float p[3];
    if (s_testType) memcpy(p, s_testPos, 12);
    else if (!find_spot(p)) return;
    Obj *o = NULL;
    switch (type) {
    case ENT_GOOMBA:
        o = obj_alloc(B_GOOMBA, ENT_GOOMBA, OM_GOOMBA, L_PUSHABLE);
        if (!o) return;
        memcpy(o->pos, p, 12); set_home(o);
        o->moveYaw = (s16)random_u16(); o->faceYaw = o->moveYaw;
        goomba_init(o, 0);
        break;
    case ENT_BOBOMB:
        o = obj_alloc(B_BOBOMB, ENT_BOBOMB, OM_BOBOMB, L_DESTRUCTIVE);
        if (!o) return;
        memcpy(o->pos, p, 12); set_home(o);
        o->moveYaw = (s16)random_u16(); o->faceYaw = o->moveYaw;
        bobomb_init(o);
        break;
    case ENT_KOOPA:
        o = obj_alloc(B_KOOPA, ENT_KOOPA, OM_KOOPA, L_PUSHABLE);
        if (!o) return;
        memcpy(o->pos, p, 12); set_home(o);
        o->moveYaw = (s16)random_u16(); o->faceYaw = o->moveYaw;
        koopa_init(o, KOOPA_BP_NORMAL);
        break;
    }
}

// ---- tests: a model on show -----------------------------------------------------------------------------------------------------------------
void ents_debug_show(int model, const float *posSm, int yaw, int animState, int animIdx, int frame)
{
    if (model == 255) { for (int i = 0; i < MAX_OBJ; i++) if (s_obj[i].used && s_obj[i].bhv == B_NONE) s_obj[i].used = 0; return; }
    if (model >= 200) { s_testType = model - 200; memcpy(s_testPos, posSm, 12); if (s_testType >= ENT_GOOMBA) spawn_enemy(); else spawn_pickup(); s_testType = 0; return; }   // a real one
    if (model < 0 || model >= OM_COUNT) return;
    Obj *o = obj_alloc(B_NONE, ENT_EXPLOSION + 5, model, L_LEVEL);
    if (!o) return;
    memcpy(o->pos, posSm, 12);
    o->faceYaw = (s16)yaw; o->animState = animState;
    static const u32 tables[OM_COUNT] = { [OM_GOOMBA] = 0x0801DA4C, [OM_BOBOMB] = 0x0802396C, [OM_KOOPA] = 0x06011364, [OM_KOOPA_NOSHELL] = 0x06011364 };
    o->animTable = tables[model];
    if (animIdx != 255 && o->animTable) {
        u32 a = objrom_anim_from_table(o->animTable, animIdx);
        o->curAnim = a; o->animFrame = frame;
    }
    o->billboard = (model <= OM_COIN_BLUE || model == OM_EXPLOSION);
}

// ---- a car hits an enemy -------------------------------------------------------------------------------------------------------------------------------
void ents_car_hit(int id, int kind, int haveDir, float dirX, float dirZ)
{
    (void)kind;
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *o = &s_obj[i];
        if (!o->used || o->id != id || !is_enemy(o->ent) || o->bhv == B_SHELL) continue;
        if (o->action >= 100 || (o->interactStatus & INT_STATUS_INTERACTED)) return;
        // a car at speed is a "fast attack": the same status Mario's slide kick or a dive gives; the knock-back goes the way the car went
        if (haveDir) { o->carHit = 1; o->carYaw = atan2s(dirZ, dirX); }
        o->interactStatus = ATTACK_FAST_ATTACK + (INT_STATUS_INTERACTED | INT_STATUS_WAS_ATTACKED);
        if (o->bhv == B_BOBOMB) o->interactStatus |= INT_STATUS_MARIO_KNOCKBACK_DMG;   // it is launched like a kicked bob-omb
        return;
    }
}

void ents_car_kill(int id, int kind) { ents_car_hit(id, kind, 0, 0, 0); }

// ---- the tick --------------------------------------------------------------------------------------------------------------------------------------------
static void update_object(Obj *o)
{
    // OBJ_FLAG_COMPUTE_DIST_TO_MARIO / ANGLE_TO_MARIO
    o->distToMario = sqrtf(sqr(o->pos[0] - M.pos[0]) + sqr(o->pos[1] - M.pos[1]) + sqr(o->pos[2] - M.pos[2]));
    o->angleToMario = obj_angle_to_mario(o);
    if (o->action != o->prevAction) { o->timer = 0; o->subAction = 0; o->prevAction = o->action; }

    switch (o->bhv) {
    case B_COIN: bhv_coin_loop(o); break;
    case B_SPAWNED_COIN: bhv_spawned_coin_loop(o); break;
    case B_STAR: case B_STAR_POWER: bhv_star_loop(o); break;
    case B_CAP: bhv_cap_loop(o); break;
    case B_GOOMBA: bhv_goomba_update(o); break;
    case B_BOBOMB: bhv_bobomb_loop(o); break;
    case B_KOOPA: bhv_koopa_update(o); break;
    case B_SHELL: bhv_koopa_shell_loop(o); break;
    case B_EXPLOSION: bhv_explosion_loop(o); break;
    case B_MIST: bhv_white_puff_exploding_loop(o); break;
    case B_SPARKLE: bhv_celebration_star_sparkle_loop(o); break;
    case B_CELEB_STAR: bhv_celebration_star_loop(o); break;
    case B_SMOKE: bhv_dust_smoke_loop(o); break;
    }
    if (!o->used) return;

    if (o->timer < 0x3FFFFFFF) o->timer++;
    if (o->action != o->prevAction) { o->timer = 0; o->subAction = 0; o->prevAction = o->action; }
    if (o->curAnim) anim_update(o);      // the animation steps with the drawing, once a tick
}

void ents_tick(uint32_t tick, int marioId, const struct SM64MarioState *st)
{
    M.prevAction = M.action;
    M.id = marioId;
    for (int k = 0; k < 3; k++) { M.pos[k] = st->position[k]; M.vel[k] = st->velocity[k]; }
    M.fwd = st->forwardVelocity;
    M.faceYaw = (s16)(int)(st->faceAngle * (32768.0f / PI_F));
    M.action = st->action; M.flags = st->flags; M.invinc = st->invincTimer;
    M.hitH = (M.action & ACT_FLAG_SHORT_HITBOX) ? 100.0f : 160.0f;

    // the invincibility star
    if (s_starTicks > 0) {
        s_host.lock(); sm64_set_mario_invincibility(marioId, 40); s_host.unlock();
        if (--s_starTicks == 0) emit(EV_POWER_END, 0, NULL, EV_STAR_POWER);
    }
    // caps: libsm64 clears the flag when the time is up
    int capFlags = (int)(st->flags & (MARIO_METAL_CAP | MARIO_WING_CAP | MARIO_VANISH_CAP));
    if ((s_prevCapFlags & MARIO_METAL_CAP) && !(capFlags & MARIO_METAL_CAP)) emit(EV_POWER_END, 0, NULL, EV_CAP_METAL);
    if ((s_prevCapFlags & MARIO_WING_CAP) && !(capFlags & MARIO_WING_CAP)) emit(EV_POWER_END, 0, NULL, EV_CAP_WING);
    s_prevCapFlags = capFlags;

    // a star dance has begun: general_star_dance_handler's first frame spawns the celebration star
    if ((M.action == ACT_STAR_DANCE_NO_EXIT || M.action == ACT_STAR_DANCE_WATER) && M.prevAction != M.action) spawn_celebration_star();

    update_held_object();

    // Mario touches things (SM64: detect_object_collisions, then Mario's own update), then they all update
    detect_collisions_and_interact();

    // spawning
    if (s_pickupsOn && ents_count(0) < MAX_PICKUPS && tick >= s_nextPickupSpawn) {
        spawn_pickup();
        s_nextPickupSpawn = tick + (u32)(TICKS_PER_S * (1.2f + random_float() * 1.8f));
    }
    if (s_enemiesOn && ents_count(1) < MAX_ENEMIES && tick >= s_nextEnemySpawn) {
        spawn_enemy();
        s_nextEnemySpawn = tick + (u32)(TICKS_PER_S * (3.0f + random_float() * 4.0f));
    }

    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *o = &s_obj[i];
        if (!o->used) continue;
        float dist2d = sqrtf(sqr(o->pos[0] - M.pos[0]) + sqr(o->pos[2] - M.pos[2]));
        if (dist2d > 100.0f / S && !o->ridden) { o->used = 0; continue; }
        if (o->bhv == B_NONE) continue;
        update_object(o);
    }
}

// ---- the packet ---------------------------------------------------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct { uint16_t piece; uint8_t flags, pad; float pos[3]; int16_t q[4]; float scale; } PackedPart;
#pragma pack(pop)

int ents_pack(uint8_t *out, size_t cap)
{
    uint16_t n = 0;
    uint8_t *p = out + 2;
    for (int i = 0; i < MAX_OBJ; i++) {
        Obj *o = &s_obj[i];
        if (!o->used || o->model < 0) continue;
        ObjPose pose;
        memset(&pose, 0, sizeof(pose));
        memcpy(pose.pos, o->pos, 12);
        pose.pos[1] += o->graphYOffset;
        pose.angle[0] = o->facePitch; pose.angle[1] = o->faceYaw; pose.angle[2] = o->faceRoll;
        pose.scale[0] = o->scale[0]; pose.scale[1] = o->scale[1]; pose.scale[2] = o->scale[2];
        pose.animState = o->animState;
        pose.anim = o->curAnim;
        pose.animFrame = o->animFrame;
        ObjPart parts[16];
        int np = o->invisible ? 0 : objrom_pose(o->model, &pose, parts, 16);
        if ((size_t)(p - out) + 18 + (size_t)np * sizeof(PackedPart) > cap) continue;
        float b[3] = { o->pos[0] * S, -o->pos[2] * S, o->pos[1] * S };
        uint16_t id = o->id;
        uint8_t state = (o->action >= 100 || o->intangible != 0) && is_enemy(o->ent) ? 9 : (uint8_t)(o->action & 0xFF);
        memcpy(p, &id, 2); p[2] = (uint8_t)o->ent; p[3] = state; p[4] = (uint8_t)np; p[5] = 0; p += 6;
        memcpy(p, b, 12); p += 12;
        for (int k = 0; k < np; k++) {
            PackedPart pp;
            pp.piece = (uint16_t)parts[k].piece; pp.flags = (uint8_t)(parts[k].billboard || o->billboard); pp.pad = 0;
            memcpy(pp.pos, parts[k].pos, 12);
            for (int c = 0; c < 4; c++) pp.q[c] = (int16_t)(parts[k].quat[c] * 32767.0f);
            pp.scale = parts[k].scale;
            memcpy(p, &pp, sizeof(pp)); p += sizeof(pp);
        }
        n++;
    }
    memcpy(out, &n, 2);
    return (int)(p - out);
}
