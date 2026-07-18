# Standalone Cortex-M7 boot test: the machine auto-detects a cm7 -kernel image
# and boots the M7 (per-core ITCM/DTCM view), holding the M33.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
# -fno-delete-null-pointer-checks: on the M7, address 0 IS valid ITCM (the vector
# table); without it, -Os treats the itcm[0] read as a null deref and traps.
CF    := -mthumb -mcpu=cortex-m7 -Os -ffreestanding -nostdlib -nostartfiles -Wall \
         -fno-delete-null-pointer-checks
QEMU  ?= ../../build/qemu-system-arm
all: cm7boot.elf
cm7boot.elf: cm7boot.c cm7boot.ld
	$(CC) $(CF) -T cm7boot.ld cm7boot.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel cm7boot.elf \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f cm7boot.elf
.PHONY: all run clean
