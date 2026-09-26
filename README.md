# NG64 — Super Mario 64's Mario in BeamNG.drive

Spawn **Mario (NG64)** from the vehicle selector and you play as Mario, running on
[libsm64](https://github.com/libsm64/libsm64): real SM64 movement, jumps, punches, kicks and ground pounds,
on any BeamNG map, against real soft-body vehicles.

You supply your own Super Mario 64 (US) ROM. Nothing from the ROM is shipped: textures, animations and audio are
read from your copy at runtime, and the only file derived from it (Mario's texture atlas) is written to your own
BeamNG user folder.

## How it fits together

```
BeamNG.drive (GE Lua mod: ng64.zip)              ng64helper.exe (C, libsm64)
  - "Mario (NG64)" stub vehicle        UDP        - Mario physics @ 30 Hz
  - terrain raycasts -> heightfield  <------->    - XInput/keyboard input, SM64 camera
  - vehicle bounding boxes          127.0.0.1     - Mario mesh + texture atlas
  - draws Mario (ProceduralMesh)      :47064      - attack detection vs vehicles
  - camera, hits/damage, BeamMP                   - SM64 audio (waveOut)
```

BeamNG mods can't load native code, so libsm64 runs in a small helper process next to the game.

## Install

1. Copy `ng64.zip` into `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods\`.
2. Put `ng64helper.exe` anywhere, with your Super Mario 64 (US) `.z64` ROM in the same folder. Any file name
   works: the helper uses the first `.z64` there whose header is SM64 (US) and skips any others. You can also put
   the ROM's full path in `rom.txt` next to the exe, or pass `--rom <path>`.
3. Start `ng64helper.exe`, then BeamNG. Spawn **NG64 → Mario** from the vehicle selector.

**Multiplayer (BeamMP):** put `ng64.zip` in the server's `Resources/Client/` and the `beammp_server_plugin`
folder as `Resources/Server/NG64/`. Every player needs the helper running.

## Controls

| N64 | Xbox pad | Keyboard |
|---|---|---|
| Stick | Left stick | W A S D |
| A (jump) | A | Space |
| B (punch/kick) | B or X | J |
| Z (crouch / ground pound) | RT or LT | K |
| C buttons (camera) | Right stick | Arrow keys |
| Camera zoom | RB / LB | — |
| Pick up / throw | Y | E |

Input is only read while the BeamNG window has focus. To stop playing as Mario, switch to any other vehicle.

## What works (verified by `tests/uat_ingame.py` in the real game)

- Spawns and renders textured Mario; frames arrive live at 30 Hz.
- Stands on and walks over the map (terrain and static meshes), and jumps and runs with SM64 physics.
- No flicker: Mario's mesh is triple-buffered and rebuilt at most once per rendered frame.
- Smooth motion: SM64 runs at 30 Hz, but Mario is drawn at the game's frame rate. Poses are timed by the helper's
  simulation tick (not by when they arrive, which is lumpy because Lua reads the socket once per rendered frame),
  using a high-resolution wall clock. They're kept in a short history, and Mario and the camera are both drawn a
  fixed delay behind the newest pose, blending whichever two poses bracket that time. His position updates every
  rendered frame, and his pose up to 60 times a second. Measured: 0 stalled frames in 330+, where the previous
  version stalled in about 1 frame in 4. The helper sends each distinct vertex once
  (about 540 instead of about 2250 per-corner copies). BeamNG's createMesh cost scales with that count, so Mario
  costs about 1 ms of Lua per frame.
- Wrecks: a car's hull is split into the pieces still held together by unbroken beams, so a truck torn into cab,
  chassis and bed collides as separate pieces with open space between them. A car that's still being damaged is
  re-read every 0.3 s.
- Level loads: BeamNG respawns the last vehicle (Mario) while a level is still loading. He's now only brought in
  once the level reports ready and there's ground under him. His material is recreated if a level change deleted
  it (that was why he went invisible after switching maps, e.g. to West Coast USA).
- Resets: the invisible anchor vehicle follows Mario, so BeamNG's reset (R), recover and map teleports put him
  back where the anchor is, at full health, instead of where he was first spawned. `tests/flicker_capture.ps1` grabs the game window off the desktop
  and finds Mario in 80/80 grabs, where the single-mesh version missed him in 22/60.
- Attacks dent and shove cars. On a stock Gavril D-Series the damage is roughly: punch 4.9k (the door buckles),
  slide kick 10k, dive 25-47k, ground pound 150k (the roof caves in).
- Cars collide as their real shape: each car rasterizes its own collision triangles (its skin) into a height grid
  (the hull), so Mario lands in a pickup's bed, below the cab roof, and can't drop through a roof between nodes.
  The hull is smooth over gentle changes, so only real edges are ledges to grab, and it's sealed down to the
  underside. If Mario still ends up inside a car (it drove into him, a bad landing), he's pushed out through the
  nearest side, or up onto the top if that's a short hop.
- Picking things up (Y / E): Mario lifts the nearest car or wreck piece in front of him using SM64's own
  moves. A whole car gets the overhead heavy lift and heavy walk; a piece that came off gets the light carry. Y
  again or B throws it (SM64's heavy throw); the car flies about 24 m and crashes with normal BeamNG damage. Z
  sets a heavy car down (SM64 itself has no heavy put-down) and does SM64's put-down for light pieces. While it's
  in his hands the car has no collision with him and can't hurt him; collision comes back 1.5 s after release.
  The carried car's own Lua holds it with a damped spring, so it stays a soft body. It's placed from the actual
  animation each frame, found by the gloves (the only pure-white part of the model): a car rests its floor pan
  just on top of him, on his raised hands, like King Bob-omb, and bobs as he heavy-walks; a light piece sits
  between his hands. Other players
  don't see the carry yet (local only). libsm64 needs a small addition for this (`helper/patches/libsm64-carry.patch`,
  applied by `helper/build.sh`): SM64's carry code expects a real held object.
- Getting run over: SM64's own thrown knockback (the tumble from an explosion), launched along the car's travel
  and scaled by its speed. Only the car's own speed toward Mario counts, so running or sliding into a parked car,
  or a car he just punched away, doesn't hurt him. The hull is re-read every 2 s and right after a hit, so dents change the
  shape. Mario rides moving cars.
- Ledges: he lands on and jumps onto the top of height steps instead of bonking off them. If he ever ends up
  below the collision, he is put back on the surface above.
- Cars that hit him at speed knock him back and take health.
- SM64-style orbit camera with collision pull-in. Switching vehicles gives the normal camera back.
- BeamMP: his state goes to the server at 15 Hz, and other players' Marios are drawn and removed when they leave.

## Known limitations

- BeamNG's level loader logs `Failed to spawn vehicle: { "ng64_mario" ...` on every level load. Its placement
  box is built from collidable nodes, and the anchor deliberately has none, because it would be an invisible,
  immovable post. The anchor spawns and works; the message is cosmetic.

- **Map collision** is the map's real collision geometry: every colliding object within 32 m (buildings, ramps,
  walls, rails, props, trees, rocks) is loaded from its shape's COLLADA collision mesh, placed exactly where
  BeamNG has it. "Visible Mesh" objects use their visible mesh, like BeamNG's physics does. Terrain is sampled on
  a grid (a heightfield, which sampling captures), and terrain-less levels use their ground plane. Shapes are
  parsed once, in the background, prefetched within 120 m of Mario. A very large one (West Coast USA's island
  backdrop is a 164 MB file) can take up to about 20 s the first time, and until then that one object has no
  collision. There is no water. Measured with `tests/clip_probe.py` (60 s of random running, jumping, diving and
  ground pounds in the busiest spot on the map): Mario inside a solid object dropped from 11% of samples to 0% on
  Gridmap, and from 14% to 0.2% on West Coast USA. SM64's floor agrees with BeamNG's raycasts at 99%+ of points
  (`tests/world_check.py`).
- **Vehicle hulls are top-down** (the highest node per 0.35 m cell, with walls down to the underside), so Mario
  can't go under or inside a car. Attacks still aim at the car's bounding box, then dent the nodes nearest the
  hit.
- The helper is Windows-only (XInput, waveOut). Sound is not positional.
- Remote players' Marios are re-posed from synced state (action, animation and frame), so they can look
  slightly off during fast actions.

## Legal / licensing note: read before distributing

The mod's Lua, the helper's own C code and the server plugin are original to this project. However, libsm64
**compiles Mario's model and geometry data** (`src/decomp/mario/geo.inc.c` / `model.inc.c`, downloaded from the
SM64 decompilation by libsm64's own `import-mario-geo.py`) **into the helper binary**. That data comes from the
original game. This is the same situation as libsm64 itself and sm64-san-andreas, but it means a built
`ng64helper.exe` is not purely "open-source code with no copyrighted material". Those generated files are
git-ignored here and never committed. See the open question in the handoff notes about loading the model from
the user's ROM at runtime instead.

## Building

Needs MSYS2 with MinGW-w64 gcc (`C:\msys64`), and libsm64's source in `libsm64-master/` (not committed).

```bash
./package.sh          # builds helper + dist/ng64.zip + dist/NG64/
```

## Tests

- `tests/helper_smoke.py`: drives the real helper over its UDP protocol with your ROM (ground, jump, run,
  walls, 1 m ledge landing and running jump, rescue from below the ground, punch hits, car roof, hull bed/cab).
- `tests/Harness/`: runs the BeamMP server plugin in the BeamMP Server Manager project's
  `BeamMpServerLuaHarness` (`dotnet test`).
- `tests/clip_probe.py`, `tests/world_check.py`: world collision against BeamNG's own geometry (see above).
- `tests/uat_ingame.py`: full in-game UAT through BeamNG's built-in MCP server. Launch BeamNG with
  `-enablemcp`, have the helper running, then run the script. It saves screenshots to `tests/shots/`. The
  dive check depends on timing: SM64 only dives if Mario is past speed 28 when B lands, otherwise it's a jump
  kick that fires too early to reach the car, and the test's input timing over MCP isn't frame-exact.
  `tests/probe_attack.py` runs a single attack against a fresh car for tuning.
