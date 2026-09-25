"""Debug helper: spawn a fresh pickup next to Mario, do one scripted attack, print what happened.
Usage: python probe_attack.py <punch|dive|slide> [approach_m]"""
import sys, time, json
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp
def lua(c):
    r = mcp.call("run_lua", {"code": c})
    return "".join(x.get("text", "") for x in r.get("result", {}).get("content", []))
tune = float(sys.argv[3]) if len(sys.argv) > 3 else None
kind = sys.argv[1]; approach = float(sys.argv[2]) if len(sys.argv) > 2 and float(sys.argv[2]) > 0 else {"punch": 0.45, "dive": 2.2, "slide": 2.6}[kind]
st = json.loads(lua("return jsonEncode(ng64.getStatus())"))
mx, my, mz = st["pos"]
lua("if _probeCar then _probeCar:delete() end _probeCar = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 'ok'" % (mx + 6, my, mz + 0.5))
time.sleep(5)
if tune: lua("_probeCar:queueLuaCommand(\"if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.tune(%f)\")" % tune)
g = lua("""local id=_probeCar:getID() local c=vec3(be:getObjectOOBBCenterXYZ(id)) local best,bl
for i=0,2 do local a=vec3(be:getObjectOOBBHalfAxisXYZ(id,i)) if math.abs(a.z)<0.5*a:length() and (not bl or a:length()<bl) then best,bl=a,a:length() end end
local n=best/bl return string.format('%f %f %f %f %f', c.x,c.y,n.x,n.y,bl)""").split()
cx, cy, nx, ny, half = map(float, g)
lua("ng64.teleport(%f,%f,%f)" % (cx + nx * (half + approach), cy + ny * (half + approach), mz + 0.2))
time.sleep(0.8)
lua("ng64.scriptInput(0,-0.2,false,false,false,3,%f,%f)" % (-nx, -ny)); time.sleep(0.4)
d0 = float(lua("return tostring(map.objects[_probeCar:getID()].damage)"))
seqs = {"punch": [(0, 0, 1, 0, 4, 0.1)],
        "dive": [(-1, 0, 0, 0, 40, 1.3), (-1, 1, 0, 0, 3, 0.1), (-1, 0, 1, 0, 4, 0.1)],
        "slide": [(-1, 0, 0, 0, 26, 0.8), (-1, 0, 0, 1, 3, 0.1), (-1, 0, 1, 1, 4, 0.1)]}[kind]
acts = []
for stick, a, b, z, fr, wait in seqs:
    lua("ng64.scriptInput(0,%f,%s,%s,%s,%d,%f,%f)" % (stick, "true" if a else "false", "true" if b else "false", "true" if z else "false", fr, -nx, -ny))
    t_end = time.time() + wait
    while time.time() < t_end:
        acts.append(hex(json.loads(lua("return jsonEncode(ng64.getStatus())"))["action"] or 0)); time.sleep(0.03)
for _ in range(20):
    acts.append(hex(json.loads(lua("return jsonEncode(ng64.getStatus())"))["action"] or 0)); time.sleep(0.05)
time.sleep(1.5)
d1 = float(lua("return tostring(map.objects[_probeCar:getID()].damage)"))
print("actions:", " ".join(dict.fromkeys(acts)))
print("damage %.0f -> %.0f" % (d0, d1))
