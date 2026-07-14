#!/usr/bin/env bash
# MCXN947 UDP-over-ENET test: boot the Zephyr echo_server, SLIRP-forward a host
# UDP port to the guest, send a datagram from the host and assert it is echoed
# back.  Connectionless datagram path over the ENET descriptor-ring DMA.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ECHO_ELF="${ECHO_ELF:-$HOME/mcxn-images/mcxn-echo.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
[ -f "$ECHO_ELF" ] || { echo "SKIP: no echo_server ELF"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: no python3"; exit 0; }
PORT=$(( (RANDOM % 20000) + 20000 )); CON="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none -serial "file:$CON" \
  -nic "user,model=mcxn-enet,hostfwd=udp::${PORT}-:4242" \
  -kernel "$ECHO_ELF" -no-reboot >/dev/null 2>&1 &
QPID=$!; trap 'kill $QPID 2>/dev/null; rm -f "$CON"' EXIT
# Wait for the DHCP lease (the IPv4 address), not just the listener banner:
# with a real entropy source the DHCPv4 client applies its randomized startup
# delay, so the lease can land ~9s in — probing before it raced and failed.
for i in $(seq 1 30); do grep -aqi 'IPv4 address' "$CON" 2>/dev/null && break; sleep 1; done
sleep 1
MSG="MCX-UDP-OVER-ENET-$$-$RANDOM"
REPLY=$(timeout -k 5 6 python3 -c '
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(5)
m=b"'"$MSG"'"
for _ in range(4):
    s.sendto(m,("127.0.0.1",'"$PORT"'))
    try:
        d,_=s.recvfrom(len(m)+16); sys.stdout.write(d.decode(errors="replace")); break
    except socket.timeout: pass' 2>/dev/null)
echo "sent: $MSG | echoed: $REPLY"
echo "$REPLY" | grep -q "$MSG" && { echo "PASS: UDP datagram echo round-trip over ENET"; exit 0; } || { echo "FAIL: no UDP echo"; exit 1; }
