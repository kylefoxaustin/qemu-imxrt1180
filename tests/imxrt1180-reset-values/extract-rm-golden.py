#!/usr/bin/env python3
"""Build a reset-value GOLDEN from the i.MX RT1180 Reference Manual + its CMSIS headers.

    AN ORACLE WE DID NOT AUTHOR.  That is the entire point.

Ported from mcxn947qemu's tests/mcxn-reset-values/extract-rm-golden.py (2026-07-12,
shared on the fleet bus).  Their finding, in their words:

    "A test that gets its addresses from the MODEL is not a test, it is a MIRROR,
     and mutation testing is blind to it BY CONSTRUCTION -- mutate the model and the
     mirror moves with it."

We proved that the same day and needed no convincing.  Our eDMA channel registers sat
at base + 0x1000*(n+1) -- the MCXN947's geometry, inherited when the model was adapted
from it and never re-derived from the RT1180 header.  The real addresses are
base + 0x10000 + n*0x10000 (DMA3) and base + 0x10000 + n*0x8000 (DMA4).  EVERY eDMA
TEST WAS GREEN, because every test used the model's own addresses.  ALL SIXTEEN
MUTATIONS CAME BACK "CAUGHT".  The stock NXP driver hung on the first register it
touched.

So this golden comes from two sources the model's author did not write:
    * the RM's reset-value column   -> what each register READS at reset
    * the CMSIS headers             -> WHERE that register lives
and check.py reads every one of them back out of a running machine.

------------------------------------------------------------------------------
USAGE
    pdftotext -f 1 -l 9999 reference/IMXRT1180RM.pdf rm.txt
    ./extract-rm-golden.py rm.txt rm-golden.json
------------------------------------------------------------------------------
TWO THINGS THE RT1180 DOES THAT THE MCX DOES NOT.  Both would have poisoned the
golden SILENTLY, and a poisoned golden is worse than none -- it makes the CHECKER
lie, and then your oracle is the thing that needs an oracle.

 1. THE CMSIS IS SPLIT.  MCX has one header.  The RT1180 puts its 107 peripheral
    typedefs in devices/.../periph/PERI_*.h and its ~496 base addresses in
    MIMXRT1189_cm33_COMMON.h.  Point the original parser at either file alone and it
    finds registers with no bases, or bases with no registers, and emits a confident
    EMPTY golden -- which passes.

 2. TRUSTZONE DEFINES EVERY BASE TWICE.

        #if (__ARM_FEATURE_CMSE & 0x2)
          #define DMA3_BASE      (0x54000000u)   <- SECURE alias
          #define DMA3_BASE_NS   (0x44000000u)
        #else
          #define DMA3_BASE      (0x44000000u)   <- non-secure
        #endif

    A regex that builds a dict keeps whichever the FILE HAPPENS TO DEFINE LAST.  Here
    that lands on the non-secure address -- correctly, BY ACCIDENT OF FILE ORDERING.
    Reorder the header and every probe silently moves to the secure alias.  So we do
    not lean on it: a secure/non-secure pair differs by exactly
    FSL_MEM_M33_SECURE_ADDRESS_OFFSET (0x10000000, fsl_memory.h), and we take the
    non-secure member of any such pair -- DERIVED, not lucky.

------------------------------------------------------------------------------
COVERAGE IS A FLOOR, NOT A CEILING.  mcxn's rule, and they paid full price for it:

    "YOU WILL ASSUME IT PROBES EVERY REGISTER.  IT DOES NOT, AND IT WILL NOT TELL
     YOU UNLESS YOU MAKE IT."

The RM prints array registers as a RANGE on one line; CMSIS stores them as ONE array
field.  A naive parser matches NEITHER, so every array register on the chip is
invisible -- silently.  They shipped it that way, got a confident PASS, and the
registers they had just finished fixing were among the ones it could not see.

Hence the ANCHORS below, asserted before a single byte is emitted.  A parser is a
MODEL OF A DOCUMENT, and a model that is its own oracle passes every test.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import collections, glob, json, os, re, sys

SINGLE = re.compile(r'^([0-9A-F]{1,5})h$')

# THE RT1180 RM PRINTS ARRAY ROWS DIFFERENTLY FROM THE MCX, AND THE DIFFERENCE IS
# SILENT.  mcxn's manual puts the range and the name on ONE line:
#
#     2C0h - 2CCh ADC Trigger Input Connections (ADC1_TRIG0 - ADC1_TRIG3)
#     32 / RW / 0000_007Fh                                     <- a 4-line row
#
# The RT1180 puts them on TWO:
#
#     80h - FCh
#     Interrupt Control Register a (ICR0 - ICR31)
#     32 / RW / 0000_0000h                                     <- a 5-line row
#
# Their regex matched NOTHING here.  Ported verbatim, this tool reported
# "0 array ranges expanded" and a confident golden -- with EVERY ARRAY REGISTER ON
# THE CHIP missing, and no complaint.  That is the EXACT failure mcxn warned about
# ("you will assume it probes every register; it does not, and it will not tell you
# unless you make it") arriving in a different disguise in the port of their fix.
# A parser is a model of a document, and THIS IS A DIFFERENT DOCUMENT.
ARRAY_RANGE = re.compile(r'^([0-9A-F]{1,5})h\s*-\s*([0-9A-F]{1,5})h$')
ARRAY_1LINE = re.compile(r'^([0-9A-F]{1,5})h\s*-\s*([0-9A-F]{1,5})h\s+.*'
                         r'\((\w+)\s*-\s*(\w+)\)$')
# An element pair may be numbered at the END (ICR0 - ICR31) or in the MIDDLE
# (P0DR - P31DR).  Capture prefix/index/suffix and require both ends to agree.
ELEM = re.compile(r'^([A-Za-z_]\w*?)(\d+)(\w*)$')
ARRAY_NAME  = re.compile(r'^.*\((\w+)\s*-\s*(\w+)\)$')
NAME   = re.compile(r'^.*\(([A-Za-z0-9_]+)\)$')


def expand_array(first, last, lo, hi):
    """(ICR0, ICR31, 0x80, 0xFC) -> [(ICR0,0x80), (ICR1,0x84), ...] or None."""
    a, b = ELEM.match(first), ELEM.match(last)
    if not a or not b:
        return None
    if a.group(1) != b.group(1) or a.group(3) != b.group(3):
        return None                    # prefixes/suffixes disagree -> DROP, don't guess
    i0, i1 = int(a.group(2)), int(b.group(2))
    n = i1 - i0 + 1
    if n < 2 or hi <= lo:
        return None
    step = (hi - lo) // (n - 1)
    return [("%s%d%s" % (a.group(1), i0 + k, a.group(3)), lo + k * step)
            for k in range(n)]
WIDTH  = re.compile(r'^(8|16|32|64)$')
ACCESS = re.compile(r'^(RW|RO|WO|W1C|R|W)$')
RESET  = re.compile(r'^([0-9A-F]{4}_[0-9A-F]{4}|[0-9A-F]{2}_[0-9A-F]{4}|[0-9A-F]{1,16})h$')

SDK    = os.environ.get("SDK_ROOT", os.path.expanduser("~/.cache/rt1180-sdk/sdk/mcuxsdk"))
DEV    = os.path.join(SDK, "devices/RT/RT1180")
COMMON = os.path.join(DEV, "MIMXRT1189/MIMXRT1189_cm33_COMMON.h")
PERIPH = os.path.join(DEV, "periph")

SECURE_OFFSET = 0x10000000        # fsl_memory.h FSL_MEM_M33_SECURE_ADDRESS_OFFSET

# ---------------------------------------------------------------- anchors ----
# HAND-READ OUT OF THE PDF TEXT, BY EYE, BEFORE THE PARSER EXISTED.  If the parser
# cannot reproduce these then it is wrong, and every number downstream of it is a lie
# wearing a coverage figure.  NEVER "fix" an anchor to match the parser.
# NOTE the SHAPE of this check: a (name, offset) is NOT unique on this chip -- VERID
# sits at offset 0 on LPUART, LPI2C, LPSPI and more, with a DIFFERENT reset value in
# each.  The first version of this gate looked the anchor up in a dict keyed on
# (name, offset), the dict kept whichever row came last, and the gate FAILED against
# a parser that was reading the document perfectly well.  So the anchor asserts that
# the TRIPLE is among the parsed rows -- which is the actual claim being made.
ANCHORS = [
    ("VERID", 0x0, 0x02002C1B),     # an LPI2C-family VERID
    ("PARAM", 0x4, 0x0F041008),
    ("MCR0",  0x0, 0xFFFF80C2),     # FlexSPI MCR0 -- unique name, non-zero reset
]


def parse_rm(path):
    """(name, offset, width, access, reset) for every register row we can read."""
    lines = [l.strip() for l in open(path, errors="replace")]
    lines = [l for l in lines if l]
    rows, arrays, i = [], 0, 0

    while i < len(lines) - 4:
        # --- array, RT1180 form: range / name / width / access / reset (5 lines)
        r = ARRAY_RANGE.match(lines[i])
        if r:
            nm = ARRAY_NAME.match(lines[i+1])
            if nm and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
                  and RESET.match(lines[i+4]):
                els = expand_array(nm.group(1), nm.group(2),
                                   int(r.group(1), 16), int(r.group(2), 16))
                if els:
                    w, acc = int(lines[i+2]), lines[i+3]
                    rst = int(lines[i+4].rstrip('h').replace('_', ''), 16)
                    for nme, off in els:
                        rows.append((nme, off, w, acc, rst))
                    arrays += 1
                    i += 5
                    continue

        # --- array, MCX form: range+name on one line (kept so this ports back)
        a = ARRAY_1LINE.match(lines[i])
        if a and WIDTH.match(lines[i+1]) and ACCESS.match(lines[i+2]) \
             and RESET.match(lines[i+3]):
            els = expand_array(a.group(3), a.group(4),
                               int(a.group(1), 16), int(a.group(2), 16))
            if els:
                w, acc = int(lines[i+1]), lines[i+2]
                rst = int(lines[i+3].rstrip('h').replace('_', ''), 16)
                for nme, off in els:
                    rows.append((nme, off, w, acc, rst))
                arrays += 1
                i += 4
                continue

        m = SINGLE.match(lines[i])
        if m:
            nm = NAME.match(lines[i+1])
            if nm and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
                  and RESET.match(lines[i+4]):
                rows.append((nm.group(1), int(m.group(1), 16), int(lines[i+2]),
                             lines[i+3],
                             int(lines[i+4].rstrip('h').replace('_', ''), 16)))
                i += 5
                continue
        i += 1
    return rows, arrays


def parse_cmsis():
    """peripheral type -> {register: offset}, NON-SECURE bases, and instance -> type."""
    periph = {}
    for path in sorted(glob.glob(os.path.join(PERIPH, "PERI_*.h"))):
        src = open(path, errors="replace").read()
        for m in re.finditer(r'typedef struct \{(.*?)\} (\w+)_Type;', src, re.S):
            regs = {}
            for f in re.finditer(
                    r'__[IO]+\s+\w+\s+(\w+)\s*(?:\[(\d+)\])?\s*;\s*/\*\*<[^*]*?'
                    r'(?:array )?offset:\s*(0x[0-9A-Fa-f]+)'
                    r'(?:[^*]*?array step:\s*(0x[0-9A-Fa-f]+))?', m.group(1)):
                name, n, off = f.group(1), f.group(2), int(f.group(3), 16)
                if n:
                    step = int(f.group(4), 16) if f.group(4) else 4
                    for k in range(int(n)):
                        regs["%s%d" % (name, k)] = off + k * step
                regs[name] = off
            if regs:
                periph[m.group(2)] = regs

    src = open(COMMON, errors="replace").read()

    allbases = collections.defaultdict(set)     # TrustZone defines each base TWICE
    for m in re.finditer(r'#define (\w+)_BASE\s+\(?\(?(0x[0-9A-Fa-f]+)u?\)?', src):
        allbases[m.group(1)].add(int(m.group(2), 16))

    bases, sec = {}, 0
    for inst, vals in allbases.items():
        if len(vals) == 1:
            bases[inst] = next(iter(vals))
        elif len(vals) == 2 and (max(vals) - min(vals)) == SECURE_OFFSET:
            bases[inst] = min(vals)             # the non-secure member of the pair
            sec += 1
        # else: DROP, never guess (mcxn's rule 2)

    inst2type = {}
    for m in re.finditer(r'#define (\w+)_BASE_PTRS\s+\{([^}]*)\}', src):
        for inst in re.findall(r'\b(\w+)\b', m.group(2)):
            if inst in bases:
                inst2type[inst] = m.group(1)
    return periph, bases, inst2type, sec


def main(rm_txt, out_json):
    rows, arrays = parse_rm(rm_txt)

    # ---- ANCHORS FIRST.  Never emit a golden from a parser you have not checked.
    triples = {(n, o, r) for n, o, _w, _a, r in rows}
    bad = [a for a in ANCHORS if a not in triples]
    if bad:
        print("PARSER IS WRONG -- hand-read anchors do not reproduce:")
        for n, o, want in bad:
            got = sorted({r for nn, oo, _w, _a, r in rows if (nn, oo) == (n, o)})
            g = "NO ROW AT ALL" if not got else ", ".join("0x%08X" % v for v in got)
            print("    %-8s @0x%03X   RM(hand-read)=0x%08X   parser saw: %s"
                  % (n, o, want, g))
        print("\nA parser is a MODEL OF A DOCUMENT.  Fix the parser, never the anchor.")
        sys.exit(1)
    print("anchors                   : %d/%d hand-read values reproduced"
          % (len(ANCHORS), len(ANCHORS)))

    # ---- THE RM ITSELF DISAGREES WITH THE RM, 22 TIMES.
    #
    # The same (name, offset) is printed with DIFFERENT reset values in different
    # chapters, because different peripherals share a register name at a shared
    # offset:  MP_CSR@0 resets to 0031_0000h on DMA3 and 0040_0000h on DMA4.
    # VERID@0 has FOUR distinct values.
    #
    # The upstream tool drops a row only when CMSIS attributes it to more than one
    # peripheral TYPE.  That covers most of these by luck -- but not the case where
    # CMSIS is unambiguous and the MANUAL is not, and there it would emit ONE OF THE
    # TWO VALUES ARBITRARILY and call it a golden.  A wrong golden is worse than a
    # missing one: it makes the checker lie, and then the oracle needs an oracle.
    conflict = collections.defaultdict(set)
    for n, o, _w, _a, r in rows:
        conflict[(n, o)].add(r)
    conflicted = {k for k, v in conflict.items() if len(v) > 1}
    if conflicted:
        rows = [row for row in rows if (row[0], row[1]) not in conflicted]

    periph, bases, inst2type, sec = parse_cmsis()

    owner = collections.defaultdict(set)
    for t, regs in periph.items():
        for n, o in regs.items():
            owner[(n, o)].add(t)

    golden, unmatched, ambiguous = [], 0, 0
    for name, off, width, acc, reset in rows:
        types = owner.get((name, off))
        if not types:
            unmatched += 1
            continue
        if len(types) > 1:
            ambiguous += 1          # DROP, never guess -- and COUNT what you dropped
            continue
        if width != 32 or acc not in ("RW", "RO", "R"):
            continue
        t = next(iter(types))
        for inst, ity in inst2type.items():
            if ity == t:
                golden.append({"inst": inst, "reg": name,
                               "addr": bases[inst] + off, "reset": reset})

    golden.sort(key=lambda x: (x["inst"], x["addr"]))
    ded, key = [], set()                 # the RM prints some rows in two chapters
    for g in golden:
        k = (g["inst"], g["addr"])
        if k not in key:
            key.add(k)
            ded.append(g)
    json.dump(ded, open(out_json, "w"), indent=0)

    print("RM rows parsed            : %d  (%d array ranges expanded)" % (len(rows), arrays))
    print("  RM self-conflicts       : %d (name,offset) DROPPED -- the manual prints two"
          % len(conflicted))
    print("                            different reset values for them (shared names)")
    print("  unmatched in CMSIS      : %d   (RM names a register CMSIS does not put there)" % unmatched)
    print("  ambiguous (>1 periph)   : %d   (DROPPED, not guessed)" % ambiguous)
    print("CMSIS                     : %d peripheral types, %d instances, "
          "%d TrustZone pairs resolved to the NON-SECURE base" % (len(periph), len(inst2type), sec))
    print("golden                    : %d registers, %d instances -> %s"
          % (len(ded), len({g['inst'] for g in ded}), out_json))
    print()
    print("⚠ COVERAGE IS A FLOOR, NOT A CEILING.  Rows whose table layout did not parse")
    print("  are INVISIBLE to every check built on this file, and will not announce")
    print("  themselves.  This is a lower bound on the bugs, never an upper one.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
