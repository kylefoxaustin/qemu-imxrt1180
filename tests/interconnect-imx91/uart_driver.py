#!/usr/bin/env python3
#
# i.MX91-side UART driver for the uart-link-91-rt1180 holobench cell.
#
# Stands in for the i.MX91 (which writes /dev/ttyLP1 = LPUART2): connects to the
# RT1180 QEMU's LPUART2 socket, does the fleet's resend-until-connected GO
# handshake, then sends a known 32-byte pattern and verifies the RT1180's
# passthrough.elf echoes it back byte-exact.  Prints DRIVER: PASS / FAIL.
#
# Usage: uart_driver.py <port> [host]
# SPDX-License-Identifier: GPL-2.0-or-later
import socket, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 15791
host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"

# Connect (retry: the RT1180 may still be booting — connect-timing lesson).
sock = None
deadline = time.time() + 30
while time.time() < deadline and sock is None:
    try:
        sock = socket.create_connection((host, port), timeout=2)
    except OSError:
        time.sleep(0.2)
if sock is None:
    print("DRIVER: FAIL - could not connect to the RT1180 LPUART2 socket")
    sys.exit(1)
sock.settimeout(5)

def drain(n):
    got = b""
    try:
        while len(got) < n:
            d = sock.recv(n - len(got))
            if not d:
                break
            got += d
    except OSError:
        pass
    return got

# GO handshake: passthrough echoes everything, so a GO byte comes straight back.
GO = 0xA5
ok = True
sock.sendall(bytes([GO]))
if drain(1) != bytes([GO]):
    ok = False

# 32-byte pattern, verify the echo byte-exact.
pattern = bytes((0x10 + i) & 0xFF for i in range(32))
sock.sendall(pattern)
echo = drain(len(pattern))
if echo != pattern:
    ok = False

print("DRIVER: PASS - 91->RT1180 GO handshake + 32-byte echo byte-exact"
      if ok else f"DRIVER: FAIL - handshake/echo mismatch (echo={echo!r})")
sock.close()
sys.exit(0 if ok else 1)
