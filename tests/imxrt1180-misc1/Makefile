CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
CF := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU ?= ../../build/qemu-system-arm
all: misc1.elf
misc1.elf: misc1.c misc1.ld
	$(CC) $(CF) -T misc1.ld misc1.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel misc1.elf -serial null -semihosting-config enable=on,target=native
clean:
	rm -f misc1.elf
.PHONY: all run clean
