# NG64 development notes

How NG64 works, how to build and test it, and what's known about its behaviour. For installing and playing, see
the [README](../README.md).

## How it fits together

```
BeamNG.drive (GE Lua mod: ng64.zip)               ng64helper.exe (C, libsm64)
  - "Mario (NG64)" anchor vehicle       UDP        - Mario physics @ 30 Hz
  - terrain grid + map collision  <------------>   - XInput/keyboard input, SM64 camera
    (16 m cells), vehicle hulls      127.0.0.1     - Mario's body parts and poses
  - draws Mario (ProceduralMesh      :47064        - attacks, landings, carrying
    per body part), camera, HUD                    - SM64 audio (waveOut), HUD graphics
  - hits/damage, BeamMP sync                       - Mario's model, read from the ROM
```

BeamNG mods can't load native code or start programs, so libsm64 runs in a small helper process next to the game.
The installer registers `ng64helper.exe --watch` to start at sign-in: it waits for `BeamNG.drive.x64.exe`,
starts the helper for it (windowless), and the helper exits when that game does (`helper/src/lifecycle.c`).

Nothing from the game is built into NG64. The helper reads from the player's ROM at startup: Mario's model (the
geo layout from segment 0x17 and the Fast3D display lists, vertices and lights from the MIO0 Mario bank, translated
into libsm64's own forms: `helper/src/mario_rom_*.c`), his textures and animations (libsm64), the HUD graphics
(`hud.c`) and the music. libsm64 is built without the model files its build would normally download from the
decompilation (`helper/patches/libsm64-rommodel.patch`; `build.sh` also deletes any downloaded copies). Before that
switch, the model read from the ROM was checked to be identical to libsm64's compiled-in one (17 geo layouts, 506
display lists, 14,984 vertices).

## Building

Needs:
- MSYS2 with MinGW-w64 gcc (`C:\msys64`)
- libsm64's source in `libsm64-master/` (not committed; get it from https://github.com/libsm64/libsm64)
- Inno Setup 6 for the installer (`winget install JRSoftware.InnoSetup`; script: `installer/ng64.iss`)

```bash
./package.sh
```

builds, into `dist/`:
- `ng64.zip`: the BeamNG mod
- `NG64/`: the helper, the mod, the BeamMP server plugin
- `NG64-Setup.exe`: the installer
- `NG64-<version>.zip`: what players download: the installer plus the files for installing by hand

The version is in `VERSION`. `helper/build.sh` applies NG64's libsm64 patches (`helper/patches/`) once each, then
builds the helper.

Silent install (for testing): `NG64-Setup.exe /VERYSILENT /ROM=<rom path> [/MODSDIR=<mods folder>]`.

## Tests

- `tests/helper_smoke.py`: drives the real helper over its UDP protocol with your ROM (ground, jump, run,
  walls, 1 m ledge landing and running jump, rescue from below the ground, punch hits, car roof, hull bed/cab,
  body parts, streamed map cells).
- `tests/Harness/`: runs the BeamMP server plugin in the BeamMP Server Manager project's
  `BeamMpServerLuaHarness` (`dotnet test`). Needs that project checked out next to this one.
- `tests/uat_ingame.py`: full in-game UAT through BeamNG's built-in MCP server. Launch BeamNG with
  `-enablemcp` (`tests/setup_mario.sh` does it and spawns Mario), then run the script. It saves screenshots to
  `tests/shots/`. The dive check depends on timing: SM64 only dives if Mario is past speed 28 when B lands,
  otherwise it's a jump kick that fires too early to reach the car, and the test's input timing over MCP isn't
  frame-exact. `tests/probe_attack.py` runs a single attack against a fresh car for tuning.
- `tests/clip_probe.py`, `tests/world_check.py`: world collision against BeamNG's own geometry.
- `tests/soak.py`: memory and frame time over a long session. `tests/traverse_probe.py`: runs Mario across a
  map and lists stutters.

If NG64 is installed on the test machine, its standby watcher starts the installed helper whenever BeamNG starts:
stop `ng64helper` before testing a fresh build.

## What works (verified by `tests/uat_ingame.py` in the real game)

- Spawns and renders textured Mario; frames arrive live at 30 Hz.
- Stands on and walks over the map (terrain and static meshes), and jumps and runs with SM64 physics.
- No slowdown over long sessions: Mario is drawn as his rigid SM64 body parts (15 of them). Each part's shape is
  built into a mesh once and kept, and every rendered frame only moves the parts. A part is only rebuilt for a look
  it hasn't had before (blinking eyes, hand pose, cap). BeamNG never gets back the cost of drawing a rebuilt mesh,
  so rebuilding the whole of Mario about 60 times a second made the game slower and slower (unplayable after about
  30 minutes). `tests/soak.py` measures this.
- Smooth motion: SM64 runs at 30 Hz, but Mario is drawn at the game's frame rate. Poses are timed by the helper's
  simulation tick (not by when they arrive, which is lumpy because Lua reads the socket once per rendered frame),
  using a high-resolution wall clock. They're kept in a short history, and Mario and the camera are both drawn a
  fixed delay behind the newest pose, blending each part's position, rotation and scale between whichever two
  poses bracket that time.
- No flicker: a newly built part waits a frame before it replaces the old one (createMesh leaves an object blank
  until it has been drawn once).
- Wrecks: a car's hull is split into the pieces still held together by unbroken beams, so a truck torn into cab,
  chassis and bed collides as separate pieces with open space between them. A car that's still being damaged is
  re-read every 0.3 s.
- Level loads: BeamNG respawns the last vehicle (Mario) while a level is still loading. He's only brought in once
  the level reports ready and there's ground under him. His material is recreated if a level change deleted it.
- Resets: the invisible anchor vehicle follows Mario, so BeamNG's reset (R), recover and map teleports put him
  back where the anchor is, at full health, instead of where he was first spawned.
- Replacing Mario from the vehicle spawner removes him and gives the camera to the new vehicle (the spawner keeps
  the vehicle id, so the new car would otherwise be treated as his anchor).
- Attacks dent and shove cars. On a stock Gavril D-Series the damage is roughly: punch 4.9k (the door buckles),
  slide kick 10k, dive 25-47k, ground pound 150k (the roof caves in).
- Cars collide as their real shape: each car rasterizes its own collision triangles into a height grid (the
  hull), so Mario lands in a pickup's bed, below the cab roof, and can't drop through a roof between nodes. If he
  still ends up inside a car, he's pushed out through the nearest side, or up onto the top if that's a short hop.
- Picking things up (Y / E): Mario lifts the nearest car or wreck piece in front of him using SM64's own moves: a
  whole car gets the overhead heavy lift and heavy walk, a piece the light carry. Y again or B throws it; the car
  flies about 24 m and crashes with normal BeamNG damage. The carried car's own Lua holds it with a damped spring,
  placed from the actual animation each frame (found by the gloves), like King Bob-omb. Local only for now.
  libsm64 needs a small addition for this (`helper/patches/libsm64-carry.patch`).
- Landing on a car from a jump or a fall dents it where he lands, harder the faster he came down.
- Switching to another vehicle (TAB) leaves Mario in the world, standing where he was; switching back hands
  control back.
- Music: SM64's Bob-omb Battlefield theme from your ROM while you're playing as Mario (on by default). Back/View
  or M toggles it. Uses `helper/patches/libsm64-music.patch`.
- Camera: SM64-style orbit camera with collision pull-in. It swings round behind Mario as he runs away from or
  across the view, but holds and backs up when he runs at it, as SM64's does.
- Getting run over: SM64's thrown knockback, launched along the car's travel and scaled by its speed. Only the
  car's own speed toward Mario counts, so running into a parked car doesn't hurt him.
- HUD: SM64's HUD in its own UI layout ("NG64 Mario", `mod/settings/ui_apps/originalLayouts/default/`), selected
  while the player controls Mario and replaced by the game's own layout otherwise. Sized from BeamNG's UI scale
  (`--ui-rem`) and pinned to the app overlay's edges. Lives work like SM64. Maps and mods can call
  `ng64.hud.collectCoin(n)` (heals a wedge per coin), `ng64.hud.collectStar(n)` and `ng64.hud.addLife(n)`.
- BeamMP: his state goes to the server at 15 Hz, and other players' Marios are drawn and removed when they leave.
- Diagnostics: frames over 50 ms and gaps over 150 ms in Mario's poses are logged to `beamng.log`, with how long
  NG64's per-frame work took and Lua memory; the helper logs when it falls behind.

## Known limitations

- BeamNG's level loader logs `Failed to spawn vehicle: { "ng64_mario" ...` on every level load. Its placement
  box is built from collidable nodes, and the anchor deliberately has none. The anchor spawns and works; the
  message is cosmetic.
- **Map collision** is the map's real collision geometry: every colliding object near Mario is loaded from its
  shape's COLLADA collision mesh, placed exactly where BeamNG has it. "Visible Mesh" objects use their visible
  mesh, like BeamNG's physics does. Terrain is sampled on a grid, and terrain-less levels use their ground plane
  (terrain lookups are skipped on levels without one). The map is streamed to the helper in 16 m cells: the 3x3
  around Mario, each sent once when it comes into range and dropped when he's two cells away. Triangles are kept in
  FFI arrays so Lua's garbage collector never scans them, and the helper rebuilds its collision at most twice a
  second (about 15 ms). Shapes are parsed once, in the background, prefetched within 120 m of Mario. A very large
  one (West Coast USA's island backdrop is a 164 MB file) can take up to about 20 s the first time, and until then
  that one object has no collision. There is no water. Measured with `tests/clip_probe.py`: Mario inside a solid
  object is 0% on Gridmap and about 0.2% on West Coast USA; SM64's floor agrees with BeamNG's raycasts at 99%+ of
  points (`tests/world_check.py`).
- **Vehicle hulls are top-down** (the highest node per 0.35 m cell, with walls down to the underside), so Mario
  can't go under or inside a car.
- The helper is Windows-only (XInput, waveOut); it runs under Wine/Proton. Sound is not positional.
- Remote players' Marios are re-posed from synced state (action, animation and frame), so they can look slightly
  off during fast actions.
- The installer isn't code-signed, so Windows SmartScreen may warn the first time it's run.
