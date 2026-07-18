# WM8962 audio codec control-plane test (LPI2C2 @ 0x1A).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: wm8962.elf
wm8962.elf: wm8962.c wm8962.ld
	$(CC) $(CF) -T wm8962.ld wm8962.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel wm8962.elf \
	  -serial stdio -semihosting-config enable=on,target=native
clean:
	rm -f wm8962.elf
.PHONY: all run clean
