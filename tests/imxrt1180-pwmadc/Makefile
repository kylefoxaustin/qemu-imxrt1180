# eFlexPWM -> XBAR -> LPADC synchronised-sampling test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: pwmadc.elf
pwmadc.elf: pwmadc.c pwmadc.ld
	$(CC) $(CF) -T pwmadc.ld pwmadc.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel pwmadc.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f pwmadc.elf
.PHONY: all run clean
