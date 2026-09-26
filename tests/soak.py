"""Leaves the game running and samples, once a minute: BeamNG's memory (private bytes), the helper's memory, Lua
memory, BeamNG's frame timings, and Mario's mesh rebuild count. Usage: python soak.py <minutes> [label]"""
import sys, time, json, subprocess
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp

PS = "/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
NOT_TIMINGS = ("physAvailable", "physUsed", "virtUsed", "virtAvailable", "WaitNextFrame", "FPSLimiter")


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def proc_mb(name):
    out = subprocess.run([PS, "-Command", "(Get-Process %s -ErrorAction SilentlyContinue | Measure-Object PrivateMemorySize64 -Sum).Sum" % name],
                         capture_output=True, text=True).stdout.strip()
    try:
        return int(out) / 1e6
    except ValueError:
        return 0


minutes = float(sys.argv[1])
label = sys.argv[2] if len(sys.argv) > 2 else ""
print("%s  min | BeamNG MB | helper MB | Lua MB | frame ms | Lua ms | render ms | otherRender | meshBuilds" % label, flush=True)
t0 = time.time()
while True:
    m = (time.time() - t0) / 60
    luamb = float(lua("return tostring(collectgarbage('count') / 1024)"))
    txt = "".join(x.get("text", "") for x in mcp.call("get_performance_metrics", {}).get("result", {}).get("content", []))
    try:
        pm = json.loads(txt)
    except ValueError:
        pm = {}
    frame_ms = sum(v for k, v in pm.items() if isinstance(v, (int, float)) and k not in NOT_TIMINGS)
    st = json.loads(lua("return jsonEncode(ng64 and ng64.getStatus() or {})"))
    print("%5.1f | %9.0f | %9.1f | %6.1f | %8.1f | %6.2f | %9.2f | %11.2f | %s" % (m, proc_mb("BeamNG.drive.x64"), proc_mb("ng64helper"), luamb,
          frame_ms, pm.get("Lua", 0), pm.get("Render", 0), pm.get("OtherRender", 0), st.get("meshBuilds")), flush=True)
    if m >= minutes:
        break
    time.sleep(60)
