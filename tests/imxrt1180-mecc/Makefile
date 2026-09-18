# Bare-metal MECC value-golden for the QEMU mimxrt1180-evk (Cortex-M33).
#   make ; make run
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
CFLAGS := -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU   ?= ../../build/qemu-system-arm
all: mecc.elf
mecc.elf: mecc.c mecc.ld
	$(CC) $(CFLAGS) -T mecc.ld $< -o $@
run: mecc.elf
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -nographic \
	  -semihosting-config enable=on,target=native -kernel mecc.elf
clean:
	rm -f mecc.elf
.PHONY: all run clean
