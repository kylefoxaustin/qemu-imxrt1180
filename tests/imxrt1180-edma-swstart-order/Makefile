# eDMA4 software-START ordering test (SDK InitCM7DMA clear-then-poll pattern).
#
# Runs under -icount: software-START completion is virtual-time deferred, so the
# transfer is in flight across the driver's DONE-clear.  Under -icount that
# ordering is DETERMINISTIC (same discipline as the mc_pmsm / LPADC RESFIFO
# timing invariants).  A synchronous or bottom-half-immediate eDMA model hangs
# here -- the harness reports FAIL on the missing PASS line.
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
CF    := -mthumb -mcpu=cortex-m33 -Os -ffreestanding -nostdlib -nostartfiles -Wall
QEMU  ?= ../../build/qemu-system-arm
all: order.elf
order.elf: order.c order.ld
	$(CC) $(CF) -T order.ld order.c -o $@
run: all
	@for p in $$(pidof qemu-system-arm 2>/dev/null); do kill -9 $$p 2>/dev/null; done; \
	timeout 30 $(QEMU) -M mimxrt1180-evk -audio none -display none -monitor none \
	  -icount shift=0 -kernel order.elf \
	  -serial null -semihosting-config enable=on,target=native 2>&1 | grep 'EDMA-SWORDER:' \
	  || echo "EDMA-SWORDER: FAIL - no verdict (hang/timeout)"
clean:
	rm -f order.elf
.PHONY: all run clean
