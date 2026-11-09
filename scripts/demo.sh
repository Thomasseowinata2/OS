#!/usr/bin/env bash
# demo.sh - the demonstration sequence for the submitted video.
#
# Each step waits for the Enter key, so the narration can keep pace. Run the
# script with --auto to let it play through without input.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./tsim
[ -x "$BIN" ] || { echo "run make first"; exit 1; }

AUTO=0
[ "${1:-}" = "--auto" ] && AUTO=1

pause() {
    if [ "$AUTO" = 1 ]; then sleep 2; else
        printf '\n  -- press Enter to continue --'; read -r _; fi
}

banner() {
    echo
    echo "=============================================================================="
    echo "  $1"
    echo "=============================================================================="
    echo
}

banner "1. The simulated machine"
./"$BIN" --help | sed -n '1,3p'
echo
echo "  The parent process is the kernel. Each simulated program is a real"
echo "  child process. A POSIX interval timer sends SIGALRM, which acts as the"
echo "  timer interrupt. SIGSTOP and SIGCONT perform the context switch."
echo
echo "  The programs in this run:"
echo
grep -v '^#' workloads/interactive.txt
pause

banner "2. No timer: first come, first served"
echo "  cpu-hog arrives first and never blocks, so it keeps the CPU until it"
echo "  ends. Watch the response time of the shell."
echo
timeout 120 ./"$BIN" -p fcfs -w workloads/interactive.txt
pause

banner "3. The same programs with the timer: round robin"
timeout 120 ./"$BIN" -p rr -w workloads/interactive.txt
pause

banner "4. A feedback queue, which measures which programs are interactive"
timeout 120 ./"$BIN" -p mlfq -w workloads/interactive.txt
pause

banner "5. Starvation under static priority, and the cure"
echo "  Static priority. The nice 9 process does not reach the CPU."
echo
timeout 120 ./"$BIN" -p prio -w workloads/starvation.txt -Q
echo
echo "  The same programs under a feedback queue with a periodic boost:"
echo
timeout 120 ./"$BIN" -p mlfq -w workloads/starvation.txt -Q
pause

banner "6. All four policies side by side"
./scripts/compare.sh workloads/interactive.txt
pause

banner "7. Proof of the single-CPU rule"
echo "  The --verify option reads /proc/PID/stat for each child on each tick."
echo "  It counts the children that Linux reports as runnable. That count"
echo "  must never exceed one."
echo
timeout 120 ./"$BIN" -p mlfq -V -Q -w workloads/interactive.txt | tail -5
pause

banner "8. The system calls"
echo "  The --raw-syscalls option replaces the glibc wrappers with syscall(2)."
echo
timeout 120 ./"$BIN" -R -Q -w workloads/io-heavy.txt | sed -n '/simulated time/p;/MACHINE/,/throughput/p'

if command -v strace >/dev/null 2>&1; then
    echo
    echo "  The same run under strace. The output shows the timer start and"
    echo "  the signals that perform each context switch."
    echo
    strace -f -e trace=timer_create,timer_settime,kill,rt_sigqueueinfo \
        ./"$BIN" -Q -C -m 30 -w workloads/io-heavy.txt 2>&1 \
        | grep -E 'timer_create|timer_settime|kill|sigqueue' | head -14
else
    echo
    echo "  strace is not installed, so this step cannot show the system calls."
    echo "  To install it and run the trace:"
    echo
    echo "    sudo apt install strace"
    echo "    strace -e trace=timer_create,timer_settime,kill ./tsim -Q -m 30"
fi
echo

banner "End of the demonstration"
