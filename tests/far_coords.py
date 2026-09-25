"""Spawns Mario on flat ground at increasing distances from the map origin and reports his mesh size and state."""
import socket, struct, subprocess, time, sys
exe, rom, user = sys.argv[1:4]
proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--port", "47097"], stdout=subprocess.DEVNULL)
time.sleep(1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.05); dst = ("127.0.0.1", 47097)
s.sendto(b"H" + struct.pack("<H", int(sys.argv[4]) if len(sys.argv) > 4 else 4) + user.encode(), dst)
def frames(n):
    out = []; end = time.time() + 6
    while len(out) < n and time.time() < end:
        try:
            for _ in range(200):
                d, _ = s.recvfrom(65536)
                if d[0:1] == b"F": out.append(struct.unpack_from("<BIIfffffffhIhhIffffffH", d))
                elif d[0:1] == b"G":
                    k, seq, st, cnt = struct.unpack_from("<IIHH", d, 1)
                    for i in range(cnt):
                        p = struct.unpack_from("<3h", d, 13 + i * 13)
                        verts.append(p)
        except (socket.timeout, BlockingIOError): pass
    return out
try:
    for dist in (0, 250, 500, 1000, 2000):
        x, y, z = dist, -dist * 0.7, 100.0
        n = 49; hs = [z] * (n * n)
        s.sendto(b"T" + struct.pack("<fffH", x, y, 0.5, n) + struct.pack("<%df" % len(hs), *hs), dst)
        time.sleep(0.05)
        s.sendto(b"S" + struct.pack("<fff", x, y, z + 0.5), dst)
        verts = []; frames(30); verts = []
        s.sendto(b"I" + struct.pack("<ffBBBHff", 0, -1, 0, 0, 0, 30, 1, 0), dst)   # run
        f = frames(30)
        last = f[-1]
        ext = [max(v[k] for v in verts) - min(v[k] for v in verts) for k in range(3)] if verts else None
        print("dist %5d m: pos (%.1f, %.1f, %.2f) action 0x%x anim %d frame %d | mesh extent mm %s | moved %.2f m" % (
            dist, last[3], last[4], last[5], last[11], last[12], last[13], ext, abs(last[3] - x)))
finally:
    proc.kill()
