// SM64's pickups (coins, power stars, metal / wing caps, an invincibility star) and a few of its enemies (goombas,
// bob-ombs, koopas) scattered round Mario, simulated here at 30 Hz and drawn by the mod from the entity packet.
// They live in sm64 units like Mario; the packet and events are in BeamNG's (bng) coordinates and metres.
#ifndef NG64_ENTS_H
#define NG64_ENTS_H

#include <stddef.h>
#include <stdint.h>
#include "libsm64.h"

enum {
    ENT_COIN_YELLOW = 1, ENT_COIN_RED, ENT_COIN_BLUE, ENT_POWER_STAR, ENT_CAP_METAL, ENT_CAP_WING, ENT_STAR_POWER,
    ENT_GOOMBA = 20, ENT_BOBOMB, ENT_KOOPA, ENT_SHELL,
};

// events for the mod (kind, entity id, and four numbers: position bng x, y, z and a value / radius)
enum {
    EV_COIN = 1,        // d = value (1, 2 or 5)
    EV_POWER_STAR,
    EV_CAP_METAL,       // d = seconds
    EV_CAP_WING,        // d = seconds
    EV_STAR_POWER,      // d = seconds
    EV_POWER_END,       // d = which of EV_CAP_METAL / EV_CAP_WING / EV_STAR_POWER ended
    EV_EXPLOSION,       // position, d = radius (m)
    EV_ENEMY_DEAD,      // position, d = the enemy type
    EV_OPTIONS,         // a = pickups on, b = enemies on (so the settings app follows the keys)
};

typedef struct {
    // the nearest floor under (x, y, z) in sm64 units; 0 if none
    int (*floor)(float x, float y, float z, float *floorY, float *normalY);
    // not a place to spawn (inside a vehicle, under water)
    int (*blocked)(float x, float y, float z, float floorY);
    void (*event)(int kind, int id, float x, float y, float z, float d);
    void (*lock)(void);     // around the libsm64 calls that start sounds
    void (*unlock)(void);
} EntHost;

void ents_init(const EntHost *host);
void ents_set_options(int pickups, int enemies);
void ents_get_options(int *pickups, int *enemies);
// once per 30 Hz tick while Mario exists: marioId, his state, whether the player can steer him
void ents_tick(uint32_t tick, int marioId, const struct SM64MarioState *st);
// the entity list as a MSG_ENTITIES packet body (count, then the records); returns its length
int ents_pack(uint8_t *out, size_t cap);
// a car ran an enemy over (or a shell hit one): kind 0 = run over
void ents_car_kill(int id, int kind);
int ents_star_active(void);
void ents_clear(void);
int ents_count(int enemies);

#endif
