# Bare-metal test firmware for the QEMU mimxrt1180-evk machine.
#
#   make            # build hello.elf + hello_uart.elf
#   make run        # boot hello.elf (semihosting)
#   make run-uart   # boot hello_uart.elf (LPUART1 console)
#
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
CFLAGS  := -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU    ?= ../../build/qemu-system-arm
QFLAGS  := -M mimxrt1180-evk -audio none -display none -monitor none \
           -semihosting-config enable=on,target=native

ELFS := hello.elf hello_uart.elf

all: $(ELFS)

%.elf: %.c hello.ld
	$(CC) $(CFLAGS) -T hello.ld $< -o $@

run: hello.elf
	$(QEMU) $(QFLAGS) -nographic -kernel $<

run-uart: hello_uart.elf
	$(QEMU) $(QFLAGS) -serial stdio -kernel $<

clean:
	rm -f $(ELFS)

.PHONY: all run run-uart clean
