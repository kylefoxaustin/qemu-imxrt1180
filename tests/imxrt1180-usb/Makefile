# USB PHY + OTG controller test: PLL lock + reset/DCCPARAMS/run.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: usb.elf
usb.elf: usb.c usb.ld
	$(CC) $(CF) -T usb.ld usb.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel usb.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f usb.elf
.PHONY: all run clean
