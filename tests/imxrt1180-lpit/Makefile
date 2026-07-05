# LPIT timer test: periodic interrupt + counter.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: timer.elf
timer.elf: timer.c timer.ld
	$(CC) $(CF) -T timer.ld timer.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel timer.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f timer.elf
.PHONY: all run clean
