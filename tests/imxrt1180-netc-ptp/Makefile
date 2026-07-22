# NETC PTP 1588 timer test: the clock advances, the addend scales its rate, and
# it freezes on disable. Runs under -icount so the virtual clock (and thus the
# rate ratio) is deterministic.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: ptp.elf
ptp.elf: ptp.c ptp.ld
	$(CC) $(CF) -T ptp.ld ptp.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel ptp.elf \
	  -serial null -semihosting-config enable=on,target=native -icount shift=1
clean:
	rm -f ptp.elf
.PHONY: all run clean
