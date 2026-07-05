# eFlexPWM test: double-buffer commit + periodic reload IRQ + counter.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: pwm.elf
pwm.elf: pwm.c pwm.ld
	$(CC) $(CF) -T pwm.ld pwm.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel pwm.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f pwm.elf
.PHONY: all run clean
