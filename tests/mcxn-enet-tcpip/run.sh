#!/usr/bin/env bash
# MCXN947 real TCP/IP stack over ENET: boot the Zephyr dhcpv4_client sample
# (built with this dir's overlay + extra.conf, enabling the ENET-QoS MAC + a
# PHY at MDIO addr 2), attach QEMU's SLIRP backend (which runs a DHCP server),
# and assert the guest obtains a DHCP lease — exercising the full L2/ARP/UDP/IP
# path over the ENET descriptor-ring DMA with a real network stack.
#
# Build the ELF in a Zephyr workspace:
#   west build -b frdm_mcxn947/mcxn947/cpu0 samples/net/dhcpv4_client \
#     -- -DDTC_OVERLAY_FILE=$PWD/frdm_mcxn947.overlay -DEXTRA_CONF_FILE=$PWD/extra.conf
# Override the ELF with DHCP_ELF=.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
DHCP_ELF="${DHCP_ELF:-$HOME/mcxn-images/mcxn-dhcp.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
[ -f "$DHCP_ELF" ] || { echo "SKIP: no DHCP ELF at $DHCP_ELF (build the Zephyr sample)"; exit 0; }
OUT="$(timeout -k 5 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -nic user,model=mcxn-enet -kernel "$DHCP_ELF" -no-reboot 2>/dev/null \
        | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g' || true)"
echo "--- guest output ---"; echo "$OUT" | grep -aiE 'dhcp|address|subnet|router|lease' | head; echo "--------------------"
echo "$OUT" | grep -q "Received: 10.0.2.15" && { echo "PASS: MCX got a DHCP lease (10.0.2.15) over ENET"; exit 0; } || { echo "FAIL"; exit 1; }
