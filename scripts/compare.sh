#!/usr/bin/env bash
# compare.sh - run one workload under all four policies and print the main
# numbers side by side.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./tsim
[ -x "$BIN" ] || { echo "run make first"; exit 1; }

WORKLOAD="${1:-workloads/interactive.txt}"
echo
echo "workload: $WORKLOAD"
echo

printf '%-28s %10s %10s %10s %8s %8s\n' \
    POLICY AVG-WAIT AVG-TURN AVG-RESP SWITCHES FORCED
printf '%-28s %10s %10s %10s %8s %8s\n' \
    '----------------------------' ---------- ---------- ---------- -------- --------

label_of() {
    case "$1" in
        fcfs) echo "fcfs  (no preemption)" ;;
        rr)   echo "rr    (round robin)" ;;
        prio) echo "prio  (static priority)" ;;
        mlfq) echo "mlfq  (feedback queue)" ;;
        *)    echo "$1" ;;
    esac
}

for p in fcfs rr prio mlfq; do
    out=$("$BIN" -C -Q -p "$p" -w "$WORKLOAD" 2>/dev/null)
    wait=$(printf '%s\n' "$out" | awk '/waiting time/    {print $3}')
    turn=$(printf '%s\n' "$out" | awk '/turnaround time/ {print $3}')
    resp=$(printf '%s\n' "$out" | awk '/response time/   {print $3}')
    swi=$(printf  '%s\n' "$out" | awk '/context switches/{print $3}')
    pre=$(printf  '%s\n' "$out" | awk '/context switches/{gsub(/\(/,"",$4); print $4}')
    printf '%-28s %10s %10s %10s %8s %8s\n' \
        "$(label_of "$p")" "${wait:--}" "${turn:--}" "${resp:--}" \
        "${swi:--}" "${pre:--}"
done

cat <<'NOTES'

  avg-wait  time that a process spent READY and did not hold the CPU
  avg-turn  time from submission to termination
  avg-resp  time from submission until the process first held the CPU
  forced    context switches caused by the timer, not by a process that
            gave up the CPU by itself

Results to look for:

  1. FCFS forces no context switch and gives the worst response time.
     Without a timer, a compute-bound program keeps the machine until it
     ends.
  2. Round robin improves the response time and causes many more switches.
  3. Static priority is good for the favored processes. It can starve the
     others. Run it against workloads/starvation.txt.
  4. The feedback queue gives interactive processes a response time close to
     round robin, and it switches the compute-bound processes less often.
NOTES
