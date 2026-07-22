# LPADC RESFIFO alignment + honest-overflow test: guards the mc_pmsm FOC read
# contract (2-command DualBoth chain, FIFO0/1/0/1 = Ia/Ib/dummy/U_DCbus) whose
# violation (an over-triggered, mis-aligned FIFO) deadlocked the closed-loop spin.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: fifoalign.elf
fifoalign.elf: fifoalign.c fifoalign.ld
	$(CC) $(CF) -T fifoalign.ld fifoalign.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel fifoalign.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f fifoalign.elf
.PHONY: all run clean
