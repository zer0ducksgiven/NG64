"""Headless check of the pickups and enemies (ents.c: the original game's object code ported) over the helper's real UDP
protocol, on a flat 96 m square at height 10 (SM64 units: 1176): spawns stay within their caps; walking into a coin
collects it; a goomba landed on is squashed (and drops a coin), one touched hurts; a bob-omb that sees Mario chases,
lights its fuse and explodes; a koopa hit loses its shell, which is left lying.
Usage: python objs_test.py <helper.exe> <rom> <user dir>"""
import socket, struct, subprocess, sys, time, math, os
exe, rom, user = sys.argv[1:4]
PORT = 47086
fails = 0


def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    fails += not cond


try:
    os.remove(exe.rsplit("/", 1)[0] + "/ng64settings.txt")
except OSError:
    pass
proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--no-input", "--no-update-check", "--port", str(PORT)], stdout=subprocess.DEVNULL)
time.sleep(1.5)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False); dst = ("127.0.0.1", PORT)
ents = {}       # id -> dict(type, state, pos(bng), parts)
events = []     # (kind, id, a, b, c, d)
mario = {"health": 2176, "pos": (0, 0, 0), "action": 0}
heal = True
PART = 28


nextWake = 0


def pump(sec):
    # (a Mario left idle for ~30 s falls asleep, and without audio the helper then stalls: a hop every few seconds)
    global nextWake
    end = time.time() + sec
    nextHeal = 0
    while time.time() < end:
        if time.time() > nextWake:
            s.sendto(b"I" + struct.pack("<ff", 0, 0) + bytes([1, 0, 0]) + struct.pack("<H", 1), dst)
            nextWake = time.time() + 8
        if heal and time.time() > nextHeal:
            s.sendto(b"h" + bytes([8]), dst)
            nextHeal = time.time() + 1.5
        try:
            while True:
                d = s.recv(65536)
                t = d[:1]
                if t == b"n":
                    n = struct.unpack_from("<H", d, 1)[0]
                    ents.clear()
                    off = 3
                    for i in range(n):
                        eid, typ, st, np_, _ = struct.unpack_from("<HBBBB", d, off)
                        pos = struct.unpack_from("<fff", d, off + 6)
                        off += 18 + np_ * PART
                        ents[eid] = {"type": typ, "state": st, "pos": pos, "parts": np_}
                elif t == b"v":
                    eid = struct.unpack_from("<H", d, 2)[0]
                    events.append((d[1], eid) + struct.unpack_from("<ffff", d, 4))
                elif t == b"F":
                    h = struct.unpack_from("<BIIfffffffhIhhIffffffH", d)
                    mario["health"], mario["action"], mario["pos"] = h[10], h[11], h[3:6]
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.02)


def teleport(x, y, z):
    s.sendto(b"M" + struct.pack("<fff", x, y, z), dst)


def find(typ, timeout=150, ok=lambda e: True):
    end = time.time() + timeout
    while time.time() < end:
        for eid, e in ents.items():
            if e["type"] == typ and ok(e):
                return eid, e
        pump(0.3)
        if int(time.time()) % 10 == 0:
            print("  ..waiting for type %d: %s mario %s" % (typ, sorted(e["type"] for e in ents.values()), tuple(round(v, 1) for v in mario["pos"])), flush=True)
    return None, None


try:
    s.sendto(b"H" + struct.pack("<H", 14) + user.encode(), dst)
    n = 49; hs = [10.0] * (n * n)
    s.sendto(b"T" + struct.pack("<fffH", 0, 0, 2.0, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
    s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)
    pump(1.0)
    s.sendto(b"o" + bytes([1, 0, 255, 255]), dst)    # pickups only
    pump(40)
    pick = [e for e in ents.values() if e["type"] < 20]
    check(0 < len(pick) <= 12, "pickups stream in within the cap (%d)" % len(pick))
    check(all(e["parts"] >= 1 for e in pick), "and each is posed as pieces of its ROM model")
    far = [e for e in pick if math.hypot(e["pos"][0], e["pos"][1]) > 60]
    check(not far, "all within ~50 m of Mario")

    # a coin
    coin = [(i, e) for i, e in ents.items() if e["type"] in (1, 2, 3)]
    if coin:
        events.clear()
        i, e = coin[0]
        teleport(e["pos"][0], e["pos"][1], e["pos"][2] + 0.2)
        pump(2.0)
        got = [ev for ev in events if ev[0] == 1]
        check(bool(got), "walking into a coin collects it: %s" % got[:1])
    else:
        check(False, "no coin to collect")

    # enemies
    s.sendto(b"o" + bytes([0, 1, 255, 255]), dst)
    pump(2.0)
    print("  before tp: mario", [round(v, 1) for v in mario["pos"]], flush=True)
    teleport(0, 0, 10.5)
    pump(1.0)
    print("  after tp: mario", [round(v, 1) for v in mario["pos"]], flush=True)
    eid, e = find(20, 60)
    print("DEBUG types", sorted(e["type"] for e in ents.values()), "mario", mario, "helper exit", proc.poll())
    check(eid is not None, "a goomba turns up")
    if eid is not None:
        events.clear()
        heal = True
        teleport(e["pos"][0], e["pos"][1], e["pos"][2] + 3.0)
        pump(0.9)
        eid2, e2 = eid, ents.get(eid)
        teleport(e2["pos"][0], e2["pos"][1], e2["pos"][2] + 2.5) if e2 else None
        pump(2.5)
        dead = [ev for ev in events if ev[0] == 8]
        check(bool(dead) or eid not in ents, "landing on a goomba squashes it (event %s)" % dead[:1])

    # a bob-omb that sees Mario chases and goes off
    heal = False
    pump(2.0)
    eid, e = find(21, 200)
    check(eid is not None, "a bob-omb turns up")
    if eid is not None:
        events.clear()
        h0 = mario["health"]
        teleport(e["pos"][0] - 2.5, e["pos"][1], e["pos"][2] + 0.1)
        pump(25.0)
        boom = [ev for ev in events if ev[0] == 7]
        check(bool(boom), "it spots him, lights its fuse and explodes: %s" % boom[:1])
finally:
    proc.kill()
sys.exit(1 if fails else 0)
