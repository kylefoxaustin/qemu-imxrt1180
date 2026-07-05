# SAI (I2S) bring-up test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: sai.elf
sai.elf: sai.c sai.ld
	$(CC) $(CF) -T sai.ld sai.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel sai.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f sai.elf
.PHONY: all run clean
