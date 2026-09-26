"""Runs Mario across the map for a while and reports stutters: every rendered frame over 30 ms, with how long each
part of NG64's per-frame work took in it and what it was doing (world streaming, terrain grid, part builds).
Usage: python traverse_probe.py [seconds] [dirX dirY]"""
import sys, time, json
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40
dx, dy = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else (1, 0)
lua("ng64.clearHitches() return 1")
t0 = time.time()
while time.time() - t0 < secs:
    lua("ng64.scriptInput(0,-1,false,false,false,120,%f,%f) return 1" % (dx, dy))
    time.sleep(3.5)
lua("ng64.scriptInput(0,0,false,false,false,5) return 1")
all_ = json.loads(lua("return jsonEncode(ng64.getHitches())") or "{}")
hs = all_.get("frames") or []
gaps = all_.get("poseGaps") or []
st = json.loads(lua("return jsonEncode(ng64.getStatus())"))
print("%d frames over 30 ms in %.0f s; Mario ended at %s" % (len(hs), secs, st.get("pos")))
prev = None
for h in hs:
    sec = h.get("sections") or {}
    top = sorted(sec.items(), key=lambda kv: -kv[1])[:3]
    w = h.get("world") or {}
    print("  t %7.2f  frame %5.0f ms | Lua %4.0f MB, freed %5.1f MB | NG64: %s | builds %s grids %s worldTris %s parsed %s pending %s" % (
        h["t"], h["dt"] * 1000, h.get("luaMB", 0), h.get("gcFreedMB", 0), ", ".join("%s %.1f ms" % (k, v * 1000) for k, v in top) or "-",
        h.get("builds"), h.get("grids"), h.get("worldTris"), w.get("shapesParsed"), w.get("shapesPending")))
print("%d gaps over 100 ms between Mario's poses arriving" % len(gaps))
for g in gaps:
    print("  t %7.2f  gap %5.0f ms (%s ticks)" % (g["t"], g["gap"] * 1000, g["ticks"]))
