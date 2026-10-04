"""Helper-only: does the music hold up when the machine is busy (a demanding map)? Starts the helper with Mario and
music playing, loads the CPU with busy loops, and reads the helper's own audio stats ("audio: N underruns, longest
wait ...", logged every 5 s). Underruns are audible hitches.
Usage: python audio_load_probe.py <helper.exe> <rom> <user dir> [seconds] [burners]"""
import socket, struct, subprocess, sys, time, re, os
args = [a for a in sys.argv[1:] if not a.startswith("--")]
exe, rom, user = args[0:3]
secs = float(args[3]) if len(args) > 3 else 40
burners = int(args[4]) if len(args) > 4 else (os.cpu_count() or 4)

proc = subprocess.Popen([exe, "--rom", rom, "--ignore-focus", "--port", "47098"], stdout=subprocess.DEVNULL)
time.sleep(1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); dst = ("127.0.0.1", 47098)
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
drain(6)   # music starts, steady state
burn = []

for _ in range(burners):
    burn.append(subprocess.Popen([sys.executable, "-c", "while True: pass"]))
try:
    drain(secs)
finally:
    for b in burn: b.kill()
    drain(1)
    proc.kill()
log = open(exe.rsplit("/", 1)[0] + "/ng64helper.log").read().splitlines()
last = None
worst = 0
for l in log:
    m = re.search(r"audio: (\d+) underrun\(s\) so far, longest wait between audio loops (\d+) ms", l)
    if m:
        last = int(m.group(1)); worst = max(worst, int(m.group(2)))
print("%s: %d burners%s, %.0f s: %s underrun(s), longest audio-loop wait %d ms" % (
    os.path.basename(exe), burners, "", secs, last, worst))
