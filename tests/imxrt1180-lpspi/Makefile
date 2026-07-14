# LPSPI controller test: read a serial-flash JEDEC ID over SPI.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: spi.elf
spi.elf: spi.c spi.ld
	$(CC) $(CF) -T spi.ld spi.c -o $@
# bus-less -device attaches to the default (last-registered) SSI bus = LPSPI4.
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel spi.elf \
	  -device at25df321a,bus=lpspi4-bus -serial null -semihosting-config enable=on,target=native
clean:
	rm -f spi.elf
.PHONY: all run clean
