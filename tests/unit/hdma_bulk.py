"""
hdma_bulk.py: a wait's HDMA lines made in bulk against the same lines made one
at a time (hdma_bulk_check.c): every scene's every frame the same.

  python3 tests/unit/hdma_bulk.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.environ.get("NSHVC_TESTBUILD", os.path.join(ROOT, "build", "null", "tests"))
PROGRAM = os.path.join(BUILD, "hdma_bulk_check")
SCENES, FRAMES = 16, 3


def lines(*args):
    r = subprocess.run([PROGRAM, *args], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        sys.exit("hdma_bulk: %s exit %d: %s" % (" ".join(args) or "bulk", r.returncode, r.stderr.strip()[-400:]))
    return r.stdout.splitlines()


def main():
    bulk, stepped = lines(), lines("--stepped")
    if len(bulk) != SCENES * FRAMES or len(stepped) != len(bulk):
        sys.exit("hdma_bulk: %d and %d lines, %d wanted" % (len(bulk), len(stepped), SCENES * FRAMES))
    bad = 0
    for a, b in zip(bulk, stepped):
        if a != b:
            bad += 1
            if bad <= 8:
                print("bulk    %s\nstepped %s" % (a, b))
    print("%d of %d frames differ" % (bad, len(bulk)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
