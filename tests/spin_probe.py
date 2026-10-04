"""Spin throw: Mario grabs a pickup by holding Y, spins it (the keyboard's automatic wind-up, driven by a scripted Y
hold), then lets go. Reports the swing (radius, speed, how high the far end lifts), the throw (distance, launch
angle, how much it tumbles) and, with --wall, a second car in the path that must stop the spin and be knocked.
Run with the game up (-enablemcp), Mario active, on flat ground (Gridmap). Usage: python spin_probe.py [--wall]"""
import sys, time, json, math
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def status():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


def car_state(name="_ng64sp"):
    r = lua("""local v=%s local p=v:getPosition() local d=v:getDirectionVector() local u=v:getDirectionVectorUp() local vel=v:getVelocity()
return string.format('%%f %%f %%f %%f %%f %%f %%f %%f %%f %%f %%f %%f', p.x,p.y,p.z, d.x,d.y,d.z, u.x,u.y,u.z, vel.x,vel.y,vel.z)""" % name).split()
    return [float(x) for x in r]


wall = "--wall" in sys.argv
shots = "--shots" in sys.argv
if shots:
    import shot as shotmod
base = status()["pos"]
bx, by, bz = base
# the car 6 m north of Mario's start, nose east
lua("if _ng64sp then _ng64sp:delete() end _ng64sp = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (bx, by + 8, bz + 0.5))
time.sleep(5)
c0 = car_state()
# Mario beside it, facing it
lua("ng64.teleport(%f,%f,%f,true) return 1" % (c0[0], c0[1] - 2.2, bz + 0.3))
time.sleep(2)
lua("ng64.scriptInput(0,-0.2,false,false,false,2,0,1) return 1")
time.sleep(0.5)
if wall:
    # a second car where the swing will pass: 3.2 m east of Mario
    lua("if _ng64wall then _ng64wall:delete() end _ng64wall = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(0,1,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (c0[0] + 4.5, c0[1] - 2.2, bz + 0.5))
    time.sleep(4)
    w0 = car_state("_ng64wall")
HOLD = 9.0   # seconds of Y held
lua("ng64.scriptInput(0,0,false,false,false,%d,0,1,false,false,0,true) return 1" % int(HOLD * 30))
t0 = time.time()
samples = []
done = set()
while time.time() - t0 < HOLD + 0.5:
    s = status()
    cs = car_state()
    mp = s["pos"]
    r = math.hypot(cs[0] - mp[0], cs[1] - mp[1])
    sp = math.hypot(cs[9], cs[10])
    samples.append((time.time() - t0, s.get("action", 0), r, sp, cs[2] - mp[2], cs[5]))
    for when, name in ((0.7, "grab"), (1.9, "start"), (4.5, "swing")):
        if shots and time.time() - t0 > when and name not in done:
            done.add(name); shotmod.shot("F:/NG64/tests/.tmp/spin_%s.jpg" % name, 0.5)
    time.sleep(0.25)
print("t     action    radius  speed  height-above-feet  nose-z")
for t, a, r, sp, h, nz in samples[::2]:
    print("%4.1f  %08x  %5.2f   %5.1f  %5.2f              %5.2f" % (t, a, r, sp, h, nz))
if shots:
    time.sleep(0.2); shotmod.shot("F:/NG64/tests/.tmp/spin_flight.jpg", 0.5)
print("flight:  t    height  speed  up.z (1 = upright, -1 = upside down)  nose.z")
t1 = time.time()
for _ in range(16):
    cs = car_state()
    print("      %5.2f  %6.1f  %5.1f   %5.2f   %5.2f" % (time.time() - t1, cs[2], math.sqrt(cs[9] ** 2 + cs[10] ** 2 + cs[11] ** 2), cs[8], cs[5]))
    time.sleep(0.25)
c1 = car_state()
mp = status()["pos"]
dist = math.hypot(c1[0] - mp[0], c1[1] - mp[1])
print("after: car %.1f m from Mario, Mario action %08x" % (dist, status().get("action", 0)))
log = open("F:/NG64/helper/dist/ng64helper.log").read().splitlines()
for l in log[-60:]:
    if any(k in l for k in ("spin", "grabbed", "put down", "dropped", "hit something")): print("helper:", l)
if wall:
    w1 = car_state("_ng64wall")
    print("wall car moved %.1f m" % math.hypot(w1[0] - w0[0], w1[1] - w0[1]))
# why the spin ended, from the car's own Lua (the game log is buffered for a long time)
lua("_ng64last = nil _ng64sp:queueLuaCommand([[obj:queueGameEngineLua('_ng64last = ' .. string.format('%q', tostring(ng64Hit and ng64Hit.lastSpin)))]]) return 1")
time.sleep(1)
print("car says:", lua("return tostring(_ng64last)"))
lua("if _ng64sp then _ng64sp:delete() _ng64sp = nil end if _ng64wall then _ng64wall:delete() _ng64wall = nil end return 1")
