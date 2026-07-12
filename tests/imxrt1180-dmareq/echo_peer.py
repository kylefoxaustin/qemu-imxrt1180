#!/usr/bin/env python3
# Board-to-board UART peer: connect to the QEMU LPUART2 socket and echo bytes.
import socket, sys, time
host, port = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 14780
deadline = time.time() + 30
s = None
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2); break
    except OSError:
        time.sleep(0.2)
if s is None:
    print("peer: could not connect", file=sys.stderr); sys.exit(1)
s.settimeout(20)
try:
    while True:
        data = s.recv(256)
        if not data:
            break
        s.sendall(data)          # echo everything
except OSError:
    pass
