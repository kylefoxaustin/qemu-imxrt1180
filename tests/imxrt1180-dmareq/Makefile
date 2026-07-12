CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
all: dmareq.elf
dmareq.elf: dmareq.c dmareq.ld
	$(CC) $(CF) -T dmareq.ld dmareq.c -o $@
run: all
	bash run.sh
clean:
	rm -f dmareq.elf
.PHONY: all run clean
