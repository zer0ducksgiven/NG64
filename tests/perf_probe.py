"""Frame cost of playing as Mario, on a big map (West Coast USA): average fps, worst frame and the engine's own
per-frame Lua / render times, for a plain car (baseline), Mario standing, Mario running, each without and with AI
traffic. Needs tests/ng64perf.lua copied to <user folder>/lua/ge/extensions (a frame counter), the game up
(-enablemcp) and Mario active. Usage: python perf_probe.py [seconds per phase] [traffic cars]"""
import sys, time, json
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12
traffic = int(sys.argv[2]) if len(sys.argv) > 2 else 8
lua("extensions.load('ng64perf') return 1")


def measure(label):
    lua("return ng64perf.take()")
    keys = ("Lua", "LuaPre", "LuaGC", "Render", "OtherRender", "PhysicsAndVLUA")
    acc = dict.fromkeys(keys, 0.0)
    n = 0
    end = time.time() + secs
    while time.time() < end:
        m = json.loads("".join(c.get("text", "") for c in mcp.call("get_performance_metrics", {})["result"]["content"]))
        for k in keys: acc[k] += m.get(k, 0)
        n += 1
        time.sleep(0.25)
    frames, total, worst, spikes = lua("return ng64perf.take()").split()
    frames, total, worst, spikes = int(frames), float(total), float(worst), int(spikes)
    print("%-26s %5.1f fps  worst %4.0f ms  >50ms x%-3d | Lua %.2f  LuaPre %.2f  GC %.2f  Render %.2f  OtherRender %.2f ms" % (
        label, frames / max(total, 1e-6), worst * 1000, spikes, acc["Lua"] / n, acc["LuaPre"] / n, acc["LuaGC"] / n, acc["Render"] / n, acc["OtherRender"] / n), flush=True)


def settle(t=4): time.sleep(t)


spot = lua("local s = ng64.getStatus() return string.format('%f %f %f', s.pos[1], s.pos[2], s.pos[3])").split()
spot = [float(v) for v in spot]
for with_traffic in (False, True):
    suffix = " + %d traffic" % traffic if with_traffic else ""
    if with_traffic:
        lua("gameplay_traffic.setupTraffic(%d, {}) return 1" % traffic)
        time.sleep(25)
    # Mario standing, then running
    lua("ng64.teleport(%f,%f,%f,true) return 1" % tuple(spot)); settle()
    measure("Mario standing" + suffix)
    lua("ng64.scriptInput(0,-1,false,false,false,%d,0,1) return 1" % int(secs * 30 + 60)); settle(2)
    measure("Mario running" + suffix)
    # a plain car in the same place, for comparison
    lua("core_vehicles.replaceVehicle('pickup', {}) return 1"); settle(8)
    measure("plain car" + suffix)
    lua("core_vehicles.replaceVehicle('ng64_mario', {config='vehicles/ng64_mario/mario.pc'}) return 1"); settle(10)
    if with_traffic: lua("gameplay_traffic.deactivate(true) gameplay_traffic.removeTraffic() return 1")
