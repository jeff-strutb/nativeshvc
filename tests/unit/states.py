#!/usr/bin/env python3
"""states.py: the script's save, load and calls, with fork and without
(NSHVC_SCRIPT_NOFORK=1, as on Windows), on tests/unit/states.c: every dump
the same both ways, byte for byte, and each the machine it should be.

    python3 tests/unit/states.py [BUILD]     (BUILD: build/null)
"""
import os
import subprocess
import sys
import tempfile

SCRIPT = """\
hold B 10
dump a
save s1
wait 5
dump b
set 7E0103 AA
call 009000
dump c
call 009000 keep
dump d
load s1
dump e
hold B 3
dump f
load s1
call 009000 keep
save s2
wait 2
dump g
load s2
dump h
"""

build = sys.argv[1] if len(sys.argv) > 1 else "build/null"
binary = os.path.abspath(os.path.join(build, "tests", "states"))
fails = 0


def check(ok, what):
    global fails
    print("ok  " if ok else "FAIL", what)
    fails += not ok


def run(nofork, d):
    path = os.path.join(d, "s.txt")
    with open(path, "w") as f:
        f.write(SCRIPT)
    env = dict(os.environ)
    for k in ("NSHVC_SCRIPT_NOFORK", "NSHVC_SCRIPT_PLAN", "NSHVC_SCRIPT_RESUME", "NSHVC_SCRIPT_SAVED",
              "NSHVC_SCRIPT_FRAMES"):
        env.pop(k, None)
    if nofork:
        env["NSHVC_SCRIPT_NOFORK"] = "1"
    out = os.path.join(d, "out")
    os.makedirs(out)
    r = subprocess.run([binary, "--headless", "--script", path, "--out", out], env=env, timeout=120,
                       capture_output=True, text=True)
    check(r.returncode == 0, "%s: the run ends well (%d) %s" % ("no fork" if nofork else "fork", r.returncode,
                                                                 r.stderr.strip()[-300:]))
    dumps = {}
    for n in "abcdefgh":
        for ext in (".wram", ".vram", ".cgram", ".oam"):
            p = os.path.join(out, n + ext)
            dumps[n + ext] = open(p, "rb").read() if os.path.exists(p) else None
    return dumps


with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
    forked = run(False, d1)
    spawned = run(True, d2)

for k in sorted(forked):
    check(forked[k] is not None and forked[k] == spawned[k], "%s: the same with fork and without" % k)


def w(n, off):
    return forked[n + ".wram"][off]


check(w("a", 0x100) == w("e", 0x100) and forked["a.wram"] == forked["e.wram"], "load s1: the machine as saved")
check(w("c", 0x101) == w("b", 0x101) and forked["b.wram"][:0x103] == forked["c.wram"][:0x103],
      "a call without keep: the machine as it was before it")
check(w("c", 0x103) == 0x00, "a call without keep: its set bytes gone with it")
check(w("d", 0x101) == (w("c", 0x101) + 0x10) & 0xFF, "a call with keep: its change kept")
check(w("f", 0x100) == (w("e", 0x100) + 3) & 0xFF and w("f", 0x102) != w("e", 0x102),
      "after the load, the frames and the buttons held count on from it")
check(w("h", 0x101) == (w("a", 0x101) + 0x10) & 0xFF and forked["h.wram"] != forked["g.wram"],
      "load s2, saved after a load and a kept call: that machine")
print("%d check(s) failed" % fails)
sys.exit(1 if fails else 0)
