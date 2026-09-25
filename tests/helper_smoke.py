"""End-to-end check of ng64helper over its real UDP protocol, using the user's own ROM.
Usage: python helper_smoke.py <helper.exe> <rom.z64> <tmp user dir>"""
import socket, struct, subprocess, sys, time, math, os

exe, rom, user = sys.argv[1:4]
os.makedirs(user, exist_ok=True)
proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--port", "47099"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1.0)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
dst = ("127.0.0.1", 47099)
fails = []
def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond: fails.append(msg)

try:
    s.sendto(b"H" + struct.pack("<H", 2) + user.encode(), dst)
    d, _ = s.recvfrom(65536)
    check(d[0:1] == b"W" and d[1] == 1, "welcome ok")
    atlas = d[2:].split(b"\0")[1].decode()
    check(atlas.startswith("/ng64_cache/"), "atlas game path " + atlas)
    check(any(f.endswith(".png") for f in os.listdir(os.path.join(user, "ng64_cache"))), "atlas png written to user dir")

    # flat 40x40 m ground at z=10, plus a 3 m high block (wall test) east of spawn
    n, sp = 81, 0.5
    hs = []
    for j in range(n):
        for i in range(n):
            x = -20 + i * sp
            hs.append(13.0 if x > 6 else (11.0 if x < -6 else 10.0))   # 3 m block east, 1 m ledge west
    s.sendto(b"T" + struct.pack("<fffH", 0, 0, sp, n) + struct.pack("<%df" % len(hs), *hs), dst)
    time.sleep(0.1)
    s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)

    def frames(seconds):
        out, end = [], time.time() + seconds
        while time.time() < end:
            try: d, _ = s.recvfrom(65536)
            except socket.timeout: break
            if d[0:1] == b"F":
                h = struct.unpack_from("<BIIfffffffhIhhIffffffH", d)
                h = h[:2] + h[3:]
                out.append(h)
        return out

    f = frames(1.5)
    check(len(f) > 20, "receiving frames (%d)" % len(f))
    last = f[-1]
    check(abs(last[4] - 10.0) < 0.2, "mario standing on ground z=%.3f" % last[4])
    check(last[-1] > 300, "mesh has %d verts" % last[-1])
    check(len(f[-1]) and True, "frame parse")

    # jump: A for a few frames -> z rises
    s.sendto(b"I" + struct.pack("<ffBBBH", 0, 0, 1, 0, 0, 6), dst)
    f = frames(0.6)
    check(max(x[4] for x in f) > 10.6, "jump height max z=%.2f" % max(x[4] for x in f))

    # run: stick forward relative to camera; camera starts behind (yaw pi) => sm64 -z => bng +y? just check he moves
    f0 = frames(0.8)[-1]
    s.sendto(b"I" + struct.pack("<ffBBBH", 0, -1, 0, 0, 0, 60), dst)
    f = frames(2.0)
    moved = math.hypot(f[-1][2] - f0[2], f[-1][3] - f0[3])
    check(moved > 3, "running moved %.2f m" % moved)

    # run east into the 3 m block: must stop at the wall, not climb it
    s.sendto(b"M" + struct.pack("<fff", 0, 0, 10.2), dst)
    frames(0.5)
    s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -1, 0, 0, 0, 90, 1, 0), dst)
    f = frames(3.2)
    xs = max(x[2] for x in f)
    check(4.5 < xs < 6.3 and max(x[4] for x in f) < 11, "blocked by wall at x=%.2f z=%.2f" % (xs, f[-1][4]))

    def drain(seconds, keep_veh=False):
        lastz, end = None, time.time() + seconds
        while time.time() < end:
            if keep_veh: veh()
            s.setblocking(False)
            try:
                while True:
                    d, _ = s.recvfrom(65536)
                    if d[0:1] == b"F": lastz = struct.unpack_from("<BIIfff", d)[5]
            except (BlockingIOError, socket.timeout): pass
            s.settimeout(2.0)
            time.sleep(0.01)
        return lastz

    # ledge: dropping right next to the 1 m step lands on top (the old grid left a hole there)
    s.sendto(b"M" + struct.pack("<fff", -6.35, 0, 12.5), dst)
    z = drain(1.5)
    check(z is not None and abs(z - 11.0) < 0.05, "lands on the edge of a 1 m ledge z=%s" % z)
    # below the ground with nothing underneath: rescued back onto the surface instead of falling forever
    s.sendto(b"M" + struct.pack("<fff", 2, 2, 7.0), dst)
    z = drain(1.5)
    check(z is not None and abs(z - 10.0) < 0.05, "rescued from below the ground z=%s" % z)
    # jump up onto the ledge from a run instead of bonking off it
    s.sendto(b"M" + struct.pack("<fff", -3, 0, 10.2), dst)
    drain(0.8)
    s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -1, 0, 0, 0, 15, -1, 0), dst)
    drain(0.45)
    s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -1, 1, 0, 0, 25, -1, 0), dst)
    z = drain(2.0)
    check(z is not None and abs(z - 11.0) < 0.05, "running jump onto the 1 m ledge z=%s" % z)

    # vehicle box: 4x2x1.5 m car at (0, 5); punch it
    s.sendto(b"M" + struct.pack("<fff", 0, 3.3, 10.2), dst)
    def veh():
        # OOBB (4 x 2 x 1.5 m at (0, 5)) + frame: origin (0, 5, 10), forward +x, up +z
        s.sendto(b"V" + struct.pack("<H", 1) + struct.pack("<I3f9f3f3f3f", 7, 0, 5, 10.75, 0, 2, 0, -1, 0, 0, 0, 0, 0.75, 0, 5, 10, 1, 0, 0, 0, 0, 1), dst)
    hits = []
    end = time.time() + 3
    punched = False
    while time.time() < end:
        veh()
        if not punched and time.time() > end - 2.5:
            s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -0.3, 0, 0, 0, 2, 0, 1), dst)  # turn to face the car
            time.sleep(0.3)
            s.sendto(b"I" + struct.pack("<ffBBBHff", 0, 0, 0, 1, 0, 4, 0, 1), dst)  # punch
            punched = True
        try: d, _ = s.recvfrom(65536)
        except socket.timeout: continue
        if d[0:1] == b"A": hits.append(struct.unpack_from("<BI3f3ff", d))
        time.sleep(0.01)
    check(len(hits) >= 1, "punch hit reported %s" % (hits[:1],))

    # stand on car roof: drop onto it
    s.sendto(b"M" + struct.pack("<fff", 0, 5, 13), dst)
    end = time.time() + 2
    lastz = None
    while time.time() < end:
        veh()
        s.setblocking(False)
        try:
            while True:
                d, _ = s.recvfrom(65536)
                if d[0:1] == b"F": lastz = struct.unpack_from("<BIIfff", d)[5]
        except (BlockingIOError, socket.timeout): pass
        s.settimeout(2.0)
        time.sleep(0.01)
    check(lastz is not None and abs(lastz - 11.5) < 0.2, "standing on car roof z=%s" % lastz)

    # hull: pickup-ish shape in the vehicle frame (x right, y fwd, z up): bed 0.9 m at the back, cab 1.5 m in front
    nx, ny, cell = 4, 8, 0.5
    top = [(1.5 if j >= 4 else 0.9) for j in range(ny) for i in range(nx)]
    s.sendto(b"U" + struct.pack("<I4fHH", 7, cell, -1, -2, 0, nx, ny) + struct.pack("<%df" % len(top), *top), dst)
    s.sendto(b"M" + struct.pack("<fff", -1.5, 5, 13), dst)
    zb = drain(2.0, True)
    s.sendto(b"M" + struct.pack("<fff", 1.5, 5, 13), dst)
    zc = drain(2.0, True)
    check(zb is not None and abs(zb - 10.9) < 0.05, "hull: lands in the bed z=%s" % zb)
    check(zc is not None and abs(zc - 11.5) < 0.05, "hull: lands on the cab roof z=%s" % zc)
finally:
    proc.kill()
    print(proc.stdout.read().decode(errors="replace")[-1500:])
print("FAILURES:", fails if fails else "none")
sys.exit(1 if fails else 0)
