# Virtual-motor plant / closed-loop test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: motor.elf
motor.elf: motor.c motor.ld
	$(CC) $(CF) -T motor.ld motor.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel motor.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f motor.elf
.PHONY: all run clean
