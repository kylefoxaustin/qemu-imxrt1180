# NETC switch broadcast-flood (multi-physical-port) test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: flood.elf
flood.elf: flood.c flood.ld
	$(CC) $(CF) -T flood.ld flood.c -o $@
run: all
	@bash run.sh
clean:
	rm -f flood.elf
.PHONY: all run clean
