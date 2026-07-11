#!/usr/bin/env bash
#
# RT1180 execute-in-place (XIP) from the FlexSPI NOR.
#
# The RT1180 is a crossover MCU: its defining execution mode is running firmware
# IN PLACE out of the serial NOR.  This guards two things that are easy to break
# and easy to not notice:
#
#  1. CORRECTNESS -- an image linked into the XIP window (0x28000000) is actually
#     PROGRAMMED INTO THE FLASH and executes from it.  A model whose flash window
#     refuses the loader's writes boots from erased flash (0xFF) and locks up;
#     one whose window is plain RAM "works" but is a false green (see
#     tests/imxrt1180-flexspi).  Neither shows up in the TCM-linked demo corpus,
#     which is exactly how a broken XIP path can hide.
#
#  2. SPEED -- the XIP window must be a rom_device, so reads/instruction fetches
#     come from a RAM mirror and TCG can cache translations.  Backing it with an
#     MMIO (init_io) window is functionally correct but ~100x slower, because
#     every instruction fetch turns into an SPI read shifted over the SSI bus.
#     A regression to that is silent apart from the clock, so we time it.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
CC="${CROSS:-arm-none-eabi-}gcc"
BUDGET="${BUDGET:-8}"       # seconds; rom_device does this in well under 1s
rc=0

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

cat > "$BUILD/xip.c" <<'EOF'
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
static void sh(int op, void *a) { register int r0 asm("r0") = op;
    register void *r1 asm("r1") = a;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory"); }
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
extern void reset_handler(void);
__attribute__((section(".vectors"), used))
void (*const vt[])(void) = { [0] = (void (*)(void))0x20020000u, [1] = reset_handler };
static volatile uint32_t sink;
void reset_handler(void)
{
    /* This code is IN the NOR: reaching here at all proves XIP execution. */
    puts_("XIP: executing from FlexSPI NOR\r\n");
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 3000000u; i++) { acc += i ^ (acc >> 3); }   /* hot loop, in place */
    sink = acc;
    puts_("XIP: PASS - 3,000,000-iteration loop executed in place from flash\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
EOF

cat > "$BUILD/xip.ld" <<'EOF'
ENTRY(reset_handler)
MEMORY { FLASH (rx) : ORIGIN = 0x28000000, LENGTH = 1M
         DTCM  (rw) : ORIGIN = 0x20000000, LENGTH = 128K }
SECTIONS {
  .vectors 0x28000000 : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > DTCM
  .bss  : { *(.bss*) *(COMMON) } > DTCM
}
EOF

$CC -mthumb -mcpu=cortex-m33 -O2 -ffreestanding -nostdlib -nostartfiles \
    -T "$BUILD/xip.ld" "$BUILD/xip.c" -o "$BUILD/xip.elf" || { echo "BUILD FAILED"; exit 1; }

start=$(date +%s.%N)
timeout 120 "$QEMU" -M mimxrt1180-evk -display none -monitor none \
    -kernel "$BUILD/xip.elf" -serial null \
    -semihosting-config enable=on,target=native > "$BUILD/con" 2>&1 </dev/null
end=$(date +%s.%N)
wall=$(echo "$end - $start" | bc)

if grep -q "XIP: PASS" "$BUILD/con"; then
    echo "XIP execute-in-place: PASS  (image runs from the NOR)"
else
    echo "XIP execute-in-place: FAIL  (image did not execute from flash)"
    head -3 "$BUILD/con"
    rc=1
fi

if [ "$(echo "$wall < $BUDGET" | bc)" = "1" ]; then
    printf "XIP speed: PASS  (%.2fs < %ss budget -- window is a rom_device)\n" "$wall" "$BUDGET"
else
    printf "XIP speed: FAIL  (%.2fs >= %ss -- the XIP window has regressed to MMIO;\n" "$wall" "$BUDGET"
    printf "           reads are no longer RAM-backed, every fetch is an SPI read)\n"
    rc=1
fi

[ $rc -eq 0 ] && echo "XIP: PASS" || echo "XIP: FAIL"
exit $rc
