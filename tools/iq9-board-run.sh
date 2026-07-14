#!/usr/bin/env bash
#
# Run the RT1180 emulator test suite from a deploy bundle on the sealed board
# (no make / no compiler needed).  Expects, next to this script:
#   ./qemu-system-arm        prebuilt aarch64 binary
#   ./libs/                  bundled shared libs not on the board (LD_LIBRARY_PATH)
#   ./tests/imxrt1180-*/     prebuilt Cortex-M33 test ELFs (+ uart_peer.py)
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
cd "$(dirname "$0")"
Q="./qemu-system-arm"
export LD_LIBRARY_PATH="$PWD/libs:${LD_LIBRARY_PATH:-}"
COMMON="-M mimxrt1180-evk -audio none -display none -monitor none -serial null -semihosting-config enable=on,target=native"

echo ">> $($Q --version | head -1) on $(uname -m)"
echo ">> $($Q -M help | grep -i mimxrt1180 || echo 'MACHINE NOT FOUND')"
pass=0; fail=0
check() {  # <name> <marker-regex> <qemu-args...>
    local name=$1 want=$2; shift 2
    local out; out=$(timeout 120 $Q $COMMON "$@" 2>&1 | grep -iE "$want" | head -1)
    case "$out" in
        *FAIL*) echo "  FAIL  $name — $out"; fail=$((fail+1)) ;;
        "")     echo "  FAIL  $name — <no marker>"; fail=$((fail+1)) ;;
        *)      echo "  pass  $name — $out"; pass=$((pass+1)) ;;
    esac
}

T=tests
check hello    'hello|alive'   -kernel $T/imxrt1180-hello/hello.elf
check lpit     'PASS|FAIL'     -kernel $T/imxrt1180-lpit/timer.elf
check lpi2c    'PASS|FAIL'     -kernel $T/imxrt1180-lpi2c/i2c.elf   -device tmp105,address=0x48
check lpspi    'PASS|FAIL'     -kernel $T/imxrt1180-lpspi/spi.elf   -device at25df321a
check edma     'PASS|FAIL'     -kernel $T/imxrt1180-edma/dma.elf
check flexcan  'PASS|FAIL'     -kernel $T/imxrt1180-flexcan/can.elf
check mu       'PASS|FAIL'     -kernel $T/imxrt1180-mu/m33.elf      -device loader,file=$T/imxrt1180-mu/m7.elf
check dualcore 'PASS|alive'    -kernel $T/imxrt1180-dualcore/m33.elf -device loader,file=$T/imxrt1180-dualcore/m7.elf
check sai      'PASS|FAIL'     -kernel $T/imxrt1180-sai/sai.elf
check pwm      'PASS|FAIL'     -kernel $T/imxrt1180-pwm/pwm.elf
check eqdc     'PASS|FAIL'     -kernel $T/imxrt1180-eqdc/eqdc.elf
check adc      'PASS|FAIL'     -kernel $T/imxrt1180-adc/adc.elf
check pwmadc   'PASS|FAIL'     -kernel $T/imxrt1180-pwmadc/pwmadc.elf
check motor    'PASS|FAIL'     -kernel $T/imxrt1180-motor/motor.elf
[ -f $T/imxrt1180-usb/usb.elf ] && check usb 'PASS|FAIL' -kernel $T/imxrt1180-usb/usb.elf

# UART b2b link (needs python3 peer; board has it)
if [ -f $T/imxrt1180-uartlink/uartlink.elf ] && command -v python3 >/dev/null; then
    PORT=15780
    timeout 60 $Q $COMMON -kernel $T/imxrt1180-uartlink/uartlink.elf \
        -chardev socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off \
        -serial null -serial chardev:ul >/tmp/ul.$$ 2>&1 &
    qp=$!; sleep 1
    python3 $T/imxrt1180-uartlink/uart_peer.py $PORT >/dev/null 2>&1 &
    for _ in $(seq 1 40); do grep -qi uartlink /tmp/ul.$$ && break; sleep 0.25; done
    out=$(grep -iE 'UARTLINK' /tmp/ul.$$ | head -1)
    case "$out" in *PASS*) echo "  pass  uartlink — $out"; pass=$((pass+1)) ;; *) echo "  FAIL  uartlink — ${out:-<none>}"; fail=$((fail+1)) ;; esac
    kill $qp 2>/dev/null; rm -f /tmp/ul.$$
fi

echo ">> RESULT: $pass passed, $fail failed  (host $(uname -m), kernel $(uname -r))"
[ "$fail" -eq 0 ]
