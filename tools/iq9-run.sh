#!/usr/bin/env bash
#
# Build + run the i.MX RT1180 QEMU emulator and its bare-metal test suite on an
# aarch64 Linux host (e.g. the Qualcomm IQ-9075).  CPU-ONLY: pure TCG emulation,
# headless, no GPU/HTP/NPU — coexists with GPU workloads on the same board.
#
# Uses the prebuilt Cortex-M33 test ELFs checked into the tree, so no ARM
# cross-toolchain (arm-none-eabi-gcc) is needed on the board.  Build deps:
# a C compiler, meson, ninja, python3, glib-2.0-dev, pixman-1-dev.
#
#   ./tools/iq9-run.sh            # configure + build (if needed) + run all tests
#   ./tools/iq9-run.sh --build    # (re)configure + build only
#   ./tools/iq9-run.sh --run      # run tests only (assumes build/ exists)
#
# SPDX-License-Identifier: GPL-2.0-or-later
# `timeout N make run` DOES NOT BOUND THE QEMU UNDERNEATH IT. Measured, this box, today:
#
#     timeout 2      <wrapper spawning a TERM-ignoring grandchild>  -> exit 124, 3 ORPHANS
#     timeout -k 5 2 <same>                                          -> exit 124, 1 ORPHAN
#     bounded 2      <same>                                          -> exit 124, 0 orphans
#
# ⭐ ALL THREE REPORT EXIT 124. THE EXIT CODE CANNOT TELL A BOUND FROM A LEAK.
#    `timeout` signals its CHILD (make); the QEMU under it reparents to init and runs
#    forever. A census of this box found a 15-hour orphan of mine and two 6-hour orphans
#    on a LIVE multicast group -- one of them an IMPOSTOR beacon still on the wire.
#
# ⭐ A KILL THAT REACHES THE WRAPPER AND NOT THE PROCESS IS NOT A KILL. (mcxn947qemu)
source "$(dirname "${BASH_SOURCE[0]}")/bounded.sh"

set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
QEMU="$ROOT/build/qemu-system-arm"
JOBS=$(nproc 2>/dev/null || echo 4)

do_build() {
    echo ">> configuring a lean headless arm-softmmu build ($(uname -m))"
    ./configure --target-list=arm-softmmu \
        --disable-gtk --disable-sdl --disable-vnc --disable-opengl \
        --disable-virglrenderer --disable-docs --disable-tools \
        --disable-werror || exit 1
    echo ">> building qemu-system-arm (-j$JOBS)"
    make -j"$JOBS" || exit 1
}

do_run() {
    [ -x "$QEMU" ] || { echo "!! $QEMU missing — run with --build first"; exit 1; }
    echo ">> $("$QEMU" --version | head -1) on $(uname -m)"
    echo ">> machine: $("$QEMU" -M help | grep -i mimxrt1180 || echo 'NOT REGISTERED')"

    # Mark the checked-in ELFs up-to-date so `make run` reuses them instead of
    # recompiling — no ARM cross-toolchain (gcc-arm-none-eabi) needed on the
    # board unless you actually edit a test's .c.
    touch tests/imxrt1180-*/*.elf 2>/dev/null

    local pass=0 fail=0
    # Delegate to each test's own `make run` so its exact QEMU args (attached
    # I2C/SPI devices, dual-core images, socket peers) are used.  The prebuilt
    # ELFs are reused when the ARM cross-toolchain is absent.
    local dir name out
    for dir in tests/imxrt1180-*/; do
        name=${dir#tests/imxrt1180-}; name=${name%/}
        [ -f "$dir/Makefile" ] || continue
        case "$name" in corpus|zephyr) continue ;; esac   # need SDK bins, not bundled
        out=$(QEMU="$QEMU" bounded 120 make -C "$dir" run 2>&1 \
              | grep -iE 'PASS|FAIL|alive|hello (world|from)' | head -1)
        case "$out" in
            *FAIL*)                          echo "  FAIL  $name — $out"; fail=$((fail+1)) ;;
            *PASS*|*alive*|*[Hh]ello*)       echo "  pass  $name — $out"; pass=$((pass+1)) ;;
            *)                               echo "  ????  $name — ${out:-<no marker>}"; fail=$((fail+1)) ;;
        esac
    done
    echo ">> RESULT: $pass passed, $fail failed"
    [ "$fail" -eq 0 ]
}

case "${1:-all}" in
    --build) do_build ;;
    --run)   do_run ;;
    all|"")  [ -x "$QEMU" ] || do_build; do_run ;;
    *) echo "usage: $0 [--build|--run]"; exit 2 ;;
esac
