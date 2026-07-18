# LPI2C register read driven by eDMA hardware request lines.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: i2cdma.elf
i2cdma.elf: i2cdma.c i2cdma.ld
	$(CC) $(CF) -T i2cdma.ld i2cdma.c -o $@
# Attach a TMP105 to LPI2C1's bus (LPI2C1 is the eDMA3 source, SRC 7/8).
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel i2cdma.elf \
	  -device tmp105,bus=lpi2c1-bus,address=0x48 -serial stdio \
	  -semihosting-config enable=on,target=native
clean:
	rm -f i2cdma.elf
.PHONY: all run clean
