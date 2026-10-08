# Bare-metal SAI RX deterministic-source value test for the mimxrt1180-evk.
# -icount shift=0: the fs-paced RX fill is virtual-clock driven; icount makes it deterministic.
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
CFLAGS  := -mcpu=cortex-m33 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU    ?= ../../build/qemu-system-arm
QFLAGS  := -M mimxrt1180-evk -audio none -display none -monitor none -nographic \
           -semihosting-config enable=on,target=native -icount shift=0
all: sairx.elf
sairx.elf: sairx.c sairx.ld
	$(CC) $(CFLAGS) -T sairx.ld $< -o $@
run: sairx.elf
	$(QEMU) $(QFLAGS) -kernel $<
clean:
	rm -f sairx.elf
.PHONY: all run clean
