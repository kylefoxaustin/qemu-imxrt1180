# FlexCAN loopback test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: can.elf
can.elf: can.c can.ld
	$(CC) $(CF) -T can.ld can.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel can.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f can.elf
.PHONY: all run clean
