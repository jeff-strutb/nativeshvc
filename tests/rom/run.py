"""
run.py: build each test cartridge, run it on the oracle and on nativeshvc,
compare what they show, pixel for pixel, and what they read back.

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
OUT = os.path.join(OUTROOT, "rom")
ORACLE = os.environ.get("NSHVC_ORACLE", os.path.join(BUILD, "oracle"))
CART = os.environ.get("NSHVC_TESTCART", os.path.join(BUILD, "testcart"))
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
    cmd = [ORACLE, "--core", CORE, "--rom", rom, "--frames", str(t.frames), "--out", od,
           "--shot", ",".join(map(str, t.shots)), "--wram", os.path.join(od, "end.wram")]
    if getattr(t, "overscan", False):
        cmd.append("--overscan")
    subprocess.run(cmd, check=True, timeout=120)
    script = os.path.join(d, "script.txt")
    with open(script, "w") as f:
        at = 0
        for s in t.shots:
            f.write("wait %d\nshot frame_%d\n" % (s - at, s))
            at = s
        if t.frames > at:
            f.write("wait %d\n" % (t.frames - at))
        f.write("dump end\n")
    r = subprocess.run([CART, "--rom", rom, "--headless", "--script", script, "--out", nd],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return False, "nativeshvc exit %d: %s" % (r.returncode, r.stderr.strip()[-400:])
    ok, notes = True, []
    for s in t.shots:
        bad, first = compare(ppm(os.path.join(od, "frame_%d.ppm" % s)), ppm(os.path.join(nd, "frame_%d.ppm" % s)))
        if bad != 0:
            ok = False
            notes.append("frame %d: %s pixels differ, first %s" % (s, bad, first))
    if t.reads:
        with open(os.path.join(od, "end.wram"), "rb") as f:
            a = f.read()[0x2000:0x2000 + t.reads]
        with open(os.path.join(nd, "end.wram"), "rb") as f:
            b = f.read()[0x2000:0x2000 + t.reads]
        if a != b:
            ok = False
            k = next(i for i in range(len(a)) if a[i] != b[i])
            notes.append("read %d of %d differs: oracle %02X nativeshvc %02X" % (k, t.reads, a[k], b[k]))
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
