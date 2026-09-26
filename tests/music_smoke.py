"""Helper-only music check: Mario spawned with audio on -> music plays (log + output level); the toggle (script
flag 2) stops it and starts it again. Usage: python music_smoke.py <helper.exe> <rom> <user dir>"""
import socket, struct, subprocess, sys, time
exe, rom, user = sys.argv[1:4]
proc = subprocess.Popen([exe, "--rom", rom, "--ignore-focus", "--port", "47096"], stdout=subprocess.DEVNULL)
time.sleep(1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.05); dst = ("127.0.0.1", 47096)
def drain(sec):
    end = time.time() + sec
    while time.time() < end:
        try:
            for _ in range(500): s.recvfrom(65536)
        except (socket.timeout, BlockingIOError): pass
s.sendto(b"H" + struct.pack("<H", 6) + user.encode(), dst)
n = 49; hs = [10.0] * (n * n)
s.sendto(b"T" + struct.pack("<fffH", 0, 0, 0.5, n) + struct.pack("<%df" % len(hs), *hs), dst); time.sleep(0.1)
s.sendto(b"S" + struct.pack("<fff", 0, 0, 10.5), dst)
drain(11)
s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, 2), dst)   # toggle: off
drain(11)
s.sendto(b"I" + struct.pack("<ffBBBHffB", 0, 0, 0, 0, 0, 2, 0, 1, 2), dst)   # toggle: on
drain(6)
proc.kill()
log = open(exe.rsplit("/", 1)[0] + "/ng64helper.log").read().splitlines()
for l in log:
    if "music" in l or "audio level" in l: print(l)
