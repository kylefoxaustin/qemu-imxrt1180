# Bare-metal Cortex-M33 WFI wake-from-interrupt probe for the mimxrt1180-evk.
#
#   make        # build wfi.elf
#   make run    # boot it (semihosting output to stdout), under -icount
#
# -icount is load-bearing: it makes WFI warp virtual time to the SysTick
# deadline deterministically, so the wake always arrives AFTER WFI is entered
# (never before, which would no-op WFI and mask the defect under test).
#
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
CFLAGS  := -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU    ?= ../../build/qemu-system-arm
QFLAGS  := -M mimxrt1180-evk -audio none -display none -monitor none -nographic \
           -semihosting-config enable=on,target=native -icount shift=0

all: wfi.elf

wfi.elf: wfi.c wfi.ld
	$(CC) $(CFLAGS) -T wfi.ld $< -o $@

run: wfi.elf
	$(QEMU) $(QFLAGS) -kernel $<

clean:
	rm -f wfi.elf

.PHONY: all run clean
