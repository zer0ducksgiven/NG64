"""Mario scrambling around a busy part of the map: random running, jumping, diving, ground pounds for a while,
sampled 10x a second against BeamNG's own geometry. Counts:
  sunk      - his feet more than 20 cm below BeamNG's floor under him (clipped into / fell through something)
  fell out  - more than 1 m below the terrain (out of the world)
  mismatch  - SM64's floor under him vs BeamNG's differ by more than 25 cm
Usage: python clip_probe.py [seconds] [--here]"""
import sys, time, json, random
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def status():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


secs = float(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].replace(".", "").isdigit() else 45
if "--here" not in sys.argv:
    spot = lua("""
local names = scenetree.findClassObjects('TSStatic') local pts = {}
for _, n in ipairs(names) do local o = scenetree.findObject(n)
  if o and o:getField('collisionType',0) ~= 'None' then local b = o:getWorldBox() local c = (b.minExtents + b.maxExtents) * 0.5
    local sz = (b.maxExtents - b.minExtents):length() if sz > 1 and sz < 60 then pts[#pts+1] = c end end end
math.randomseed(7)
local best, bc = -1, nil
for k = 1, math.min(300, #pts) do local c = pts[math.random(#pts)] local cnt = 0
  for _, q in ipairs(pts) do if (q - c):length() < 25 then cnt = cnt + 1 end end
  if cnt > best then best, bc = cnt, c end end
local g = bc.z - castRayStatic(bc + vec3(0,0,30), vec3(0,0,-1), 200) + 30
return string.format('%f %f %f', bc.x, bc.y, g)""").split()
    lua("ng64.teleport(%s,%s,%f)" % (spot[0], spot[1], float(spot[2]) + 1))
    time.sleep(4)

start = status()["pos"]
random.seed(3)
samples = []
t_end = time.time() + secs
next_input = 0
while time.time() < t_end:
    if time.time() >= next_input:
        a = random.random() < 0.6
        b = random.random() < 0.12
        z = random.random() < 0.08
        ang = random.uniform(0, 6.283)
        import math
        lua("ng64.scriptInput(0,-1,%s,%s,%s,40,%f,%f)" % ("true" if a else "false", "true" if b else "false", "true" if z else "false", math.cos(ang), math.sin(ang)))
        next_input = time.time() + random.uniform(0.6, 1.6)
    samples.append(status()["pos"])
    time.sleep(0.1)

# BeamNG's floor under each sample (ray from 1.2 m above his feet), and SM64's (floor query at the same points)
pts = "{" + ",".join("{%f,%f,%f}" % (p[0], p[1], p[2]) for p in samples) + "}"
beam = lua("local out = {} for _, p in ipairs(%s) do local d = castRayStatic(vec3(p[1],p[2],p[3]+1.2), vec3(0,0,-1), 6) out[#out+1] = d < 6 and string.format('%%.3f', p[3]+1.2-d) or 'n' end return table.concat(out, ' ')" % pts).split()
lua("if ng64.floorQuery then ng64.floorQuery(%s) end" % "{" + ",".join("{%f,%f,%f}" % (p[0], p[1], p[2] + 0.3) for p in samples) + "}")
sm = None
for _ in range(60):
    time.sleep(0.05)
    r = lua("local r = ng64.getFloorReply and ng64.getFloorReply() return r and table.concat((function() local t = {} for i, v in ipairs(r) do t[i] = (v ~= v) and 'n' or string.format('%.3f', v) end return t end)(), ' ') or ''")
    if r.strip():
        sm = r.split()
        break
sm = sm or ["n"] * len(samples)

terr = lua("local out = {} for _, p in ipairs(%s) do local h = core_terrain.getTerrainHeight(vec3(p[1],p[2],p[3])) out[#out+1] = h and string.format('%%.3f', h) or 'n' end return table.concat(out, ' ')" % pts).split()
# inside a solid object? (exact collision triangles; the mesh reader is pushed into the game so this also runs
# against builds that predate it)
base = __file__.rsplit("/", 1)[0] + "/../mod/lua/ge/extensions/"
for mod in ("ng64Dae", "ng64World"):
    code = open(base + mod + ".lua").read()
    mcp.call("run_lua", {"code": "package.loaded['ge/extensions/%s'] = package.loaded['ge/extensions/%s'] or loadstring([==[" % (mod, mod) + code + "]==])() return 1"})
lua("_insideSamples = {%s}" % ",".join("{%f,%f,%f}" % (p[0], p[1], p[2] + 0.7) for p in samples))
inside = lua(open(__file__.rsplit("/", 1)[0] + "/inside_check.lua").read()).split()
if len(inside) != len(samples):
    print("inside check failed:", " ".join(inside)[:200]); inside = ["0"] * len(samples)
examples = []
sunk = fell = mism = 0
ins = sum(1 for v in inside if v == "1")
for p, v in zip(samples, inside):
    if v == "1": examples.append(("inside", p, "-", "-"))
for p, b, s, t in zip(samples, beam, sm, terr):
    if t != "n" and p[2] < float(t) - 1: fell += 1; examples.append(("fell out", p, b, s))
    if b != "n" and p[2] < float(b) - 0.2:
        sunk += 1; examples.append(("sunk", p, b, s))
    if b != "n" and s != "n" and abs(float(b) - float(s)) > 0.25:
        mism += 1; examples.append(("mismatch", p, b, s))
span = max(max(p[2] for p in samples) - min(p[2] for p in samples), 0)
print("samples %d over %.0f s, height range %.1f m | INSIDE objects %d | sunk %d | fell out %d | floor mismatch %d" % (len(samples), secs, span, ins, sunk, fell, mism))
for kind, p, b, s in examples[:8]:
    print("  %s at (%.1f, %.1f, %.2f): BeamNG floor %s, SM64 floor %s" % (kind, p[0], p[1], p[2], b, s))
