#!/usr/bin/env bash
# edge_cases.sh - the difficult inputs. A scheduler must work on more than
# the demonstration workload.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./tsim
[ -x "$BIN" ] || { echo "run make first"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0

ok()  { PASS=$((PASS+1)); printf '  \033[1;32mPASS\033[0m  %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  \033[1;31mFAIL\033[0m  %s\n' "$1"; }

# Run the simulator. It must finish without an error, and each process
# must terminate.
expect_clean() {
    local desc="$1"; shift
    local out rc
    out=$(timeout 180 "$BIN" -C -Q "$@" 2>&1); rc=$?
    if [ $rc -ne 0 ]; then
        bad "$desc (exit $rc)"
        printf '        %s\n' "$(printf '%s\n' "$out" | tail -2)"
        return
    fi
    if printf '%s\n' "$out" | grep -q 'ERROR'; then
        bad "$desc (an ERROR was logged)"
        printf '        %s\n' "$(printf '%s\n' "$out" | grep ERROR | head -2)"
        return
    fi
    if printf '%s\n' "$out" | awk '/^  P[0-9]/ {if ($7=="-") e=1} END{exit e?1:0}'
    then ok "$desc"; else bad "$desc (a process never terminated)"; fi
}

echo
echo "tsim edge cases"
echo

# -- one process only: nothing to switch to --------------------------
printf 'solo 200\n' > "$TMP/one.txt"
expect_clean "a single process with no competition" -w "$TMP/one.txt"

# -- a process that wants less CPU than one tick ---------------------
printf 'blink 5\nnormal 200\n' > "$TMP/short.txt"
expect_clean "a process shorter than one timer tick" -w "$TMP/short.txt" -t 50

# -- fill the process table to its limit ----------------------------
: > "$TMP/many.txt"
for i in $(seq 1 32); do echo "job$i 40 1 20 $((i % 10)) $((i * 5))" >> "$TMP/many.txt"; done
expect_clean "the process table filled to its limit (32)" -w "$TMP/many.txt"

# -- one more than the table can hold must be refused ---------------
cp "$TMP/many.txt" "$TMP/toomany.txt"; echo "overflow 40" >> "$TMP/toomany.txt"
if ! timeout 60 "$BIN" -C -Q -w "$TMP/toomany.txt" >/dev/null 2>&1
then ok "a workload larger than the process table is refused"
else bad "a workload larger than the process table was accepted"; fi

# -- a 1 ms tick with a 1 tick quantum, so each interrupt switches ---
expect_clean "1 ms tick, 1 tick quantum (switching flat out)" \
             -t 1 -q 1 -m 20000 -w workloads/batch.txt

# -- a quantum longer than any process needs ------------------------
expect_clean "a quantum nothing can use up" -q 500 -m 20000 -w workloads/batch.txt

# -- every process blocked at once. The CPU must go idle and recover -
out=$(timeout 180 "$BIN" -C -Q -w workloads/io-heavy.txt 2>&1)
idle=$(printf '%s\n' "$out" | awk '/CPU idle/ {print $3}')
if [ "${idle:-0}" -gt 0 ]
then ok "the CPU goes idle when every process is waiting for I/O (${idle} ms)"
else bad "an I/O-bound workload reported no idle time at all"; fi

# -- I/O shorter than a tick must still take at least one tick ------
printf 'fast-io 100 8 1 0 0\n' > "$TMP/fastio.txt"
expect_clean "I/O faster than the tick resolution" -w "$TMP/fastio.txt"

# -- no preemption plus blocking calls ------------------------------
expect_clean "fcfs with processes that block" -p fcfs -w workloads/interactive.txt

# -- the watchdog must stop the run instead of hanging, and it must kill
#    the remaining children ---------------------------------------------
out=$(timeout 60 "$BIN" -C -Q -m 5 -w workloads/batch.txt 2>&1)
ticks=$(printf '%s\n' "$out" | awk '/simulated time/ {
            for (i = 1; i <= NF; i++) if ($i == "ticks") print $(i-1) }')
killed=$(printf '%s\n' "$out" | awk '/^  P[0-9]/ && $4 == -9' | wc -l)
if [ "${ticks:-0}" = 5 ] && [ "${killed:-0}" -gt 0 ]
then ok "the watchdog stops the run at the tick limit and kills the survivors"
else bad "the watchdog did not fire (ran $ticks ticks, killed $killed)"; fi

# -- Ctrl-C must stop the machine and leave no child process --------
timeout 180 "$BIN" -C -w workloads/batch.txt > "$TMP/sig.log" 2>&1 &
kpid=$!
sleep 1
kill -INT "$kpid" 2>/dev/null
wait "$kpid" 2>/dev/null
sleep 0.5
left=$(pgrep -f "[t]sim" | wc -l)
if grep -q 'operator interrupt' "$TMP/sig.log" && [ "$left" = 0 ]
then ok "Ctrl-C shuts down cleanly and kills every child"
else bad "Ctrl-C left $left process(es) behind"; fi

# -- a report must still be produced after an interrupt ------------
if grep -q 'SIMULATION REPORT' "$TMP/sig.log"
then ok "a report is still produced after an interrupt"
else bad "no report after an interrupt"; fi

# -- zombies ------------------------------------------------------
timeout 180 "$BIN" -C -Q -w workloads/interactive.txt >/dev/null 2>&1
z=$(ps -eo stat= | grep -c '^Z' || true)
if [ "${z:-0}" -eq 0 ]
then ok "no zombie processes left in the system"
else bad "$z zombie process(es) found"; fi

# -- a run longer than the trace buffer must still complete ---------
expect_clean "a run longer than the trace buffer" -t 1 -m 6000 -w workloads/batch.txt

echo
echo "  $PASS passed, $FAIL failed"
echo
[ "$FAIL" = 0 ] || exit 1
