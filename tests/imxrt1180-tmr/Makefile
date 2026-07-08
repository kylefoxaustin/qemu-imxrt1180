# LPIT tmr test: periodic interrupt + counter.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: tmr.elf
tmr.elf: tmr.c tmr.ld
	$(CC) $(CF) -T tmr.ld tmr.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -display none -monitor none -kernel tmr.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f tmr.elf
.PHONY: all run clean
