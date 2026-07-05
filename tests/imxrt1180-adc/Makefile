# LPADC test: calibration + command-chain conversion + result FIFO.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: adc.elf
adc.elf: adc.c adc.ld
	$(CC) $(CF) -T adc.ld adc.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel adc.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f adc.elf
.PHONY: all run clean
