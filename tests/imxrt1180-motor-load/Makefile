# Virtual-motor plant / speed-squared load coast-down value-test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm

all: load.elf
load.elf: load.c load.ld
	$(CC) $(CF) -T load.ld load.c -o $@
run: all
	@bash run.sh
clean:
	rm -f load.elf
.PHONY: all run clean
