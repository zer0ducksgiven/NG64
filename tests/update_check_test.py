"""The update check, end to end against GitHub: the helper pretends to be an old version, the mod says hello, and
after ~5 s the helper must send the update notice (a "toastl:" log message to the mod). Also checks the opt-outs and that
the real current version gets no notice. Usage: python update_check_test.py <helper.exe> <rom> <user dir>"""
import os, socket, struct, subprocess, sys, time
exe, rom, user = sys.argv[1:4]
exe_dir = exe.rsplit("/", 1)[0]


def run(env_version, extra=(), seconds=14):
    env = dict(os.environ)
    if env_version: env["NG64_UPDATE_TEST_VERSION"] = env_version
    proc = subprocess.Popen([exe, "--rom", rom, "--no-audio", "--ignore-focus", "--port", "47090"] + list(extra), stdout=subprocess.DEVNULL, env=env)
    time.sleep(1.2)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False); dst = ("127.0.0.1", 47090)
    got = []
    try:
        s.sendto(b"H" + struct.pack("<H", 12) + user.encode(), dst)
        end = time.time() + seconds
        while time.time() < end:
            try:
                while True:
                    d = s.recv(65536)
                    if d[:1] == b"L" and b"toastl:" in d: got.append(d[1:-1].decode(errors="replace"))
            except (BlockingIOError, OSError): pass
            time.sleep(0.05)
    finally:
        proc.kill()
    return got


fails = 0
def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    fails += not cond

got = run("0.1.0")
check(len(got) == 1 and "is out (you have 0.1.0)" in got[0], "an old version is told a newer release exists: %s" % got)
got = run(None)
check(not got, "the current version gets no notice: %s" % got)
got = run("0.1.0", ["--no-update-check"])
check(not got, "--no-update-check: no notice")
flag = exe_dir + "/no-update-check.txt"
open(flag, "w").write("")
try:
    got = run("0.1.0")
    check(not got, "no-update-check.txt: no notice")
finally:
    os.remove(flag)
sys.exit(1 if fails else 0)
