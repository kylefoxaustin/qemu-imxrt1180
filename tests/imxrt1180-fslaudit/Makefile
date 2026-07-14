CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
CF := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU ?= ../../build/qemu-system-arm
all: fslaudit.elf
fslaudit.elf: fslaudit.c fslaudit.ld
	$(CC) $(CF) -T fslaudit.ld fslaudit.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel fslaudit.elf -serial null -semihosting-config enable=on,target=native
clean:
	rm -f fslaudit.elf
.PHONY: all run clean
