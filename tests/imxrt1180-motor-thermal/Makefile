# Virtual-motor plant / winding-thermal value-test.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm

# Plant thermal knobs (must match the golden derived in thermal.c):
#   R_th = 20 degC/W (mcw = 20000), tau = 1 ms (fast: reach steady state), amb 25.
THERM := -global imxrt1180-motor.thermal=1 \
         -global imxrt1180-motor.therm-rth-mcw=20000 \
         -global imxrt1180-motor.therm-tau-ms=1 \
         -global imxrt1180-motor.therm-amb-c=25

all: thermal.elf
thermal.elf: thermal.c thermal.ld
	$(CC) $(CF) -T thermal.ld thermal.c -o $@
run: all
	$(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none -kernel thermal.elf \
	  -serial null -semihosting-config enable=on,target=native $(THERM) </dev/null
clean:
	rm -f thermal.elf
.PHONY: all run clean
