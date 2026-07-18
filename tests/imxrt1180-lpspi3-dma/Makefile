CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: spidma3.elf
spidma3.elf: spidma3.c spidma3.ld
	$(CC) $(CF) -T spidma3.ld spidma3.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel spidma3.elf \
	  -device at25df321a,bus=lpspi3-bus -serial stdio -semihosting-config enable=on,target=native
clean:
	rm -f spidma3.elf
.PHONY: all run clean
