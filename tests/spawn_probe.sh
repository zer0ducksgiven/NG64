#!/bin/bash
# Fresh game, then a level loaded with Mario as the player's vehicle (the level loader spawns him, as when you load a
# map with Mario selected). Reports where he ends up against the ground below him, a few seconds apart.
# Usage: tests/spawn_probe.sh [level] [runs]
cd "$(dirname "$0")/.."
LEVEL="${1:-west_coast_usa}"
RUNS="${2:-1}"
M="/c/msys64/usr/bin/python3 tests/mcp.py lua"
for run in $(seq 1 "$RUNS"); do
  powershell -Command "Get-Process BeamNG*, ng64helper -ErrorAction SilentlyContinue | Stop-Process -Force"
  sleep 3
  powershell -Command "Start-Process -FilePath '$(cygpath -w helper/dist/ng64helper.exe)' -ArgumentList '--rom \"F:\NG64\Super Mario 64 (USA).z64\"'; Start-Process -FilePath 'C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive\BeamNG.drive.exe' -ArgumentList '-enablemcp' -WorkingDirectory 'C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive'"
  for i in $(seq 1 100); do $M "return 'up'" 2>/dev/null | grep -q up && break; sleep 3; done
  $M "freeroam_freeroam.startFreeroamByName('$LEVEL', nil, nil, {'ng64_mario', {config='vehicles/ng64_mario/mario.pc'}}) return 'ok'" >/dev/null
  for i in $(seq 1 120); do $M "return tostring(ng64 and ng64.getStatus().active and ng64.getStatus().pos ~= nil)" 2>/dev/null | grep -q true && break; sleep 2; done
  for t in 0 3 8; do
    sleep $t
    echo -n "run $run +${t}s: "
    $M "local s=ng64.getStatus() local p=s.pos if not p then return 'no Mario' end
local top = p[3] + 60
local d = castRayStatic(vec3(p[1],p[2],top), vec3(0,0,-1), 200)
local ground = d < 200 and top - d or nil
local th = core_terrain and core_terrain.getTerrainHeight and core_terrain.getTerrainHeight(vec3(p[1],p[2],p[3]))
return string.format('Mario z %.2f | highest ground below 60 m up: %s | terrain %s | action %08x health %d', p[3], ground and string.format('%.2f', ground) or 'none', th and string.format('%.2f', th) or 'none', s.action or 0, s.health or 0)"
  done
done
