# eFlexPWM value-register update driven by eDMA hardware request (eDMA4, SRC 62).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: pwmdma.elf
pwmdma.elf: pwmdma.c pwmdma.ld
	$(CC) $(CF) -T pwmdma.ld pwmdma.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -icount shift=0 \
	  -kernel pwmdma.elf -serial stdio -semihosting-config enable=on,target=native
clean:
	rm -f pwmdma.elf
.PHONY: all run clean
