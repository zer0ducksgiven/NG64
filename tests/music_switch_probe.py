"""Helper-only: flips through songs the way a player does (next song, over and over) and checks the song the sequence
player actually loaded is the one asked for each time - the helper logs both on every "music playing" line. A
queue that fills up (the old behaviour) leaves it on the same song from the sixth flip on.
Usage: python music_switch_probe.py <helper.exe> <rom> <user dir> [flips] [interval seconds] [--prev]"""
import socket, struct, subprocess, sys, time, re
args = [a for a in sys.argv[1:] if not a.startswith("--")]
exe, rom, user = args[0:3]
flips = int(args[3]) if len(args) > 3 else 14
gap = float(args[4]) if len(args) > 4 else 0.4
flag = 8 if "--prev" in sys.argv else 4
T0 = time.time()
proc = subprocess.Popen([exe, "--rom", rom, "--ignore-focus", "--port", "47097"], stdout=subprocess.DEVNULL)
time.sleep(1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.05); dst = ("127.0.0.1", 47097)
def drain(sec):
    end = time.time() + sec
    s.setblocking(False)
    while time.time() < end:
        try:
            for _ in range(2000): s.recv(65536)
        except (BlockingIOError, OSError): pass
        time.sleep(0.01)
s.sendto(b"H" + struct.pack("<H", 10) + user.encode(), dst)
n = 49; hs = [10.0] * (n * n)
s.sendto(b"T" + struct.pack("<fffH", 0, 0, 0.5, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)
drain(2)
for i in range(flips):
    print("flip", i, round(time.time() - T0, 1), flush=True)
    s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, flag), dst)
    drain(gap)
drain(2)
proc.kill()
log = open(exe.rsplit("/", 1)[0] + "/ng64helper.log").read().splitlines()
bad = 0
for l in log:
    m = re.search(r"music playing \(sequence 0x(\w+); player has 0x(\w+), queue (\d+)\)", l)
    if m:
        ok = m.group(1) == m.group(2)
        bad += not ok
        print(("ok   " if ok else "STUCK"), l)
    elif "song " in l or "underrun" in l or "audio" in l and "level" not in l:
        print("     ", l)
print("%d of %d song changes did not take" % (bad, sum("music playing" in l for l in log)))
