# CM7 CPUWAIT (M7_CFG.WAIT) two-gate release test.
CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
CF     := -mthumb -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU   ?= ../../build/qemu-system-arm
all: m33.elf m7.elf
m33.elf: m33.c m33.ld
	$(CC) -mcpu=cortex-m33 $(CF) -T m33.ld m33.c -o $@
m7.elf: m7.c m7.ld
	$(CC) -mcpu=cortex-m7 $(CF) -T m7.ld m7.c -o $@
run: all
	@for p in $$(pidof qemu-system-arm 2>/dev/null); do kill -9 $$p 2>/dev/null; done; \
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none \
	  -kernel m33.elf -device loader,file=m7.elf \
	  -serial stdio -semihosting-config enable=on,target=native </dev/null 2>&1 | grep 'CM7WAIT:'
clean:
	rm -f m33.elf m7.elf
.PHONY: all run clean
