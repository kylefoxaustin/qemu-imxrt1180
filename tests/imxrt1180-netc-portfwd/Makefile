# NETC switch wire->wire (multi-physical-port) routing test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: portfwd.elf
portfwd.elf: portfwd.c portfwd.ld
	$(CC) $(CF) -T portfwd.ld portfwd.c -o $@
run: all
	@bash run.sh
clean:
	rm -f portfwd.elf
.PHONY: all run clean
