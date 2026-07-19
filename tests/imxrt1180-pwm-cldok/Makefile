# eFlexPWM commit via the SDK's CLDOK-then-LDOK read-modify-write sequence.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: pwmcldok.elf
pwmcldok.elf: pwmcldok.c pwmcldok.ld
	$(CC) $(CF) -T pwmcldok.ld pwmcldok.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel pwmcldok.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f pwmcldok.elf
.PHONY: all run clean
