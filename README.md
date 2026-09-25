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
- Punches and kicks dent and shove cars. A ground pound on a roof crushes it.
- Stands on car roofs and rides moving cars (vehicles are moving collision boxes).
- Cars that hit him at speed knock him back and take health.
- SM64-style orbit camera with collision pull-in. Switching vehicles gives the normal camera back.
- BeamMP: his state goes to the server at 15 Hz, and other players' Marios are drawn and removed when they leave.

## Known limitations

- **Map collision is a raycast heightfield** (24 m square, 0.5 m grid) around Mario. Height steps over 0.7 m
  become walls. Overhangs are handled with a heuristic, so the interiors of complex buildings and multi-level
  structures are approximate. There is no water.
- **Vehicles are boxes** (their bounding box), so Mario stands at cab height over a pickup's bed.
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
  walls, punch hits, car roof).
- `tests/Harness/`: runs the BeamMP server plugin in the BeamMP Server Manager project's
  `BeamMpServerLuaHarness` (`dotnet test`).
- `tests/uat_ingame.py`: full in-game UAT through BeamNG's built-in MCP server. Launch BeamNG with
  `-enablemcp`, have the helper running, then run the script. It saves screenshots to `tests/shots/`.
