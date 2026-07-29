# Virtual-motor plant / magnetic-saturation value-test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm

all: sat.elf
sat.elf: sat.c sat.ld
	$(CC) $(CF) -T sat.ld sat.c -o $@
run: all
	@bash run.sh
clean:
	rm -f sat.elf
.PHONY: all run clean
