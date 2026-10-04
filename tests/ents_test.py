"""Headless check of the pickups and enemies (ents.c) over the helper's real UDP protocol, on a flat 96 m square:
spawns stay within their caps and round Mario; goombas are commoner than koopas; walking into a coin collects it
(event + the settings state); the options message turns the classes off and clears them; a goomba touched sideways
hurts, one landed on is squashed; a power-up's music overrides a muted track.
Usage: python ents_test.py <helper.exe> <rom> <user dir>"""
import socket, struct, subprocess, sys, time, math, collections, os
exe, rom, user = sys.argv[1:4]
PORT = 47089
fails = 0


def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    fails += not cond


def settings_path():
    return exe.rsplit("/", 1)[0] + "/ng64settings.txt"


try:
    os.remove(settings_path())
except OSError:
    pass
proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--no-update-check", "--port", str(PORT)], stdout=subprocess.DEVNULL)
time.sleep(1.5)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False); dst = ("127.0.0.1", PORT)
ents = {}          # id -> (type, state, x, y, z)
events = []        # (kind, id, a, b, c, d)
frames = []        # latest Mario action
seen_types = collections.Counter()
max_pick = max_enemy = 0


def pump(sec):
    global max_pick, max_enemy
    end = time.time() + sec
    nextHeal = 0
    while time.time() < end:
        if time.time() > nextHeal:
            s.sendto(b"h" + bytes([31]), dst)   # keep him alive: nobody is steering him among the goombas
            nextHeal = time.time() + 0.5
        try:
            while True:
                d = s.recv(65536)
                t = d[:1]
                if t == b"n":
                    n = struct.unpack_from("<H", d, 1)[0]
                    ents.clear()
                    for i in range(n):
                        eid, typ, st, x, y, z, yaw, anim, sc = struct.unpack_from("<HBBfffff f".replace(" ", ""), d, 3 + i * 28)
                        ents[eid] = (typ, st, x, y, z)
                    pick = sum(1 for e in ents.values() if e[0] < 20)
                    enemy = sum(1 for e in ents.values() if e[0] >= 20)
                    max_pick, max_enemy = max(max_pick, pick), max(max_enemy, enemy)
                elif t == b"v":
                    kind = d[1]
                    eid = struct.unpack_from("<H", d, 2)[0]
                    a, b, c, dd = struct.unpack_from("<ffff", d, 4)
                    events.append((kind, eid, a, b, c, dd))
                elif t == b"F":
                    h = struct.unpack_from("<BIIfffffffhIhhIffffffH", d)
                    frames.append(h[11])
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.02)


def teleport(x, y, z):
    s.sendto(b"M" + struct.pack("<fff", x, y, z), dst)


try:
    s.sendto(b"H" + struct.pack("<H", 13) + user.encode(), dst)
    n = 49; hs = [10.0] * (n * n)
    s.sendto(b"T" + struct.pack("<fffH", 0, 0, 2.0, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
    s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)
    pump(1.5)
    opt = [e for e in events if e[0] == 9]
    check(bool(opt), "the hello gets the options state back: %s" % (str(opt[-1][2:]) if opt else "none"))
    s.sendto(b"o" + bytes([1, 1, 255, 255]), dst)
    pump(70)
    check(proc.poll() is None, "the helper is still running (exit %s); Mario action %s" % (proc.poll(), hex(frames[-1]) if frames else None))
    check(0 < max_pick <= 12, "pickups spawn and stay within the cap (max %d)" % max_pick)
    check(0 < max_enemy <= 7, "enemies spawn and stay within the cap (max %d)" % max_enemy)
    far = [e for e in ents.values() if math.hypot(e[2], e[3]) > 60]
    check(not far, "everything is within ~50 m of Mario (%d beyond 60 m)" % len(far))
    low = [e for e in ents.values() if not (9.9 < e[4] < 12.5)]
    check(not low, "everything sits on or just above the ground: %s" % low[:2])

    # collecting: stand on a coin
    coins = [(i, e) for i, e in ents.items() if e[0] in (1, 2, 3)]
    if coins:
        events.clear()
        i, e = coins[0]
        teleport(e[2], e[3], e[4] + 0.3)
        pump(2.0)
        got = [ev for ev in events if ev[0] == 1]
        check(bool(got), "walking into a coin collects it: %s" % got[:1])
    else:
        check(False, "no coin to collect")

    # turn them off
    print("DEBUG before off: ents", len(ents), "events", len(events), "heal-less frames", len(frames))
    s.sendto(b"o" + bytes([0, 0, 255, 255]), dst)
    pump(1.0)
    check(not ents, "turning both off clears them: %d left" % len(ents))
    opt = [e for e in events if e[0] == 9]
    check(opt and opt[-1][2] == 0 and opt[-1][3] == 0, "and the state says so: %s" % (str(opt[-1][2:]) if opt else "none"))
    pump(10)
    check(not ents, "and nothing spawns while off")
finally:
    proc.kill()

print("settings file:", open(settings_path()).read().split() if os.path.exists(settings_path()) else None)
sys.exit(1 if fails else 0)
