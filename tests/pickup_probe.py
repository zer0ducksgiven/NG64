"""How long picking up a thrown, damaged car takes from each corner and side: the car is picked up and thrown (so
it's damaged and displaced), then Mario runs at a corner/side of it tapping Y until he has it. Reports the time per
approach; the helper logs why Y found nothing in reach. Run with the game up (-enablemcp), Mario active, on flat
ground (Gridmap). Usage: python pickup_probe.py [rounds] [--wreck]"""
import sys, time, json, math
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mcp


def lua(c):
    return "".join(x.get("text", "") for x in mcp.call("run_lua", {"code": c})["result"]["content"])


def st():
    return json.loads(lua("return jsonEncode(ng64.getStatus())"))


def car():
    # centre, forward and left (horizontal), half-length and half-width from the car's OOBB
    r = lua("""local id=_ng64pp:getID() local c=vec3(be:getObjectOOBBCenterXYZ(id))
local f=_ng64pp:getDirectionVector() f.z=0 f=f:normalized() local l=vec3(-f.y,f.x,0)
local hl,hw=0,0
for i=0,2 do local a=vec3(be:getObjectOOBBHalfAxisXYZ(id,i)) hl=math.max(hl,math.abs(a:dot(f))) hw=math.max(hw,math.abs(a:dot(l))) end
return string.format('%f %f %f %f %f %f %f %f %f',c.x,c.y,c.z,f.x,f.y,l.x,l.y,hl,hw)""").split()
    return [float(x) for x in r]


def throw_car():
    # pick the car up from its side and throw it: damaged and displaced
    cx, cy, cz, fx, fy, lx, ly, hl, hw = car()
    lua("ng64.teleport(%f,%f,%f,true)" % (cx + lx * (hw + 0.6), cy + ly * (hw + 0.6), cz))
    time.sleep(1.5)
    lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f)" % (-lx, -ly)); time.sleep(0.4)
    lua("ng64.scriptInput(0,0,false,false,false,2,%f,%f,true)" % (-lx, -ly)); time.sleep(3)
    lua("ng64.scriptInput(0,0,false,false,false,2,1,0,true)"); time.sleep(4.5)
    if "--wreck" in sys.argv:
        # much more damage, like a car Mario has beaten up: dropped from 8 m onto its roof, then again
        for roll in (3.1416, 0.8):
            lua("local p=_ng64pp:getPosition() local r=quatFromDir(_ng64pp:getDirectionVector(), vec3(0,0,1)) * quatFromEuler(0,%f,0) _ng64pp:setPosRot(p.x,p.y,p.z+8,r.x,r.y,r.z,r.w) return 1" % roll)
            time.sleep(4)


rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2
base = st()["pos"]
lua("if _ng64pp then _ng64pp:delete() end _ng64pp = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 1" % (base[0], base[1] + 6, base[2] + 0.5))
time.sleep(4)
approaches = [("rear left", -1, 1), ("rear right", -1, -1), ("front left", 1, 1), ("front right", 1, -1),
              ("left side", 0, 1), ("rear", -1, 0)]
slow = 0
for r in range(rounds):
    throw_car()
    for name, sf, sl in approaches:
        cx, cy, cz, fx, fy, lx, ly, hl, hw = car()
        # the target point on the car's outline, and a start 2.5 m further out along the same line
        tx, ty = cx + fx * hl * sf + lx * hw * sl, cy + fy * hl * sf + ly * hw * sl
        ox, oy = tx - cx, ty - cy
        d = math.hypot(ox, oy) or 1
        ux, uy = ox / d, oy / d
        lua("ng64.teleport(%f,%f,%f,true)" % (tx + ux * 2.5, ty + uy * 2.5, cz))
        time.sleep(1.5)
        cid = lua("return tostring(_ng64pp:getID())")
        t0 = time.time()
        got = None
        lua("ng64.scriptInput(0,-1,false,false,false,200,%f,%f)" % (-ux, -uy))   # run at it
        while time.time() - t0 < 6:
            lua("ng64.scriptInput(0,-1,false,false,false,200,%f,%f,true)" % (-ux, -uy))   # ... tapping Y
            time.sleep(0.25)
            if str(st().get("carrying")) == cid:
                got = time.time() - t0
                break
        print("round %d, %s: %s" % (r + 1, name, "picked up after %.1f s" % got if got is not None else "NOT picked up in 6 s"), flush=True)
        if got is None or got > 1.5:
            slow += 1
        # put it down where it is (Z) for the next approach
        lua("ng64.scriptInput(0,0,false,false,true,3)")
        time.sleep(2.5)
print("%d slow or failed pickups out of %d" % (slow, rounds * len(approaches)))
lua("if _ng64pp then _ng64pp:delete() _ng64pp = nil end return 1")
