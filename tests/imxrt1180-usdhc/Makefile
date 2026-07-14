CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
CF := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU ?= ../../build/qemu-system-arm
all: usdhc.elf
usdhc.elf: usdhc.c usdhc.ld
	$(CC) $(CF) -T usdhc.ld usdhc.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel usdhc.elf -serial null -semihosting-config enable=on,target=native
clean:
	rm -f usdhc.elf
.PHONY: all run clean
