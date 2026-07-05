# eDMA memory-to-memory test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: dma.elf
dma.elf: dma.c dma.ld
	$(CC) $(CF) -T dma.ld dma.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel dma.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f dma.elf
.PHONY: all run clean
