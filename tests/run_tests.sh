#!/usr/bin/env bash
# run_tests.sh - check that the simulator operates as a time-sharing system.
# Each test states a requirement about measurable behavior. No test examines
# the structure of the code.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./tsim
[ -x "$BIN" ] || { echo "run make first"; exit 1; }

PASS=0
FAIL=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ok()   { PASS=$((PASS+1)); printf '  \033[1;32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[1;31mFAIL\033[0m  %s\n' "$1"
         [ $# -gt 1 ] && printf '        %s\n' "$2"; }

check() { # description, condition-result
    if [ "$2" = 0 ]; then ok "$1"; else bad "$1" "${3:-}"; fi
}

echo
echo "tsim regression tests"
echo

# ---------------------------------------------------------------- 1
run() { timeout 180 "$BIN" -C -Q "$@" 2>"$TMP/err"; }

for pol in fcfs rr prio mlfq; do
    out=$(run -p "$pol" -V -w workloads/interactive.txt)
    rc=$?
    if [ $rc -ne 0 ]; then
        bad "policy $pol completes" "exit status $rc: $(head -2 "$TMP/err")"
        continue
    fi
    ok "policy $pol completes"

    # every process must reach termination, so every row must have a turnaround
    if printf '%s\n' "$out" | awk '/^  P[0-9]/ {if ($7 == "-") bad=1} END {exit bad?1:0}'; then
        ok "policy $pol: every process terminated"
    else
        bad "policy $pol: some process never finished"
    fi

    # the independent /proc check must not have seen two runnable children
    v=$(printf '%s\n' "$out" | awk '/EXCLUSIVITY/ {
            for (i = 1; i <= NF; i++) if ($i ~ /^violation/) print $(i-1) }')
    check "policy $pol: single-CPU rule held ($v violations)" \
          "$([ "${v:-1}" = 0 ] && echo 0 || echo 1)"
done

# ---------------------------------------------------------------- 2
# FCFS must never preempt. The other policies must preempt.
out=$(run -p fcfs -w workloads/batch.txt)
forced=$(printf '%s\n' "$out" | awk '/context switches/ {gsub(/\(/,"",$4); print $4}')
check "fcfs performs no timer-forced preemption (got ${forced:-?})" \
      "$([ "${forced:-1}" = 0 ] && echo 0 || echo 1)"

out=$(run -p rr -q 1 -w workloads/batch.txt)
forced=$(printf '%s\n' "$out" | awk '/context switches/ {gsub(/\(/,"",$4); print $4}')
check "round robin does preempt (got ${forced:-?})" \
      "$([ "${forced:-0}" -gt 0 ] && echo 0 || echo 1)"

# ---------------------------------------------------------------- 3
# A smaller quantum must cause more context switches. If it does not, the
# timer does not drive the scheduler.
big=$(run -p rr -q 8 -w workloads/batch.txt | awk '/context switches/ {print $3}')
small=$(run -p rr -q 1 -w workloads/batch.txt | awk '/context switches/ {print $3}')
check "a shorter quantum causes more switches ($big -> $small)" \
      "$([ "${small:-0}" -gt "${big:-0}" ] && echo 0 || echo 1)"

# ---------------------------------------------------------------- 4
# Time sharing exists to improve response time. The feedback queue must
# give a better response time than FCFS on the interactive workload.
rfcfs=$(run -p fcfs -w workloads/interactive.txt | awk '/response time/ {print $3}')
rmlfq=$(run -p mlfq -w workloads/interactive.txt | awk '/response time/ {print $3}')
check "mlfq response time beats fcfs ($rfcfs ms -> $rmlfq ms)" \
      "$(awk -v a="$rfcfs" -v b="$rmlfq" 'BEGIN{exit !(b<a)}' && echo 0 || echo 1)"

# ---------------------------------------------------------------- 5
# The periodic boost must give the starved process the CPU. Under static
# priority the nice 9 job waits much longer than under the feedback queue.
wprio=$(run -p prio -w workloads/starvation.txt | awk '/^  P4/ {print $8}')
wmlfq=$(run -p mlfq -w workloads/starvation.txt | awk '/^  P4/ {print $8}')
check "mlfq boost rescues the starved process (resp ${wprio} ms -> ${wmlfq} ms)" \
      "$(awk -v a="$wprio" -v b="$wmlfq" 'BEGIN{exit !(b<a)}' && echo 0 || echo 1)"

# ---------------------------------------------------------------- 6
# The accounting of the scheduler must agree with the CPU time that Linux
# charged the children. A difference above 15% indicates a fault.
out=$(run -p mlfq -w workloads/interactive.txt)
mine=$(printf '%s\n' "$out" | awk '/scheduler accounting/ {print $3}')
real=$(printf '%s\n' "$out" | awk '/scheduler accounting/ {print $6}')
check "scheduler accounting agrees with getrusage ($mine ms vs $real ms)" \
      "$(awk -v a="$mine" -v b="$real" \
         'BEGIN{if(b<=0){exit 1} d=(a-b)/b; if(d<0)d=-d; exit !(d<0.15)}' \
         && echo 0 || echo 1)"

# ------------------------------------------------------------- 6b
# Check each process, not only the total. The kernel can record a process as
# the holder of the CPU while the process sleeps and waits for a lost I/O
# completion. Such a process gains scheduler CPU time and no real CPU time.
# This comparison detects a lost signal. A short tick and --verify make the
# kernel fall behind the timer, which opens the window for the fault.
lost=0
for args in "-t 1 -q 1 -m 40000" "-t 2 -q 2 -m 20000" "-t 4 -m 10000"; do
    # shellcheck disable=SC2086
    n=$(run -V $args -w workloads/interactive.txt | awk '
            /^  P[0-9]/ { s = $5 + 0; o = $12 + 0;
                          if (o >= 20 && s > o * 2 + 100) c++ }
            END { print c + 0 }')
    lost=$((lost + n))
done
check "no process credited with CPU time it did not use ($lost suspect)" \
      "$([ "${lost:-1}" = 0 ] && echo 0 || echo 1)"

# ---------------------------------------------------------------- 7
# The run must leave no zombie process and no running child.
before=$(pgrep -c -P $$ 2>/dev/null || echo 0)
run -p mlfq -w workloads/io-heavy.txt >/dev/null
sleep 0.3
leaked=$(pgrep -f "[t]sim" | wc -l)
check "no simulator processes left running (found $leaked)" \
      "$([ "$leaked" = 0 ] && echo 0 || echo 1)"
: "$before"

# ---------------------------------------------------------------- 8
# The raw system call path must give the same results as the wrappers.
a=$(run -p rr -w workloads/batch.txt    | awk '/turnaround time/ {print $3}')
b=$(run -p rr -R -w workloads/batch.txt | awk '/turnaround time/ {print $3}')
check "raw syscalls behave like the wrappers ($a ms vs $b ms)" \
      "$(awk -v a="$a" -v b="$b" \
         'BEGIN{if(a<=0){exit 1} d=(a-b)/a; if(d<0)d=-d; exit !(d<0.25)}' \
         && echo 0 || echo 1)"

# ---------------------------------------------------------------- 9
# The CSV trace must have a correct header and hold the scheduling events.
run -p mlfq -c "$TMP/trace.csv" -w workloads/interactive.txt >/dev/null
hdr=$(head -1 "$TMP/trace.csv" 2>/dev/null)
nd=$(grep -c ',DISPATCH,' "$TMP/trace.csv" 2>/dev/null || echo 0)
check "CSV trace written with a header and $nd DISPATCH rows" \
      "$([ "$hdr" = 'tick,ms,event,detail' ] && [ "${nd:-0}" -gt 0 ] && echo 0 || echo 1)"

# --------------------------------------------------------------- 10
# The program must refuse bad input instead of running part of it.
printf 'broken\n' > "$TMP/bad.txt"
timeout 20 "$BIN" -C -Q -w "$TMP/bad.txt" >/dev/null 2>&1
check "an incomplete workload line is rejected" \
      "$([ $? -ne 0 ] && echo 0 || echo 1)"

timeout 20 "$BIN" -C -Q -p nonsense >/dev/null 2>&1
check "an unknown policy name is rejected" \
      "$([ $? -ne 0 ] && echo 0 || echo 1)"

echo
echo "  $PASS passed, $FAIL failed"
echo
[ "$FAIL" = 0 ] || exit 1
