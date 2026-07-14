# LPI2C controller test: read a TMP105 on LPI2C2 + probe NACK.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: i2c.elf
i2c.elf: i2c.c i2c.ld
	$(CC) $(CF) -T i2c.ld i2c.c -o $@
# A bus-less -device attaches to the default (last-registered) I2C bus = LPI2C4.
# The tmp105 'temperature' property does not apply at -device time (its reset
# clears it), so 'make run' proves the transaction (ACK + 2-byte read + NACK);
# to see a real value, qom-set the temperature after boot (see scratchpad).
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel i2c.elf \
	  -device tmp105,bus=lpi2c4-bus,address=0x48 \
	  -serial null -semihosting-config enable=on,target=native
clean:
	rm -f i2c.elf
.PHONY: all run clean
