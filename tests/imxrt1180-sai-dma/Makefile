# SAI TX-by-eDMA (hardware request line) test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: saidma.elf
saidma.elf: saidma.c saidma.ld
	$(CC) $(CF) -T saidma.ld saidma.c -o $@
run: all
	python3 check.py
clean:
	rm -f saidma.elf
.PHONY: all run clean
