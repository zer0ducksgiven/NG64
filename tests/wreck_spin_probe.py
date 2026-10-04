"""Spin throw on a wreck: a pickup with every breakgroup broken (wheels, doors and panels lying about it). Mario tries
spots round it until a tap lifts a LOOSE PART (the ordinary lift, nearest first), then at the same spot holds Y: he must
grab the car's main body to spin it, whatever lies nearer. Run with the game up (-enablemcp), Mario active, on flat
ground (Gridmap). Usage: python wreck_spin_probe.py"""
import sys, time, json, math, re
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def status():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


def helper_log():
    return open("F:/NG64/helper/dist/ng64helper.log").read().splitlines()


def newest(pattern, since):
    return [l for l in helper_log()[since:] if re.search(pattern, l)]


bx, by, bz = status()["pos"]
lua("if _ng64wr then _ng64wr:delete() end _ng64wr = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (bx, by + 8, bz + 0.5))
time.sleep(5)
lua("_ng64wr:queueLuaCommand('beamstate.breakAllBreakgroups()') return 1")
time.sleep(5)
cx, cy = [float(v) for v in lua("local p=_ng64wr:getPosition() return p.x..' '..p.y").split()]
print("hull:", [l for l in helper_log() if "hull:" in l][-1])
found = None
for k in range(16):
    ang = k * math.pi / 8
    for rad in (1.6, 2.4, 3.2):
        sx, sy = cx + math.cos(ang) * rad, cy + math.sin(ang) * rad
        mark = len(helper_log())
        lua("ng64.teleport(%f,%f,%f,true) return 1" % (sx, sy, bz + 0.3)); time.sleep(1.2)
        face = (cx - sx, cy - sy); d = math.hypot(*face)
        lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f) return 1" % (face[0] / d, face[1] / d)); time.sleep(0.4)
        lua("ng64.scriptInput(0,0,false,false,false,2,%f,%f,true) return 1" % (face[0] / d, face[1] / d)); time.sleep(1.5)
        got = newest(r"picked up vehicle \d+ piece (\d+)", mark)
        if got:
            piece = int(re.search(r"piece (\d+)", got[-1]).group(1))
            lua("ng64.scriptInput(0,0,false,false,true,3) return 1"); time.sleep(3)   # Z puts it down
            if piece != 0:
                found = (sx, sy, face, piece); break
            lua("ng64.scriptInput(0,0,false,false,true,3) return 1"); time.sleep(2)
    if found: break
if not found:
    print("no spot where a loose part was nearest"); sys.exit(1)
sx, sy, face, piece = found
print("a tap at (%.1f, %.1f) lifts loose piece %d" % (sx, sy, piece))
lua("ng64.teleport(%f,%f,%f,true) return 1" % (sx, sy, bz + 0.3)); time.sleep(1.5)
d = math.hypot(*face)
lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f) return 1" % (face[0] / d, face[1] / d)); time.sleep(0.4)
mark = len(helper_log())
lua("ng64.scriptInput(0,0,false,false,false,150,%f,%f,false,false,0,true) return 1" % (face[0] / d, face[1] / d)); time.sleep(4.5)
print("hold at the same spot:")
for l in newest(r"picked up|grabbed|spin|nothing in reach", mark)[:4]: print("   helper:", l)
time.sleep(3)
lua("if _ng64wr then _ng64wr:delete() _ng64wr = nil end return 1")
