# tsim - a time-sharing system built from signals and timers

**COMP6697001 Operating Systems - AOL Case Study**
Learning objectives LO 3 and LO 4. Written in C for Linux, on Ubuntu 20.04 or
newer, including WSL 2.

---

## What this program is

`tsim` is a preemptive single-CPU time-sharing operating system kernel. It
runs as an ordinary user-space C program, and it uses only POSIX signals and a
POSIX interval timer.

The parent process acts as the kernel. Each simulated user program is a real
child process created by `fork()`, and each child performs real CPU work. Only
one child runs at a time. The kernel holds every other child stopped with
`SIGSTOP`. An interval timer from `timer_create()` sends `SIGALRM` once per
tick. That signal acts as the timer interrupt. Without a timer interrupt, a
kernel cannot take the CPU back from a running program.

The program does not use `sleep()` and it does not step through a table of
imagined processes. When the report states that a process received 140 ms of
CPU time, Linux agrees. `tsim` compares its own accounting against
`getrusage()`, and with `--verify` it also compares against `/proc/PID/stat`.

```
[     60 ms | tick     3 ]  PREEMPT   P0 cpu-hog        used its whole slice -> demoted to level 1
[     60 ms | tick     3 ]  DISPATCH  P1 text-editor    pid=464  level 0  time slice 3 tick(s) = 60 ms
[     80 ms | tick     4 ]  IO-REQ    P1 text-editor    blocking call, 60 ms (3 tick(s)) - CPU given up early
[     80 ms | tick     4 ]  DISPATCH  P2 compiler       pid=465  level 0  time slice 3 tick(s) = 60 ms
```

---

## How to start

Open a WSL Ubuntu shell, then run these commands.

```bash
cd "/mnt/c/Users/thoma/Documents/Project OS"
./scripts/setup_wsl.sh      # check the toolchain, offer to install what is missing
make                        # build ./tsim
make run                    # the built-in workload under a feedback queue
make compare                # the same workload under all four policies
make check                  # 38 tests in three suites
make demo                   # the sequence for the video
```

**[docs/HOWTO.md](docs/HOWTO.md)** is the place to start. It runs the program
on WSL Ubuntu step by step, then explains the code in the order that it
executes.

For the installation of WSL and Ubuntu from the start, read
**[docs/INSTALL_WSL.md](docs/INSTALL_WSL.md)**.

---

## The signal map

A time-sharing kernel needs a set of mechanisms. This program supplies each
one with a signal or a timer.

| Mechanism in a real operating system | Mechanism in `tsim` | Reason for the choice |
|---|---|---|
| Timer interrupt | `timer_create(CLOCK_MONOTONIC)` sends `SIGALRM` | An asynchronous event that the running program cannot block or refuse |
| Context switch out | `SIGSTOP` | A process cannot catch, block or ignore it, so preemption is involuntary |
| Context switch in | `SIGCONT` | The only path back to the CPU, and only the kernel sends it |
| Blocking I/O system call | `sigqueue(SIGRTMIN)` from child to kernel | A queued real-time signal, so two requests cannot merge into one |
| I/O completion interrupt | `SIGUSR2` from kernel to child | It wakes a process that sleeps in `sigsuspend()` |
| Process exit report | `SIGCHLD` and `wait4()` | The method that Linux uses to report a dead child |
| Disable interrupts | `sigprocmask(SIG_BLOCK, ...)` | It makes each update to the process table a critical section |
| Halt until the next interrupt | `sigsuspend()` | It unblocks and waits in one atomic step. `pause()` cannot do this |
| Process creation | `fork()` | A real process with a real address space |
| CPU accounting | `CLOCK_PROCESS_CPUTIME_ID`, `getrusage()` | Per-process CPU clocks that stop while the process is stopped |

**[docs/DESIGN.md](docs/DESIGN.md)** gives the reason for each choice. It also
lists the eleven race conditions that the design must prevent.

---

## The four schedulers

```
$ make compare

POLICY                         AVG-WAIT   AVG-TURN   AVG-RESP SWITCHES   FORCED
---------------------------- ---------- ---------- ---------- -------- --------
fcfs  (no preemption)             675.0     1280.0      645.0       27        0
rr    (round robin)               400.0     1030.0       30.0       44       17
prio  (static priority)           130.0      760.0        0.0       57       13
mlfq  (feedback queue)            160.0      790.0       20.0       39        5
```

All times are in milliseconds, measured on `workloads/interactive.txt`.

**`fcfs`** uses no timer preemption. The compute-bound program `cpu-hog`
arrives first and keeps the machine until it ends. The average response time is
**645 ms**. A machine without a timer interrupt behaves in this way, and this
result is the reason for the rest of the program.

**`rr`** is round robin. The timer takes the CPU back at the end of each
quantum, and the average response time falls from 645 ms to **30 ms**. The last
column shows the cost: 17 forced context switches instead of 0.

**`prio`** is static priority. It serves the favored processes well, and it can
starve the others. Run it against `workloads/starvation.txt`. The `nice 9`
process then waits 1200 ms for its first turn on the CPU.

**`mlfq`** is a multi-level feedback queue. Early UNIX systems used this
policy. The kernel receives no statement about which programs are interactive.
It measures their behavior instead. A process that uses a whole quantum moves
down one level. A process that blocks early keeps its level. The result is a
response time close to static priority with **a third fewer forced switches**.
A periodic priority boost prevents starvation. The same `nice 9` process that
waited 1200 ms now reaches the CPU after **60 ms**.

---

## Proof that the simulation is correct

A simulator that agrees only with itself has proved nothing. This program uses
three independent checks.

**1. The single-CPU rule, checked against Linux.** The `--verify` option reads
`/proc/PID/stat` for each child on each tick. It counts the children that Linux
reports as runnable. On a correct single-CPU simulation that count can never
exceed one.

```
EXCLUSIVITY CHECK   77 tick(s) sampled from /proc, 0 violation(s)
```

**2. The accounting of the scheduler, checked against `getrusage()`.** The
kernel counts the ticks that it grants to each process. Linux charges each
child for the CPU time that the child used. The report prints both figures.

```
scheduler accounting      1540 ms  vs 1541 ms charged by the real
                                    kernel to the children (getrusage)
```

**3. The timer, checked against the wall clock.** Simulated time is the tick
count multiplied by the tick period. A kernel that lost interrupts would show a
growing difference between the two figures.

```
simulated time      77 ticks = 1540 ms      wall clock 1543 ms (drift +3 ms)
```

---

## How to read the output

Each run ends with a chart of the whole timeline.

```
  EXECUTION TIMELINE   one column = 2 tick(s) = 40 ms
  legend:  # on the CPU    - ready and waiting    ~ blocked on I/O    . finished

  cpu-hog        |##---###----------######---###---######|
  text-editor    |-#~~#~~--#~--#~-#~~-----#~#............|
  compiler       |--##-----###~####--------##~~-####.....|
  log-writer     |   ##~--#~-##~~--#~-----##.............|
                 ++---------+---------+---------+--------+
                  0         400       800       1200        ms
```

The row for `cpu-hog` shows one long run of `#` that the timer breaks into
blocks. The row for `text-editor` shows the pattern of an interactive program.
It computes for a short time, then it makes a blocking call, and it repeats.
The `-` marks at the start of its row show the time that it waited in the ready
queue.

The table of processes uses the standard measures.

| Column | Meaning |
|---|---|
| `CPU(ms)` | time on the CPU |
| `WAIT(ms)` | time in the READY state, waiting for the CPU |
| `TURN(ms)` | turnaround time, from submission to termination |
| `RESP(ms)` | response time, from submission to the first time on the CPU |
| `DSP` and `PRE` | dispatches, and the number that ended in forced preemption |
| `IO` | blocking calls served |
| `OS-CPU(ms)` | CPU time that Linux charged the child, from `getrusage()` |

---

## Options

```
usage: tsim [options]

  -w, --workload FILE   read the list of programs to run from FILE
  -p, --policy NAME     fcfs | rr | prio | mlfq            (default mlfq)
  -t, --tick MS         timer interrupt period in ms       (default 20)
  -q, --quantum TICKS   base time slice in ticks           (default 3)
  -b, --boost TICKS     MLFQ anti-starvation period        (default 40)
  -T, --table TICKS     print the process table every TICKS ticks
  -c, --csv FILE        also write the event trace to FILE as CSV
  -m, --max-ticks N     watchdog limit                     (default 3000)
  -V, --verify          check the single-CPU rule against /proc
  -R, --raw-syscalls    use syscall(2) instead of the glibc wrappers
  -C, --no-color        plain output, no ANSI escapes
  -Q, --quiet           no event trace, only the final report
  -E, --example         print an example workload file and exit
```

### Workload files

The file holds one program on each line. A line must give the name and
`cpu_ms`.

```
# name            cpu_ms  io_count  io_ms  nice  arrival_ms
cpu-hog              900         0      0     3           0
shell                 90        10     40     0           0
text-editor          150         8     60     0          40
file-indexer         300         5     50     2         100
```

| Field | Meaning |
|---|---|
| `cpu_ms` | CPU time that the program needs to finish |
| `io_count` | number of blocking I/O calls that it makes |
| `io_ms` | time that each of those calls needs |
| `nice` | static priority, 0 is the highest, used by `-p prio` |
| `arrival_ms` | time at which the program is submitted |

The project includes four workloads. Each one shows one result.

| File | Result |
|---|---|
| `workloads/interactive.txt` | why a timer is necessary: one CPU-bound program against three interactive programs |
| `workloads/batch.txt` | the trade-off between turnaround time and fairness, under FCFS and round robin |
| `workloads/starvation.txt` | static priority starves a process, and the MLFQ boost prevents it |
| `workloads/io-heavy.txt` | why one CPU can serve many users: each user waits for most of the time |

---

## Files

```
Makefile                 build, run, test, demonstrate
include/                 one header for each module
src/
  main.c                 configuration and the command line
  kernel.c               the dispatcher: handlers, context switches, the tick
  worker.c               the inside of a simulated user process
  scheduler.c            the four policies
  timer.c                the interval timer
  proc.c  queue.c        the process table and the ready queues
  log.c   report.c       the kernel console, the statistics and the chart
  workload.c             reads the list of programs
  syscalls.c             glibc wrappers or raw syscall(2), chosen at run time
workloads/               four workloads
tests/
  run_tests.sh           24 behavioral regression tests
  edge_cases.sh          14 difficult inputs
  stress.sh              searches for lost signals under timer pressure
scripts/
  setup_wsl.sh           toolchain check and build
  compare.sh             all four policies side by side
  demo.sh                the sequence for the video
  package.sh             builds the submission archive
docs/
  HOWTO.md               how to run it on WSL Ubuntu, and how the code works
  INSTALL_WSL.md         how to install Ubuntu under WSL 2
  DESIGN.md              how each part works, and the reason for each choice
  VIDEO_SCRIPT.md        a timed plan for the video
  SUBMISSION.md          what to submit
```

The project holds about 2,400 lines of C and headers. The build produces no
warnings under these options:

```
-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wold-style-definition
```

---

## Tested environment

| Item | Version |
|---|---|
| Distribution | Ubuntu 24.04.5 LTS (Noble Numbat) under WSL 2 |
| Kernel | Linux 6.18.33.2-microsoft-standard-WSL2 x86_64 |
| Compiler | gcc 13.3.0 |
| C library | glibc 2.39 |
| Build tool | GNU Make 4.3 |
| Tests | 24 regression tests, 14 edge cases, and the stress suite, all passing |

The code targets **Ubuntu 20.04 and newer**. The link line includes `-lrt`,
because `timer_create()` was part of `librt` in versions of glibc before 2.34.
Ubuntu 20.04 ships glibc 2.31. Newer releases still supply an empty `librt`
stub, so the same link line works on both.

WSL 2 runs a real Linux kernel and not a translation layer. POSIX timers,
real-time signals, the `SIGSTOP` and `SIGCONT` job control signals, and
`CLOCK_PROCESS_CPUTIME_ID` therefore operate as they do on hardware. For this
reason the simulation can be checked against `/proc` and `getrusage()`.

---

## Known limits

A simulator with unknown limits receives too much trust, so this section
states them.

- **Tick-granular accounting.** The kernel charges CPU time in whole ticks. It
  does not charge a process for a part of a tick, so a process that blocks or
  ends inside a tick loses that fraction. One process can therefore differ from
  `getrusage()` by up to one tick, which is 20 ms by default. The errors have
  no bias and they cancel across a batch, so the totals agree to about 1%. A
  smaller `--tick` reduces the error.
- **A context switch costs nothing here.** The model excludes the real cost of
  a switch, which includes the register save, the page table reload, and the
  cache and TLB misses that follow. The report gives the switch count, so a
  reader can reason about the cost. A quantum of one tick receives no penalty.
- **One CPU only.** The design depends on one runnable child at a time.
  Multiprocessor scheduling, run-queue balancing and the necessary locking are
  outside the scope of this project.
- **Signal delivery takes time.** `SIGSTOP` needs a few microseconds to take
  effect, and the kernel waits for confirmation before it gives the CPU to
  another process. The wait has a limit, but it is not zero. With a 1 ms
  `--tick` the switch itself uses a measurable part of each tick.
- **I/O is a countdown, not a device.** The model represents a blocking call as
  a fixed number of ticks. It has no device queue, no seek time and no
  bandwidth limit.
