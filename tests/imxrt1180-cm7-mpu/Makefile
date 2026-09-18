# Bare-metal cm7 MPU region-count check for the QEMU mimxrt1180-evk.
#   make ; make run
# SPDX-License-Identifier: GPL-2.0-or-later
CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
CFLAGS := -mcpu=cortex-m7 -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU   ?= ../../build/qemu-system-arm
all: mpu.elf
mpu.elf: mpu.c mpu.ld
	$(CC) $(CFLAGS) -T mpu.ld $< -o $@
run: mpu.elf
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -nographic \
	  -semihosting-config enable=on,target=native -kernel mpu.elf
clean:
	rm -f mpu.elf
.PHONY: all run clean
