"""Headless check of what Y reaches on a wreck: a car broken into a big main body and a small loose part (say a wheel) that
lies nearer Mario than the body. A TAP must lift the nearest thing (the loose part, piece 1); a HOLD must grab the
main body (piece 0) to spin it. Usage: python wreck_pick_test.py <helper.exe> <rom> <user dir>"""
import socket, struct, subprocess, sys, time, re
exe, rom, user = sys.argv[1:4]
PORT = 47091
fails = 0


def run(mode):
    proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--port", str(PORT)], stdout=subprocess.DEVNULL)
    time.sleep(1.2)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); dst = ("127.0.0.1", PORT)
    s.setblocking(False)
    try:
        def pump(sec, vehicle=True):
            end = time.time() + sec
            while time.time() < end:
                if vehicle:
                    # vehicle 7: origin (0, 0, 10), forward +y, up +z (so right is +x); OOBB over the whole wreck
                    s.sendto(b"V" + struct.pack("<H", 1) + struct.pack("<I3f9f3f3f3f", 7, 0, 1.5, 10.75, 1.5, 0, 0, 0, 2.5, 0, 0, 0, 0.75, 0, 0, 10, 0, 1, 0, 0, 0, 1), dst)
                    # piece 0: the main body, y 0..3, 1.5 m tall. piece 1: a loose part, y -0.9..-0.4, 0.9 m tall
                    s.sendto(b"U" + struct.pack("<I4fHHBB", 7, 0.5, -1.0, 0.0, 0, 4, 6, 0, 2) + struct.pack("<24f", *([1.5] * 24)), dst)
                    s.sendto(b"U" + struct.pack("<I4fHHBB", 7, 0.5, -0.5, -0.9, 0, 2, 1, 1, 2) + struct.pack("<2f", 0.9, 0.9), dst)
                try:
                    while True: s.recv(65536)
                except (BlockingIOError, OSError): pass
                time.sleep(0.05)
        s.sendto(b"H" + struct.pack("<H", 11) + user.encode(), dst)
        n = 49; hs = [10.0] * (n * n)
        s.sendto(b"T" + struct.pack("<fffH", 0, 0, 0.5, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
        s.sendto(b"S" + struct.pack("<fff", 0, -1.0, 10.5), dst)
        pump(2.0)
        s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, -0.3, 0, 0, 0, 14, 0, 1, 0), dst)   # turn to face +y
        pump(1.2)
        if mode == "tap":
            s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, 1), dst)   # Y pulse
        else:
            s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 45, 0, 1, 16), dst)   # Y held 1.5 s
        pump(2.5)
    finally:
        proc.kill()
    log = open(exe.rsplit("/", 1)[0] + "/ng64helper.log").read().splitlines()
    return [l for l in log if re.search(r"picked up|grabbed|nothing in reach|Y: nearest", l)]


for mode, want in (("tap", r"picked up vehicle 7 piece 1"), ("hold", r"grabbed vehicle 7 to spin")):
    lines = run(mode)
    ok = any(re.search(want, l) for l in lines)
    print("%s %s: %s" % ("PASS" if ok else "FAIL", mode, lines[-2:] if lines else "(nothing logged)"))
    fails += not ok
sys.exit(1 if fails else 0)
