CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: ele.elf
ele.elf: ele.c ele.ld
	$(CC) $(CF) -T ele.ld ele.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel ele.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f ele.elf
