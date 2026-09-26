"""Mario runs straight at the camera (stick held toward it, camera-relative like a player) for a few seconds.
A steady camera lets him run in a straight line; a camera that swings round makes him curve and wander.
Reports how much his facing and the camera's heading changed, and how straight his path was."""
import sys, time, json, math
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def st():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


def cam():
    return [float(v) for v in lua("local p = core_camera.getPosition() return string.format('%f %f %f', p.x, p.y, p.z)").split()]


# settle: stand still, camera behind him
lua("ng64.scriptInput(0,0,false,false,false,30)")
time.sleep(1.5)
# stick "down" = toward the camera (no world direction: camera-relative, as a player's stick is)
lua("ng64.scriptInput(0,1,false,false,false,110)")
samples = []
t0 = time.time()
while time.time() - t0 < 3.5:
    s = st()
    c = cam()
    samples.append((s["pos"], c))
    time.sleep(0.05)
samples = samples[10:]   # skip the turn-around at the start

def heading(v):
    return math.atan2(v[1], v[0])

def unwrap(angles):
    out = [angles[0]]
    for a in angles[1:]:
        d = a - out[-1]
        while d > math.pi: d -= 2 * math.pi
        while d < -math.pi: d += 2 * math.pi
        out.append(out[-1] + d)
    return out

cam_yaw = unwrap([heading((c[0] - p[0], c[1] - p[1])) for p, c in samples])
moves = [(b[0][0] - a[0][0], b[0][1] - a[0][1]) for a, b in zip(samples, samples[1:])]
run_dir = unwrap([heading(m) for m in moves if abs(m[0]) + abs(m[1]) > 0.02])
start, end = samples[0][0], samples[-1][0]
path = sum(math.hypot(*m) for m in moves)
straight = math.hypot(end[0] - start[0], end[1] - start[1]) / max(path, 1e-6)
print("camera heading swung %.0f deg | his running direction swung %.0f deg | path straightness %.2f (1 = straight line)"
      % (math.degrees(max(cam_yaw) - min(cam_yaw)), math.degrees(max(run_dir) - min(run_dir)) if run_dir else 0, straight))
