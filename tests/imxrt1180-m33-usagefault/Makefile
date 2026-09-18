# Bare-metal ARMv8-M UsageFault-delivery probe for the QEMU mimxrt1180-evk.
#
#   make        # build usagefault.elf
#   make run    # boot it (semihosting output to stdout)
#
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
CFLAGS  := -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU    ?= ../../build/qemu-system-arm
QFLAGS  := -M mimxrt1180-evk -audio none -display none -monitor none -nographic \
           -semihosting-config enable=on,target=native

all: usagefault.elf

usagefault.elf: usagefault.c usagefault.ld
	$(CC) $(CFLAGS) -T usagefault.ld $< -o $@

run: usagefault.elf
	$(QEMU) $(QFLAGS) -kernel $<

clean:
	rm -f usagefault.elf

.PHONY: all run clean
