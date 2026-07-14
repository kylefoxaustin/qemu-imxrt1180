#!/usr/bin/env bash
#
# RT1180 FlexSPI serial-NOR: storage-write-verified.
#
# Runs the UNMODIFIED MCUXpresso SDK driver example
# (examples/driver_examples/flexspi/nor/polling_transfer) against the model.  It
# is the real fsl_flexspi driver doing a real round trip:
#
#     read JEDEC id -> chip erase -> quad mode -> sector erase
#       -> read back through the AHB/XIP window, expect all 0xFF
#       -> page-program 256 bytes -> read back through AHB/XIP, expect byte-exact
#
# The write path (FlexSPI IP commands) and the read-back path (the memory-mapped
# XIP window) are DIFFERENT paths, so a pass cannot be a self-consistent lie.
#
# It then runs an ADVERSARIAL physics check that the SDK example does not: program
# a second pattern WITHOUT erasing first.  Real NOR only clears bits (1->0), so the
# result must equal (old AND new).  If the flash were RAM-backed -- the classic
# false-green, where guest stores into flash address space just land -- the second
# pattern would simply overwrite, and this check FAILS.  That is the bug this test
# exists to prevent regressing.
#
# SETUP: an extracted MCUXpresso SDK + a python venv with west (see
#        tools/sdk-run.sh).  USAGE:
#   SDK_ROOT=~/.cache/rt1180-sdk/sdk/mcuxsdk VENV=~/.cache/rt1180-sdk/venv \
#       tests/imxrt1180-flexspi/run.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
SDK_ROOT="${SDK_ROOT:?set SDK_ROOT to the extracted .../sdk/mcuxsdk}"
VENV="${VENV:?set VENV to the python venv with west+kconfiglib}"
export ARMGCC_DIR="${ARMGCC_DIR:-/usr}" PATH="$VENV/bin:$PATH"

EX=examples/driver_examples/flexspi/nor/polling_transfer
SRC="$SDK_ROOT/$EX/flexspi_nor_polling_transfer.c"
BUILD=/tmp/rt1180_flexspi_nor
CON=/tmp/rt1180_flexspi_nor.con
rc_all=0

# --- 1. stock example: erase -> program -> read-back (byte-exact) ------------
echo ">> building stock SDK flexspi_nor polling_transfer"
( cd "$SDK_ROOT" && west build -b evkmimxrt1180 --toolchain armgcc "$EX" \
    -Dcore_id=cm33 --config debug -d "$BUILD" ) >/tmp/rt1180_flexspi_build.log 2>&1 \
  || { echo "   BUILD FAILED"; tail -5 /tmp/rt1180_flexspi_build.log; exit 1; }
ELF="$(ls "$BUILD"/*.elf | head -1)"

echo ">> running"
timeout -k 5 30 "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none -kernel "$ELF" \
    -semihosting-config enable=on,target=native -serial "file:$CON" >/dev/null 2>&1

grep -q "Erase data - successfully"   "$CON" && e=PASS || { e=FAIL; rc_all=1; }
grep -q "Program data - successfully" "$CON" && p=PASS || { p=FAIL; rc_all=1; }
echo "   erase   -> read-back 0xFF      : $e"
echo "   program -> read-back byte-exact: $p"

# --- 2. adversarial: program WITHOUT erase must AND bits, not overwrite ------
# (This is the check that catches a RAM-backed flash region.)
echo ">> adversarial: program-without-erase must only clear bits (1->0)"
cp "$SRC" "$SRC.orig"
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
extra = '''    {
        uint8_t second[256], expect_and[256];
        uint32_t k;
        for (k = 0; k < 256U; k++) { second[k] = 0x0FU; }
        for (k = 0; k < 256U; k++) { expect_and[k] = s_nor_program_buffer[k] & 0x0FU; }
        (void)flexspi_nor_flash_page_program(EXAMPLE_FLEXSPI,
                                             EXAMPLE_SECTOR * SECTOR_SIZE,
                                             (void *)second);
        DCACHE_InvalidateByRange(EXAMPLE_FLEXSPI_AMBA_BASE + EXAMPLE_SECTOR * SECTOR_SIZE, FLASH_PAGE_SIZE);
        memcpy(s_nor_read_buffer, (void *)(EXAMPLE_FLEXSPI_AMBA_BASE + EXAMPLE_SECTOR * SECTOR_SIZE), 256);
        if (memcmp(s_nor_read_buffer, second, 256) == 0)
            PRINTF("PHYS: FAIL - overwrote without erase (flash is acting like RAM)\\r\\n");
        else if (memcmp(s_nor_read_buffer, expect_and, 256) == 0)
            PRINTF("PHYS: PASS - bits only went 1->0 (result == old AND new)\\r\\n");
        else
            PRINTF("PHYS: UNEXPECTED - neither overwrite nor AND\\r\\n");
    }

'''
t = t.replace("    while (1)", extra + "    while (1)", 1)
open(p, "w").write(t)
PY
( cd "$SDK_ROOT" && west build -b evkmimxrt1180 --toolchain armgcc "$EX" \
    -Dcore_id=cm33 --config debug -d "$BUILD" ) >>/tmp/rt1180_flexspi_build.log 2>&1
rc=$?
mv "$SRC.orig" "$SRC"                     # always restore the pristine SDK source
if [ $rc -ne 0 ]; then
    echo "   BUILD FAILED (adversarial)"; exit 1
fi
timeout -k 5 30 "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
    -kernel "$(ls "$BUILD"/*.elf | head -1)" \
    -semihosting-config enable=on,target=native -serial "file:$CON" >/dev/null 2>&1
if grep -q "PHYS: PASS" "$CON"; then
    echo "   NOR physics (no overwrite w/o erase): PASS"
else
    echo "   NOR physics: FAIL"; grep -m1 "PHYS:" "$CON"; rc_all=1
fi

echo
[ $rc_all -eq 0 ] && echo "FLEXSPI-NOR: PASS (storage-write-verified)" \
                  || echo "FLEXSPI-NOR: FAIL"
exit $rc_all
