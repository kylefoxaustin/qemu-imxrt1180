#!/usr/bin/env bash
# MCXN947 eIQ Neutron NPU: prove the model never hangs the guest (exec/done
# handshake completes) AND that it honestly flags the compute as un-run
# (FLAG-AT-OPERATOR: qom compute-modelled=false + jobs-started counter).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/neutron.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

SERIAL="$(mktemp)"; QMP="$(mktemp -u).sock"
timeout -k 5 15 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$SERIAL" -qmp "unix:$QMP,server=on,wait=off" \
    -kernel "$ELF" -no-reboot &
QPID=$!

QOUT="$("$PY" - "$QMP" <<'PY'
import socket, json, sys, time
p = sys.argv[1]
s = None
for _ in range(50):
    try: s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(p); break
    except OSError: s=None; time.sleep(0.1)
if not s: print("QMPERR"); sys.exit(0)
f = s.makefile("rwb", buffering=0)
def cmd(o): f.write((json.dumps(o)+"\n").encode()); 
f.readline()                                  # greeting
cmd({"execute":"qmp_capabilities"}); f.readline()
time.sleep(0.5)                               # let firmware run its kicks
cmd({"execute":"qom-get","arguments":{"path":"/machine/soc/neutron0","property":"compute-modelled"}})
cm=f.readline()
cmd({"execute":"qom-get","arguments":{"path":"/machine/soc/neutron0","property":"jobs-started"}})
js=f.readline()
cmd({"execute":"quit"})
print("compute-modelled="+cm.decode().strip())
print("jobs-started="+js.decode().strip())
PY
)"
sleep 0.3; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
OUT="$(cat "$SERIAL" 2>/dev/null)"; rm -f "$SERIAL"
echo "--- guest console ---"; echo "$OUT"; echo "--- qmp ---"; echo "$QOUT"; echo "----------"
# PASS: firmware didn't hang (NEUTRON OK) AND model is honest (compute-modelled=false, jobs-started=8)
if echo "$OUT" | grep -q "NEUTRON OK" \
   && echo "$QOUT" | grep -q '"return": false' \
   && echo "$QOUT" | grep -q '"return": 8'; then
    echo "PASS"; exit 0
else
    echo "FAIL"; exit 1
fi
