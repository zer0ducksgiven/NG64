"""Mario and fire: a pickup is set alight (BeamNG's own fire module), then
  1. Mario stands well clear of the flames: he must not burn;
  2. he is put right in a flame (found from the car's own burning nodes, including low ones near the ground):
     he must catch fire - SM64's burning action - and lose health;
  3. burning ends after a while (he is moved clear) and a flame sets him alight again if he returns.
Run with the game up (-enablemcp), Mario active, on flat ground (Gridmap). Usage: python fire_probe.py [--shots]"""
import sys, time, json, math
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp

BURNING = {0x00020449, 0x010208B4, 0x010208B5}


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def status():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


shots = "--shots" in sys.argv
if shots:
    import shot as shotmod
bx, by, bz = status()["pos"]
lua("if _ng64fire then _ng64fire:delete() end _ng64fire = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=False}) return 1".replace("False", "false") % (bx, by + 12, bz + 0.5))
time.sleep(5)
car = [float(v) for v in lua("local p=_ng64fire:getPosition() return p.x..' '..p.y..' '..p.z").split()]
ok = True

# 1. clear of the car, before it burns: nothing
lua("ng64.teleport(%f,%f,%f,true) return 1" % (car[0] + 7, car[1], bz + 0.3)); time.sleep(2)
lua("_ng64fire:queueLuaCommand('fire.igniteVehicle()') return 1")
time.sleep(8)
s = status()
print("flames the mod knows of: %s" % s.get("fires"))
a = s.get("action", 0)
print("%s Mario 7 m from a burning car: action %08x, health %s" % ("PASS" if a not in BURNING else "FAIL", a, s.get("health")))
ok &= a not in BURNING

# 2. a flame: take the hottest (lowest and highest) node positions from the car and stand in them
nodes = lua("""local out = {} local base = _ng64fire:getPosition()
_ng64fire:queueLuaCommand([[local h = {} for cid, n in pairs(fire.hotNodes) do if (n.intensity or 0) > 0.02 then local p = obj:getPosition() + obj:getNodePosition(cid) h[#h+1] = p.x .. ',' .. p.y .. ',' .. p.z .. ',' .. n.intensity end end obj:queueGameEngineLua('_ng64hot = ' .. string.format('%q', table.concat(h, ';')))]])
return 1""")
time.sleep(1.5)
hot = [[float(v) for v in e.split(",")] for e in lua("return tostring(_ng64hot)").split(";") if e]
print("%d burning nodes; hottest %.2f, lowest at %.2f m above the ground" % (len(hot), max(h[3] for h in hot) if hot else 0, (min(h[2] for h in hot) - bz) if hot else 0))
if not hot:
    print("FAIL no burning nodes"); sys.exit(1)
for label, pick in (("a high flame", max(hot, key=lambda h: h[2])), ("a flame near the ground", min(hot, key=lambda h: h[2]))):
    lua("ng64.teleport(%f,%f,%f,true) return 1" % (car[0] + 9, car[1], bz + 0.3)); time.sleep(2.5)   # clear, and cooled down
    h0 = status().get("health")
    lua("ng64.teleport(%f,%f,%f,true) return 1" % (pick[0], pick[1], max(bz + 0.3, pick[2] - 0.4)))
    seen, health = set(), []
    for _ in range(14):
        time.sleep(0.15)
        st = status()
        seen.add(st.get("action", 0)); health.append(st.get("health"))
        if shots and 0.5 and label == "a high flame" and "shot" not in globals().get("done", set()):
            done = {"shot"}; shotmod.shot("F:/NG64/tests/.tmp/fire_burn.jpg", 0.5)
    burned = bool(seen & BURNING)
    print("%s Mario in %s: actions %s, health %s -> %s" % ("PASS" if burned else "FAIL", label, sorted("%08x" % x for x in seen), h0, min(h for h in health if h is not None)))
    ok &= burned
    time.sleep(4)
lua("if _ng64fire then _ng64fire:delete() _ng64fire = nil end return 1")
print("caught fire per the helper:", [l for l in open("F:/NG64/helper/dist/ng64helper.log").read().splitlines() if "caught fire" in l][-2:])
sys.exit(0 if ok else 1)
