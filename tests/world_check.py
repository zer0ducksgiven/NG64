"""Compares Mario's collision world with BeamNG's own: at random points around a busy spot, the floor SM64 finds
under a point vs where BeamNG's raycast hits. Run with the game up (-enablemcp), Mario spawned.
Usage: python world_check.py [points] [--here]"""
import sys, time, json, random
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


N = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 600
if "--here" not in sys.argv:
    # the densest cluster of colliding objects: most object centres within 25 m of one of 300 sampled centres
    spot = lua("""
local names = scenetree.findClassObjects('TSStatic') local pts = {}
for _, n in ipairs(names) do local o = scenetree.findObject(n)
  if o and o:getField('collisionType',0) ~= 'None' then local b = o:getWorldBox() local c = (b.minExtents + b.maxExtents) * 0.5
    local sz = (b.maxExtents - b.minExtents):length() if sz > 1 and sz < 60 then pts[#pts+1] = c end end end
local best, bc = -1, nil
for k = 1, math.min(300, #pts) do local c = pts[math.random(#pts)] local cnt = 0
  for _, q in ipairs(pts) do if (q - c):length() < 25 then cnt = cnt + 1 end end
  if cnt > best then best, bc = cnt, c end end
local g = bc.z - castRayStatic(bc + vec3(0,0,30), vec3(0,0,-1), 200) + 30
return string.format('%f %f %f %d', bc.x, bc.y, g, best)""").split()
    sx, sy, sz, cnt = float(spot[0]), float(spot[1]), float(spot[2]), int(spot[3])
    print("busiest spot (%.1f, %.1f, %.1f) with %d objects within 25 m" % (sx, sy, sz, cnt))
    lua("ng64.teleport(%f,%f,%f)" % (sx, sy, sz + 1.0))
    time.sleep(3)
st = json.loads(lua("return jsonEncode(ng64.getStatus())"))
cx, cy, cz = st["pos"]
print("Mario at (%.1f, %.1f, %.1f); world region %s triangles" % (cx, cy, cz, st.get("worldTris")))

random.seed(1)
pts = [(cx + random.uniform(-11, 11), cy + random.uniform(-11, 11), cz + 20) for _ in range(N)]   # inside the terrain grid (+-12 m)
beam = lua("local out = {} for _, p in ipairs(%s) do local d = castRayStatic(vec3(p[1],p[2],p[3]), vec3(0,0,-1), 200) out[#out+1] = d < 200 and string.format('%%.3f', p[3]-d) or 'n' end return table.concat(out, ' ')"
           % ("{" + ",".join("{%f,%f,%f}" % p for p in pts) + "}")).split()
sm = []
for i in range(0, N, 500):
    chunk = pts[i:i + 500]
    lua("ng64.floorQuery(%s)" % ("{" + ",".join("{%f,%f,%f}" % p for p in chunk) + "}"))
    got = None
    for _ in range(40):
        time.sleep(0.05)
        r = lua("local r = ng64.getFloorReply() return r and table.concat((function() local t = {} for i, v in ipairs(r) do t[i] = (v ~= v) and 'n' or string.format('%.3f', v) end return t end)(), ' ') or ''")
        if r.strip():
            got = r.split()
            break
    sm += got or ["n"] * len(chunk)

agree = off = missing = extra = none = 0
worst = []
for p, b, s in zip(pts, beam, sm):
    if b == "n" and s == "n": none += 1
    elif b == "n": extra += 1
    elif s == "n": missing += 1; worst.append((99, p, b, s))
    else:
        d = abs(float(b) - float(s))
        if d <= 0.15: agree += 1
        else: off += 1; worst.append((d, p, b, s))
total = N - none
print("points with ground: %d | agree within 15 cm: %d (%.1f%%) | differ: %d | no SM64 floor: %d | SM64 floor, BeamNG none: %d"
      % (total, agree, 100.0 * agree / max(1, total), off, missing, extra))
for d, p, b, s in sorted(worst, key=lambda w: -w[0])[:8]:
    print("  at (%.1f, %.1f): BeamNG %s, SM64 %s" % (p[0], p[1], b, s))
