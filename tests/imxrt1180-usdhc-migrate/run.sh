#!/usr/bin/env bash
# uSDHC VEND_SPEC across migration, and the predicate that decides whether the
# subsection is emitted at all.
#   values : the guest's value survives -- INCLUDING a deliberate zero
#   format : the state file GROWS only when the field differs from its reset value,
#            so every other SDHCI platform's wire format is byte-for-byte unchanged.
# A migration test that only checks values cannot see a wire format it has broken.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$DIR/../../build/qemu-system-arm}"
exec python3 "$DIR/check.py"
