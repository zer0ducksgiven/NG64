"""A pickup is shoved at ~9 m/s into Mario standing 7 m away; prints his action changes and where he ends up.
Usage: python runover_probe.py"""
import sys, time, json
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def st():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


mx, my, mz = st()["pos"]
lua("if _pc then _pc:delete() end _pc = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 'ok'" % (mx + 6, my, mz + 0.5))
time.sleep(6)
cx, cy, cz = [float(v) for v in lua("local c=vec3(be:getObjectOOBBCenterXYZ(_pc:getID())) return string.format('%f %f %f',c.x,c.y,c.z)").split()]
if "--crushed" in sys.argv:
    # the UAT's order: ground-pound the roof first
    lua("ng64.teleport(%f,%f,%f)" % (cx, cy, cz + 3.5)); time.sleep(0.35)
    lua("ng64.scriptInput(0,0,false,false,true,6)"); time.sleep(2.5)
    cx, cy, cz = [float(v) for v in lua("local c=vec3(be:getObjectOOBBCenterXYZ(_pc:getID())) return string.format('%f %f %f',c.x,c.y,c.z)").split()]
lua("ng64.teleport(%f,%f,%f)" % (cx + 7, cy, mz + 0.2))
time.sleep(1.5)
lua("_pc:queueLuaCommand(\"if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,1,0,0,9)\")" % (cx - 200, cy, cz))
seen, last, t0 = [], None, time.time()
while time.time() - t0 < 3:
    s = st()
    a = hex(s["action"])
    if a != last:
        seen.append("%.2fs %s" % (time.time() - t0, a))
        last = a
    time.sleep(0.02)
p = st()["pos"]
c = [float(v) for v in lua("local c=vec3(be:getObjectOOBBCenterXYZ(_pc:getID())) return string.format('%f %f %f',c.x,c.y,c.z)").split()]
print("actions:", " -> ".join(seen))
print("thrown: %s | Mario ends %.1f m ahead of the car centre, %.2f m above it" % ("0x10208be" in " ".join(seen), p[0] - c[0], p[2] - c[2]))
