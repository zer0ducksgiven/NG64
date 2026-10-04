"""A spinning car that swings into terrain / a building must stop and drop. Finds a flat spot on Gridmap with a wall
3-4 m from it, stands Mario there with a car on the far side, and spins. Run with the game up, Mario active.
Usage: python terrain_spin_probe.py"""
import sys, time, json, math
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


spot = json.loads(lua("""
local s = ng64.getStatus() local bx, by, bz = s.pos[1], s.pos[2], s.pos[3]
for x = -400, 100, 6 do for y = -400, 100, 6 do
  local px, py = bx + x, by + y
  local g = 400 - castRayStatic(vec3(px, py, 400), vec3(0,0,-1), 800)
  if math.abs(g - bz) < 0.5 then
    local best, bang = 99, 0
    for a = 0, 15 do
      local ang = a * math.pi / 8
      local d = castRayStatic(vec3(px, py, g + 1.0), vec3(math.cos(ang), math.sin(ang), 0), 7)
      if d < best then best, bang = d, ang end
    end
    if best > 2.8 and best < 4.0 then return jsonEncode({px, py, g, bang, best}) end
  end
end end
return jsonEncode({})"""))
if not spot:
    print("no suitable wall found"); sys.exit(1)
px, py, pz, ang, d = spot
print("Mario at (%.1f, %.1f), wall %.1f m away towards %.0f deg" % (px, py, d, math.degrees(ang)))
ox, oy = -math.cos(ang), -math.sin(ang)       # away from the wall
lua("ng64.teleport(%f,%f,%f,true) return 1" % (px, py, pz + 0.3)); time.sleep(3)
lua("if _ng64tp then _ng64tp:delete() end _ng64tp = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(%f,%f,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (px + ox * 2.4, py + oy * 2.4, pz + 0.5, -oy, ox))
time.sleep(5)
lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f) return 1" % (ox, oy)); time.sleep(0.6)
lua("ng64.scriptInput(0,0,false,false,false,150,0,1,false,false,0,true) return 1")
time.sleep(6)
log = open("F:/NG64/helper/dist/ng64helper.log").read().splitlines()
for l in [l for l in log[-40:] if any(k in l for k in ("spin", "grabbed", "hit something", "put down"))][-3:]:
    print("helper:", l)
lua("if _ng64tp then _ng64tp:delete() _ng64tp = nil end return 1")
