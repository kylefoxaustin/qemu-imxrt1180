# LPADC A/B-side dual-conversion test: the plant drives distinct Ia/Ib, a single
# DualSingleEndBothSide command reads them from RESFIFO[0]/RESFIFO[1].
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: adcab.elf
adcab.elf: adcab.c adcab.ld
	$(CC) $(CF) -T adcab.ld adcab.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel adcab.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f adcab.elf
.PHONY: all run clean
