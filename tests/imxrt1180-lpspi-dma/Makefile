# LPSPI full-duplex JEDEC read driven by eDMA hardware request lines.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: spidma.elf
spidma.elf: spidma.c spidma.ld
	$(CC) $(CF) -T spidma.ld spidma.c -o $@
# Attach the flash to LPSPI1's bus (LPSPI1 is the eDMA3 source, SRC 11/12).
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel spidma.elf \
	  -device at25df321a,bus=lpspi1-bus -serial stdio \
	  -semihosting-config enable=on,target=native
clean:
	rm -f spidma.elf
.PHONY: all run clean
