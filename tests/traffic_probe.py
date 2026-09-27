"""AI traffic vs Mario: puts Mario standing in a road lane, spawns a car ~60 m back up the road and has the AI drive it
along the road through where he stands. Reports whether the car sees him (its object list), how close it got, how
slow it was going then, and whether Mario was hit. Run with the game up (-enablemcp) and Mario active, on a map with
roads (West Coast USA). Usage: python traffic_probe.py [back_m] [lane_offset_m] [--baseline]"""
import sys, time, json, math
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


back = float(sys.argv[1]) if len(sys.argv) > 1 else 60
lane = float(sys.argv[2]) if len(sys.argv) > 2 else 1.8     # metres right of the road's centre line
baseline = "--baseline" in sys.argv   # hide Mario from traffic, to compare
# a straight-ish stretch of road near Mario: node A (car start), B (Mario), C (the car's target), along one road
plan = json.loads(lua("""
local s = ng64.getStatus() local p = vec3(s.pos[1], s.pos[2], s.pos[3])
local m = map.getMap() local n1, n2 = map.findClosestRoad(p)
local best
for name, node in pairs(m.nodes) do
  local d = (node.pos - p):length()
  if d > 40 and d < 400 then
    for lnk, _ in pairs(node.links) do
      local other = m.nodes[lnk]
      if other then
        local seg = other.pos - node.pos
        local len = seg:length()
        if len > %f + 30 and (not best or d < best.d) then best = { d = d, a = node.pos, c = other.pos, len = len } end
      end
    end
  end
end
if not best then return jsonEncode({}) end
local dir = (best.c - best.a):normalized()
local b = best.a + dir * %f + vec3(dir.y, -dir.x, 0) * %f
return jsonEncode({ a = { best.a.x, best.a.y, best.a.z }, b = { b.x, b.y, b.z }, c = { best.c.x, best.c.y, best.c.z }, len = best.len })
""" % (back, back, lane)))
if not plan:
    print("no long enough road segment found near Mario")
    sys.exit(1)
a, b, c = plan["a"], plan["b"], plan["c"]
print("road segment %.0f m; car starts %.0f m before Mario, who stands %.1f m right of the centre line%s" % (plan["len"], back, lane, " (hidden from traffic: baseline)" if baseline else ""))
if baseline:
    lua("_G.ng64RealTempObjectData = _G.ng64RealTempObjectData or map.tempObjectData map.tempObjectData = function() end return 1")
lua("ng64.teleport(%f,%f,%f,true)" % (b[0], b[1], b[2] + 0.5))
time.sleep(3)
lua("ng64.scriptInput(0,0,false,false,false,5)")
# the car: facing along the road, at A, told to drive to C
lua("""
local a, c = vec3(%f,%f,%f), vec3(%f,%f,%f)
local dir = (c - a):normalized()
local rot = quatFromDir(dir, vec3(0,0,1))
local v = core_vehicles.spawnNewVehicle('covet', { pos = a + vec3(0,0,0.5), rot = rot, autoEnterVehicle = false })
_G.ng64TrafficCar = v:getID()
return 1""" % (a[0], a[1], a[2], c[0], c[1], c[2]))
time.sleep(4)
cid = lua("return tostring(_G.ng64TrafficCar)")
stub = lua("return tostring(ng64.getStatus().stubId)")
lua("""local v = be:getObjectByID(%s)
v:queueLuaCommand('ai.setMode("traffic") ai.setSpeedMode("legal")')
return 1""" % cid)
hurts0 = json.loads(lua("return jsonEncode(ng64.getStatus())")).get("hurts", 0)
lua("_G.ng64SeesMario = nil be:getObjectByID(%s):queueLuaCommand('obj:queueGameEngineLua(\"_G.ng64SeesMario = \" .. tostring(mapmgr.getObjects()[%s] ~= nil))')" % (cid, stub))
closest, speedAtClosest, t0 = 1e9, 0, time.time()
while time.time() - t0 < 25:
    r = lua("""local v = be:getObjectByID(%s) local s = ng64.getStatus()
local cp, mp = v:getPosition(), vec3(s.pos[1], s.pos[2], s.pos[3])
return string.format('%%f %%f', (cp - mp):length(), v:getVelocity():length())""" % cid).split()
    d, spd = float(r[0]), float(r[1])
    if d < closest: closest, speedAtClosest = d, spd
    time.sleep(0.2)
st = json.loads(lua("return jsonEncode(ng64.getStatus())"))
print("car's AI sees Mario in its object list: %s" % lua("return tostring(_G.ng64SeesMario)"))
print("closest approach %.1f m (car at %.1f m/s then); Mario hit %d time(s)" % (closest, speedAtClosest, st.get("hurts", 0) - hurts0))
lua("local v = be:getObjectByID(%s) if v then v:delete() end return 1" % cid)
if baseline:
    lua("if _G.ng64RealTempObjectData then map.tempObjectData = _G.ng64RealTempObjectData end return 1")
