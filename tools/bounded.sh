# shellcheck shell=bash
#
# bounded <secs> <cmd...>  --  a time limit that ACTUALLY BOUNDS THE COMMAND.
#
# ─────────────────────────────────────────────────────────────────────────────
# `timeout N cmd` DOES NOT DO THIS, AND IT LIES ABOUT IT. Both facts measured, on this
# box, 2026-07-14:
#
#   ① A CHILD THAT DOES NOT DIE ON SIGTERM IS NOT BOUNDED -- AND timeout STILL RETURNS 124.
#
#        timeout 2 bash -c 'trap "" TERM; sleep 8'
#          -> exit 124 after EIGHT seconds.
#
#      Exit 124 means "I timed out and killed it". It waited the full 8s and killed nothing.
#      A harness that reads 124 believes the process is gone. IT IS NOT.
#      (`-k` fixes this case: `timeout -k 1 2 ...` -> SIGKILL at +1s, exit 137, 3s total.)
#
#   ② EVEN WITH -k, THE SIGNAL REACHES THE CHILD -- NOT THE PROCESS GROUP.
#
#        timeout -k 1 2 bash -c 'bash -c "trap \"\" TERM; sleep 10" & wait'
#          -> exit 124 after 2s.  THE GRANDCHILD SURVIVED.
#
#      So `timeout N make run` kills `make` and ORPHANS THE QEMU underneath it -- which
#      then reparents to init and runs forever. If that QEMU is on a multicast group, it
#      is still BEACONING, and the next lab run lands on the same wire.
#
# ⭐ A KILL THAT REACHES THE WRAPPER AND NOT THE PROCESS IS NOT A KILL. (mcxn947qemu)
# ⭐ AN ORPHANED PROCESS ON A SHARED BUS IS NOT A LEAK. IT IS A LIAR THAT OUTLIVED THE RUN
#    THAT CREATED IT -- AND ITS TESTIMONY IS INDISTINGUISHABLE FROM A PEER'S. (holobench)
#
# This is not theoretical. A census of this box today found 15 hours of an orphaned
# rt1180 QEMU (mine), and two 6-hour orphans on a LIVE multicast group -- one of them
# running an IMPOSTOR beacon.
#
# So: run the command in its OWN PROCESS GROUP (setsid) and signal THE GROUP.
#
# Returns the command's exit status; 124 if it had to be timed out (matching `timeout`).
# SPDX-License-Identifier: GPL-2.0-or-later

bounded() {
    local secs=$1; shift
    local pg rc reaper

    setsid "$@" &
    pg=$!

    # The reaper signals the GROUP (-$pg), not the leader. SIGTERM, then SIGKILL 5s later:
    # a process that ignores TERM must still die, or it becomes tomorrow's ghost.
    ( sleep "$secs"
      kill -TERM -"$pg" 2>/dev/null || exit 0
      sleep 5
      kill -KILL -"$pg" 2>/dev/null
    ) &
    reaper=$!

    if wait "$pg" 2>/dev/null; then
        rc=0
    else
        rc=$?
    fi

    # Stop the reaper's timer -- but DO NOT rely on it having finished the job.
    #
    # ⚠ THIS LINE WAS THE BUG, AND IT IS THE SAME BUG THIS FILE EXISTS TO FIX.
    #   The group leader dies on SIGTERM, `wait` returns immediately, and I cancelled the
    #   reaper BEFORE its SIGKILL escalation ever ran -- so a grandchild that IGNORED
    #   SIGTERM survived, while bounded() returned a confident 124.
    #   Measured: exit 124 after 2s, grandchild still alive. A reaper that gets reaped
    #   first is not a reaper.
    kill -TERM "$reaper" 2>/dev/null
    wait "$reaper" 2>/dev/null

    # SO SWEEP THE GROUP OURSELVES, ALWAYS. The leader being gone says nothing about its
    # children: an orphan is exactly a child that outlived the process we were watching.
    if [ "$rc" -eq 143 ] || [ "$rc" -eq 137 ]; then
        sleep 2                          # grace: let the group unwind after SIGTERM
        kill -KILL -"$pg" 2>/dev/null    # then kill whatever ignored it
        rc=124                           # report it the way `timeout` does
    else
        kill -KILL -"$pg" 2>/dev/null    # exited on its own -- sweep any strays it left
    fi
    return "$rc"
}
