#!/bin/bash
# (Re)launches helper + BeamNG (-enablemcp), loads Gridmap v2 and spawns Mario. Usage: tests/setup_mario.sh [helper.exe]
cd "$(dirname "$0")/.."
HELPER="${1:-helper/dist/ng64helper.exe}"
powershell -Command "Get-Process BeamNG*, ng64helper -ErrorAction SilentlyContinue | Stop-Process -Force"
sleep 3
powershell -Command "Start-Process -FilePath '$(cygpath -w "$HELPER")' -ArgumentList '--rom \"F:\NG64\Super Mario 64 (USA).z64\"' -WindowStyle Minimized; Start-Process -FilePath 'C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive\BeamNG.drive.exe' -ArgumentList '-enablemcp' -WorkingDirectory 'C:\Program Files (x86)\Steam\steamapps\common\BeamNG.drive'"
M="/c/msys64/usr/bin/python3 tests/mcp.py lua"
for i in $(seq 1 100); do $M "return 'up'" 2>/dev/null | grep -q up && break; sleep 3; done
$M "freeroam_freeroam.startFreeroamByName('${LEVEL:-gridmap_v2}') return 'ok'" >/dev/null
for i in $(seq 1 80); do $M "return tostring(core_gamestate.state and core_gamestate.state.state == 'freeroam' and getPlayerVehicle(0) ~= nil)" 2>/dev/null | grep -q true && break; sleep 3; done
sleep 3
$M "core_vehicles.replaceVehicle('ng64_mario', {config='vehicles/ng64_mario/mario.pc'}) return 'ok'" >/dev/null
sleep 5
$M "return jsonEncode(ng64.getStatus())"
