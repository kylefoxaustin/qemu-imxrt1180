#!/usr/bin/env bash
# MCXN947 TCP-over-ENET test: boot the Zephyr echo_server sample (built with this
# dir's overlay enabling ENET-QoS + a PHY at MDIO addr 2), attach SLIRP with a
# host->guest TCP port-forward, then make a REAL TCP connection from the host and
# assert the payload is echoed back.  Exercises TCP SYN/ACK + data transfer over
# the ENET descriptor-ring DMA (the full transport layer, not just UDP/DHCP).
#
# Build: west build -b frdm_mcxn947/mcxn947/cpu0 samples/net/sockets/echo_server \
#   -- -DDTC_OVERLAY_FILE=$PWD/frdm_mcxn947.overlay -DEXTRA_CONF_FILE=$PWD/extra.conf
# Override the ELF with ECHO_ELF=.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ECHO_ELF="${ECHO_ELF:-$HOME/mcxn-images/mcxn-echo.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
[ -f "$ECHO_ELF" ] || { echo "SKIP: no echo_server ELF at $ECHO_ELF"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
PORT=$(( (RANDOM % 20000) + 20000 )); CON="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none -serial "file:$CON" \
  -nic "user,model=mcxn-enet,hostfwd=tcp::${PORT}-:4242" \
  -kernel "$ECHO_ELF" -no-reboot >/dev/null 2>&1 &
QPID=$!; trap 'kill $QPID 2>/dev/null; rm -f "$CON"' EXIT
# Wait for the DHCP lease (the IPv4 address), not just the listener banner:
# with a real entropy source the DHCPv4 client applies its randomized startup
# delay, so the lease can land ~9s in — probing before it raced and failed.
for i in $(seq 1 30); do grep -aqi 'IPv4 address' "$CON" 2>/dev/null && break; sleep 1; done
sleep 1
MSG="MCX-TCP-OVER-ENET-$$-$RANDOM"
# Retry the whole connect+echo round-trip: the lease can be applied a beat
# before the echo_server's TCP listener is accepting, and slirp can drop the
# first SYN — a single attempt is a flaky oracle (per the fleet net-flake
# post-mortem), so re-attempt a few times within the outer timeout.
REPLY=$(printf '%s\n' "$MSG" | timeout -k 5 12 python3 -c '
import socket,sys,time
d=sys.stdin.buffer.read()
for _ in range(4):
    try:
        s=socket.create_connection(("127.0.0.1",'"$PORT"'),timeout=5)
        s.sendall(d)
        sys.stdout.write(s.recv(len(d)+16).decode(errors="replace")); s.close()
        break
    except OSError:
        time.sleep(1)' 2>/dev/null)
echo "sent: $MSG | echoed: $REPLY"
echo "$REPLY" | grep -q "$MSG" && { echo "PASS: TCP connect + echo round-trip over ENET"; exit 0; } || { echo "FAIL: no TCP echo"; exit 1; }
