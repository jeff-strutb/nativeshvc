"""
fuzz.py: the sound chip on its own: the native mixer against the
reference's chip, given what a program of every kind of use gives it.

A test cartridge (tests/rom/snestest.py's interpreter) uploads an SPC700
program through the boot ROM and starts it. The program fills the APU RAM
from $1000 with random bytes (sample data, directories), then for ever: a
random DSP register written with a random value, a random one read back, a
byte of the echo's delay line read, and a random pause, so the writes fall
on every clock of the chip's frame; each pass also reads the three timers'
counts, writes two ports and reads all four, and one in 32 sets the timers'
targets and which run. Every register is reached (KON, KOFF,
ENDX, ENVX and OUTX too); a few are kept where they leave the program
running: the echo's delay line inside $8000-$F7FF (ESA $80-$BF, EDL 0-7),
the directory in the random bytes (DIR $10-$4F), and FLG's reset and mute
bits set one write in sixteen.

The S-CPU, meanwhile, writes a port and reads another every 3 frames.

The reference runs it (tests/oracle: --sound, --voices, --spclog with every
read) and mix_replay holds the mixer to it: every sample, every voice's
state, every DSP register read and every read of the APU RAM. Then the same
cartridge runs on nativeshvc (tests/rom/testcart, whose SPC700 C is this
program's, tests/dsp/fuzz_spc.c, cycle for cycle) and the whole sound unit
is held to the reference: every sample it plays, every SPC700 access (its
time, the timers' counts and the ports' values it reads), and the S-CPU's
reads of the ports.

  python3 tests/dsp/fuzz.py [SEED...]       (default seeds 1-4; about 2 minutes)

Needs NSHVC_ORACLE_CORE and NSHVC_ASAR, as tests/rom/run.py.
"""
import concurrent.futures, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "rom"))
import snestest  # noqa: E402

BUILD = os.path.join(ROOT, "build", "tests", "dsp")
CART = os.environ.get("NSHVC_CART", os.path.join(ROOT, "build", "null", "tests", "testcart"))
ORACLE = os.environ.get("NSHVC_ORACLE", os.path.join(ROOT, "build", "null", "tests", "oracle"))
REPLAY = os.environ.get("NSHVC_MIX_REPLAY", os.path.join(ROOT, "build", "null", "tests", "mix_replay"))
CORE = os.environ.get("NSHVC_ORACLE_CORE")
FRAMES = int(os.environ.get("FUZZ_FRAMES", "3600"))
ORG = 0x0200

PROGRAM = r"""
arch spc700
norom
org 0
base $0200

; eight steps of a 16-bit Galois shift register ($00-$01), its low byte in A
; (a macro: the program makes no calls, so it pushes nothing)
macro rand()
    mov x,#$08
?step:
    lsr $01
    ror $00
    bcc ?next
    mov a,$01
    eor a,#$B4
    mov $01,a
?next:
    dec x
    bne ?step
    mov a,$00
endmacro

start:
    mov x,#$EF
    mov sp,x
    mov $00,#SEED_LO
    mov $01,#SEED_HI
    ; the random bytes, $1000-$7FFF
    mov $02,#$00
    mov $03,#$10
    mov y,#$00
fill:
    %rand()
    mov ($02)+y,a
    inc y
    bne fill
    inc $03
    mov a,$03
    cmp a,#$80
    bne fill
    ; a start: the echo's delay line at $8000 (taken at its clock, so a
    ; frame's pause before its writes go on), unmuted, the voices keyed on
    mov $06,#$80
    mov $f2,#$6D
    mov $f3,#$80
    mov $f2,#$7D
    mov $f3,#$02
    mov x,#$20
settle:
    dec x
    bne settle
    mov $f2,#$6C
    mov $f3,#$00
    mov $f2,#$5D
    mov $f3,#$20
    mov $f2,#$0C
    mov $f3,#$50
    mov $f2,#$1C
    mov $f3,#$50
    mov $f2,#$2C
    mov $f3,#$30
    mov $f2,#$3C
    mov $f3,#$D0
    mov $f2,#$4C
    mov $f3,#$FF
    ; the timers: targets, all three on
    mov $FA,#$10
    mov $FB,#$C0
    mov $FC,#$07
    mov $F1,#$07
main:
    ; a register written: kept where the program goes on running
    %rand()
    and a,#$7F
    mov $04,a
    %rand()
    mov $05,a
    mov a,$04
    cmp a,#$6D
    bne keep_edl
    mov a,$05
    and a,#$3F
    or a,#$80
    mov $05,a
    mov $06,a
    bra keep_end
keep_edl:
    cmp a,#$7D
    bne keep_dir
    mov a,$05
    and a,#$07
    mov $05,a
    bra keep_end
keep_dir:
    cmp a,#$5D
    bne keep_flg
    mov a,$05
    and a,#$3F
    or a,#$10
    mov $05,a
    bra keep_end
keep_flg:
    cmp a,#$6C
    bne keep_end
    %rand()
    and a,#$0F
    beq keep_end
    mov a,$05
    and a,#$3F
    mov $05,a
keep_end:
    mov a,$04
    mov $f2,a
    mov a,$05
    mov $f3,a
    ; a register read back
    %rand()
    and a,#$7F
    mov $f2,a
    mov a,$f3
    ; a byte of the echo's delay line
    %rand()
    mov $EC,a
    %rand()
    and a,#$07
    clrc
    adc a,$06
    mov $ED,a
    mov y,#$00
    mov a,($EC)+y
    ; the timers' counts (each read clears it); the ports, two written, all
    ; read
    mov a,$FD
    mov a,$FE
    mov a,$FF
    %rand()
    mov $F4,a
    %rand()
    mov $F7,a
    mov a,$F4
    mov a,$F5
    mov a,$F6
    mov a,$F7
    ; one pass in 32: the timers' targets, and which run
    %rand()
    and a,#$1F
    bne timers_kept
    %rand()
    mov $FA,a
    %rand()
    mov $FB,a
    %rand()
    mov $FC,a
    %rand()
    and a,#$07
    mov $F1,a
timers_kept:
    ; a pause of 1-32 turns
    %rand()
    and a,#$1F
    mov x,a
    inc x
pause:
    dec x
    bne pause
    jmp main
"""


def program(seed, outdir):
    asm = os.path.join(outdir, "spc_%d.asm" % seed)
    bin_path = os.path.join(outdir, "spc_%d.bin" % seed)
    s = (seed * 2654435761) & 0xFFFF or 1
    with open(asm, "w") as f:
        f.write(PROGRAM.replace("SEED_LO", "$%02X" % (s & 0xFF)).replace("SEED_HI", "$%02X" % (s >> 8)))
    subprocess.run([snestest.ASAR, "--no-title-check", asm, bin_path], check=True, stdout=subprocess.DEVNULL)
    with open(bin_path, "rb") as f:
        return f.read()


def cartridge(seed, outdir):
    """the upload by the boot ROM's protocol, then the S-CPU waits"""
    code = program(seed, outdir)
    t = snestest.Test("dsp_fuzz_%d" % seed, frames=FRAMES, shots=(), prologue=False)
    t.poll(0x2140, 0xAA)
    t.w(0x2142, ORG & 0xFF)
    t.w(0x2143, ORG >> 8)
    t.w(0x2141, 0x01)
    t.w(0x2140, 0xCC)
    t.poll(0x2140, 0xCC)
    for i, b in enumerate(code):
        t.w(0x2141, b)
        t.w(0x2140, i & 0xFF)
        t.poll(0x2140, i & 0xFF)
    t.w(0x2142, ORG & 0xFF)
    t.w(0x2143, ORG >> 8)
    t.w(0x2141, 0x00)
    t.w(0x2140, (len(code) + 2) & 0xFF or 2)
    for i in range(FRAMES // 3):
        t.w(0x2140 + i % 4, t.rng.getrandbits(8))
        t.read(0x2140 + (i + 1) % 4)
        t.wait_frames(3)
    return t


def one(seed):
    d = os.path.join(BUILD, str(seed))
    os.makedirs(d, exist_ok=True)
    t = cartridge(seed, d)
    rom = t.build(d)
    r = subprocess.run([ORACLE, "--core", CORE, "--rom", rom, "--frames", str(FRAMES), "--out", d,
                        "--sound", os.path.join(d, "sound.raw"), "--voices", os.path.join(d, "voices.bin"),
                        "--spclog", os.path.join(d, "spc.log"), "--spcreads", "--wram", os.path.join(d, "end.wram")],
                       capture_output=True, text=True, timeout=1200)
    if r.returncode:
        return seed, False, "reference: " + r.stderr.strip()[-300:]
    q = subprocess.run([REPLAY, os.path.join(d, "spc.log"), os.path.join(d, "sound.raw"),
                        os.path.join(d, "voices.bin")], capture_output=True, text=True, timeout=1200)
    notes = ["the mixer against the reference's chip: " + (q.stderr + q.stdout).strip().replace("\n", "; ")]
    ok = q.returncode == 0
    native_ok, native_notes = native(t, d)
    return seed, ok and native_ok, "\n".join(notes + native_notes)


def records(path):
    with open(path, "rb") as f:
        return list(struct.iter_unpack("<IBHB", f.read()))


def native(t, d):
    """the cartridge on nativeshvc: its sound, its SPC700's accesses (the
       reference's writes and reads of $F0-$FF; a port read the reference
       times between its cycle's halves) and the S-CPU's port reads"""
    nd = os.path.join(d, "native")
    os.makedirs(nd, exist_ok=True)
    script = os.path.join(nd, "script.txt")
    with open(script, "w") as f:
        f.write("wait %d\ndump end\n" % FRAMES)
    env = dict(os.environ, NSHVC_SPCLOG=os.path.join(nd, "spc.log"))
    r = subprocess.run([CART, "--rom", os.path.join(d, t.name + ".sfc"), "--headless", "--script", script, "--out",
                        nd, "--sound", os.path.join(nd, "sound.raw")], capture_output=True, text=True, timeout=1200,
                       env=env)
    if r.returncode:
        return False, ["nativeshvc exit %d: %s" % (r.returncode, r.stderr.strip()[-300:])]
    ok, notes = True, []
    ref = [x for x in records(os.path.join(d, "spc.log")) if x[1] or 0xF0 <= x[2] <= 0xFF]
    nat = records(os.path.join(nd, "spc.log"))
    n = min(len(ref), len(nat))
    port = lambda x: not x[1] and 0xF4 <= x[2] <= 0xF7
    k = next((i for i in range(n) if (ref[i][0] + port(ref[i]),) + ref[i][1:] != nat[i]), None)
    if k is not None:
        ok = False
        notes.append("SPC700 access %d of %d differs: reference %s, nativeshvc %s" % (k, n, ref[k], nat[k]))
    else:
        notes.append("SPC700 accesses: all %d the same (time, address, value)" % n)
    with open(os.path.join(d, "sound.raw"), "rb") as f:
        a = f.read()
    with open(os.path.join(nd, "sound.raw"), "rb") as f:
        b = f.read()
    m = min(len(a), len(b)) // 4 * 4
    k = next((i for i in range(0, m, 4) if a[i:i + 4] != b[i:i + 4]), None)
    if k is not None:
        ok = False
        notes.append("sound: sample %d of %d differs" % (k // 4, m // 4))
    else:
        notes.append("sound: all %d samples the same (reference %d, nativeshvc %d made)" % (m // 4, len(a) // 4,
                                                                                         len(b) // 4))
    with open(os.path.join(d, "end.wram"), "rb") as f:
        a = f.read()[0x2000:0x2000 + t.reads]
    with open(os.path.join(nd, "end.wram"), "rb") as f:
        b = f.read()[0x2000:0x2000 + t.reads]
    k = next((i for i in range(t.reads) if a[i] != b[i]), None)
    if k is not None:
        ok = False
        notes.append("S-CPU port read %d of %d differs: reference %02X, nativeshvc %02X" % (k, t.reads, a[k], b[k]))
    else:
        notes.append("S-CPU port reads: all %d the same" % t.reads)
    return ok, notes


def main():
    if not CORE or not snestest.ASAR:
        sys.exit("fuzz.py: set NSHVC_ORACLE_CORE and NSHVC_ASAR")
    seeds = [int(a) for a in sys.argv[1:]] or [1, 2, 3, 4]
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(len(seeds)) as ex:
        for seed, ok, out in ex.map(one, seeds):
            print("seed %d: %s\n  %s" % (seed, "ok" if ok else "DIFFERS", out.replace("\n", "\n  ")), flush=True)
            bad += not ok
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
