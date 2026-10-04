// Pickups and enemies round Mario. See ents.h. All positions are sm64 units (Mario is ~160 units tall, NG64_SCALE m
// per unit); the packet and events convert to bng.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ents.h"
#include "protocol.h"
#include "objrom.h"

#define S NG64_SCALE

#define MAX_ENT 48
#define MAX_PICKUPS 12
#define MAX_ENEMIES 7
#define TICKS_PER_S 30

// sm64 bits
#define ACT_FLAG_AIR       0x00000800u
#define ACT_FLAG_ATTACKING 0x00800000u
#define MARIO_VANISH_CAP   0x00000002u
#define MARIO_METAL_CAP    0x00000004u
#define MARIO_WING_CAP     0x00000008u
#define MARIO_PUNCHING     0x00100000u
#define MARIO_KICKING      0x00200000u

// sounds (audio_defines.h SOUND_ARG_LOAD(bank, play flags, id, priority, flags2), spelled out)
#define SND_ARG(bank, play, id, prio, f2) (((uint32_t)(bank) << 28) | ((uint32_t)(play) << 24) | ((uint32_t)(id) << 16) | ((uint32_t)(prio) << 8) | ((uint32_t)(f2) << 4) | 1u)
#define SOUND_COIN       SND_ARG(3, 8, 0x11, 0x80, 8)
#define SOUND_RED_COIN   SND_ARG(7, 8, 0x28, 0x90, 8)
#define SOUND_STAR       SND_ARG(7, 0, 0x1E, 0xFF, 8)
#define SOUND_POWERUP    SND_ARG(3, 0, 0x58, 0xFF, 8)
#define SOUND_GOOMBA     SND_ARG(5, 0, 0x2F, 0x00, 8)

typedef struct {
    int used;
    uint16_t id;
    uint8_t type, state;
    float pos[3];          // sm64 units
    float yaw;             // radians, sm64 (forward = (sin, cos))
    float vel[3];          // units per tick
    int timer;             // ticks in the current state
    int life;              // ticks left (pickups), or since spawn
    float anim;            // walk / spin phase
    float scale;
    float aim;             // where it is heading (yaw)
    int hurtCooldown;
} Ent;

static Ent s_ent[MAX_ENT];
static EntHost s_host;
static int s_pickupsOn, s_enemiesOn;
static uint16_t s_nextId = 1;
static uint32_t s_rng = 0x1234567u;
static int s_starTicks;           // invincibility star left
static int s_hurtMarioCooldown;
static int s_prevCapFlags;
static uint32_t s_nextPickupSpawn, s_nextEnemySpawn;

static float frand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (s_rng & 0xFFFFFF) / (float)0x1000000;
}

static int is_pickup(int type) { return type >= ENT_COIN_YELLOW && type <= ENT_STAR_POWER; }
static int is_enemy(int type) { return type >= ENT_GOOMBA && type <= ENT_SHELL; }

static void emit(int kind, int id, const float *pos, float d)
{
    if (s_host.event) s_host.event(kind, id, pos ? pos[0] * S : 0, pos ? -pos[2] * S : 0, pos ? pos[1] * S : 0, d);
}

void ents_init(const EntHost *host)
{
    s_host = *host;
    memset(s_ent, 0, sizeof(s_ent));
}

void ents_set_options(int pickups, int enemies)
{
    pickups = pickups != 0; enemies = enemies != 0;
    if (!pickups && s_pickupsOn) for (int i = 0; i < MAX_ENT; i++) if (s_ent[i].used && is_pickup(s_ent[i].type)) s_ent[i].used = 0;
    if (!enemies && s_enemiesOn) for (int i = 0; i < MAX_ENT; i++) if (s_ent[i].used && is_enemy(s_ent[i].type)) s_ent[i].used = 0;
    s_pickupsOn = pickups; s_enemiesOn = enemies;
}

void ents_get_options(int *pickups, int *enemies) { *pickups = s_pickupsOn; *enemies = s_enemiesOn; }
int ents_star_active(void) { return s_starTicks > 0; }
void ents_clear(void) { memset(s_ent, 0, sizeof(s_ent)); s_starTicks = 0; }

int ents_count(int enemies)
{
    int n = 0;
    for (int i = 0; i < MAX_ENT; i++) if (s_ent[i].used && (enemies ? is_enemy(s_ent[i].type) : is_pickup(s_ent[i].type))) n++;
    return n;
}

static Ent *ent_alloc(int type, const float *pos, float yaw)
{
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &s_ent[i];
        if (e->used) continue;
        memset(e, 0, sizeof(*e));
        e->used = 1; e->id = s_nextId++; if (!s_nextId) s_nextId = 1;
        e->type = (uint8_t)type;
        memcpy(e->pos, pos, 12);
        e->yaw = e->aim = yaw;
        e->scale = 1;
        e->anim = frand() * 6.28f;
        e->life = is_pickup(type) ? 150 * TICKS_PER_S : 0;
        return e;
    }
    return NULL;
}

// ---- spawning ------------------------------------------------------------------------------------------------

static int pick_weighted(const int *types, const int *weights, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += weights[i];
    int r = (int)(frand() * total);
    for (int i = 0; i < n; i++) { if (r < weights[i]) return types[i]; r -= weights[i]; }
    return types[0];
}

static int count_type(int type)
{
    int n = 0;
    for (int i = 0; i < MAX_ENT; i++) if (s_ent[i].used && s_ent[i].type == type) n++;
    return n;
}

// a random spot on flat ground 12 - 50 m from Mario; 0 if none after a few tries
static int find_spot(const struct SM64MarioState *st, float *out)
{
    for (int tries = 0; tries < 8; tries++) {
        float ang = frand() * 6.2832f, dist = (12.0f + frand() * 38.0f) / S;
        float x = st->position[0] + sinf(ang) * dist, z = st->position[2] + cosf(ang) * dist;
        float fy, ny;
        if (!s_host.floor(x, st->position[1] + 400, z, &fy, &ny)) continue;
        if (ny < 0.93f) continue;                                // flat ground only
        if (fabsf(fy - st->position[1]) > 700) continue;          // not up on a roof or down a pit
        if (s_host.blocked && s_host.blocked(x, fy + 50, z, fy)) continue;
        out[0] = x; out[1] = fy; out[2] = z;
        return 1;
    }
    return 0;
}

static void spawn_pickup(const struct SM64MarioState *st)
{
    static const int types[] = { ENT_COIN_YELLOW, ENT_COIN_RED, ENT_COIN_BLUE, ENT_POWER_STAR, ENT_CAP_METAL, ENT_CAP_WING, ENT_STAR_POWER };
    static const int weights[] = { 55, 8, 4, 4, 8, 8, 5 };
    int type = pick_weighted(types, weights, 7);
    if (type >= ENT_POWER_STAR && count_type(type) >= 1) return;   // one of each rare thing at a time
    float p[3];
    if (!find_spot(st, p)) return;
    float lift = type == ENT_POWER_STAR || type == ENT_STAR_POWER ? 90 : type >= ENT_CAP_METAL ? 40 : 70;
    p[1] += lift / 1.0f;
    ent_alloc(type, p, frand() * 6.28f);
}

static void spawn_enemy(const struct SM64MarioState *st)
{
    static const int types[] = { ENT_GOOMBA, ENT_BOBOMB, ENT_KOOPA };
    static const int weights[] = { 60, 28, 12 };   // goombas are everywhere in SM64, koopas rare
    int type = pick_weighted(types, weights, 3);
    float p[3];
    if (!find_spot(st, p)) return;
    ent_alloc(type, p, frand() * 6.28f);
}

// ---- Mario meets things -----------------------------------------------------------------------------------------

static float dist2d(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }

static void hurt_mario(int marioId, const struct SM64MarioState *st, const float *from, int wedges)
{
    if (s_hurtMarioCooldown > 0 || s_starTicks > 0) return;
    if (st->flags & (MARIO_METAL_CAP | MARIO_VANISH_CAP)) return;
    s_hurtMarioCooldown = 45;
    s_host.lock();
    sm64_mario_take_damage(marioId, wedges, 0, from[0], from[1], from[2]);
    s_host.unlock();
}

static void kill_enemy(Ent *e, int dropCoin)
{
    emit(EV_ENEMY_DEAD, e->id, e->pos, e->type);
    if (e->type == ENT_GOOMBA) { s_host.lock(); sm64_ng64_play_sound(SOUND_GOOMBA); s_host.unlock(); }
    if (dropCoin && (e->type == ENT_GOOMBA || e->type == ENT_KOOPA)) {
        float p[3] = { e->pos[0], e->pos[1] + 60, e->pos[2] };
        Ent *c = ent_alloc(ENT_COIN_YELLOW, p, 0);
        if (c) c->life = 20 * TICKS_PER_S;
    }
    e->state = 9; e->timer = 0;   // squashed, then gone
}

static void explode(Ent *b, int marioId, const struct SM64MarioState *st)
{
    const float R = 3.5f / S;
    emit(EV_EXPLOSION, b->id, b->pos, 3.5f);
    s_host.lock(); sm64_ng64_play_sound(SND_ARG(3, 0, 0x3A, 0xF0, 8)); s_host.unlock();   // an explosion
    float mid[3] = { st->position[0], st->position[1] + 60, st->position[2] };
    float dx = mid[0] - b->pos[0], dy = mid[1] - b->pos[1], dz = mid[2] - b->pos[2];
    if (sqrtf(dx * dx + dy * dy + dz * dz) < R) hurt_mario(marioId, st, b->pos, 2);
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *o = &s_ent[i];
        if (!o->used || o == b || !is_enemy(o->type) || o->state == 9) continue;
        if (o->type == ENT_SHELL) continue;
        float ox = o->pos[0] - b->pos[0], oy = o->pos[1] - b->pos[1], oz = o->pos[2] - b->pos[2];
        if (sqrtf(ox * ox + oy * oy + oz * oz) < R) {
            if (o->type == ENT_BOBOMB) { if (o->state != 2) { o->state = 2; o->timer = 18; } }
            else kill_enemy(o, 1);
        }
    }
    b->used = 0;
}

// ---- movement ---------------------------------------------------------------------------------------------------

static float angle_diff(float a, float b)
{
    float d = fmodf(a - b + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return d - 3.14159265f;
}

// walk towards `aim` at `speed` (units per tick), staying on flat ground; false if blocked (so it turns instead)
static int step_ground(Ent *e, float speed, float turn)
{
    float d = angle_diff(e->aim, e->yaw);
    e->yaw += fmaxf(-turn, fminf(turn, d));
    float nx = e->pos[0] + sinf(e->yaw) * speed, nz = e->pos[2] + cosf(e->yaw) * speed;
    float fy, ny;
    if (!s_host.floor(nx, e->pos[1] + 150, nz, &fy, &ny) || ny < 0.7f || fy < e->pos[1] - 120 || fy > e->pos[1] + 120) return 0;
    if (s_host.blocked && s_host.blocked(nx, fy + 50, nz, fy)) return 0;
    e->pos[0] = nx; e->pos[1] = fy; e->pos[2] = nz;
    return 1;
}

static void wander(Ent *e, float speed, float turn)
{
    if (e->timer <= 0 || !step_ground(e, speed, turn)) {
        e->aim = frand() * 6.2832f;
        e->timer = (int)(TICKS_PER_S * (1.5f + frand() * 3));
    }
    e->timer--;
}

static void tick_enemy(Ent *e, int marioId, const struct SM64MarioState *st, int attacking, int mighty)
{
    const float cm = 1.0f / S / 100.0f;   // 1 cm in units
    (void)cm;
    float toM = dist2d(e->pos, st->position);
    float aimM = atan2f(st->position[0] - e->pos[0], st->position[2] - e->pos[2]);
    float dy = st->position[1] - e->pos[1];
    e->anim += 0.35f;
    if (e->hurtCooldown > 0) e->hurtCooldown--;

    if (e->state == 9) {                                // squashed: flat for half a second, then gone
        e->scale = fmaxf(0.1f, e->scale - 0.12f);
        if (++e->timer > 15) e->used = 0;
        return;
    }

    switch (e->type) {
    case ENT_GOOMBA: {
        float speed;
        if (toM < 12.0f / S && fabsf(dy) < 300) { e->state = 1; e->aim = aimM; speed = 2.6f / S / TICKS_PER_S; step_ground(e, speed, 0.12f); }
        else { e->state = 0; speed = 0.9f / S / TICKS_PER_S; wander(e, speed, 0.05f); }
        break;
    }
    case ENT_BOBOMB: {
        if (e->state == 2) {                            // fuse lit: stands still, swelling, then goes off
            e->scale = 1.0f + 0.12f * sinf(e->anim * 2.2f) + 0.02f * (18 - e->timer < 0 ? 0 : 18 - e->timer);
            if (--e->timer <= 0) { explode(e, marioId, st); return; }
        } else if (toM < 8.0f / S && fabsf(dy) < 300) {
            e->state = 1; e->aim = aimM;
            step_ground(e, 1.9f / S / TICKS_PER_S, 0.1f);
            if (toM < 1.6f / S) { e->state = 2; e->timer = 2 * TICKS_PER_S; }
        } else { e->state = 0; wander(e, 0.8f / S / TICKS_PER_S, 0.05f); }
        break;
    }
    case ENT_KOOPA: {
        if (toM < 5.0f / S && fabsf(dy) < 300) { e->state = 1; e->aim = aimM + 3.14159265f; step_ground(e, 2.3f / S / TICKS_PER_S, 0.15f); }
        else { e->state = 0; wander(e, 0.7f / S / TICKS_PER_S, 0.05f); }
        break;
    }
    case ENT_SHELL: {
        if (e->state == 1) {                            // kicked: slides on, flattening what it meets
            e->anim += 0.8f;
            if (!step_ground(e, 9.0f / S / TICKS_PER_S, 0.0f) || --e->timer <= 0) { e->used = 0; return; }
            for (int i = 0; i < MAX_ENT; i++) {
                Ent *o = &s_ent[i];
                if (!o->used || o == e || !is_enemy(o->type) || o->type == ENT_SHELL || o->state == 9) continue;
                if (dist2d(o->pos, e->pos) < 0.9f / S) {
                    if (o->type == ENT_BOBOMB) { o->state = 2; o->timer = 6; } else kill_enemy(o, 1);
                }
            }
        }
        break;
    }
    }
    if (!e->used) return;

    // meeting Mario
    float contact = (e->type == ENT_SHELL ? 0.75f : 0.65f) / S;
    // close enough sideways, and level with it: from above (falling onto it) down to about its feet
    if (toM < contact + 20 && dy > -150 && dy < 170 && e->hurtCooldown <= 0 && !(e->type == ENT_SHELL && e->state == 1)) {
        int stomp = (st->action & ACT_FLAG_AIR) && st->velocity[1] < -4 && dy > 0.35f * 140;
        int hit = attacking || mighty;
        if (e->type == ENT_SHELL) {
            if (e->state == 0) {                        // kick it away from him
                e->state = 1; e->timer = 6 * TICKS_PER_S; e->aim = aimM + 3.14159265f; e->yaw = e->aim;
                e->hurtCooldown = 10;
            }
        } else if (stomp || hit) {
            if (stomp) {
                s_host.lock(); sm64_set_mario_velocity(marioId, st->velocity[0], 38.0f, st->velocity[2]); s_host.unlock();
            }
            if (e->type == ENT_BOBOMB) {
                if (e->state != 2) { e->state = 2; e->timer = (stomp ? 2 : 1) * TICKS_PER_S; }
                e->hurtCooldown = 20;
                if (hit && !stomp) {                    // thrown clear
                    e->aim = aimM + 3.14159265f; e->yaw = e->aim;
                    for (int k = 0; k < 5; k++) step_ground(e, 40, 0);
                }
            } else if (e->type == ENT_KOOPA) {
                e->type = ENT_SHELL; e->state = 0; e->hurtCooldown = 15; e->scale = 1;
                emit(EV_ENEMY_DEAD, e->id, e->pos, ENT_KOOPA);
                { float p[3] = { e->pos[0], e->pos[1] + 60, e->pos[2] }; Ent *c = ent_alloc(ENT_COIN_YELLOW, p, 0); if (c) c->life = 20 * TICKS_PER_S; }
            } else {
                kill_enemy(e, 1);
            }
        } else if (e->type == ENT_GOOMBA && dy < 100) {
            hurt_mario(marioId, st, e->pos, 1);
            e->hurtCooldown = 30;
        }
    }
}

static void collect(Ent *e, int marioId, const struct SM64MarioState *st)
{
    s_host.lock();
    switch (e->type) {
    case ENT_COIN_YELLOW: sm64_ng64_play_sound(SOUND_COIN); emit(EV_COIN, e->id, e->pos, 1); break;
    case ENT_COIN_RED:    sm64_ng64_play_sound(SOUND_RED_COIN); emit(EV_COIN, e->id, e->pos, 2); break;
    case ENT_COIN_BLUE:   sm64_ng64_play_sound(SOUND_RED_COIN); emit(EV_COIN, e->id, e->pos, 5); break;
    case ENT_POWER_STAR:
        sm64_ng64_play_sound(SOUND_STAR);
        sm64_mario_heal(marioId, 31);
        emit(EV_POWER_STAR, e->id, e->pos, 1);
        break;
    case ENT_CAP_METAL:
        sm64_mario_interact_cap(marioId, MARIO_METAL_CAP, 0, 0);
        emit(EV_CAP_METAL, e->id, e->pos, 20);
        break;
    case ENT_CAP_WING:
        sm64_mario_interact_cap(marioId, MARIO_WING_CAP, 0, 0);
        emit(EV_CAP_WING, e->id, e->pos, 60);
        break;
    case ENT_STAR_POWER:
        sm64_ng64_play_sound(SOUND_STAR);
        s_starTicks = 20 * TICKS_PER_S;
        emit(EV_STAR_POWER, e->id, e->pos, 20);
        break;
    }
    s_host.unlock();
    (void)st;
    e->used = 0;
}

void ents_car_kill(int id, int kind)
{
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &s_ent[i];
        if (!e->used || e->id != id || !is_enemy(e->type) || e->state == 9) continue;
        if (e->type == ENT_BOBOMB) { if (e->state != 2) { e->state = 2; e->timer = 1; } }
        else if (e->type == ENT_SHELL && kind == 1) e->used = 0;
        else kill_enemy(e, 1);
        return;
    }
}

void ents_tick(uint32_t tick, int marioId, const struct SM64MarioState *st)
{
    (void)tick;
    if (s_hurtMarioCooldown > 0) s_hurtMarioCooldown--;

    // the invincibility star
    if (s_starTicks > 0) {
        s_host.lock(); sm64_set_mario_invincibility(marioId, 40); s_host.unlock();
        if (--s_starTicks == 0) { emit(EV_POWER_END, 0, NULL, EV_STAR_POWER); }
    }
    // caps: libsm64 clears the flag when the time is up
    int capFlags = (int)(st->flags & (MARIO_METAL_CAP | MARIO_WING_CAP | MARIO_VANISH_CAP));
    if ((s_prevCapFlags & MARIO_METAL_CAP) && !(capFlags & MARIO_METAL_CAP)) emit(EV_POWER_END, 0, NULL, EV_CAP_METAL);
    if ((s_prevCapFlags & MARIO_WING_CAP) && !(capFlags & MARIO_WING_CAP)) emit(EV_POWER_END, 0, NULL, EV_CAP_WING);
    s_prevCapFlags = capFlags;

    int attacking = (st->action & ACT_FLAG_ATTACKING) != 0 || (st->flags & (MARIO_PUNCHING | MARIO_KICKING)) != 0;
    int mighty = s_starTicks > 0 || (st->flags & MARIO_METAL_CAP) != 0;   // touching is enough

    // spawning
    if (s_pickupsOn && ents_count(0) < MAX_PICKUPS && tick >= s_nextPickupSpawn) {
        spawn_pickup(st);
        s_nextPickupSpawn = tick + (uint32_t)(TICKS_PER_S * (1.2f + frand() * 1.8f));
    }
    if (s_enemiesOn && ents_count(1) < MAX_ENEMIES && tick >= s_nextEnemySpawn) {
        spawn_enemy(st);
        s_nextEnemySpawn = tick + (uint32_t)(TICKS_PER_S * (3.0f + frand() * 4.0f));
    }

    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &s_ent[i];
        if (!e->used) continue;
        float far = dist2d(e->pos, st->position);
        if (far > 100.0f / S) { e->used = 0; continue; }
        if (is_pickup(e->type)) {
            e->yaw += (e->type >= ENT_CAP_METAL ? 0.09f : 0.17f);
            e->anim += 0.12f;
            if (e->life > 0 && --e->life == 0) { e->used = 0; continue; }
            float dy = (st->position[1] + 60) - e->pos[1];
            if (far < 0.95f / S && fabsf(dy) < 1.1f / S) collect(e, marioId, st);
        } else {
            tick_enemy(e, marioId, st, attacking, mighty);
        }
    }
}

static int ent_model(int type)
{
    switch (type) {
    case ENT_COIN_YELLOW: return OM_COIN_YELLOW;
    case ENT_COIN_RED: return OM_COIN_RED;
    case ENT_COIN_BLUE: return OM_COIN_BLUE;
    case ENT_POWER_STAR: case ENT_STAR_POWER: return OM_STAR;
    case ENT_CAP_METAL: return OM_CAP_METAL;
    case ENT_CAP_WING: return OM_CAP_WING;
    case ENT_GOOMBA: return OM_GOOMBA;
    case ENT_BOBOMB: return OM_BOBOMB;
    case ENT_KOOPA: return OM_KOOPA;
    case ENT_SHELL: return OM_KOOPA_SHELL;
    }
    return -1;
}

#pragma pack(push, 1)
typedef struct { uint16_t piece; uint8_t flags, pad; float pos[3]; int16_t q[4]; float scale; } PackedPart;
#pragma pack(pop)

int ents_pack(uint8_t *out, size_t cap)
{
    uint16_t n = 0;
    uint8_t *p = out + 2;
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &s_ent[i];
        int model = e->used ? ent_model(e->type) : -1;
        if (model < 0) continue;
        ObjPose pose;
        memset(&pose, 0, sizeof(pose));
        memcpy(pose.pos, e->pos, 12);
        pose.angle[1] = (int16_t)(int)(e->yaw * (65536.0f / 6.2831853f));
        pose.scale[0] = pose.scale[1] = pose.scale[2] = e->scale;
        pose.animState = (int)(e->anim * 2.0f) & 7;
        static const struct { uint32_t table; } anims[] = { [ENT_GOOMBA] = { 0x0801DA4C }, [ENT_BOBOMB] = { 0x0802396C }, [ENT_KOOPA] = { 0x06011364 } };
        if (e->type == ENT_GOOMBA || e->type == ENT_BOBOMB || e->type == ENT_KOOPA) {
            pose.anim = objrom_anim_from_table(anims[e->type].table, 0);
            int ls, le, fl;
            if (objrom_anim_info(pose.anim, &ls, &le, &fl) && le > 0) pose.animFrame = (int)(e->anim * 2.0f) % le;
            pose.animState = ((int)(e->anim * 0.3f) % 24 == 0) ? 1 : 0;   // an occasional blink
        }
        ObjPart parts[16];
        int np = objrom_pose(model, &pose, parts, 16);
        if ((size_t)(p - out) + 18 + (size_t)np * sizeof(PackedPart) > cap) continue;
        float b[3] = { e->pos[0] * S, -e->pos[2] * S, e->pos[1] * S };
        memcpy(p, &e->id, 2); p[2] = e->type; p[3] = e->state; p[4] = (uint8_t)np; p[5] = 0; p += 6;
        memcpy(p, b, 12); p += 12;
        for (int k = 0; k < np; k++) {
            PackedPart pp;
            pp.piece = (uint16_t)parts[k].piece; pp.flags = (uint8_t)parts[k].billboard; pp.pad = 0;
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
