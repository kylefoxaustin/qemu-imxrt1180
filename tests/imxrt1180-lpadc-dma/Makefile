# LPADC result-FIFO drained by eDMA hardware request (eDMA4, SRC 57).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: adcdma.elf
adcdma.elf: adcdma.c adcdma.ld
	$(CC) $(CF) -T adcdma.ld adcdma.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel adcdma.elf \
	  -serial stdio -semihosting-config enable=on,target=native
clean:
	rm -f adcdma.elf
.PHONY: all run clean
