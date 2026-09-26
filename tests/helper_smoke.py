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
    s.sendto(b"H" + struct.pack("<H", 8) + user.encode(), dst)
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
        # drop whatever is already queued (frames from the previous step), then collect this many seconds' worth
        s.setblocking(False)
        try:
            for _ in range(10000): s.recvfrom(65536)
        except (BlockingIOError, socket.timeout): pass
        s.settimeout(2.0)
        out, want, end = [], int(seconds * 30), time.time() + seconds * 4
        while len(out) < want and time.time() < end:
            try: d, _ = s.recvfrom(65536)
            except socket.timeout: break
            if d[0:1] == b"F":
                h = struct.unpack_from("<BIIfffffffhIhhIffffffH", d)
                h = h[:2] + h[3:]
                out.append(h)
        return out

    # body parts: each part's geometry arrives once (MSG_PART), frames only carry where the parts are
    s.settimeout(2.0)
    parts, nparts, end = {}, None, time.time() + 3
    while time.time() < end and (nparts is None or len(parts) < nparts):
        try: d, _ = s.recvfrom(65536)
        except socket.timeout: break
        if d[0:1] == b"E":
            _, key, part, hsh, nv, ni = struct.unpack_from("<BIBIHH", d)
            parts[part] = (hsh, nv, ni)
        elif d[0:1] == b"F":
            np_ = struct.unpack_from("<BIIfffffffhIhhIffffffHHI", d)[-2]
            poses = [struct.unpack_from("<I", d, struct.calcsize("<BIIfffffffhIhhIffffffHHI") + i * 52)[0] for i in range(np_)]
            nparts = sum(1 for h in poses if h)
    check(nparts and nparts >= 10, "Mario is sent as body parts (%s with geometry)" % nparts)
    check(len(parts) >= (nparts or 99), "every part's geometry arrived (%d of %s)" % (len(parts), nparts))
    check(all(ni % 3 == 0 and nv > 0 for _, nv, ni in parts.values()), "part geometry is whole triangles")
    # a lost part is sent again on request
    if parts:
        pid, (hsh, _, _) = next(iter(parts.items()))
        s.sendto(b"B" + struct.pack("<IBI", 0, pid, hsh), dst)
        got = False
        end = time.time() + 1
        while time.time() < end and not got:
            try: d, _ = s.recvfrom(65536)
            except socket.timeout: break
            got = d[0:1] == b"E" and struct.unpack_from("<BIBI", d)[2:4] == (pid, hsh)
        check(got, "part geometry re-sent on request")

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

    # map cells: a streamed 16 m cell with a floor 2 m up is solid; two cells sharing a triangle are fine; dropping
    # the cells removes the floor
    def cell(cid, tris):
        s.sendto(b"O" + struct.pack("<IHHH", cid, 0, 1, len(tris)) + b"".join(struct.pack("<9f", *t) for t in tris), dst)
    floor = [(-3, -3, 12, 3, -3, 12, 3, 3, 12), (-3, -3, 12, 3, 3, 12, -3, 3, 12)]
    cell(0x80008000, floor)
    cell(0x80008001, floor)   # the same triangles again from a neighbouring cell
    time.sleep(0.3)
    s.sendto(b"M" + struct.pack("<fff", 0, 0, 12.5), dst)
    f = frames(1.2)
    check(abs(f[-1][4] - 12.0) < 0.2, "stands on a streamed map cell z=%.3f" % f[-1][4])
    s.sendto(b"Y" + struct.pack("<I", 0x80008000), dst)
    time.sleep(0.3)
    s.sendto(b"M" + struct.pack("<fff", 0, 0, 12.5), dst)
    f = frames(1.2)
    check(abs(f[-1][4] - 12.0) < 0.2, "the other cell still holds the shared floor z=%.3f" % f[-1][4])
    s.sendto(b"Y" + struct.pack("<I", 0xFFFFFFFF), dst)
    time.sleep(0.3)
    f = frames(1.5)
    check(abs(f[-1][4] - 10.0) < 0.2, "dropping every cell removes the floor (back on the ground z=%.3f)" % f[-1][4])

    # run east into the 3 m block: must stop at the wall, not climb it
    s.sendto(b"M" + struct.pack("<fff", 0, 0, 10.2), dst)
    frames(0.5)
    s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -1, 0, 0, 0, 90, 1, 0), dst)
    f = frames(3.2)
    xs = max(x[2] for x in f)
    check(4.5 < xs < 6.3 and max(x[4] for x in f) < 11, "blocked by wall at x=%.2f z=%.2f" % (xs, f[-1][4]))

    def drain(seconds, keep_veh=False):
        # count simulated frames rather than wall-clock time, so a briefly starved helper can't end the wait early
        lastz, want, got, end = None, int(seconds * 30), 0, time.time() + seconds * 4
        while got < want and time.time() < end:
            if keep_veh: veh()
            s.setblocking(False)
            try:
                while True:
                    d, _ = s.recvfrom(65536)
                    if d[0:1] == b"F": lastz = struct.unpack_from("<BIIfff", d)[5]; got += 1
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
    landings = []
    while time.time() < end:
        veh()
        s.setblocking(False)
        try:
            while True:
                d, _ = s.recvfrom(65536)
                if d[0:1] == b"F": lastz = struct.unpack_from("<BIIfff", d)[5]
                elif d[0:1] == b"A": landings.append(struct.unpack_from("<BI3f3ff", d))
        except (BlockingIOError, socket.timeout): pass
        s.settimeout(2.0)
        time.sleep(0.01)
    check(lastz is not None and abs(lastz - 11.5) < 0.2, "standing on car roof z=%s" % lastz)
    # that 1.5 m drop onto the roof counts as a (light) landing hit
    check(any(h[1] == 7 and h[8] < 0.5 for h in landings), "landing on the car sends a light hit %s" % [round(h[8], 2) for h in landings])

    # hull: pickup-ish shape in the vehicle frame (x right, y fwd, z up): bed 0.9 m at the back, cab 1.5 m in front
    nx, ny, cell = 4, 8, 0.5
    top = [(1.5 if j >= 4 else 0.9) for j in range(ny) for i in range(nx)]
    s.sendto(b"U" + struct.pack("<I4fHHBB", 7, cell, -1, -2, 0, nx, ny, 0, 1) + struct.pack("<%df" % len(top), *top), dst)
    s.sendto(b"M" + struct.pack("<fff", -1.5, 5, 13), dst)
    zb = drain(2.0, True)
    s.sendto(b"M" + struct.pack("<fff", 1.5, 5, 13), dst)
    zc = drain(2.0, True)
    check(zb is not None and abs(zb - 10.9) < 0.05, "hull: lands in the bed z=%s" % zb)
    check(zc is not None and abs(zc - 11.5) < 0.05, "hull: lands on the cab roof z=%s" % zc)

    # inside the car (cab is 1.5 m tall, Mario placed at 0.5 m): pushed out through the nearest side
    def last_frame(seconds, keep_veh=True):
        f, want, got, end = None, max(1, int(seconds * 30)), 0, time.time() + seconds * 4
        while got < want and time.time() < end:
            if keep_veh: veh()
            s.setblocking(False)
            try:
                while True:
                    d, _ = s.recvfrom(65536)
                    if d[0:1] == b"F": f = struct.unpack_from("<BIIfffffffhIhhI", d); got += 1
            except (BlockingIOError, socket.timeout): pass
            s.settimeout(2.0)
            time.sleep(0.01)
        return f
    s.sendto(b"M" + struct.pack("<fff", 1.0, 5.2, 10.5), dst)
    f = last_frame(1.0)
    x, y, z = f[3], f[4], f[5]
    outside = abs(y - 5) > 1.0 + 0.3 or abs(x) > 2.0 + 0.3
    check(outside and z < 10.2, "pushed out of the car body to (%.2f, %.2f, %.2f)" % (x, y, z))

    # run over at 12 m/s along +x: thrown (SM64 ACT_THROWN_BACKWARD) and carried away in +x, not left inside
    s.sendto(b"M" + struct.pack("<fff", -3, -5, 10.2), dst)
    last_frame(1.0)
    s.sendto(b"K" + struct.pack("<fffBB", -4.5, -5, 10.7, 2, 0) + struct.pack("<fff", 12, 0, 0), dst)
    f0 = last_frame(0.15)
    f1 = last_frame(0.6)
    check(f0 is not None and f0[11] == 0x010208BE, "hit by car: thrown backward (action 0x%x)" % (f0[11] if f0 else 0))
    check(f1 is not None and f1[3] > -1.0, "thrown clear along the car's travel x=%.2f" % (f1[3] if f1 else 0))

    # carry: stand facing the car's side, Y -> SM64 pick-up (heavy: main body), hold messages, Y again -> heavy throw
    def collect(seconds, keep_veh=True):
        frames_, carries, want, end = [], [], int(seconds * 30), time.time() + seconds * 4
        while len(frames_) < want and time.time() < end:
            if keep_veh: veh()
            s.setblocking(False)
            try:
                for _ in range(400):
                    d, _ = s.recvfrom(65536)
                    if d[0:1] == b"F": frames_.append(struct.unpack_from("<BIIfffffffhIhhI", d))
                    elif d[0:1] == b"C": carries.append(struct.unpack_from("<BBIBB3ff3f", d))
            except (BlockingIOError, socket.timeout): pass
            s.settimeout(2.0)
            time.sleep(0.01)
        return frames_, carries
    s.sendto(b"M" + struct.pack("<fff", 0, 3.3, 10.2), dst)
    collect(1.0)
    s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -0.2, 0, 0, 0, 2, 0, 1), dst)   # face the car (+y)
    collect(0.4)
    s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, 1), dst)  # press Y
    fr, cs = collect(2.0)
    acts = {f[11] for f in fr}
    starts = [c for c in cs if c[1] == 1]; holds = [c for c in cs if c[1] == 2]
    check(0x383 in acts and 0x08000208 in acts, "Y: SM64 pick-up then heavy hold (actions %s)" % sorted(hex(a) for a in acts))
    check(starts and starts[0][2] == 7 and starts[0][4] == 1, "carry start sent for vehicle 7, heavy (%s)" % (starts[:1],))
    hz = holds[-1][7] if holds else None
    check(holds and hz is not None and 11.05 < hz < 11.35, "hold point rests just on top of his head (z %s, Mario at 10, cap ~11.14)" % hz)
    s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, 1), dst)  # Y again: throw
    fr, cs = collect(2.0)
    acts = {f[11] for f in fr}
    rel = [c for c in cs if c[1] == 3]
    check(0x80000589 in acts, "Y while carrying: SM64 heavy throw (actions %s)" % sorted(hex(a) for a in acts))
    check(rel and rel[0][10] > 8.0, "release sent with a forward throw velocity (vy %.1f m/s)" % (rel[0][10] if rel else 0))

    # wrecked into two pieces (cab in front, bed box behind, 2 m of nothing between them): two hulls, and the gap
    # between them is open ground, not an invisible floor
    cab = [1.5] * (4 * 2)
    bed = [0.9] * (4 * 2)
    s.sendto(b"U" + struct.pack("<I4fHHBB", 7, 0.5, -1, 1.0, 0, 4, 2, 0, 2) + struct.pack("<8f", *cab), dst)
    s.sendto(b"U" + struct.pack("<I4fHHBB", 7, 0.5, -1, -2.0, 0, 4, 2, 1, 2) + struct.pack("<8f", *bed), dst)
    s.sendto(b"M" + struct.pack("<fff", 0.0, 5, 13), dst)
    zg = drain(2.0, True)
    s.sendto(b"M" + struct.pack("<fff", 1.5, 5, 13), dst)
    zc = drain(2.0, True)
    s.sendto(b"M" + struct.pack("<fff", -1.5, 5, 13), dst)
    zb = drain(2.0, True)
    check(zg is not None and abs(zg - 10.0) < 0.05, "wreck: gap between pieces is open ground z=%s" % zg)
    check(zc is not None and abs(zc - 11.5) < 0.05 and zb is not None and abs(zb - 10.9) < 0.05, "wreck: both pieces still solid (cab z=%s, bed z=%s)" % (zc, zb))
finally:
    proc.kill()
    print(proc.stdout.read().decode(errors="replace")[-1500:])
print("FAILURES:", fails if fails else "none")
sys.exit(1 if fails else 0)
