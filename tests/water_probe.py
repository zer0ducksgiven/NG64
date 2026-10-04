"""Mario swims in BeamNG's own water: dropped into a lake (a WaterBlock) and out over the ocean (a WaterPlane) on West
Coast USA he must end up in SM64's water idle (action 0x380022C0) at the surface, not stand on the bed or sink.
Run with the game up (-enablemcp) on west_coast_usa and Mario active. Usage: python water_probe.py"""
import sys, time, json
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def status():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


fails = 0
# (name, x, y, drop height, the surface BeamNG draws there)
for name, x, y, z0, surface in (("lake (WaterBlock)", 654.6, -817.6, 172, 162.2), ("ocean (WaterPlane)", 0, 1000, 90, 70.7)):
    lua("ng64.teleport(%f,%f,%f,true) return 1" % (x, y, z0))
    time.sleep(9)
    s = status()
    z, act = s["pos"][2], s.get("action", 0)
    ok = act == 0x380022C0 and surface - 1.5 < z < surface
    print("%s %s: action %08x, feet at %.2f (surface %.1f)" % ("PASS" if ok else "FAIL", name, act, z, surface))
    fails += not ok
sys.exit(1 if fails else 0)
