# SAI (I2S) bring-up test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: sai.elf
sai.elf: sai.c sai.ld
	$(CC) $(CF) -T sai.ld sai.c -o $@
# The firmware demands the harness ARM its rate selector (0x20001000) and then
# renders the verdict from OUTSIDE the guest (the wav), so the real test is
# check.py -- exactly as the sibling sai-audiopll / sai-dma tests run.  Launching
# the ELF bare (no -device loader) makes the firmware correctly FAIL "rate selector
# never armed": a diagnostic, not the test.  `run` must run the test.
run: all
	python3 check.py
clean:
	rm -f sai.elf
.PHONY: all run clean
