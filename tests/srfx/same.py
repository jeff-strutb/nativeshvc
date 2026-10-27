"""
same.py: Stunt Race FX's decompilation on nativeshvc against the original
cartridge on the reference (bsnes, tests/oracle), play for play: every shot,
the work RAM and Super FX RAM dumps and the pacing record, byte for byte.
A play that differs is reported with where it first does: the first pacing
record (the race frame) and the first file.

  python3 tests/srfx/same.py [PLAY...]

SRFX (default ../stuntracefx) is the decomp's checkout (its plays and the
player's ROM); NSHVC_ORACLE_CORE the reference core, as tests/rom/run.py.
"""
import concurrent.futures, filecmp, glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRFX = os.path.abspath(os.environ.get("SRFX", os.path.join(ROOT, "..", "stuntracefx")))
ROM = os.path.join(SRFX, "reference", "stuntracefx", "Stunt Race FX (USA) (Rev 1).sfc")
CORE = os.environ.get("NSHVC_ORACLE_CORE")
ORACLE = os.environ.get("NSHVC_ORACLE", os.path.join(ROOT, "build", "null", "tests", "oracle"))
GAME = os.environ.get("GAME", "src/stuntracefx")
EXE = os.path.join(ROOT, "build", "srfx", GAME.replace("/", "_"), "srfx")
WORK = os.path.join(ROOT, "build", "srfx", GAME.replace("/", "_"), "same")


def script_for(path):
    out = os.path.join(WORK, "scripts", os.path.basename(path))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(path) as f, open(out, "w") as g:
        for line in f:
            if line.split()[:1] not in (["save"], ["load"]):
                g.write(line)
        g.write("\nshot same_end\ndump same_end\n")
    return out


def run(cmd, out):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    p = subprocess.run(cmd + ["--out", out, "--pacing", os.path.join(out, "pacing.txt")],
                       capture_output=True, text=True, timeout=1800)
    return p.returncode, p.stderr[-600:]


def first_pacing(a, b):
    """the first pacing record that differs (its frame), or None"""
    la, lb = open(a).read().splitlines(), open(b).read().splitlines()
    for x, y in zip(la, lb):
        if x != y:
            return x.split()[0]
    return None if len(la) == len(lb) else "the end (%d records against %d)" % (len(lb), len(la))


def one(path):
    name = os.path.splitext(os.path.basename(path))[0]
    s = script_for(path)
    a, b = os.path.join(WORK, name, "bsnes"), os.path.join(WORK, name, "nshvc")
    ra, ea = run([ORACLE, "--core", CORE, "--rom", ROM, "--script", s], a)
    rb, eb = run([EXE, "--rom", ROM, "--script", s, "--headless"], b)
    if ra or rb:
        return name, "exit reference %d nativeshvc %d: %s" % (ra, rb, (eb or ea).strip().splitlines()[-1:])
    names = sorted(os.listdir(a))
    for f in names:
        if not os.path.exists(os.path.join(b, f)):
            return name, "nativeshvc made no %s" % f
    bad = [f for f in names if not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False)]
    if not bad:
        return name, "same (%d files)" % len(names)
    at = first_pacing(os.path.join(a, "pacing.txt"), os.path.join(b, "pacing.txt"))
    return name, "%d of %d files differ: %s%s" % (len(bad), len(names), " ".join(bad[:6]),
                                                 "; pacing first differs at frame %s" % at if at else "")


def main(argv):
    if not CORE:
        sys.exit("same.py: set NSHVC_ORACLE_CORE")
    for p in (ORACLE, EXE, ROM):
        if not os.path.exists(p):
            sys.exit("same.py: no %s" % p)
    plays = argv[1:] or sorted(glob.glob(os.path.join(SRFX, "tools", "stuntracefx", "plays", "*.sfx")))
    plays = [p for p in plays if "poke" not in open(p).read()]
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 2) // 2)) as ex:
        for name, verdict in ex.map(one, plays):
            print("%-26s %s" % (name, verdict), flush=True)
            bad += not verdict.startswith("same")
    print("%d of %d plays differ" % (bad, len(plays)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
