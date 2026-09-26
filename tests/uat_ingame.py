"""In-game UAT for NG64, driven through BeamNG's built-in MCP server (launch the game with -enablemcp).

Needs: BeamNG running with ng64.zip installed, ng64helper.exe running with the user's ROM.
Usage: python uat_ingame.py [--port 29292] [--shots <dir>]
"""
import json, sys, time, urllib.request, base64, os, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=29292)
ap.add_argument("--shots", default=os.path.join(os.path.dirname(__file__), "shots"))
ap.add_argument("--level", default="gridmap_v2")
args = ap.parse_args()
URL = "http://127.0.0.1:%d/mcp" % args.port
os.makedirs(args.shots, exist_ok=True)
_id = [0]
fails = []


def rpc(method, params=None, timeout=60):
    _id[0] += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id[0], "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, body, {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        txt = r.read().decode()
    if txt.startswith("event:") or txt.startswith("data:"):
        txt = [l[5:] for l in txt.splitlines() if l.startswith("data:")][-1]
    return json.loads(txt)


def tool(name, **kw):
    res = rpc("tools/call", {"name": name, "arguments": kw})
    if "error" in res:
        raise RuntimeError(res["error"])
    return res["result"]


def text(res):
    return "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")


def lua(code):
    return text(tool("run_lua", code=code))


def status():
    raw = lua("return jsonEncode(ng64 and ng64.getStatus() or {missing=true})")
    try:
        start = raw.index("{")
        return json.loads(raw[start:raw.rindex("}") + 1])
    except Exception:
        return {"raw": raw}


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg, flush=True)
    if not cond:
        fails.append(msg)


LOG = []


GAME_LOG = None   # resolved from the game's own FS:getUserPath() once connected
_logStart = [0]


def pull_logs():
    # beamng.log straight from disk, from where this run started
    if os.path.getsize(GAME_LOG) < _logStart[0]:
        _logStart[0] = 0   # the game rotated its log after we connected
    with open(GAME_LOG, "rb") as f:
        f.seek(_logStart[0])
        return f.read().decode(errors="replace")


def shot(name):
    try:
        tool("screenshot_image", scale=0.5)
        time.sleep(1.5)
        res = tool("screenshot_image", scale=0.5)
        for c in res.get("content", []):
            if c.get("type") == "image":
                p = os.path.join(args.shots, name + ".jpg")
                open(p, "wb").write(base64.b64decode(c["data"]))
                print("  screenshot", p)
                return p
        print("  screenshot: no image in", text(res)[:200])
    except Exception as e:
        print("  screenshot failed:", e)


def wait_for(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = pred()
            if v:
                return v
        except Exception:
            pass
        time.sleep(step)
    return None


# -- connect ------------------------------------------------------------------------------------------------------
print("waiting for BeamNG MCP on", URL)
ok = wait_for(lambda: rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "ng64-uat", "version": "1"}}), 300, 2)
check(bool(ok), "BeamNG MCP reachable")
if not ok:
    sys.exit(1)

GAME_LOG = lua("return FS:getUserPath()").strip().replace("\\", "/").rstrip("/") + "/beamng.log"
_logStart[0] = os.path.getsize(GAME_LOG)
print("game log:", GAME_LOG)
check("true" in lua("return tostring(ng64 ~= nil)"), "ng64 extension loaded by modScript")

# -- level ----------------------------------------------------------------------------------------------------------
if "true" not in lua("return tostring(getMissionFilename() ~= '' and getMissionFilename() ~= nil)"):
    lua("freeroam_freeroam.startFreeroamByName('%s')" % args.level)
ok = wait_for(lambda: "true" in lua("return tostring(getPlayerVehicle(0) ~= nil and core_gamestate.state and core_gamestate.state.state == 'freeroam')"), 240, 2)
check(bool(ok), "freeroam level loaded")
pull_logs()
time.sleep(3)

# -- spawn Mario ----------------------------------------------------------------------------------------------------
lua("core_vehicles.replaceVehicle('ng64_mario', {config='vehicles/ng64_mario/mario.pc'})")
st = wait_for(lambda: (lambda s: s if s.get("numVerts") else None)(status()), 30)
check(bool(st), "Mario spawned and rendering: %s" % st)
if not st:
    print(lua("return jsonEncode(ng64.getStatus())"))
    sys.exit(1)
time.sleep(2)
st = status()
check(st.get("material") is not None, "atlas material created (%s)" % st.get("material"))
check(st.get("frameAge", 9) < 0.2, "frames arriving live (age %.3f s)" % st.get("frameAge", 9))
z0 = st["pos"][2]
ground = float(lua("local p=getPlayerVehicle(0):getPosition() return tostring(p.z - castRayStatic(p+vec3(0,0,2), vec3(0,0,-1), 50) + 2)").split()[-1])
check(abs(z0 - ground) < 0.15, "Mario standing on the map (mario z %.2f, ground %.2f)" % (z0, ground))
check(st.get("health") == 2176, "full health (%s)" % st.get("health"))
shot("01_spawned")

# flicker: grab the game window straight off the desktop while he runs in circles; Mario must be in every grab
lua("ng64.scriptInput(0.6,-1,false,false,false,300)")
import subprocess
ps = "/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
here = os.path.dirname(os.path.abspath(__file__)).replace("/f/", "F:/")
os.makedirs(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".tmp"), exist_ok=True)
env = dict(os.environ, TEMP=here + "/.tmp", TMP=here + "/.tmp", SystemRoot="C:\Windows")   # msys python passes neither
cap = subprocess.run([ps, "-ExecutionPolicy", "Bypass", "-File", here + "/flicker_capture.ps1", "-Frames", "150", "-LowBelow", "0", "-OutDir", here + "/shots"],   # blank grabs are kept as shots/cap_N.png
                     capture_output=True, text=True, env=env).stdout
counts = [int(c) for c in cap.split("COUNTS")[-1].strip().split(",")] if "COUNTS" in cap else []
check(len(counts) == 150 and min(counts) > 0, "no flicker: Mario drawn in %d/%d desktop grabs (min red %s; the old bug gave exactly 0)" % (sum(1 for c in counts if c > 0), len(counts), min(counts) if counts else None))

# -- jump / run -------------------------------------------------------------------------------------------------------
lua("ng64.scriptInput(0,0,true,false,false,6)")
peak = z0
for _ in range(12):
    time.sleep(0.05)
    peak = max(peak, status()["pos"][2])
check(peak > z0 + 0.5, "jump rises %.2f m" % (peak - z0))
time.sleep(1.5)
p0 = status()["pos"]
lua("ng64.scriptInput(0,-1,false,false,false,75,1,0)")
time.sleep(1.2)
shot("02_running")
time.sleep(1.5)
p1 = status()["pos"]
check(p1[0] - p0[0] > 8, "runs along +x %.2f m" % (p1[0] - p0[0]))

# -- car interaction --------------------------------------------------------------------------------------------------
mx, my, mz = p1
lua("_ng64uatCar = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return tostring(_ng64uatCar:getID())" % (mx, my + 4.5, mz + 0.5))
carId = lua("return tostring(_ng64uatCar and _ng64uatCar:getID())").strip().split()[-1]
check(carId.isdigit(), "spawned test car id %s" % carId)
time.sleep(4)
# the stub must stay the player vehicle so Mario stays active
st = status()
check(st.get("active") is True, "Mario still active after another vehicle spawned")

def car_geo():
    # centre, forward, and the car's side axis (shortest horizontal half-axis) with its half-length
    r = lua("""local id=%s local c=vec3(be:getObjectOOBBCenterXYZ(id)) local best,bl
for i=0,2 do local a=vec3(be:getObjectOOBBHalfAxisXYZ(id,i)) if math.abs(a.z)<0.5*a:length() and (not bl or a:length()<bl) then best,bl=a,a:length() end end
local n=best/bl local f=be:getObjectByID(id):getDirectionVector()
return string.format('%%f %%f %%f %%f %%f %%f %%f %%f', c.x,c.y,c.z,n.x,n.y,bl,f.x,f.y)""" % carId).split()[-8:]
    return list(map(float, r))


def damage():
    return float(lua("return tostring(map.objects[%s] and map.objects[%s].damage or -1)" % (carId, carId)).split()[-1])


def settle_car():
    time.sleep(1.5)


check(bool(wait_for(lambda: status().get("hulls", 0) > 0, 6)), "vehicle hull received from the car's nodes (%s)" % status().get("hulls"))

def fresh_car():
    # every attack gets an undamaged, unmoved pickup so one test's shove can't spoil the next
    global carId
    lua("if _ng64uatCar then _ng64uatCar:delete() end _ng64uatCar = core_vehicles.spawnNewVehicle('pickup', {pos=vec3(%f,%f,%f), rot=quatFromDir(vec3(1,0,0), vec3(0,0,1)), autoEnterVehicle=false}) return 'ok'" % (mx, my + 4.5, mz + 0.5))
    carId = lua("return tostring(_ng64uatCar:getID())").strip().split()[-1]
    time.sleep(4)
    wait_for(lambda: status().get("hulls", 0) > hullsBefore[0], 6)
    hullsBefore[0] = status().get("hulls", 0)

hullsBefore = [0]


def attack(name, approach, inputs, min_damage):
    fresh_car()
    cx, cy, cz, nx, ny, half, fx, fy = car_geo()
    sx, sy = cx + nx * (half + approach), cy + ny * (half + approach)
    lua("ng64.teleport(%f,%f,%f)" % (sx, sy, mz + 0.2))
    time.sleep(0.8)
    lua("ng64.scriptInput(0,-0.2,false,false,false,3,%f,%f)" % (-nx, -ny))   # face the car
    time.sleep(0.4)
    h0, d0 = status().get("hits", 0), damage()
    for step in inputs:
        stick, a, b, z, frames, wait = step[:6]
        lua("ng64.scriptInput(0,%f,%s,%s,%s,%d,%f,%f)" % (stick, a, b, z, frames, -nx, -ny))
        if len(step) > 6:
            # run-up: keep going until Mario is this close to the car's side, instead of trusting a timer
            wait_for(lambda: (lambda p: (p[0] - cx) * nx + (p[1] - cy) * ny - half < step[6])(status()["pos"]), 3, 0.02)
        else:
            time.sleep(wait)
    shot("03_" + name)
    time.sleep(1.5)
    h1, d1 = status().get("hits", 0), damage()
    check(h1 > h0, "%s registered on the car (%d hits)" % (name, h1 - h0))
    check(d1 - d0 >= min_damage, "%s dented the car: damage %.0f -> %.0f" % (name, d0, d1))
    settle_car()

attack("punch", 0.45, [(0, "false", "true", "false", 4, 0.1)], 1000)
# dive = B in the air at speed: run, jump, then B
attack("dive", 9.0, [(-1, "false", "false", "false", 90, 0, 2.0), (-1, "true", "false", "false", 3, 0.1), (-1, "false", "true", "false", 4, 0.1)], 1000)
# slide kick needs speed: run, crouch into a slide, then B
attack("slide_kick", 7.0, [(-1, "false", "false", "false", 90, 0, 2.0), (-1, "false", "false", "true", 3, 0.1), (-1, "false", "true", "true", 4, 0.1)], 1000)

# hull: the pickup's bed is lower than its cab roof
fresh_car()
cx, cy, cz, nx, ny, half, fx, fy = car_geo()
def drop_at(off):
    lua("ng64.teleport(%f,%f,%f)" % (cx + fx * off, cy + fy * off, cz + 2.5))
    time.sleep(2.0)
    return status()["pos"][2]
zb = drop_at(-1.6)
zc = drop_at(0.1)
check(zc - zb > 0.3 and zb > mz + 0.3, "lands in the pickup bed (z %.2f) below the cab roof (z %.2f)" % (zb, zc))
shot("05_on_roof")

# ground pound on the cab roof
d0 = damage()
lua("ng64.teleport(%f,%f,%f)" % (cx + fx * 0.1, cy + fy * 0.1, cz + 3.5))
time.sleep(0.35)
lua("ng64.scriptInput(0,0,false,false,true,6)")
time.sleep(0.15)
shot("04_groundpound")
time.sleep(1.5)
d1 = damage()
check(d1 - d0 >= 5000, "ground pound crushed the roof: damage %.0f -> %.0f" % (d0, d1))

# car drives into Mario -> Mario gets hurt
cx, cy, cz, nx, ny, half, fx, fy = car_geo()
lua("ng64.teleport(%f,%f,%f)" % (cx + 7, cy, mz + 0.2))
time.sleep(1.5)
h0 = status()["health"]
cd0 = damage()
check(h0 == 2176, "Mario's own attacks shoving cars away never hurt him (health %s)" % h0)
lua("be:getObjectByID(%s):queueLuaCommand(\"if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,1,0,0,9)\")" % (carId, cx - 200, cy, cz))
acts = set()
def hurt_seen():
    st = status()
    acts.add(st.get("action"))
    return st["health"] < h0
hurt = wait_for(hurt_seen, 4, 0.03)
for _ in range(15):
    acts.add(status().get("action")); time.sleep(0.03)
shot("06_run_over")
check(bool(hurt), "vehicle hitting Mario hurts him (health %s -> %s)" % (h0, status()["health"]))
check(0x010208BE in acts, "run over: thrown clear (SM64 thrown-backward knockback)")
time.sleep(1.0)
cd1 = damage()
check(cd1 - cd0 > 50, "the car that hit Mario took some damage: %.0f -> %.0f" % (cd0, cd1))
time.sleep(2.5)
# afterwards he must not be inside the car: under its roof line within its footprint
inside = lua("""local id=%s local p=vec3(ng64.getStatus().pos[1], ng64.getStatus().pos[2], ng64.getStatus().pos[3])
local c=vec3(be:getObjectOOBBCenterXYZ(id)) local d=p-c local inside=true local upLen
for i=0,2 do local a=vec3(be:getObjectOOBBHalfAxisXYZ(id,i)) local l=a:length() local t=d:dot(a)/l
  if math.abs(a.z)/l>0.7 then if t>l*0.3 then inside=false end elseif math.abs(t)>l-0.2 then inside=false end end
return tostring(inside)""" % carId)
check("false" in inside, "not left inside the car after being run over")

# -- smooth motion: the mesh is rebuilt every rendered frame (blending the 30 Hz poses), not 30 times a second ----
lua("ng64.scriptInput(0.6,-1,false,false,false,120)")
time.sleep(0.5)
b0 = status()["meshBuilds"]
t0 = time.time()
time.sleep(2.0)
b1 = status()["meshBuilds"]
rate = (b1 - b0) / (time.time() - t0)
perf = text(tool("get_performance_metrics"))
print("  mesh builds/s %.0f; %s" % (rate, perf[:160].replace("\n", " ")))
check(rate > 40, "Mario's mesh updates at the render rate (%.0f builds/s, 30 Hz would be 30)" % rate)

# -- motion: Mario moves every rendered frame, at an even speed (poses timed by simulation tick, history buffer) --
lua("ng64.scriptInput(0,-1,false,false,false,150,1,0)")
time.sleep(1.5)
lua("ng64.startTrace()")
time.sleep(2.5)
import statistics
trows = [list(map(float, l.split())) for l in lua("return ng64.getTrace()").strip().splitlines() if l.strip()]
sp = []
for a_, b_ in zip(trows, trows[1:]):
    dt_ = b_[0] - a_[0]
    if dt_ > 0: sp.append((((b_[2] - a_[2]) ** 2 + (b_[3] - a_[3]) ** 2) ** 0.5) / dt_)
stalls = sum(1 for v in sp if v < 0.25 * statistics.mean(sp))
jit = statistics.pstdev(sp) / statistics.mean(sp)
check(stalls == 0 and jit < 0.35, "Mario moves every rendered frame at an even speed (%d stalled of %d, jitter %.2f)" % (stalls, len(sp), jit))

# -- carrying: Y lifts the car overhead (SM64 heavy lift), it follows him, Y throws it ------------------------------
def centre():
    return [float(v) for v in lua("local c=vec3(be:getObjectOOBBCenterXYZ(%s)) return string.format('%%f %%f %%f', c.x,c.y,c.z)" % carId).split()[-3:]]
lua("ng64.scriptInput(0,0,false,false,false,2,1,0,true)")          # Y with nothing in reach: nothing happens
time.sleep(1.0)
check(status().get("carrying") is None, "Y with nothing in reach does nothing")
fresh_car()
cx, cy, cz, nx, ny, half, fx, fy = car_geo()
lua("ng64.teleport(%f,%f,%f)" % (cx + nx * (half + 0.45), cy + ny * (half + 0.45), mz + 0.2))
time.sleep(1.0)
lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f)" % (-nx, -ny)); time.sleep(0.4)
h_before = status()["health"]
z0 = centre()[2]
lua("ng64.scriptInput(0,0,false,false,false,2,%f,%f,true)" % (-nx, -ny))
time.sleep(3.0)
c1 = centre(); st = status()
check(str(st.get("carrying")) == carId and c1[2] - z0 > 0.6, "Y lifts the car onto his hands (carrying %s, centre up %.2f m)" % (st.get("carrying"), c1[2] - z0))
lua("ng64.scriptInput(0,-1,false,false,false,60,1,0)"); time.sleep(2.5)
st = status(); c2 = centre()
gap = ((st["pos"][0] - c2[0]) ** 2 + (st["pos"][1] - c2[1]) ** 2) ** 0.5
check(gap < 1.5 and c2[2] - z0 > 0.6, "the car goes where he goes (%.2f m from him, still %.2f m up)" % (gap, c2[2] - z0))
shot("10_carry")
d0 = damage()
lua("ng64.scriptInput(0,0,false,false,false,2,1,0,true)")          # Y again: throw
time.sleep(3.5)
c3 = centre(); st = status()
flew = ((c3[0] - c2[0]) ** 2 + (c3[1] - c2[1]) ** 2) ** 0.5
check(st.get("carrying") is None and st.get("throws", 0) >= 1 and flew > 8, "Y again throws it (%.1f m)" % flew)
check(damage() > d0, "the thrown car crashes (damage %.0f -> %.0f)" % (d0, damage()))
check(status()["health"] == h_before, "carrying and throwing didn't hurt Mario (health %s)" % status()["health"])
# Z puts it down instead of throwing
fresh_car()
cx, cy, cz, nx, ny, half, fx, fy = car_geo()
lua("ng64.teleport(%f,%f,%f)" % (cx + nx * (half + 0.45), cy + ny * (half + 0.45), mz + 0.2))
time.sleep(1.0)
lua("ng64.scriptInput(0,-0.2,false,false,false,2,%f,%f)" % (-nx, -ny)); time.sleep(0.4)
lua("ng64.scriptInput(0,0,false,false,false,2,%f,%f,true)" % (-nx, -ny)); time.sleep(3.0)
c4 = centre()
lua("ng64.scriptInput(0,0,false,false,true,3)"); time.sleep(3.0)
c5 = centre(); st = status()
check(st.get("carrying") is None and ((c5[0] - c4[0]) ** 2 + (c5[1] - c4[1]) ** 2) ** 0.5 < 2.5,
      "Z sets it down where he stands (moved %.1f m)" % (((c5[0] - c4[0]) ** 2 + (c5[1] - c4[1]) ** 2) ** 0.5))

# -- a wreck in two pieces: two hulls, and the gap between them is open ------------------------------------------
fresh_car()
cx, cy, cz, nx, ny, half, fx, fy = car_geo()
lua("""_ng64uatCar:queueLuaCommand([[
local f = obj:getDirectionVector() f:normalize()
local function side(cid) return obj:getNodePosition(cid):dot(f) end
for b = 0, tableSizeC(v.data.beams) - 1 do
  local bm = v.data.beams[b]
  if bm and bm.id1 and side(bm.id1) * side(bm.id2) < 0 then obj:breakBeam(bm.cid) end
end
for cid = 0, obj:getNodeCount() - 1 do
  if side(cid) < 0 then obj:applyForceVectorTime(cid, -f * (obj:getNodeMass(cid) * 30), 0.3) end
end]])""")
time.sleep(3)
lua("_ng64uatCar:queueLuaCommand(\"if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.sendHull()\")")
time.sleep(1)
pieces = int(lua("return tostring(ng64.getStatus().hullPieces[%s] or 0)" % carId).split()[-1])
check(pieces >= 2, "torn-in-half car collides as separate pieces (%d)" % pieces)
# walk along the car's axis until a drop reaches the ground: that's the gap the pull opened between the halves
# the split line is the front half's reference point (obj:getPosition()); the rear half was pulled back from it
geo = lua("""local veh=_ng64uatCar local f=veh:getDirectionVector() f.z=0 f:normalize() local o=veh:getPosition()
return string.format('%f %f %f %f %f', o.x,o.y,o.z,f.x,f.y)""").split()
gx, gy, gz, gfx, gfy = map(float, geo)
landed = None
for off in [-x * 0.25 for x in range(0, 13)]:
    px, py = gx + gfx * off, gy + gfy * off
    lua("ng64.teleport(%f,%f,%f)" % (px, py, gz + 2.5))
    time.sleep(1.2)
    z = status()["pos"][2]
    if z < mz + 0.15:
        landed = (off, z)
        break
check(landed is not None, "Mario dropped into the gap between the pieces reaches the ground (at %s)" % (landed,))
shot("08_wreck_gap")

# -- reset: Mario comes back at the anchor under him, not the original spawn point ---------------------------------
spawn_pos = status()["pos"]
lua("ng64.teleport(%f,%f,%f)" % (mx + 25, my - 25, mz + 0.3))
time.sleep(3.0)                       # anchor follows him while he stands there
lua("ng64.scriptInput(0,-1,false,false,false,20,1,0)")   # walk a bit further, then reset
time.sleep(1.0)
before = status()["pos"]
lua("getPlayerVehicle(0):queueLuaCommand('obj:requestReset(RESET_PHYSICS)')")
time.sleep(1.5)
after = status()["pos"]
d_anchor = ((after[0] - (mx + 25)) ** 2 + (after[1] - (my - 25)) ** 2) ** 0.5
check(d_anchor < 4.0 and status()["health"] == 2176, "reset puts Mario at the anchor near him (%.1f m from where it followed him), full health" % d_anchor)

# -- multiplayer (BeamMP) -----------------------------------------------------------------------------------------------
# outgoing: capture what the mod would send through BeamMP's TriggerServerEvent
lua("_ng64Sent = {} TriggerServerEvent = function(n, d) table.insert(_ng64Sent, n .. '=' .. d) end")
time.sleep(1.0)
sent = lua("return tostring(#_ng64Sent) .. ' ' .. tostring(_ng64Sent[#_ng64Sent])")
n_sent = int(sent.split()[0])
check(8 <= n_sent <= 25 and "ng64State=" in sent, "Mario state sent to server at ~15 Hz (%s)" % sent[:90])
# incoming: another player's Mario standing 3 m from ours, via the same handler BeamMP calls
mp = status()["pos"]
lua("ng64.onRemote('7|%f,%f,%f,0,%d,0,0,0')" % (mp[0] + 3, mp[1], mp[2], 0x0C400201))
time.sleep(0.2)
for _ in range(10):
    lua("ng64.onRemote('7|%f,%f,%f,0,%d,0,0,0')" % (mp[0] + 3, mp[1], mp[2], 0x0C400201))
    time.sleep(0.1)
st = status()
check(st.get("meshes", 0) >= 2, "remote player's Mario rendered (%s meshes)" % st.get("meshes"))
shot("07_remote_mario")
lua("ng64.onRemoteGone('7')")
time.sleep(0.5)
check(status().get("meshes") == 1, "remote Mario removed when the player leaves")
lua("TriggerServerEvent = nil")

# -- despawn ----------------------------------------------------------------------------------------------------------
lua("be:enterVehicle(0, _ng64uatCar)")
time.sleep(1)
st = status()
check(st.get("active") is False, "switching to another vehicle hands control back")
check("true" in lua("return tostring(not commands.isFreeCamera())"), "game camera restored")

# -- another level with Mario as the current vehicle: he must come back textured and on the ground ------------------
lua("core_vehicles.replaceVehicle('ng64_mario', {config='vehicles/ng64_mario/mario.pc'})")
time.sleep(4)
lua("freeroam_freeroam.startFreeroamByName('smallgrid')")
ok = wait_for(lambda: status().get("active") is True and "smallgrid" in lua("return tostring(getMissionFilename())"), 240, 2)
time.sleep(3)
mat_ok = "true" in lua("local n=ng64.getStatus().material return tostring(n ~= nil and scenetree.findObject(n) ~= nil)")
ground = lua("local p=getPlayerVehicle(0):getPosition() return tostring(p.z - castRayStatic(p+vec3(0,0,3), vec3(0,0,-1), 50) + 3)").split()[-1]
mz2 = status()["pos"][2]
cap2 = subprocess.run([ps, "-ExecutionPolicy", "Bypass", "-File", here + "/flicker_capture.ps1", "-Frames", "30"], capture_output=True, text=True, env=env).stdout
c2 = [int(c) for c in cap2.split("COUNTS")[-1].strip().split(",")] if "COUNTS" in cap2 else []
check(bool(ok) and mat_ok and abs(mz2 - float(ground)) < 0.2 and c2 and min(c2) > 0,
      "after loading another level Mario is textured, on the ground and drawn (material %s, z %.2f vs ground %s, drawn in %d/%d grabs)" % (mat_ok, mz2, ground, sum(1 for c in c2 if c > 0), len(c2)))
shot("09_level_switch")

errs = pull_logs()
# BeamNG's level loader logs "Failed to spawn vehicle" for the Mario anchor on every level load: its spawn-placement
# box is built from collidable nodes, and the anchor deliberately has none (it would be an invisible solid post).
# The vehicle still spawns and works; this one message is expected.
ng_errs = [l for l in errs.splitlines() if "|E|" in l and "ng64" in l.lower() and "Failed to spawn vehicle" not in l]
check(not ng_errs, "no ng64 errors in log %s" % ng_errs[:5])

print("\nFAILURES:", fails if fails else "none")
sys.exit(1 if fails else 0)
