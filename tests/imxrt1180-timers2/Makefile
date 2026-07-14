# LPIT timer test: periodic interrupt + counter.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: timers2.elf
timers2.elf: timers2.c timers2.ld
	$(CC) $(CF) -T timers2.ld timers2.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel timers2.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f timers2.elf
.PHONY: all run clean
