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

Run **`NG64-Setup.exe`** (no admin rights needed). It asks for your Super Mario 64 (US) ROM (any file name;
`.z64`, `.v64` and `.n64` dumps all work, and it checks it really is that game and region) and for BeamNG's mods
folder, which it finds for you (the game's `startup.ini` user folder if you've moved it, otherwise
`%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods`). Then just start BeamNG and spawn **NG64 → Mario** from the
vehicle selector.

What it sets up:
- the helper and a copy of your ROM in `%LOCALAPPDATA%\NG64`, and `ng64.zip` in the mods folder;
- a hidden standby process (`ng64helper.exe --watch`) that starts with Windows. A BeamNG mod can't start programs
  itself, so this does it: when BeamNG starts, it starts the helper (no window), and the helper closes with the game.
  If it isn't running, start **NG64** from the Start menu;
- Start menu entries, and an uninstaller in Windows' installed apps (it removes all of the above).

By hand instead: put `ng64.zip` in the mods folder, and `ng64helper.exe` anywhere with your ROM next to it (or its
path in `rom.txt`, or `--rom <path>`). Start the helper before or after the game; it exits when the game closes.

**Multiplayer (BeamMP):** put `ng64.zip` in the server's `Resources/Client/` and the `beammp_server_plugin`
folder as `Resources/Server/NG64/` (the installer also puts a copy in `%LOCALAPPDATA%\NG64\beammp_server_plugin`).
Every player needs NG64 installed.

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
| Music on / off | Back (View) | M |

Input is only read while the BeamNG window has focus. To stop playing as Mario, switch to any other vehicle.

**HUD:** while you play as Mario, the UI switches to the "NG64 Mario" layout: Super Mario 64's own HUD, drawn with
the HUD graphics from your ROM (the helper extracts them into `ng64_cache/hud/` in your BeamNG user folder). The
power meter shows Mario's health and hides again once he's back to full; the counters show lives, coins and stars.
Lives work like SM64: 4 to start, one lost each time Mario's health runs out (respawn him with the vehicle reset,
R), and after the last one the next respawn starts again at 4. Driving any other vehicle brings back the game's own
layout. Nothing in BeamNG gives coins or stars yet; a map or mod can call `ng64.hud.collectCoin(n)` (which also heals
a wedge per coin, as in SM64), `ng64.hud.collectStar(n)` and `ng64.hud.addLife(n)`.

**Wine / Proton (Linux):** run the helper in the game's own Wine prefix, or Mario has no textures (the helper writes
his texture into the game's user folder, and a different prefix has a different `C:\`):

```bash
WINEPREFIX=~/.steam/steam/steamapps/compatdata/284160/pfx wine ng64helper.exe
```

`protontricks-launch --appid 284160 ng64helper.exe` does the same with Proton's own Wine. The game tells the helper
when its window has focus, so the controller works there too. If it still doesn't, check that Wine sees the pad
(`wine control joy.cpl` with the same `WINEPREFIX`) and try turning Steam Input off for BeamNG.

## What works (verified by `tests/uat_ingame.py` in the real game)

- Spawns and renders textured Mario; frames arrive live at 30 Hz.
- Stands on and walks over the map (terrain and static meshes), and jumps and runs with SM64 physics.
- No slowdown over long sessions: Mario is drawn as his rigid SM64 body parts (15 of them). Each part's shape is
  built into a mesh once and kept, and every rendered frame only moves the parts. A part is only rebuilt for a look
  it hasn't had before (blinking eyes, hand pose, cap). BeamNG never gets back the cost of drawing a rebuilt mesh,
  so the previous version, which rebuilt the whole of Mario about 60 times a second, made the game slower and slower
  (unplayable after about 30 minutes). `tests/soak.py` measures this.
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
- Landing on a car from a jump or a fall dents it where he lands, harder the faster he came down: a hop does
  nothing, a 4.5 m drop onto a pickup's roof adds about 3.7k damage. Ground pounds have their own, much bigger hit.
- Switching to another vehicle (TAB) leaves Mario in the world, standing where he was: your controller and the game
  camera go to the vehicle, and he still collides, can be run over, and so on. Switching back to him hands control
  back. If he was carrying something, he puts it down.
- Music: SM64's Bob-omb Battlefield theme, played by SM64's own music engine from your ROM, while you're
  playing as Mario (on by default). Back/View or M toggles it, with a toast in game. It fades out when you
  switch to another vehicle and comes back when you return. This uses a small libsm64 addition
  (`helper/patches/libsm64-music.patch`), because libsm64's own stop calls only act on music started through SM64's
  level queue.
- Camera: it swings round behind Mario as he runs away from or across the view, but not when he runs at it.
  There, "behind him" is 180 degrees away and flipped side to side with every wobble (the view lurched about), and
  turning bent his stick direction so he curved. Like SM64's camera, it now holds and backs up in front of him.
- Getting run over: SM64's own thrown knockback (the tumble from an explosion), launched along the car's travel
  and scaled by its speed. That car's collision sits out for 0.4 s so the throw always clears it (otherwise a car
  already pressed against him turned the throw into a bonk that left him on the car). Only the car's own speed toward Mario counts, so running or sliding into a parked car,
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

- **Map collision** is the map's real collision geometry: every colliding object near Mario (buildings, ramps,
  walls, rails, props, trees, rocks) is loaded from its shape's COLLADA collision mesh, placed exactly where
  BeamNG has it. "Visible Mesh" objects use their visible mesh, like BeamNG's physics does. Terrain is sampled on
  a grid (a heightfield, which sampling captures), and terrain-less levels use their ground plane. The map is
  streamed to the helper in 16 m cells: the 3x3 around Mario, each sent once when it comes into range and dropped
  when he's two cells away. (Re-sending the whole area every 8 m was up to 128k triangles at a time on Gridmap v2,
  a stutter every second or so while running.) Triangles are kept in FFI arrays so Lua's garbage collector never
  scans them, and the helper rebuilds its collision at most twice a second (about 15 ms). Shapes are
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

The mod's Lua, the helper's own C code, the installer and the server plugin are original to this project, and
nothing from the game ships with NG64: everything Nintendo-made is read at runtime from the player's own ROM -
Mario's model (skeleton, display lists, vertices, lights), textures, animations, HUD graphics and music. Only where
those are in the US ROM is written down.

libsm64 normally downloads Mario's model from the SM64 decompilation (`import-mario-geo.py`: `geo.inc.c`,
`model.inc.c`) and compiles it in. NG64 builds it without them (`helper/patches/libsm64-rommodel.patch`;
`build.sh` also deletes any downloaded copies) and translates the model from the ROM instead
(`helper/src/mario_rom_*.c`). Before the switch it was checked to be identical to libsm64's compiled-in version
(17 geo layouts, 506 display lists, 14,984 vertices).

libsm64 itself (CC0) is still built from the decompilation's reverse-engineered game code, as every libsm64
project is.

## Building

Needs MSYS2 with MinGW-w64 gcc (`C:\msys64`), libsm64's source in `libsm64-master/` (not committed), and Inno
Setup 6 for the installer (`winget install JRSoftware.InnoSetup`; `installer/ng64.iss`). Silent install, e.g. for
testing: `NG64-Setup.exe /VERYSILENT /ROM=<rom path> [/MODSDIR=<mods folder>]`.

```bash
./package.sh          # builds helper + dist/ng64.zip + dist/NG64/ + dist/NG64-Setup.exe
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
