# RT1180 ASRC data-path value-test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm

all: asrc.elf
asrc.elf: asrc.c asrc.ld
	$(CC) $(CF) -T asrc.ld asrc.c -o $@
run: all
	@for p in $$(pidof qemu-system-arm 2>/dev/null); do kill -9 $$p 2>/dev/null; done; \
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel asrc.elf \
	  -serial null -semihosting-config enable=on,target=native </dev/null 2>&1 | grep 'ASRC:'
clean:
	rm -f asrc.elf
.PHONY: all run clean
