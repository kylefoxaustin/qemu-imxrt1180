# Dual-core release test: the M33 releases the Cortex-M7.
#   make        # build m33.elf + m7.elf
#   make run    # M33 boots, releases M7; both print via semihosting
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
CF     := -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU   ?= ../../build/qemu-system-arm

all: m33.elf m7.elf
m33.elf: m33.c m33.ld
	$(CC) -mcpu=cortex-m33 $(CF) -T m33.ld m33.c -o $@
m7.elf: m7.c m7.ld
	$(CC) -mcpu=cortex-m7 $(CF) -T m7.ld m7.c -o $@

run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none \
	  -kernel m33.elf -device loader,file=m7.elf \
	  -serial null -semihosting-config enable=on,target=native

clean:
	rm -f m33.elf m7.elf
.PHONY: all run clean
