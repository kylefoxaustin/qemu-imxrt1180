# NETC switch FDB / NTMP command-BD test: add/query/delete a forwarding-database
# entry over the NTMP command BD ring (CBDRPIR doorbell -> CBDRCIR completion).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: fdb.elf
fdb.elf: fdb.c fdb.ld
	$(CC) $(CF) -T fdb.ld fdb.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel fdb.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f fdb.elf
.PHONY: all run clean
