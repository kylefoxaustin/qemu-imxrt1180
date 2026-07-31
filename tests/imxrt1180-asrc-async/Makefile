# RT1180 ASRC true-async (clock-source ratio) value-test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm

all: async.elf
async.elf: async.c async.ld
	$(CC) $(CF) -T async.ld async.c -o $@
run: all
	@for p in $$(pidof qemu-system-arm 2>/dev/null); do kill -9 $$p 2>/dev/null; done; \
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel async.elf \
	  -serial null -semihosting-config enable=on,target=native </dev/null 2>&1 | grep 'ASRC-ASYNC:'
clean:
	rm -f async.elf
.PHONY: all run clean
