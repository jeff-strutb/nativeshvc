"""
run.py: build each test cartridge and run it on the oracle, recording its
S-CPU's reads and writes with their clocks; make the same accesses on
nativeshvc at the same clocks (replay.c); compare what the two show, pixel
for pixel, and every read. nativeshvc's own CPU is untimed (README.md,
Time): this holds its hardware to the reference's to the clock.

  python3 tests/rom/run.py [--only NAME,NAME] [--keep]

Needs NSHVC_ORACLE_CORE (a bsnes libretro core) and NSHVC_ASAR (asar);
the oracle runner and the test cartridge are built under build/tests by
tests/CMakeLists.txt (cmake -DNSHVC_TESTS=ON).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import snestest  # noqa: E402
import cases  # noqa: E402

BUILD = os.environ.get("NSHVC_TESTBUILD", os.path.join(ROOT, "build", "null", "tests"))
OUTROOT = os.path.join(ROOT, "build", "tests")
OUT = os.environ.get("NSHVC_TESTOUT", os.path.join(OUTROOT, "rom"))
ORACLE = os.environ.get("NSHVC_ORACLE", os.path.join(BUILD, "oracle"))
REPLAY = os.environ.get("NSHVC_REPLAY", os.path.join(BUILD, "replay"))
CORE = os.environ.get("NSHVC_ORACLE_CORE")


def ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def compare(a, b):
    """the pixels that differ, and the first one"""
    aw, ah, ap = a
    bw, bh, bp = b
    if (aw, ah) != (bw, bh):
        return None, "size %dx%d vs %dx%d" % (aw, ah, bw, bh)
    bad, first = 0, None
    for i in range(0, len(ap), 3):
        if ap[i:i + 3] != bp[i:i + 3]:
            bad += 1
            if first is None:
                p = i // 3
                first = "(%d,%d) oracle %s nativeshvc %s" % (p % aw, p // aw, ap[i:i + 3].hex(), bp[i:i + 3].hex())
    return bad, first


def run_one(t):
    d = os.path.join(OUT, t.name)
    os.makedirs(d, exist_ok=True)
    rom = t.build(d)
    od, nd = os.path.join(d, "oracle"), os.path.join(d, "native")
    os.makedirs(od, exist_ok=True)
    os.makedirs(nd, exist_ok=True)
    script = os.path.join(d, "script.txt")
    with open(script, "w") as f:
        if t.inputs:
            # the controllers' steps before each frame, the same script on both
            for n in range(1, t.frames + 1):
                lines = t.inputs.get(n, ())
                for line in lines:
                    f.write(line + "\n")
                if not any(line.startswith("hold") for line in lines):
                    f.write("wait 1\n")
                if n in t.shots:
                    f.write("shot frame_%d\n" % n)
        else:
            at = 0
            for s in t.shots:
                f.write("wait %d\nshot frame_%d\n" % (s - at, s))
                at = s
            if t.frames > at:
                f.write("wait %d\n" % (t.frames - at))
        f.write("dump end\n")
    log = os.path.join(od, "cpu.log")
    if t.inputs:
        cmd = [ORACLE, "--core", CORE, "--rom", rom, "--script", script, "--out", od, "--cpulog", log]
    else:
        cmd = [ORACLE, "--core", CORE, "--rom", rom, "--frames", str(t.frames), "--out", od,
               "--shot", ",".join(map(str, t.shots)), "--cpulog", log]
    if getattr(t, "overscan", False):
        cmd.append("--overscan")
    subprocess.run(cmd, check=True, timeout=120)
    r = subprocess.run([REPLAY, "--rom", rom, "--log", log, "--headless", "--script", script, "--out", nd],
                       capture_output=True, text=True, timeout=300)
    lines = [x for x in r.stderr.splitlines() if x.startswith("replay:")]
    if r.returncode not in (0, 1) or not lines:
        return False, "nativeshvc exit %d: %s" % (r.returncode, r.stderr.strip()[-400:])
    ok, notes = r.returncode == 0, []
    if not ok:
        # the summary (the last line), then the first read or time that differs
        notes.append(lines[-1][len("replay: "):])
        if len(lines) > 1:
            notes.append(lines[0][len("replay: "):])
    for s in t.shots:
        bad, first = compare(ppm(os.path.join(od, "frame_%d.ppm" % s)), ppm(os.path.join(nd, "frame_%d.ppm" % s)))
        if bad != 0:
            ok = False
            notes.append("frame %d: %s pixels differ, first %s" % (s, bad, first))
    return ok, "; ".join(notes)


def main():
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    if not CORE or not snestest.ASAR:
        sys.exit("run.py: set NSHVC_ORACLE_CORE and NSHVC_ASAR")
    known = {}
    with open(os.path.join(HERE, "KNOWN")) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                name, _, why = line.strip().partition(" ")
                known[name] = why.strip()
    tests = [t for t in cases.all_tests() if not only or t.name in only]
    fails = kfails = fixed = 0
    for t in tests:
        ok, note = run_one(t)
        if t.name in known:
            if ok:
                fixed += 1
                print("%-28s ok, but listed in KNOWN (take it out)" % t.name)
            else:
                kfails += 1
                print("%-28s KNOWN %s (%s)" % (t.name, note, known[t.name]))
            continue
        print("%-28s %s %s" % (t.name, "ok" if ok else "FAIL", note))
        fails += not ok
    print("%d of %d differ; %d more differ as KNOWN lists; %d listed in KNOWN now pass"
          % (fails, len(tests), kfails, fixed))
    sys.exit(1 if fails or fixed else 0)


if __name__ == "__main__":
    main()
