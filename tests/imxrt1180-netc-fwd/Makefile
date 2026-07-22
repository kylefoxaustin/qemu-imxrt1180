# NETC switch egress-forwarding test: a frame is forwarded to the wire per its FDB
# entry (or flooded/dropped), observed via the wire port's PM0_TFRMN counter.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: fwd.elf
fwd.elf: fwd.c fwd.ld
	$(CC) $(CF) -T fwd.ld fwd.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel fwd.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f fwd.elf
.PHONY: all run clean
