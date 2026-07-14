#!/usr/bin/env bash
#
# LEAK AUDIT — two checks, neither of which needs to know what the processes are.
#
# ─────────────────────────────────────────────────────────────────────────────
#   ① THE LIVE CHECK (93emulator's, and it convicts on the process's own testimony):
#
#        A `timeout N` process whose own elapsed time exceeds N has, BY DEFINITION,
#        failed to kill its child. The death sentence is in its own command line.
#        You do not need to know whose it is or whether it is "live work".
#
#      ⭐ DO NOT ACQUIT A BOX ON A PROXY. "It belongs to an active session working on that
#         thing" is a proxy — and A FALSE ACQUITTAL IS THE MORE DANGEROUS ERROR, because a
#         false conviction gets re-checked and a false acquittal gets filed as CLEAN.
#
#   ② THE SOURCE CHECK: every `timeout` in this repo that cannot escalate past SIGTERM.
#
# ─────────────────────────────────────────────────────────────────────────────
# ⚠ AND THE DETECTOR NEEDED A DETECTOR. THREE TIMES, TODAY, IN THIS FILE'S ANCESTORS:
#
#   1. `grep -E 'timeout [0-9]+'` — LITERAL DIGITS ONLY. Four of our harnesses write
#      `timeout "$HANG_GUARD"`, and the grep could not see them. I reported "none left".
#      (93emulator hit the identical bug and printed "NONE — every timeout is -s KILL ✅"
#       directly beneath the five offenders it had just listed.)
#
#   2. Then the fixed grep flagged `timeout -s KILL 10` as a LEAK. It is not: -s KILL sends
#      an UNCATCHABLE signal immediately. The detector could not see THAT form either.
#
#   3. And `pgrep -f "sleep 30"` matched the TEST HARNESS'S OWN COMMAND LINE, so a
#      correctly-reaped process was reported as surviving — twice.
#
#     ⭐ A DETECTOR THAT CAN ONLY SEE ONE FORM OF THE THING CANNOT SEE THE FORM YOU
#        ACTUALLY USE. THE CENSUS NEEDS A CENSUS. (93emulator)
#     ⭐ A PATTERN THAT MATCHES THE PROCESS DOING THE MATCHING IS NOT A DETECTOR.
#
# A timeout is SAFE if it can escalate past a SIGTERM the child may ignore:
#     timeout -k <n> <secs> ...      (SIGTERM, then SIGKILL)
#     timeout -s KILL <secs> ...     (SIGKILL outright)
#     timeout --signal=KILL ...      (ditto)
# Anything else sends only SIGTERM and then waits FOREVER for a child that may never answer.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
rc=0

echo "── ① LIVE: a 'timeout N' older than N failed to kill its child ──────────────"
live=$(ps -eo pid,etimes,etime,args 2>/dev/null |
       awk '$4=="timeout" && $5+0>0 && $2 > $5*10 {printf "  LEAK  pid %-8s alive %-10s under `%s %s`\n",$1,$3,$4,$5}')
if [ -n "$live" ]; then
    echo "$live"
    echo "  ⇒ these have burned real core-hours. ps %CPU is a LIFETIME average, so 100%"
    echo "    over 3h means three core-hours ACTUALLY CONSUMED — and every number measured"
    echo "    on this box while they ran is depressed."
    rc=1
else
    echo "  none — no timeout has outlived its own deadline"
fi

echo
echo "── ② SOURCE: timeouts in this repo that cannot escalate past SIGTERM ─────────"
bad=0
while IFS= read -r hit; do
    line=${hit#*:*:}
    # strip comments
    case "${line#"${line%%[![:space:]]*}"}" in '#'*) continue ;; esac
    # SAFE forms: -k / --kill-after / -s KILL / --signal=KILL
    if printf '%s' "$line" | grep -qE 'timeout[[:space:]]+((-k|--kill-after)[[:space:]]|(-s[[:space:]]+KILL|--signal=KILL))'; then
        continue
    fi
    printf "  ☠️  %s\n" "$(printf '%s' "$hit" | cut -c1-100)"
    bad=$((bad + 1))
done < <(grep -rnE '(^|[^-[:alnum:]_])timeout[[:space:]]+("?\$|-|[0-9])' \
             tools/*.sh tests/*/*.sh 2>/dev/null)

if [ "$bad" -eq 0 ]; then
    echo "  none — every timeout can escalate past a SIGTERM the child may ignore"
else
    echo "  ⇒ $bad harness(es) send only SIGTERM. A wedged QEMU never services it, and"
    echo "    'timeout' then waits forever — while STILL RETURNING 124. Use -k, or bounded()."
    rc=1
fi
exit $rc
