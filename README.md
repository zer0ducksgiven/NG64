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
2. Put `ng64helper.exe` anywhere, plus your ROM next to it as `sm64.us.z64` (or put the ROM's full path in
   `rom.txt` next to the exe, or pass `--rom <path>`).
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

Input is only read while the BeamNG window has focus. To stop playing as Mario, switch to any other vehicle.

## What works (verified by `tests/uat_ingame.py` in the real game)

- Spawns and renders textured Mario; frames arrive live at 30 Hz.
- Stands on and walks over the map (terrain and static meshes), and jumps and runs with SM64 physics.
- No flicker: Mario's mesh is triple-buffered and rebuilt at most once per rendered frame. `tests/flicker_capture.ps1` grabs the game window off the desktop
  and finds Mario in 80/80 grabs, where the single-mesh version missed him in 22/60.
- Attacks dent and shove cars. On a stock Gavril D-Series the damage is roughly: punch 4.9k (the door buckles),
  slide kick 10k, dive 25-47k, ground pound 150k (the roof caves in).
- Cars collide as their real shape: each car rasterizes its own collision triangles (its skin) into a height grid
  (the hull), so Mario lands in a pickup's bed, below the cab roof, and can't drop through a roof between nodes.
  The hull is smooth over gentle changes, so only real edges are ledges to grab, and it's sealed down to the
  underside. If Mario still ends up inside a car (it drove into him, a bad landing), he's pushed out through the
  nearest side, or up onto the top if that's a short hop.
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

- **Map collision is a raycast heightfield** (24 m square, 0.5 m grid) around Mario. Gentle ground is smooth.
  Steps over 0.35 m become flat tiles with a vertical wall between them. Overhangs are handled with a heuristic,
  so the interiors of complex buildings and multi-level structures are approximate. Anything thinner than the
  grid (poles, fences) can be missed. There is no water.
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
- `tests/uat_ingame.py`: full in-game UAT through BeamNG's built-in MCP server. Launch BeamNG with
  `-enablemcp`, have the helper running, then run the script. It saves screenshots to `tests/shots/`. The
  dive check depends on timing: SM64 only dives if Mario is past speed 28 when B lands, otherwise it's a jump
  kick that fires too early to reach the car, and the test's input timing over MCP isn't frame-exact.
  `tests/probe_attack.py` runs a single attack against a fresh car for tuning.
