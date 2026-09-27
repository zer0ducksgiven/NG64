"""Carrying a car with the stick held the way a player holds it (camera-relative, not a fixed world direction):
forward, diagonal and sideways, 7 s each. Reports how far Mario got, how far he turned and how far the camera
swung - walking in a tight circle under a swinging camera looks like walking on the spot.
Run with the game up (-enablemcp), Mario active, on flat ground (Gridmap). Usage: python carry_turn_probe.py"""
import sys, time, json, math
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def st():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


def cam():
    return [float(v) for v in lua("local p = core_camera.getPosition() return string.format('%f %f %f', p.x, p.y, p.z)").split()]


def unwrap(a):
    out = [a[0]]
    for x in a[1:]:
        d = x - out[-1]
        while d > math.pi: d -= 2 * math.pi
        while d < -math.pi: d += 2 * math.pi
        out.append(out[-1] + d)
    return out


base = st()["pos"]
for name, sx, sy in (("forward", 0, -1), ("diagonal", 0.7, -0.7), ("sideways", 1, 0)):
    lua("if _ng64ct then _ng64ct:delete() end _ng64ct = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (base[0], base[1] + 6, base[2] + 0.5))
    time.sleep(4)
    lua("ng64.teleport(%f,%f,%f,true)" % (base[0], base[1] + 3.8, base[2] + 0.3))
    time.sleep(2)
    lua("ng64.scriptInput(0,-0.2,false,false,false,2,0,1)"); time.sleep(0.4)
    lua("ng64.scriptInput(0,0,false,false,false,2,0,1,true)"); time.sleep(3)
    if not st().get("carrying"):
        print("%s: didn't pick the car up" % name)
        continue
    time.sleep(1.5)   # let the camera settle behind him
    p0 = st()["pos"]
    lua("ng64.scriptInput(%f,%f,false,false,false,215)" % (sx, sy))   # camera-relative, like a pad
    head, camh, path, last = [], [], 0.0, p0
    t0 = time.time()
    while time.time() - t0 < 7:
        s = st(); c = cam()
        p = s["pos"]
        path += math.hypot(p[0] - last[0], p[1] - last[1]); last = p
        head.append(math.atan2(p[1] - c[1], p[0] - c[0]))   # camera -> Mario direction
        time.sleep(0.2)
    net = math.hypot(last[0] - p0[0], last[1] - p0[1])
    ch = unwrap(head)
    print("%-8s stick: walked %.1f m along his path, ended %.1f m from where he started; camera swung %.0f deg" % (
        name, path, net, math.degrees(max(ch) - min(ch))), flush=True)
    lua("ng64.scriptInput(0,0,false,false,false,2,0,1,true)"); time.sleep(3)
lua("if _ng64ct then _ng64ct:delete() _ng64ct = nil end return 1")
