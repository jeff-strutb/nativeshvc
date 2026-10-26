"""
same.py: Stunt Race FX's decompilation on nativeshvc against Stunt Race
FX's own port (its reference build, ref-0ebd2401), play for play: every
shot, the work RAM and Super FX RAM dumps and the pacing record, byte for
byte.

  python3 tests/srfx/same.py [PLAY...]

SRFX (default ../stuntracefx) is the decomp's checkout; its reference build
must exist (its ports/stuntracefx/tools/same.py builds it).
"""
import concurrent.futures, filecmp, glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRFX = os.path.abspath(os.environ.get("SRFX", os.path.join(ROOT, "..", "stuntracefx")))
ROM = os.path.join(SRFX, "reference", "stuntracefx", "Stunt Race FX (USA) (Rev 1).sfc")
REF = os.path.join(SRFX, "build", "stuntracefx", "port", "ref-0ebd2401", "stuntracefx")
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
    p = subprocess.run(cmd + ["--headless", "--out", out, "--pacing", os.path.join(out, "pacing.txt")],
                       capture_output=True, text=True, timeout=1800)
    return p.returncode, p.stderr[-600:]


def one(path):
    name = os.path.splitext(os.path.basename(path))[0]
    s = script_for(path)
    a, b = os.path.join(WORK, name, "ref"), os.path.join(WORK, name, "nshvc")
    ra, ea = run([REF, "--script", s], a)
    rb, eb = run([EXE, "--rom", ROM, "--script", s], b)
    if ra or rb:
        return name, "exit ref %d nativeshvc %d: %s" % (ra, rb, (eb or ea).strip().splitlines()[-1:] )
    # the reference names the Super FX's RAM .gsuram, nativeshvc the cartridge's RAM .sram
    pairs = []
    for f in sorted(os.listdir(a)):
        g = f[:-len(".gsuram")] + ".sram" if f.endswith(".gsuram") else f
        if not os.path.exists(os.path.join(b, g)):
            return name, "nativeshvc made no %s" % g
        pairs.append((f, g))
    bad = [f for f, g in pairs if not filecmp.cmp(os.path.join(a, f), os.path.join(b, g), shallow=False)]
    return name, ("%d of %d files differ: %s" % (len(bad), len(pairs), " ".join(bad[:6]))) if bad else "same (%d files)" % len(pairs)


def main(argv):
    if not os.path.exists(REF):
        sys.exit("same.py: no reference build at %s" % REF)
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
