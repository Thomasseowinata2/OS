#!/usr/bin/env bash
# stress.sh - search for lost signals.
#
# The fault to detect: the kernel records a process as the holder of the CPU
# while the process sleeps and waits for an I/O completion that never
# arrives. The scheduler then credits the process with much more CPU time
# than Linux reports. That comparison detects the fault.
#
# The window for the fault opens only when the kernel falls behind the
# timer. The runs below use a short tick and --verify, which makes each tick
# expensive.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./tsim
[ -x "$BIN" ] || { echo "run make first"; exit 1; }

ROUNDS="${1:-20}"
BAD=0
RUNS=0

echo
echo "signal-loss stress test: $ROUNDS rounds"
echo

# Each process row holds:
#   Pn name pid exit CPU WAIT TURN RESP DSP PRE IO OS-CPU
# A sleeping process that the kernel records as running gains scheduler CPU
# time and no real CPU time. The CPU column then exceeds the OS-CPU column.
detect() {
    awk -v tag="$1" '
        /^  P[0-9]/ {
            sched = $5 + 0; os = $12 + 0; dsp = $9 + 0;
            # ignore a process that never ran
            if (os < 20) next;
            if (sched > os * 2 + 100) {
                printf "    LOST SIGNAL [%s] %s: scheduler %d ms, Linux %d ms, %d dispatches\n",
                       tag, $2, sched, os, dsp;
                bad = 1;
            }
        }
        /the machine is stalled|did not stop|were lost|second I\/O request/ {
            printf "    KERNEL ERROR [%s] %s\n", tag, $0; bad = 1;
        }
        END { exit bad ? 1 : 0 }'
}

run_round() {
    local tag="$1"; shift
    RUNS=$((RUNS + 1))
    if ! timeout 300 "$BIN" -C "$@" 2>&1 | detect "$tag"; then
        BAD=$((BAD + 1))
    fi
}

i=1
while [ "$i" -le "$ROUNDS" ]; do
    printf '\r  round %d/%d  (%d failure(s) so far)   ' "$i" "$ROUNDS" "$BAD"

    # the default configuration, with the /proc check
    run_round "default+verify"     -V
    # a short tick, so the kernel stays behind the timer
    run_round "1ms tick"           -V -t 1 -q 1 -m 40000
    run_round "2ms tick"           -V -t 2 -q 2 -m 20000
    # many blocking calls. More requests give more chances of a lost signal.
    run_round "io-heavy"           -V -t 2 -w workloads/io-heavy.txt -m 20000
    run_round "io-heavy fast"      -t 1 -q 1 -w workloads/io-heavy.txt -m 40000
    # a full process table, with every process making blocking calls
    run_round "interactive"        -V -t 2 -w workloads/interactive.txt -m 20000

    i=$((i + 1))
done

printf '\r%-60s\n' "  done"
echo
echo "  $RUNS runs, $BAD showed a lost signal or a kernel error"
echo
[ "$BAD" = 0 ] || exit 1
