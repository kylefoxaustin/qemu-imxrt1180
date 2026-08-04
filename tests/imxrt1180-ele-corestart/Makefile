# S3MU/ELE kick-CM7 core-start reply test (the word MCMGR_StartCore blocks on).
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: corestart.elf
corestart.elf: corestart.c corestart.ld
	$(CC) $(CF) -T corestart.ld corestart.c -o $@
run: all
	@timeout 20 $(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none \
	  -kernel corestart.elf -serial null -semihosting-config enable=on,target=native 2>&1 \
	  | grep 'ELE-CORESTART:' || echo "ELE-CORESTART: FAIL - no verdict (hang/timeout)"
clean:
	rm -f corestart.elf
.PHONY: all run clean
