# How to run tsim, and how the code works

**COMP6697001 Operating Systems - AOL Case Study**

This document has two parts. Part A runs the program on WSL Ubuntu. Part B
explains the code in the order that it executes.

Read this file first. `docs/DESIGN.md` holds the full reference, including the
eleven race conditions. `docs/INSTALL_WSL.md` holds the installation of WSL
and Ubuntu from the start.

---

# Part A. Run it on WSL Ubuntu

## A1. Open the Ubuntu shell

Open Windows Terminal or PowerShell, then start the distribution.

```powershell
wsl -d Ubuntu-24.04
```

Check that WSL reports version 2. The project builds on WSL 1, but the check
against `/proc` does not give correct results there.

```powershell
wsl -l -v
```

```
  NAME              STATE           VERSION
* Ubuntu-24.04      Running         2
```

## A2. Go to the project

Windows drives appear under `/mnt` inside WSL. The path contains a space, so
put it in quotation marks.

```bash
cd "/mnt/c/Users/thoma/Documents/Project OS"
```

## A3. Check the toolchain

```bash
./scripts/setup_wsl.sh
```

The script prints the distribution, the kernel and the compiler. It lists any
missing package and asks before it installs one. It then builds the project
and runs a short job to show that the timer works.

To check the toolchain by hand:

```bash
gcc --version      # expect gcc 13.3.0 on Ubuntu 24.04
make --version     # expect GNU Make 4.3
```

If `gcc` is absent:

```bash
sudo apt update && sudo apt install -y build-essential
```

## A4. Build

```bash
make
```

The build must print no warning. It compiles 11 source files and links them
with `-lrt`.

```
cc -O2 -g -std=c11 -Wall -Wextra -Wpedantic ... -c -o build/kernel.o src/kernel.c
...
built ./tsim
```

## A5. Run it

```bash
make run
```

That command runs the built-in set of four programs under a multi-level
feedback queue. The output has three sections.

1. A live trace, with one line for each scheduling event.
2. A table with one row for each process.
3. A chart of the whole timeline.

To run the program directly and choose the options:

```bash
./tsim                                    # the built-in set of programs
./tsim -w workloads/interactive.txt       # a workload file
./tsim -p fcfs                            # no preemption
./tsim -p rr -q 1 -t 10                   # round robin, a short quantum
./tsim -V                                 # check the single-CPU rule
./tsim -Q                                 # the final report only
./tsim -T 10                              # print the process table every 10 ticks
./tsim --help                             # every option
```

## A6. The commands to show in the video

```bash
make compare                  # all four policies, side by side
make check                    # 38 tests in three suites
./tsim --verify --no-color    # the /proc check
make demo                     # the full demonstration sequence
```

`make demo` waits for the Enter key between the steps, so you can speak
without typing. `./scripts/demo.sh --auto` plays through without input.

## A7. What a correct run looks like

The program must end with these three results.

```
simulated time      77 ticks = 1540 ms      wall clock 1543 ms (drift +3 ms)
scheduler accounting      1540 ms  vs 1541 ms charged by the real
                                    kernel to the children (getrusage)
EXCLUSIVITY CHECK   77 tick(s) sampled from /proc, 0 violation(s)
```

1. A small drift between simulated time and the wall clock. A large drift
   means that the kernel lost timer interrupts.
2. Two CPU figures that agree to about one per cent. The first figure comes
   from the scheduler. The second comes from Linux.
3. Zero exclusivity violations. The program exits with status 2 after any
   violation.

## A8. Problems and solutions

| Symptom | Solution |
|---|---|
| `Permission denied` on `./tsim` or a script | `chmod +x tsim scripts/*.sh tests/*.sh` |
| A script reports `$'\r': command not found` | `sed -i 's/\r$//' scripts/*.sh tests/*.sh` |
| `make: Clock skew detected` | `sudo hwclock -s`, then `make clean && make` |
| `gcc: command not found` | `sudo apt install build-essential` |
| `undefined reference to 'timer_create'` | The link line needs `-lrt`. The supplied Makefile has it |
| The output has no color | Color is off when you redirect the output. Use `-C` to force plain text |

---

# Part B. How the code works

## B0. The one idea

A time-sharing system has one defining ability. **The kernel can take the CPU
from a running program at a time that the program did not choose.** Without
that ability, one program that does not yield stops the whole machine.

That ability needs two mechanisms, and POSIX supplies both.

| Mechanism | POSIX equivalent | Why it works |
|---|---|---|
| A clock that interrupts | `timer_create()` sends `SIGALRM` | It arrives whether or not the program permits it |
| A way to suspend a process | `SIGSTOP` and `SIGCONT` | A process cannot catch, block or ignore them |

The second row is the important one. Most programs treat the uncatchable
nature of `SIGSTOP` as a restriction. This design needs it, because it makes
preemption involuntary and the real Linux kernel enforces it.

The parent process is the kernel. Each simulated program is a real child
process from `fork()`. Only one child runs at a time, and the kernel holds
every other child stopped.

## B1. The files

| File | Responsibility |
|---|---|
| `main.c` | Reads the command line. Sets `g_cfg`. Calls `kernel_run()` |
| `kernel.c` | The kernel. Handlers, the dispatch loop, context switches, the tick |
| `worker.c` | The code inside a child. It knows nothing about scheduling |
| `scheduler.c` | Selects the next process. Four policies, one interface |
| `timer.c` | Creates and starts the interval timer |
| `proc.c` | The process table, a fixed array of 32 `pcb_t` |
| `queue.c` | The ready queues, fixed-size FIFOs of slot numbers |
| `workload.c` | Reads the list of programs to run |
| `log.c` | The kernel console and the CSV trace |
| `report.c` | The per-tick recorder, the statistics, the chart |
| `syscalls.c` | glibc wrappers, or raw `syscall(2)` with `-R` |

One split matters most. **`kernel.c` decides when a scheduling decision is
needed. `scheduler.c` decides what the decision is.** Four policies therefore
share one dispatcher. Real kernels use the same split.

One variable holds the whole illusion. `g_running` in
[src/kernel.c:70](../src/kernel.c:70) is the slot that owns the CPU, or -1 for
idle. Exactly one such variable exists, which makes this a single-CPU machine
by construction.

## B2. Start-up, in order

`kernel_run()` at [src/kernel.c:713](../src/kernel.c:713) performs five steps.
The order is necessary.

1. **Install the handlers**, at
   [src/kernel.c:725](../src/kernel.c:725). This step must come before the
   timer starts. The default action for `SIGALRM` ends the process, so the
   first tick would kill the kernel.
2. **Build the block mask**, at
   [src/kernel.c:731](../src/kernel.c:731). The mask holds the five signals
   that act as interrupts.
3. **Disable interrupts** with `sigprocmask` at
   [src/kernel.c:742](../src/kernel.c:742). Interrupts stay off for the whole
   run.
4. **Create the processes that arrive at time zero**, at
   [src/kernel.c:752](../src/kernel.c:752). These forks happen before the
   timer exists, on a quiet machine.
5. **Start the timer** at [src/kernel.c:754](../src/kernel.c:754). The machine
   is now live.

## B3. The main loop

The loop is at [src/kernel.c:762](../src/kernel.c:762).

```c
while (!done) {
    if (v_shutdown) { ...; break; }          /* Ctrl-C                   */
    if (v_sigchld)  { v_sigchld = 0; reap_children(); }
    drain_io_requests();                     /* blocking calls           */

    while (v_ticks > 0 && !done) {           /* handle the tick backlog  */
        v_ticks--;
        on_tick();
    }
    if (g_running < 0) dispatch();           /* fill an idle CPU now     */
    ...
    sigsuspend(&idle);                       /* interrupts on, then halt */
}
```

Four points explain it.

**The loop reads flags, not signals.** The handlers set flags. The loop
decides what each flag means, and in what order. It reaps the dead processes
before it serves the I/O requests, so it never dispatches a process that has
ended.

**The tick loop uses `while` and not `if`**, at
[src/kernel.c:774](../src/kernel.c:774). If the kernel stayed busy for three
ticks, it owes three ticks of accounting. The loop pays all three.

**The loop fills an idle CPU at once**, at
[src/kernel.c:785](../src/kernel.c:785). An exit or a blocking call can free
the CPU between two ticks. Without this line the CPU stays idle until the next
interrupt, which wastes up to 20 ms for each event.

**The last line is the most important**, at
[src/kernel.c:803](../src/kernel.c:803). The direct method is incorrect.

```c
sigprocmask(SIG_UNBLOCK, &block, NULL);   /* interrupts on */
pause();                                  /* wait          */
```

A signal that arrives between those two lines runs its handler and sets its
flag. `pause()` then waits for the **next** signal, and nothing reads the flag
that was just set. If that was the last tick, the kernel waits forever.
`sigsuspend()` replaces the mask and waits as one atomic operation.

## B4. The handlers do almost nothing

The four handlers are at
[src/kernel.c:84](../src/kernel.c:84) to
[src/kernel.c:122](../src/kernel.c:122).

```c
static void h_alrm(int sig) { (void)sig; v_ticks++; }
```

- **`volatile`** stops the compiler from holding the variable in a register
  across the loop. Without it, the optimizer can treat `v_ticks` as constant
  and convert the loop into an infinite loop at `-O2`.
- **`sig_atomic_t`** guarantees that a signal cannot interrupt the access in
  the middle.
- **The handler counts and does not set a flag to one.** A flag loses the
  ticks that arrive while the kernel is busy, and the accounting then drifts
  with no report.

The reason for the rule is that few functions are safe inside a handler.
`printf` takes a lock on the stream. If a signal arrives while the main loop
holds that lock, and the handler also calls `printf`, the process deadlocks
against itself. The handler sets a flag and the loop prints. Linux makes the
same split between the top half and the bottom half of an interrupt handler.

One flag needs `SA_SIGINFO`. The I/O handler at
[src/kernel.c:108](../src/kernel.c:108) reads two values from `siginfo_t`. It
takes the slot number from `si_value`, which `sigqueue(3)` carries, and the
sender from `si_pid`, which Linux fills in. The kernel compares the two, so
one process cannot request I/O for another.

## B5. The timer tick

`on_tick()` at [src/kernel.c:598](../src/kernel.c:598) performs eight steps.
The order carries the meaning.

| Step | Line | Action and reason |
|---|---|---|
| 1 | [607](../src/kernel.c:607) | Record which process held the CPU. This must come first, before any state changes |
| 2 | [611](../src/kernel.c:611) | Charge the tick to that process, or to the idle counter |
| 3 | [622](../src/kernel.c:622) | Bill each READY and each BLOCKED process one tick of waiting |
| 4 | [631](../src/kernel.c:631) | Advance the devices. A finished transfer calls `io_complete()` |
| 4b | [639](../src/kernel.c:639) | Read the I/O request ring |
| 5 | [642](../src/kernel.c:642) | Create the processes whose arrival time has come |
| 6 | [647](../src/kernel.c:647) | Decrement the time slice. Preempt the process at zero |
| 6b | [652](../src/kernel.c:652) | Preempt the holder if a higher-priority process is ready |
| 7 | [657](../src/kernel.c:657) | Run the periodic work of the policy, then fill the CPU |

Three of those positions need a reason.

**Step 1 comes first.** The recorder must capture the state during the tick.
A later call would record the reaction of the scheduler against the tick that
caused it.

**Step 3 comes before steps 4 and 5.** A process whose I/O finishes during
this tick did not wait in the ready queue for it. A process that arrives
during this tick did not wait either. A bill to either one adds a tick of
waiting time for each wakeup. A workload with hundreds of wakeups accumulates
seconds of error.

**Step 4b comes before step 6.** The running process can have made a blocking
call during the tick that just ended. It is already asleep, so a preemption
would have no meaning. Fault 10 in `docs/DESIGN.md` describes what happens
when this step runs too late.

## B6. The context switch

Three functions perform it.

**`proc_stop()`** at [src/kernel.c:216](../src/kernel.c:216) takes the CPU
away. A successful return from `kill()` means that the signal is **pending**.
It does not mean that Linux delivered it. If the kernel gave the CPU to
another process inside that window, two children would be runnable at the same
time and the single-CPU rule would break.

**`confirm_stopped()`** at [src/kernel.c:182](../src/kernel.c:182) therefore
waits for Linux to report the stop.

```c
pid_t r = sys_wait4(p->pid, &st, WUNTRACED | WNOHANG, &ru);
if (WIFSTOPPED(st)) { p->os_stopped = true; return true; }
```

`WUNTRACED` makes `wait4()` report a stopped child and not only a dead one.
The function also handles the exit case, because a process can finish its work
in the microseconds between the send and the delivery of `SIGSTOP`. The wait
needs one iteration in normal operation, and the loop has a limit.

**`proc_resume()`** at [src/kernel.c:235](../src/kernel.c:235) returns the
CPU. It ignores `ESRCH`, which means that the child ended between the choice
of the scheduler and the send of the signal. That sequence is normal.

The three functions keep one invariant, and it is worth stating out loud.
**Each `SIGSTOP` that the kernel sends produces one stop report, and one
function consumes it.** The flags enforce that rule. `confirm_stopped()` passes
`WUNTRACED`. `reap_children()` at [src/kernel.c:264](../src/kernel.c:264)
does not, so the reaper cannot consume a report that the switch path waits
for.

`proc_stop()` also tests `p->os_stopped` first. A second `SIGSTOP` to a
stopped process produces no second report, so the confirmation would wait
forever.

## B7. Blocking I/O

This system has two kinds of context switch, and the difference between them
is the difference between cooperative and preemptive multitasking.

- **Involuntary.** The timer expires, the slice ends, and the kernel sends
  `SIGSTOP`. The process has no control.
- **Voluntary.** The process makes a blocking call and gives up the CPU early,
  because it has no work until a device answers.

The second kind is the reason that time sharing works. A machine that serves
twenty people at terminals can use one CPU, because nineteen of them wait at
any moment.

### The side of the application

`request_io()` at [src/worker.c:67](../src/worker.c:67):

```c
if (sigqueue(kernel_pid, SIG_IOREQ, sv) != 0) _exit(10);
while (!v_io_done)
    sigsuspend(&g_wait_mask);
```

The process asks, then it sleeps. Inside `sigsuspend()` it uses no CPU time.
Linux removes it from the run queue, `/proc` reports it as `S`, and its CPU
clock stops. The call is a real blocking call, so the kernel does not need to
stop a process that waits for I/O.

The `while` loop handles a spurious wakeup. Any caught signal returns from
`sigsuspend()`, and only the flag states that the wanted signal arrived.

### The side of the kernel

`drain_io_requests()` at [src/kernel.c:427](../src/kernel.c:427) validates the
request and marks the process `BLOCKED`. It frees the CPU at once rather than
at the end of the tick.

`io_complete()` at [src/kernel.c:493](../src/kernel.c:493) has two calls, and
**the order of those two calls is necessary**.

```c
if (!proc_stop(p)) return;                   /* stop it FIRST */
if (sys_kill(p->pid, SIG_IODONE) != 0 ...)   /* then wake it  */
```

The process sleeps in `sigsuspend()`. A wakeup sent first starts it **at
once**, while another process holds the CPU. Two children then become
runnable. The kernel therefore stops the process first. The wakeup stays
pending until the scheduler gives the process the CPU again and sends
`SIGCONT`. The process then continues and treats the data as present. A kernel
exists to maintain that view.

## B8. The child process

`worker_main()` at [src/worker.c:79](../src/worker.c:79) runs four steps.

1. **Call `prctl(PR_SET_PDEATHSIG, SIGKILL)`.** If something kills the kernel,
   Linux kills each child. Without it, `kill -9` on the parent leaves four
   processes in a busy loop with no parent to stop them.
2. **Reset the inherited signal state.** `fork()` gives the child the handlers
   and the blocked mask of the parent. The child would otherwise run the
   `SIGALRM` handler of the kernel, and it would have `SIGINT` blocked, so it
   would ignore Ctrl-C. The reset uses `SIG_SETMASK` at
   [src/worker.c:112](../src/worker.c:112), which replaces the mask and does
   not add to it.
3. **Perform the handshake, then stop.** The child writes one byte at
   [src/worker.c:121](../src/worker.c:121), then calls `raise(SIGSTOP)` at
   [src/worker.c:124](../src/worker.c:124). The kernel blocks on the read, so
   it never sends `SIGCONT` to a child that has not stopped itself.
4. **Compute, and block from time to time.** The loop at
   [src/worker.c:136](../src/worker.c:136) measures its own progress with
   `CLOCK_PROCESS_CPUTIME_ID`. That clock advances only while the process runs
   on a CPU. It stops while the kernel holds the process stopped, and it stops
   inside `sigsuspend()`. A workload line that states `cpu_ms 180` therefore
   means 180 ms of computation, whatever number of preemptions the process
   receives.

The child ends with `_exit(0)` at [src/worker.c:143](../src/worker.c:143) and
not with `exit()`. `exit()` flushes the stdio buffers that the child inherited
from the kernel, which prints the output of the kernel twice.

## B9. The schedulers

The four policies are in `scheduler.c`. `kernel.c` calls `sched_pick()` and
never learns which policy answered.

**FCFS** never preempts. `sched_quantum()` at
[src/scheduler.c:143](../src/scheduler.c:143) returns -1, which means no
limit. The project includes this policy because its results are poor in an
instructive way. The average response time is 645 ms, against 20 ms for the
feedback queue, because `cpu-hog` arrives first and owns the machine until it
ends. That number is the complete argument for a timer interrupt.

**Round robin** gives each process the same quantum in turn. For N runnable
processes the response time cannot exceed N multiplied by the quantum.
`sched_outranks()` at [src/scheduler.c:129](../src/scheduler.c:129) returns
false for this policy, because its guarantee is that a process keeps the CPU
for a full quantum.

**Static priority** takes the lowest `nice` value first. It shows starvation.
The `nice 9` process in `workloads/starvation.txt` waits 1200 ms for its first
turn.

**The multi-level feedback queue** uses three queues and four rules.

| Rule | Where | What it does |
|---|---|---|
| 1 | [sched_preempted()](../src/scheduler.c:57) | A process that uses a whole slice moves down one level. The kernel measures behavior instead of reading a declaration |
| 2 | [sched_requeue()](../src/scheduler.c:72) | A process that blocks early keeps its level. This one rule makes the policy favor interactive work |
| 3 | [sched_quantum()](../src/scheduler.c:143) | Each lower level grants a slice twice as long, so a CPU-bound process causes fewer switches |
| 4 | [sched_tick()](../src/scheduler.c:162) | Every boost period each process returns to level 0. Without this rule the lowest queue starves |

Rule 2 is the one to explain in the video. A text editor computes for 20 ms
and then waits for a keystroke. It never uses a whole slice, so it never moves
down, so it is always near the front of the top queue when the next keystroke
arrives. The kernel never receives a statement that the editor is interactive.

`preempt_for_rank()` at [src/kernel.c:515](../src/kernel.c:515) adds one more
rule for the ranking policies. A process that becomes runnable and outranks
the holder takes the CPU at once. A wait for the end of the slice would leave
a high-priority process in the queue for up to 240 ms on the lowest level. The
process that loses the CPU is **not** demoted, because it did not use its
whole slice.

## B10. Why the results can be trusted

A simulator that agrees only with itself has proved nothing. The program uses
three independent checks.

**1. The single-CPU rule, against Linux.** `verify_exclusivity()` at
[src/kernel.c:574](../src/kernel.c:574) reads `/proc/PID/stat` for each live
child on each tick and counts the children that Linux reports as runnable. The
count can never exceed one.

The parser at [src/kernel.c:548](../src/kernel.c:548) scans back from the
**last** `)` in the file. Field 2 holds the command name in brackets, and a
command name can contain brackets and spaces. A split of the line on
whitespace is a known way to misread `/proc`.

**2. The CPU accounting, against `getrusage()`.** `wait4()` returns a
`struct rusage` for each reaped child. `proc_mark_exit()` at
[src/kernel.c:242](../src/kernel.c:242) stores it, and the report prints both
figures side by side.

**3. The timer, against the wall clock.** Simulated time is the tick count
multiplied by the tick period. A kernel that lost interrupts would show a
growing difference.

### The known limit

The kernel charges CPU time in whole ticks. It does not charge a process for
part of a tick, so a process that blocks or ends inside a tick loses that
fraction. One process can differ from `getrusage()` by up to one tick, which
is 20 ms by default. The errors have no bias and they cancel across a batch,
so the totals agree to about 1%. A smaller `--tick` reduces the error. Real
kernels had this limit until high-resolution timers replaced the fixed tick.

## B11. A trace, line by line

```
[      0 ms | tick     0 ]  CREATE    P0 cpu-hog      pid=463  needs 700 ms CPU, 0 x 0 ms I/O, nice 2
[      0 ms | tick     0 ]  BOOT      interval timer armed: SIGALRM every 20 ms
[      0 ms | tick     0 ]  DISPATCH  P0 cpu-hog      pid=463  level 0  time slice 3 tick(s) = 60 ms
[     60 ms | tick     3 ]  PREEMPT   P0 cpu-hog      used its whole slice -> demoted to level 1
[     60 ms | tick     3 ]  DISPATCH  P1 text-editor  pid=464  level 0  time slice 3 tick(s) = 60 ms
[     80 ms | tick     4 ]  IO-REQ    P1 text-editor  blocking call, 60 ms (3 tick(s)) - CPU given up early
[     80 ms | tick     4 ]  DISPATCH  P2 compiler     pid=465  level 0  time slice 3 tick(s) = 60 ms
[    140 ms | tick     7 ]  IO-DONE   P1 text-editor  transfer complete -> READY (level 0)
```

| Line | What the operating system did |
|---|---|
| CREATE | `pipe()`, then `fork()`. The child installed its handlers, blocked `SIGUSR2`, wrote its handshake byte and called `raise(SIGSTOP)`. The kernel read the byte, waited for the stop report, and put the process in L0 |
| BOOT | `timer_create(CLOCK_MONOTONIC)` and `timer_settime()` with a 20 ms interval. `SIGALRM` now arrives 50 times each second |
| DISPATCH | `sched_pick()` returned P0. The state became `RUNNING`, `quantum_left` became 3, and the kernel sent `SIGCONT` |
| ticks 1 to 3 | Three deliveries of `SIGALRM`. Each one incremented `v_ticks`, and the loop charged one tick to P0 |
| PREEMPT | `quantum_left` reached zero. The kernel sent `SIGSTOP` and waited for the report. P0 used a whole slice, so rule 1 moved it to L1 |
| IO-REQ | P1 sent `sigqueue(SIGRTMIN)` with its slot number, then slept. The kernel marked it `BLOCKED` with a 3-tick countdown and freed the CPU at once. Rule 2 kept its level |
| DISPATCH | P2, the next process in L0. The CPU was never idle |
| IO-DONE | The countdown reached zero. The kernel sent `SIGSTOP`, confirmed it, then sent `SIGUSR2`. That signal stays pending until the next dispatch of P1 |

Those eight lines hold a complete time-sharing system.

## B12. The five questions to prepare

A marker can ask about any part. These five cover the design.

1. **Why `SIGSTOP` and not a flag that the process reads?** A flag needs the
   cooperation of the process. A process cannot catch, block or ignore
   `SIGSTOP`, so preemption stays involuntary and the real kernel enforces it.
2. **Why `sigsuspend()` and not an unblock followed by `pause()`?** A signal
   that arrives between the two calls runs its handler, and `pause()` then
   waits for the next signal. `sigsuspend()` replaces the mask and waits
   atomically.
3. **Why is the I/O request a queued real-time signal?** Standard signals do
   not queue. Two requests that become pending together merge into one, and
   one process then sleeps forever. `SIGRTMIN` queues and carries a payload.
4. **Why does `io_complete()` stop the process before it wakes it?** The
   process sleeps in `sigsuspend()` and would start at once, while another
   process holds the CPU.
5. **How do you know that only one process runs at a time?** `--verify` reads
   `/proc/PID/stat` on each tick and counts the runnable children. The program
   exits with status 2 after any violation, so the test suite detects it.
