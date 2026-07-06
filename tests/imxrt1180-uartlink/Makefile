CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: uartlink.elf
uartlink.elf: uartlink.c uartlink.ld
	$(CC) $(CF) -T uartlink.ld uartlink.c -o $@
run: all
	bash run.sh
clean:
	rm -f uartlink.elf
.PHONY: all run clean
