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
# ---------------------------------------------------------------------------
# EXPANDING AN ARRAY RANGE: "(ICR0 - ICR31)", "(P0DR - P31DR)",
# "(ADC1_TRIG0 - ADC1_TRIG3)", "(CLOCK_ROOT0_STATUS0 - CLOCK_ROOT73_STATUS0)".
#
# Both mcxn's regex and my first one asked the WRONG QUESTION -- "where is the
# index?" -- and each guessed differently, wrongly, and SILENTLY:
#
#   mcxn's (index must be the TRAILING run):
#       CLOCK_ROOT0_STATUS0 -> index = the trailing 0.  The run that actually
#       varies is the MIDDLE one, so all 74 CLOCK_ROOT registers are DROPPED.
#   mine (index = the FIRST digit run, non-greedy prefix):
#       ADC1_TRIG0 -> prefix "ADC", index "1", suffix "_TRIG0"; the far end gives
#       suffix "_TRIG3"; they disagree, so EVERY ADCn_TRIG VANISHES.
#       (mcxn took my regex, it broke their trailing case on the first run, and
#       their HAND-READ ANCHOR GATE caught it.  The gate caught a bug in the gate.)
#
# THE RIGHT QUESTION IS NOT "WHERE IS THE INDEX" -- IT IS "WHAT ACTUALLY VARIES".
# Split both endpoint names into runs of digits and non-digits and compare them:
#
#   * EXACTLY ONE digit run differs  -> that is the index, wherever it sits.
#   * ANY OTHER SHAPE                -> DROP, and COUNT.  "CTX0_CTR0 - CTX3_CTR1"
#     varies in TWO runs: it is a 2-D array and there is no single step that
#     describes it.  Guessing one would emit a confidently wrong golden, and a
#     wrong golden makes the CHECKER lie.
#
# This needs no regex for the name at all, and it cannot be wrong about the
# position of the index because it never has to decide where the index is.
# ---------------------------------------------------------------------------
RUNS = re.compile(r'(\d+|\D+)')
ARRAY_NAME  = re.compile(r'^.*\((\w+)\s*-\s*(\w+)\)$')
NAME   = re.compile(r'^.*\(([A-Za-z0-9_]+)\)$')

DROPPED_AMBIGUOUS_ARRAYS = []
# THE MANUAL DECLINES TO ANSWER.  87 register-summary rows print "See section" in the
# RESET column instead of a value -- because the reset DEPENDS ON THE INSTANCE, or is
# spelled out only in a bit diagram.  The RESET regex correctly refuses them.
#
#   A REFUSAL IS NOT A CHECK.  (mcxn947qemu, 2026-07-13)
#
# Their refusal pile was hiding their SWD debug pins.  MINE WAS HIDING USBPHY CTRL --
# the register that holds the PHY in SOFT RESET AND CLOCK-GATED out of reset.  With it
# invisible, this model came up as A BOARD THAT HAD ALREADY BOOTED.
# These are now COUNTED and NAMED, and the ones that matter are hand-read in
# tests/imxrt1180-blindspots.
DECLINED_ROWS = []


def expand_array(first, last, lo, hi):
    """(ADC1_TRIG0, ADC1_TRIG3, 0x2C0, 0x2CC) -> [(ADC1_TRIG0,0x2C0), ...] or None."""
    a, b = RUNS.findall(first), RUNS.findall(last)
    if len(a) != len(b):
        DROPPED_AMBIGUOUS_ARRAYS.append((first, last))
        return None
    diff = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if len(diff) != 1 or not a[diff[0]].isdigit() or not b[diff[0]].isdigit():
        DROPPED_AMBIGUOUS_ARRAYS.append((first, last))   # 2-D, or not an index
        return None
    k = diff[0]
    i0, i1 = int(a[k]), int(b[k])
    n = i1 - i0 + 1
    if n < 2 or hi <= lo or (hi - lo) % (n - 1):
        DROPPED_AMBIGUOUS_ARRAYS.append((first, last))
        return None
    step = (hi - lo) // (n - 1)
    out = []
    for j in range(n):
        parts = list(a)
        parts[k] = str(i0 + j)
        out.append(("".join(parts), lo + j * step))
    return out


WIDTH  = re.compile(r'^(8|16|32|64)$')
ACCESS = re.compile(r'^(RW|RO|WO|W1C|R|W)$')
RESET  = re.compile(r'^([0-9A-F]{4}_[0-9A-F]{4}|[0-9A-F]{2}_[0-9A-F]{4}|[0-9A-F]{1,16})h$')

# ---------------------------------------------------------------------------
# CMSIS FIELD.  groups: 1=name  2=flat array count  3=offset  4=array step
#
# THE BUG THIS REGEX USED TO HAVE, AND IT COST 4371 REGISTERS:
# it expands "[N]" when N sits on the FIELD --
#       __IO uint32_t ADC1_TRIG[4];   /**< array offset: 0x2C0, array step: 0x4 */
# but CMSIS ALSO declares register arrays as a STRUCT ARRAY, where the count sits
# on the CLOSING BRACE OF THE ENCLOSING STRUCT and the field is a plain scalar:
#
#       struct {                        /* offset: 0x0, array step: 0x80 */
#           __IO uint32_t CONTROL;      /**< array offset: 0x0, array step: 0x80 */
#           ...
#       } CLOCK_ROOT[CCM_CLOCK_ROOT_COUNT];        /* 74 */
#
# This regex saw the inner scalar and emitted ONE register literally called
# "CONTROL" at offset 0.  The manual calls it CLOCK_ROOT0_CONTROL.  THEY NEVER
# JOINED, so an entire register class was INVISIBLE TO THE GATE -- including all
# 74 CCM clock roots (the clock tree), the 149 LPCG clock gates, OSCPLL, OBSERVE,
# and eFlexPWM's SM submodules.  The gate printed "unmatched in CMSIS: 4371" on
# every single run and returned PASS.  A published number that cannot FAIL the
# gate is decoration -- see EXPECTED_GOLDEN below, which is the fix for that.
# ---------------------------------------------------------------------------
#
# AND A THIRD INSTANCE OF THE SAME DISEASE, FOUND THE SAME DAY, IN THIS FILE:
# the FLAT-array path required a NUMERIC LITERAL count -- `\[(\d+)\]`, matching
# `__IO uint32_t ADC1_TRIG[4];`.  THIS CHIP'S CMSIS NEVER WRITES ONE.  All 286 flat
# arrays are sized by a #define:
#
#       __IO uint16_t SEL[XBAR_NUM_OUT221_SEL_COUNT];   /* 111 */
#       __IO uint32_t TCTRL[ADC_TCTRL_COUNT];
#
# 286 macro-sized, ZERO literal-sized.  So the flat-array expansion -- present since
# this tool was born, "fixed" twice, once by each of us -- HAD NEVER MATCHED A SINGLE
# ARRAY ON THIS CHIP.  It emitted one bare `SEL` at 0x0 and dropped 110 more.
# The macro table was already being built two functions down, for struct arrays.
#
#   THE SAME BUG, THIRD TIME, ON A THIRD CODE PATH, WITH THE FIX ALREADY IN HAND.
#   Ask of every count: WHO SAYS SO, AND WOULD I NOTICE IF THEY STOPPED SAYING IT?
#
FIELD = re.compile(r'__[IO]+\s+\w+\s+(\w+)\s*(?:\[(\w+)\])?\s*;\s*/\*\*<[^*]*?'
                   r'(?:array )?offset:\s*(0x[0-9A-Fa-f]+)'
                   r'(?:[^*]*?array step:\s*(0x[0-9A-Fa-f]+))?')

# struct { ... } NAME[COUNT];   -- COUNT is nearly always a #define, not a literal.
#
# THIS CANNOT BE A REGEX, AND MY FIRST ATTEMPT WAS ONE.  `struct\s*\{(.*?)\}\s*(\w+)\[`
# is not brace-aware, so on a two-level type like ENETC_SI --
#
#       struct {                       /* a PLAIN nested struct, NOT an array */
#           __IO uint32_t PSIMSGRR;
#           struct { ... } VSI_NUM[..];         <- an array INSIDE it
#       } PSI_A;
#       ...
#       struct { ... } BDR[ENETC_SI_BDR_COUNT];
#
# -- the non-greedy span ran from an OUTER `struct {` to a LATER `} BDR[...];`,
# swallowing 16344 characters and DELETING 67 flat registers that had been covered.
# It was a COVERAGE REGRESSION hiding inside a coverage FIX, and the only reason I
# caught it is that I diffed the new golden against the old one instead of admiring
# the bigger number.  ALWAYS DIFF THE ORACLE, NOT JUST THE RESULT.
STRUCT_OPEN  = re.compile(r'\bstruct\s*\{')
STRUCT_TAIL  = re.compile(r'\}\s*(\w+)\s*\[(\w+)\]\s*;')
STRUCT_STEP  = re.compile(r'/\*[^*]*?array step:\s*(0x[0-9A-Fa-f]+)')
DEFINE_INT   = re.compile(r'#define\s+(\w+)\s+\(?(\d+|0x[0-9A-Fa-f]+)[uU]?\)?\s*$', re.M)

DROPPED_STRUCT_ARRAYS = []      # DROP, never guess -- and COUNT what you dropped
DROPPED_FLAT_ARRAYS   = []      # a flat array whose count macro we could not resolve
COLLIDING_NAMES       = []      # one CMSIS name, two offsets -- DROPPED (a false witness)


def find_struct_arrays(body):
    """Brace-balanced `struct {...} NAME[COUNT];` spans.  (start, end, inner, name, count)"""
    out = []
    for m in STRUCT_OPEN.finditer(body):
        i = m.end() - 1                       # sits on '{'
        depth, j = 0, m.end() - 1
        while j < len(body):
            if body[j] == '{':
                depth += 1
            elif body[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        else:
            continue                          # unbalanced: leave it alone
        tail = STRUCT_TAIL.match(body[j:])
        if tail:                              # ...it IS an array.  A plain `} PSI_A;`
            out.append((m.start(), j + tail.end(),   # is left in place: its fields
                        body[i + 1:j], tail.group(1), tail.group(2)))  # carry absolute
    return out                                                          # offsets already

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
    # Struct-array registers.  Hand-read from the RM's own summary tables.  The
    # RM parser always read these correctly -- it was the CMSIS JOIN that dropped
    # them -- so these anchors alone would NOT have caught the bug.  That is what
    # GOLDEN_ANCHORS below is for.  Kept because 73 * 0x80 == 0x2480 is an
    # INDEPENDENT confirmation of the array stride, from the manual, not the header.
    ("CLOCK_ROOT73_CONTROL", 0x2480, 0x00000000),
    ("LPCG0_DIRECT",         0x8000, 0x00000001),
    ("OSCPLL0_STATUS0",      0x5020, 0x00000001),
]

# ---------------------------------------------------------------------------
# GOLDEN_ANCHORS -- "DID THE CLASS I EXPECT TO BE COVERED ACTUALLY GET COVERED?"
#
# THE CONTROL WHOSE ABSENCE COST 4371 REGISTERS.  Every ANCHOR above passed, on
# every run, while an entire register class silently failed to JOIN and never
# reached the golden.  A parser anchor proves you can READ the document; it says
# NOTHING about whether the row survived to the output.
#
# So: assert that specific (instance, register) pairs are IN the emitted golden.
# A whole class going missing now FAILS here instead of being quietly not-tested.
# ---------------------------------------------------------------------------
GOLDEN_ANCHORS = [
    ("CCM",  "CLOCK_ROOT0_CONTROL"),    # the clock tree -- 74 roots
    ("CCM",  "CLOCK_ROOT73_CONTROL"),   # ...and its far end, so the stride is proven
    ("CCM",  "LPCG0_DIRECT"),           # the 149 clock gates
    ("CCM",  "OSCPLL0_STATUS0"),        # the oscillators/PLLs
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
                  and lines[i+4].startswith("See section"):
                DECLINED_ROWS.append((nm.group(1), int(m.group(1), 16)))
            if nm and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
                  and RESET.match(lines[i+4]):
                rows.append((nm.group(1), int(m.group(1), 16), int(lines[i+2]),
                             lines[i+3],
                             int(lines[i+4].rstrip('h').replace('_', ''), 16)))
                i += 5
                continue
        i += 1
    return rows, arrays


def expand_struct_array(inner, arr, count_tok, counts, cand):
    """struct {...} CLOCK_ROOT[74];  ->  CLOCK_ROOT0_CONTROL, CLOCK_ROOT1_CONTROL, ...

    Writes into `cand` (name -> {offsets}), NOT a plain dict: a name that resolves to
    two different offsets must be DROPPED, not silently overwritten by whichever the
    parser happened to see last.  See the collision note in parse_cmsis().
    """
    n = int(count_tok, 0) if count_tok.isdigit() else counts.get(count_tok)
    if not n:
        DROPPED_STRUCT_ARRAYS.append((arr, count_tok, "count unresolved"))
        return
    sstep = STRUCT_STEP.search(inner)
    sstep = int(sstep.group(1), 16) if sstep else None

    for f in FIELD.finditer(inner):
        fname, off = f.group(1), int(f.group(3), 16)
        step = int(f.group(4), 16) if f.group(4) else sstep
        if not step:
            DROPPED_STRUCT_ARRAYS.append((arr, fname, "no array step"))
            continue
        if n == 1:
            # A ONE-ELEMENT struct array (SYSPM's PMCR[1]).  The manual has no index
            # to print, so it prints the bare name -- and expanding to "PMCR0_PMCR"
            # DELETED four registers that had been covered.  Found by diffing the
            # oracle, not by reading it.
            cand[fname].add(off)

        for k in range(n):
            # THE MANUAL SPELLS THESE TWO DIFFERENT WAYS AND WE DO NOT GET TO PICK:
            #     CCM  ->  CLOCK_ROOT0_CONTROL   (underscore)
            #     PWM  ->  SM0CTRL               (no underscore)
            # Guessing one silently loses the other -- the same "where is the index?"
            # mistake that cost mcxn947qemu every ADCn_TRIG.  So emit BOTH candidate
            # spellings: the RM is the golden, and a name it never prints never joins.
            # A collision across peripheral TYPES still lands in the ambiguity drop.
            cand["%s%d_%s" % (arr, k, fname)].add(off + k * step)
            cand["%s%d%s"  % (arr, k, fname)].add(off + k * step)


def parse_cmsis():
    """peripheral type -> {register: offset}, NON-SECURE bases, and instance -> type."""
    periph = {}
    for path in sorted(glob.glob(os.path.join(PERIPH, "PERI_*.h"))):
        src = open(path, errors="replace").read()
        counts = {m.group(1): int(m.group(2), 0)
                  for m in DEFINE_INT.finditer(src)}

        for m in re.finditer(r'typedef struct \{(.*?)\} (\w+)_Type;', src, re.S):
            body, regs = m.group(1), {}
            cand = collections.defaultdict(set)   # name -> {offsets}; >1 => DROP

            # Struct arrays FIRST, and strip them out of the body -- otherwise their
            # inner scalars also get emitted as bogus flat names ("CONTROL" @ 0x0).
            # INNERMOST FIRST (a struct array can sit inside another struct), and
            # iterate to a fixpoint so an outer array is expanded only once its
            # nested one is gone.  Anything that is NOT an array is left in the body:
            # a plain `struct {...} PSI_A;` is just grouping, and its fields already
            # carry absolute offsets, so the flat scanner below reads them correctly.
            while True:
                spans = [s for s in find_struct_arrays(body)
                         if not STRUCT_OPEN.search(s[2])]      # innermost only
                if not spans:
                    break
                for start, end, inner, arr, cnt in reversed(spans):
                    expand_struct_array(inner, arr, cnt, counts, cand)
                    body = body[:start] + body[end:]

            #
            # EVERY LOOSENING OF A JOIN MUST BE PAID FOR WITH A NEW REFUSAL.
            # (mcxn947qemu, hours before this bug landed here.)
            #
            # Expanding arrays makes name COLLISIONS possible.  ChipIdea's USB has a
            # SCALAR `ENDPTCTRL0` at 0x1C0 *and* an ARRAY `ENDPTCTRL[7]` starting at
            # 0x1C4.  Expanding the array emits a SECOND `ENDPTCTRL0`, at 0x1C4, and
            # a plain dict keeps whichever was written LAST -- so the name silently
            # names the wrong address.
            #
            #   A MISSING REGISTER IS A GAP.  A WRONG ONE IS A FALSE WITNESS: the
            #   gate would report a LIE in a register that is perfectly correct.
            #
            # So collect name -> {offsets} and DROP any name that resolves to more
            # than one.  Drop-don't-guess, same as RM contradictions and >1-type
            # ambiguity.  It cost us exactly the two ENDPTCTRL0 rows, which is the
            # correct price.
            for f in FIELD.finditer(body):
                name, n, off = f.group(1), f.group(2), int(f.group(3), 16)
                if n:
                    cnt = int(n) if n.isdigit() else counts.get(n)
                    if not cnt:
                        DROPPED_FLAT_ARRAYS.append((name, n))   # DROP, and COUNT
                    else:
                        step = int(f.group(4), 16) if f.group(4) else 4
                        for k in range(cnt):
                            cand["%s%d" % (name, k)].add(off + k * step)
                cand[name].add(off)

            #
            # KEEP BOTH OFFSETS.  DO NOT DROP, AND DO NOT OVERWRITE.
            #
            # My first fix here DROPPED any name with two offsets -- and that was
            # too aggressive, which I only saw by reading the list it refused:
            # it threw away NETC_IERB's SBCR and USB's ENDPTCTRL0, both of which
            # the RM names UNAMBIGUOUSLY.
            #
            #   THE JOIN IS ON (name, OFFSET).  A name at two offsets is not an
            #   ambiguity -- THE RM ROW CARRIES ITS OWN OFFSET AND PICKS ONE.
            #
            # The real bug was never the collision: it was that `regs` was a
            # name -> offset DICT, so the second offset SILENTLY OVERWROTE the
            # first and the surviving name pointed at the wrong address. That is
            # mcxn947qemu's false witness, and the cure is to stop overwriting,
            # not to start refusing. A name -> {offsets} map keeps every pair, and
            # a pair the RM never mentions simply never joins.
            #
            # (Cross-TYPE collisions are still dropped, downstream, by the >1-type
            # ambiguity check. That one IS a real ambiguity: nothing disambiguates it.)
            for nm, offs in cand.items():
                if len(offs) > 1:
                    COLLIDING_NAMES.append((m.group(2), nm, sorted(offs)))
                regs[nm] = offs                      # ALL of them

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
        for n, offs in regs.items():
            for o in offs:
                owner[(n, o)].add(t)

    golden, unmatched, ambiguous, notwide, notread = [], 0, 0, 0, 0
    for name, off, width, acc, reset in rows:
        types = owner.get((name, off))
        if not types:
            unmatched += 1
            continue
        if len(types) > 1:
            ambiguous += 1          # DROP, never guess -- and COUNT what you dropped
            continue
        if width not in (8, 16, 32):
            notwide += 1            # 64-bit: qtest has no readq for our purposes
            continue
        if acc not in ("RW", "RO", "R"):
            notread += 1            # write-only: nothing to read back -- WAS SILENT
            continue
        t = next(iter(types))
        for inst, ity in inst2type.items():
            if ity == t:
                golden.append({"inst": inst, "reg": name, "w": width,
                               "addr": bases[inst] + off, "reset": reset})

    golden.sort(key=lambda x: (x["inst"], x["addr"]))
    ded, key = [], set()                 # the RM prints some rows in two chapters
    for g in golden:
        k = (g["inst"], g["addr"])
        if k not in key:
            key.add(k)
            ded.append(g)
    # ---- DID THE CLASSES I EXPECT TO BE COVERED ACTUALLY REACH THE OUTPUT?
    have = {(g["inst"], g["reg"]) for g in ded}
    missing = [a for a in GOLDEN_ANCHORS if a not in have]
    if missing:
        print("\nGOLDEN IS MISSING A CLASS IT IS SUPPOSED TO COVER:")
        for inst, reg in missing:
            print("    %s / %s  -- parsed from the RM, but it never reached the golden"
                  % (inst, reg))
        print("\nThe RM parser can read a row and the CMSIS JOIN can still drop it.")
        print("A parser anchor proves you can READ the document.  This proves the row")
        print("SURVIVED TO THE OUTPUT -- which is the claim the gate actually makes.")
        sys.exit(1)
    print("golden anchors            : %d/%d classes reached the output"
          % (len(GOLDEN_ANCHORS), len(GOLDEN_ANCHORS)))

    json.dump(ded, open(out_json, "w"), indent=0)

    print("RM rows parsed            : %d  (%d array ranges expanded)" % (len(rows), arrays))
    print("  ambiguous array ranges  : %d DROPPED (2-D, e.g. CTX0_CTR0 - CTX3_CTR1:"
          % len(DROPPED_AMBIGUOUS_ARRAYS))
    print("                            TWO runs vary, no single step describes it)")
    print("  RM self-conflicts       : %d (name,offset) DROPPED -- the manual prints two"
          % len(conflicted))
    print("                            different reset values for them (shared names)")
    print("  unmatched in CMSIS      : %d   (RM names a register CMSIS does not put there)" % unmatched)
    print("  ambiguous (>1 periph)   : %d   (DROPPED, not guessed)" % ambiguous)
    print("  width not 8/16/32       : %d   (nothing to read back with qtest)" % notwide)
    print("  not readable            : %d   (write-only: no reset value to read back)"
          % notread)
    print("  struct arrays unresolved: %d   (DROPPED, not guessed)"
          % len(DROPPED_STRUCT_ARRAYS))
    print("  flat arrays unresolved  : %d   (count macro unknown -- DROPPED, not guessed)"
          % len(DROPPED_FLAT_ARRAYS))
    print("  RM DECLINED to answer   : %d   ('See section' in the reset column -- REFUSED."
          % len(DECLINED_ROWS))
    print("                            A REFUSAL IS NOT A CHECK: these are hand-read in")
    print("                            tests/imxrt1180-blindspots.  Mine hid USBPHY CTRL,")
    print("                            which holds the PHY in reset out of reset.)")
    print("  CMSIS name collisions   : %d   (one name, two offsets -- BOTH KEPT; the RM row's"
          % len(COLLIDING_NAMES))
    print("                            own offset disambiguates. NOT overwritten: a silently")
    print("                            overwritten name is a FALSE WITNESS, not a gap.)")
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
