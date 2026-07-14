#!/usr/bin/env bash
# Delete allowlist lines the gate says are now FIXED.
#
# The gate REFUSES to pass while a listed deviation matches the RM ("delete the
# line"), because an allowlist that never shrinks stops being a to-do list and
# becomes a certificate.  This just performs the deletion it is demanding -- it
# does not decide anything.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

DUMP_ALL=1 ./run.sh 2>&1 | sed -n '/now MATCH the RM/,/^$/p' \
    | grep -E '^\s+\S+\s+\S+$' > "$TMP" || true

n=$(wc -l < "$TMP")
if [ "$n" -eq 0 ]; then
    echo "nothing to prune."
    exit 0
fi

python3 - "$TMP" <<'PY'
import sys
fixed = {tuple(l.split()) for l in open(sys.argv[1]) if l.strip()}
out, gone = [], 0
for line in open("known-deviations.txt"):
    s = line.strip()
    if s and not s.startswith("#"):
        p = s.split()
        if (p[0], p[1]) in fixed:
            gone += 1
            continue
    out.append(line)
open("known-deviations.txt", "w").writelines(out)
print("pruned %d FIXED entries from the allowlist" % gone)
PY
