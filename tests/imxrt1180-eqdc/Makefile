# EQDC quadrature-encoder test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: eqdc.elf
eqdc.elf: eqdc.c eqdc.ld
	$(CC) $(CF) -T eqdc.ld eqdc.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel eqdc.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f eqdc.elf
.PHONY: all run clean
