"""Runs Mario in a straight line and measures how evenly his mesh and the camera move per rendered frame.
Reports: frames dropped by the network path, and speed jitter (std/mean of per-frame speed) for mesh and camera."""
import sys, time, json, statistics
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp
def lua(c): return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])
st0 = json.loads(lua("return jsonEncode(ng64.getStatus())"))
lua("ng64.scriptInput(0,-1,false,false,false,150,1,0)")
time.sleep(1.5)                       # reach full running speed
lua("ng64.startTrace()")
time.sleep(2.5)
rows = [list(map(float, l.split())) for l in lua("return ng64.getTrace()").strip().splitlines() if l.strip()]
st1 = json.loads(lua("return jsonEncode(ng64.getStatus())"))
started = st1["framesStarted"] - st0["framesStarted"]; done = st1["framesCompleted"] - st0["framesCompleted"]
def jitter(ix):
    sp = []
    for a, b in zip(rows, rows[1:]):
        dt = b[0] - a[0]
        if dt <= 0: continue
        d = ((b[ix] - a[ix]) ** 2 + (b[ix + 1] - a[ix + 1]) ** 2) ** 0.5
        sp.append(d / dt)
    m = statistics.mean(sp)
    stalls = sum(1 for v in sp if v < 0.25 * m)
    return m, statistics.pstdev(sp) / m, stalls, len(sp)
dts = [b[0] - a[0] for a, b in zip(rows, rows[1:])]
print("render frames %d, mean %.1f fps (dt jitter %.2f)" % (len(rows), 1 / statistics.mean(dts), statistics.pstdev(dts) / statistics.mean(dts)))
print("mario frames: %d started, %d complete (%.1f%% dropped)" % (started, done, 100 * (1 - done / max(1, started))))
for name, ix in (("mesh", 2), ("camera", 5)):
    m, j, stalls, n = jitter(ix)
    print("%-6s speed %.2f m/s, jitter %.2f, frames where it barely moved: %d of %d" % (name, m, j, stalls, n))
if "--clock" in sys.argv:
    for r in rows[:45]:
        print("t %.3f dt %.3f alpha %.2f renderT-cur %.3f cur.t %.3f" % (r[0], r[1], r[8], r[9], r[10]))
