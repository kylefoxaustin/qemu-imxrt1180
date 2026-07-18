# SAI streaming clocked by the AUDIO PLL (verifies ccm_audio_pll_hz end-to-end).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: saiapll.elf
saiapll.elf: saiapll.c saiapll.ld
	$(CC) $(CF) -T saiapll.ld saiapll.c -o $@
run: all
	python3 check.py
clean:
	rm -f saiapll.elf
.PHONY: all run clean
