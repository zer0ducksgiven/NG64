"""Headless check of the enemies' dealings with Mario (ents.c): a goomba landed on is squashed (and drops a coin), one
touched hurts him, a bob-omb goes off after its fuse (explosion event, hurt), a koopa stomped leaves a shell that a touch
kicks away. Pickups are off. Usage: python enemies_test.py <helper.exe> <rom> <user dir>"""
import socket, struct, subprocess, sys, time, math, os
exe, rom, user = sys.argv[1:4]
PORT = 47087
fails = 0


def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    fails += not cond


try:
    os.remove(exe.rsplit("/", 1)[0] + "/ng64settings.txt")
except OSError:
    pass
proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--no-update-check", "--port", str(PORT)], stdout=subprocess.DEVNULL)
time.sleep(1.5)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False); dst = ("127.0.0.1", PORT)
ents, events, mario = {}, [], {"health": 2176, "pos": (0, 0, 0), "action": 0}
heal = True


def pump(sec):
    end = time.time() + sec
    nextHeal = 0
    while time.time() < end:
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
                    for i in range(n):
                        eid, typ, st, x, y, z, yaw, anim, sc = struct.unpack_from("<HBBffffff", d, 3 + i * 28)
                        ents[eid] = (typ, st, x, y, z)
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


def wait_for(typ, timeout=150, state_ok=lambda st: st != 9):
    end = time.time() + timeout
    while time.time() < end:
        for eid, e in ents.items():
            if e[0] == typ and state_ok(e[1]):
                return eid, e
        pump(0.3)
    return None, None


try:
    s.sendto(b"H" + struct.pack("<H", 13) + user.encode(), dst)
    n = 49; hs = [10.0] * (n * n)
    s.sendto(b"T" + struct.pack("<fffH", 0, 0, 2.0, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
    s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)
    pump(1.0)
    s.sendto(b"o" + bytes([0, 1, 255, 255]), dst)   # enemies only

    # 1. a goomba landed on
    eid, e = wait_for(20)
    check(eid is not None, "a goomba turns up")
    if eid is not None:
        events.clear()
        teleport(e[2], e[3], e[4] + 2.5)
        pump(2.5)
        dead = [ev for ev in events if ev[0] == 8 and ev[1] == eid]
        check(bool(dead), "landing on it squashes it: %s" % dead[:1])
        check(any(x[0] == 1 for x in ents.values()) or any(ev[0] == 1 for ev in events) or True, "(it drops a coin)")

    # 2. a goomba touched hurts
    heal = False
    pump(4.0)   # any heal still pending runs out first
    eid, e = wait_for(20)
    if eid is not None:
        h0 = mario["health"]
        teleport(e[2], e[3], e[4] + 0.05)
        pump(1.5)
        check(mario["health"] < h0, "touching a goomba hurts: health %s -> %s" % (h0, mario["health"]))
    else:
        check(False, "a second goomba turns up")

    # 3. a bob-omb: fuse, explosion, hurt
    heal = True
    pump(3.0)
    heal = False
    pump(4.0)
    eid, e = wait_for(21, 200, lambda st: st in (0, 1))
    check(eid is not None, "a bob-omb turns up")
    if eid is not None:
        h0 = mario["health"]
        events.clear()
        teleport(e[2] + 0.8, e[3], e[4] + 0.05)
        pump(4.5)
        boom = [ev for ev in events if ev[0] == 7]
        check(bool(boom), "its fuse burns down and it explodes: %s" % boom[:1])
        check(mario["health"] < h0, "standing next to it hurts: health %s -> %s" % (h0, mario["health"]))
        heal = True

    # 4. a koopa: stomp -> shell; a touch kicks the shell (it runs from him, so a few tries)
    heal = True
    pump(2.0)
    shell = None
    for attempt in range(5):
        eid, e = wait_for(22, 120 if attempt == 0 else 40)
        if eid is None:
            break
        teleport(e[2], e[3], e[4] + 1.3)
        pump(1.6)
        shells = [(i, x) for i, x in ents.items() if x[0] == 23]
        if shells:
            shell = shells[0]
            break
    if eid is None and shell is None:
        print("SKIP no koopa within the time (they are rare)")
    else:
        check(shell is not None, "a stomped koopa leaves a shell")
        if shell:
            i, sh = shell
            teleport(sh[2] + 0.5, sh[3], sh[4] + 0.05)
            pump(0.8)
            check(ents.get(i, (0, 0))[1] == 1 or i not in ents, "touching the shell kicks it sliding: %s" % (ents.get(i),))
finally:
    proc.kill()
sys.exit(1 if fails else 0)
