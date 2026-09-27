"""Picking cars up, over and over, looking for Mario "walking on the spot": after each pickup he walks for 3 s and
his SM64 action and movement are sampled; a walking action with next to no movement is reported. Half the pickups
are from standing next to the car, half from running at it. Run with the game up (-enablemcp), Mario active,
on flat ground (Gridmap). Usage: python carry_probe.py [cycles]"""
import sys, time, json, math, random
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def st():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 10
random.seed(4)
base = st()["pos"]
bx, by, bz = base
stuck_cycles = 0
for n in range(cycles):
    lua("ng64.scriptInput(0,0,false,false,true,3)")   # Z: put down anything still held
    time.sleep(1)
    lua("if _ng64cp then _ng64cp:delete() end _ng64cp = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (bx, by + 6, bz + 0.5))
    time.sleep(4)
    cid = lua("return tostring(_ng64cp:getID())")
    running = n % 2 == 1
    gap = 3.5 if running else random.uniform(1.6, 2.2)   # metres from the car's centre line (half-width ~1 m)
    lua("ng64.teleport(%f,%f,%f,true)" % (bx + random.uniform(-1.5, 1.5), by + 6 - gap, bz + 0.3))
    time.sleep(2)
    if running:
        lua("ng64.scriptInput(0,-1,false,false,false,14,0,1)")   # run at the car
        time.sleep(0.45)
    lua("ng64.scriptInput(0,0,false,false,false,2,0,1,true)")     # Y
    time.sleep(2.5)
    s = st()
    got = str(s.get("carrying")) == cid
    # walk for 3 s, sampling
    lua("ng64.scriptInput(0,-0.8,false,false,false,95,1,0)")
    samples = []
    t0 = time.time()
    while time.time() - t0 < 3.0:
        s = st()
        samples.append((time.time() - t0, s["pos"], s.get("action", 0)))
        time.sleep(0.1)
    # stuck: a walking action (ACT_FLAG_MOVING 0x400) while moving < 0.15 m over the last ~1 s
    stuck = 0.0
    for i in range(10, len(samples)):
        t, p, a = samples[i]
        _, p0, _ = samples[i - 10]
        moved = math.hypot(p[0] - p0[0], p[1] - p0[1])
        if (a & 0x400) and moved < 0.15:
            stuck += samples[i][0] - samples[i - 1][0]
    total = math.hypot(samples[-1][1][0] - samples[0][1][0], samples[-1][1][1] - samples[0][1][1])
    actions = sorted({"%08x" % a for _, _, a in samples})
    print("cycle %d (%s): picked up %s, walked %.1f m in 3 s, stuck walking %.1f s, actions %s" % (
        n + 1, "running" if running else "standing", got, total, stuck, " ".join(actions)), flush=True)
    if stuck > 0.3:
        stuck_cycles += 1
    lua("ng64.scriptInput(0,0,false,false,false,2,1,0,true)")    # Y: throw
    time.sleep(3)
print("%d of %d cycles had Mario walking on the spot" % (stuck_cycles, cycles))
lua("if _ng64cp then _ng64cp:delete() _ng64cp = nil end return 1")
