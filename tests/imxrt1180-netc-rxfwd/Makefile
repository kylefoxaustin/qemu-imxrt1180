# NETC switch wire->CPU forwarding test over a QEMU mcast socket (self-echo).
# Uses a private mcast group to avoid colliding with the fleet's lab3 segment.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
MCAST ?= 239.255.42.99:39917
all: rxfwd.elf
rxfwd.elf: rxfwd.c rxfwd.ld
	$(CC) $(CF) -T rxfwd.ld rxfwd.c -o $@
run: all
	timeout -k 5 25 $(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none \
	  -kernel rxfwd.elf -semihosting-config enable=on,target=native \
	  -nic socket,mcast=$(MCAST),mac=54:27:8d:11:11:11 -serial null
clean:
	rm -f rxfwd.elf
.PHONY: all run clean
