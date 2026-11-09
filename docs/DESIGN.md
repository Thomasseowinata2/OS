# How tsim works

**COMP6697001 Operating Systems - AOL Case Study**

This document explains the program. It states what each part does, the reason
for each design choice, and the concurrency faults that the design must
prevent. Read `README.md` first for a description of the program and the
instructions to run it.

---

## Contents

1. [The central requirement](#1-the-central-requirement)
2. [Architecture](#2-architecture)
3. [The signal map](#3-the-signal-map)
4. [The timer](#4-the-timer)
5. [The main loop](#5-the-main-loop)
6. [The work of one timer tick](#6-the-work-of-one-timer-tick)
7. [Context switching](#7-context-switching)
8. [Blocking I/O](#8-blocking-io)
9. [Eleven race conditions](#9-eleven-race-conditions)
10. [The schedulers](#10-the-schedulers)
11. [Accounting and verification](#11-accounting-and-verification)
12. [A trace, line by line](#12-a-trace-line-by-line)
13. [Rejected alternatives](#13-rejected-alternatives)
14. [Appendix: system calls used](#14-appendix-system-calls-used)

---

## 1. The central requirement

One ability defines a time-sharing system. **The kernel can take the CPU from
a running program at a time that the program did not choose.** A system
without this ability has cooperative multitasking. In such a system, one
program that does not yield stops the whole machine. A fault in that program
is enough to cause this.

That ability needs two mechanisms.

1. **A clock that interrupts.** An asynchronous event that reaches the kernel
   whether or not the running program permits it.
2. **A method to suspend and resume a process** that the process cannot
   refuse.

Real hardware supplies a timer that raises an interrupt line, and the
scheduler performs the context switch. POSIX supplies an equivalent for each
one.

- `timer_create()` with `SIGEV_SIGNAL` sends `SIGALRM` on a schedule. This is
  the nearest equivalent in user space to an interrupt line.
- `SIGSTOP` and `SIGCONT` **cannot be caught, blocked or ignored**. Most
  programs treat that property as a restriction. This design needs it. The
  property makes preemption involuntary, and the real Linux kernel enforces
  it. The target process needs no cooperation and can give none.

`tsim` is therefore not a model of a scheduler. It is a scheduler, and it
schedules real processes. Only the hardware below it is simulated.

---

## 2. Architecture

```
                        +-------------------------------------+
                        |   tsim  -  the kernel (parent)      |
                        |                                     |
   timer_create() ----->|  SIGALRM   handler: v_ticks++       |
   CLOCK_MONOTONIC      |                                     |
                        |  SIGCHLD   handler: v_sigchld = 1   |
                        |  SIGRTMIN  handler: push on ring    |
                        |  SIGINT    handler: v_shutdown = 1  |
                        |                |                    |
                        |     +----------v-----------+        |
                        |     |  main dispatch loop  |        |
                        |     |  (interrupts masked) |        |
                        |     +----------+-----------+        |
                        |                |                    |
                        |  process table | ready queues       |
                        |  (pcb_t x 32)  | L0 / L1 / L2       |
                        +--------+-------+--------------------+
                                 |
            SIGSTOP / SIGCONT    |    SIGRTMIN (I/O request)
            SIGUSR2 (I/O done)   |    SIGCHLD  (exit)
                 v               |               ^
       +-----------------+---------------+-----------------+
       |  child (P0)     |  child (P1)   |  child (P2)     |
       |  STOPPED        |  RUNNING      |  sigsuspend()   |
       |  burn_until()   |  burn_until() |  waits on I/O   |
       +-----------------+---------------+-----------------+
          exactly one of these processes is runnable
```

### Module responsibilities

| File | Responsibility |
|---|---|
| `main.c` | Configuration, the command line, and the globals `g_cfg` and `g_tick` |
| `kernel.c` | The kernel. Signal handlers, the dispatch loop, context switches, the tick, process creation, verification |
| `worker.c` | The code inside a simulated user process. It holds no knowledge of the scheduler |
| `scheduler.c` | Selects the next process. Four policies behind one interface |
| `timer.c` | Creates and starts the interval timer |
| `proc.c` | The process table, a fixed array of `pcb_t` |
| `queue.c` | The ready queues, fixed-size FIFOs of slot numbers |
| `log.c` | The kernel console and the CSV trace |
| `report.c` | The per-tick recorder, the statistics, and the chart |
| `workload.c` | Reads the list of programs to run |
| `syscalls.c` | The same kernel services through glibc wrappers or raw `syscall(2)` |

One split matters more than the others. **`kernel.c` decides when the kernel
must make a scheduling decision. `scheduler.c` decides what that decision
is.** Four policies therefore share one dispatcher, and the dispatch path
holds no test of the policy. Real kernels use the same split between the
scheduler core and the pluggable scheduling classes.

---

## 3. The signal map

| Event in a real operating system | Signal here | Direction | Reason for the choice |
|---|---|---|---|
| Timer interrupt | `SIGALRM` | timer to kernel | `timer_create()` delivers it asynchronously, whether or not the kernel is busy |
| Context switch out | `SIGSTOP` | kernel to process | The target cannot catch, block or ignore it. This is the definition of preemption |
| Context switch in | `SIGCONT` | kernel to process | The only path back to the CPU, and also uncatchable |
| Blocking I/O system call | `SIGRTMIN` | process to kernel | A real-time signal. It queues and it carries a payload |
| I/O completion interrupt | `SIGUSR2` | kernel to process | Each process has at most one open request, so one signal is enough |
| Process termination | `SIGCHLD` | OS to kernel | The method that Linux uses to report a dead child |
| Operator shutdown | `SIGINT`, `SIGTERM` | user to kernel | Ctrl-C. The kernel handles it, so the machine stops and still prints a report |

### Why the I/O request uses a real-time signal

This is the least obvious choice in the program. A wrong choice here produces
a fault that appears about once in a thousand runs.

**Standard signals do not queue.** If `SIGUSR1` is already pending for a
process, and a second `SIGUSR1` arrives before Linux delivers the first, Linux
merges the second into the first. The second signal is lost. The handler runs
once.

Consider the kernel with interrupts masked, and two children that each make a
blocking call inside that window. With a standard signal the kernel reads one
request and serves one child. The other child waits in `sigsuspend()` forever.
That is a deadlock, and only an exact sequence of events produces it.

**POSIX real-time signals, `SIGRTMIN` and above, do queue.** `sigqueue(3)`
also adds a payload, `si_value`, which the handler reads through `SA_SIGINFO`.
`tsim` uses the payload to carry the process slot number. It compares that
number against `si_pid`, which Linux fills in and a process cannot change.

```c
/* kernel.c */
static void h_ioreq(int sig, siginfo_t *si, void *uc)
{
    ...
    v_io_slot[v_io_head] = si->si_value.sival_int;   /* the sender */
    v_io_pid[v_io_head]  = si->si_pid;               /* filled in by Linux */
    v_io_head = next;
}
```

In the present design only one child is ever runnable, so at most one request
can be in flight. A design that depends on that fact makes the correctness of
the signal layer depend on the scheduler layer. A queued signal removes that
dependency.

### Why SA_NOCLDSTOP is necessary

By default a parent receives `SIGCHLD` when a child stops, and not only when
it ends. This kernel sends `SIGSTOP` and `SIGCONT` continuously. Without
`SA_NOCLDSTOP`, each context switch returns to the kernel as a `SIGCHLD`, and
the kernel handles reports about its own decisions.

```c
sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;   /* report deaths, not stops */
sigaction(SIGCHLD, &sa, NULL);
```

The flag also separates the two consumers of `wait()` status values. Fault 6
below explains why that separation matters.

---

## 4. The timer

```c
/* timer.c */
struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM };
timer_create(CLOCK_MONOTONIC, &sev, &g_timer);

its.it_value    = (struct timespec){ tick_ms / 1000, (tick_ms % 1000) * 1000000L };
its.it_interval = its.it_value;      /* repeating, not single-shot */
timer_settime(g_timer, 0, &its, NULL);
```

The code makes three choices here.

**`CLOCK_MONOTONIC` and not `CLOCK_REALTIME`.** Monotonic time only moves
forward. A scheduler driven by wall-clock time can stop or move backward after
an NTP correction or a time zone change. No scheduling decision may depend on
the current civil time.

**`SIGEV_SIGNAL`.** The expiry arrives as a signal and interrupts the work of
the kernel. The alternative, `SIGEV_THREAD`, starts a thread for each expiry.
That choice adds thread synchronization and gives no benefit here.

**`timer_create()` and not `alarm()` or `setitimer()`.** `alarm(2)` has a
resolution of one second, which cannot express a 20 ms tick. `setitimer(2)`
has microsecond resolution, but it gives one timer for each process and no
choice of clock. `timer_create(2)` gives nanosecond resolution, several
timers, and the choice of `CLOCK_MONOTONIC`.

`it_interval` makes the timer repeat. A single-shot timer delivers one
interrupt. The kernel needs a steady tick.

**A child does not inherit a timer across `fork()`.** POSIX states this, and
the design depends on it. Children that inherited the interval timer would
receive `SIGALRM` as well, and the trace would hold no meaning. A child does
inherit the signal handlers and the blocked-signal mask, so `worker_main()`
resets both.

---

## 5. The main loop

The loop is the center of `kernel.c`. Real kernels use the same pattern for
the same reasons.

```c
sigprocmask(SIG_BLOCK, &block, &original);   /* interrupts OFF */

while (!done) {
    if (v_shutdown)  { ... break; }
    if (v_sigchld)   { v_sigchld = 0; reap_children(); }
    drain_io_requests();

    while (v_ticks > 0 && !done) {
        v_ticks--;
        on_tick();
        if (proc_all_done()) done = true;
    }
    if (g_running < 0) dispatch();     /* an exit or a block freed the CPU */
    ...
    sigsuspend(&idle);                 /* interrupts ON, then halt */
}
```

### Each handler does almost nothing

Each handler in this program sets a `volatile sig_atomic_t` and returns.

```c
static void h_alrm(int sig) { (void)sig; v_ticks++; }
```

- **`volatile`** stops the compiler from holding the variable in a register
  across the loop. Without it, the optimizer may assume that nothing changes
  `v_ticks` and may convert the loop into an infinite loop. This failure is
  common in signal-driven code at `-O2`.
- **`sig_atomic_t`** guarantees that a signal cannot interrupt a read or a
  write of the variable in the middle.

The reason for the rule is that **few functions are safe inside a signal
handler.** `printf` takes a lock on the stream. If a signal arrives while the
main loop holds that lock, and the handler also calls `printf`, the process
deadlocks against itself. `malloc` has the same problem. POSIX publishes a
short list of async-signal-safe functions, and it includes neither function.

The main loop therefore does the work, in a known order, with no interference.
Real kernels make the same division between a short interrupt service routine
and the deferred work that the routine queues. Linux calls the two parts the
top half and the bottom half.

### Why the handler counts the ticks

A flag records only that at least one tick arrived. If the kernel stays busy
for longer than one tick, a flag loses the ticks that arrive in the meantime,
and the accounting drifts without any report. A counter keeps those ticks, and
the main loop pays them later. The difference between simulated time and wall
time, printed in the report, shows that no ticks are lost.

### Interrupts stay off, except inside sigsuspend()

The loop runs with `SIGALRM`, `SIGCHLD`, `SIGRTMIN`, `SIGINT` and `SIGTERM`
blocked. Each change to the process table, each queue operation and each state
change therefore runs inside a **critical section**. No handler can read a
partly updated queue. This is the equivalent of the `cli` and `sti`
instructions around a scheduler, written with `sigprocmask`.

### Why the loop uses sigsuspend() and not pause()

The direct method is incorrect.

```c
sigprocmask(SIG_UNBLOCK, &block, NULL);   /* interrupts on  */
pause();                                  /* wait for one   */
```

If a signal arrives between those two lines, Linux runs the handler at once
and the handler sets its flag. `pause()` then waits for the **next** signal.
Nothing reads the flag until another event arrives. With a 20 ms tick that
costs 20 ms. If it was the last tick of the run, the kernel waits forever.

`sigsuspend()` exists to close this window. It replaces the mask and waits as
**one atomic operation**, and it restores the previous mask on return. It is
the only correct way to write this loop.

---

## 6. The work of one timer tick

`on_tick()` in `kernel.c` performs these steps in this order.

```
1. record which process held the CPU during the tick that just ended
2. charge that tick to that process, or to the idle counter
3. bill each READY process one tick of waiting, and each BLOCKED process
   one tick of I/O wait
4. advance the I/O devices. A finished transfer raises a completion interrupt
4b. read the I/O request ring
5. create the processes whose arrival time has come
6. decrement the time slice of the running process. Preempt it at zero
6b. preempt the running process if a higher-priority process is ready
7. run the periodic work of the policy, then fill the CPU
8. print the process table and run the /proc check, if enabled
```

Three points of the order need a reason.

**Step 1 runs first.** The recorder must capture the state during the tick,
and not the state after the scheduler reacts to the tick. A later call would
record the results of the tick against the tick itself.

**Step 3 runs before steps 4 and 5.** A process whose I/O finishes during this
tick did not wait in the ready queue for it. A process that arrives during
this tick did not wait either. A bill to either process adds one tick of
waiting time for each wakeup. A workload with hundreds of wakeups accumulates
seconds of error, so this is not a rounding difference.

**Step 4b runs before step 6.** The running process can have made a blocking
call during the tick that just ended. It is already asleep. A preemption of
such a process has no meaning, and fault 10 below describes the result.

---

## 7. Context switching

### Taking the CPU from a process

```c
static bool proc_stop(pcb_t *p)
{
    if (sys_kill(p->pid, SIGSTOP) != 0) { ... return false; }
    return confirm_stopped(p);
}
```

A successful return from `kill()` means that the signal is **pending**. It
does not mean that Linux has delivered the signal. If the kernel gave the CPU
to another process inside that window, two children would be runnable at the
same time. The single-CPU rule would break, and the result would appear as a
small accounting error with no clear cause.

The kernel therefore waits for Linux to confirm the stop.

```c
pid_t r = sys_wait4(p->pid, &st, WUNTRACED | WNOHANG, &ru);
if (r == p->pid) {
    if (WIFSTOPPED(st))                      return true;   /* confirmed */
    if (WIFEXITED(st) || WIFSIGNALED(st)) {  /* it exited instead */ }
}
```

`WUNTRACED` makes `wait4()` report a stopped child, and not only a dead one.
The code handles the exit case as well, because a process can finish its work
in the microseconds between the send and the delivery of `SIGSTOP`.

The confirmation needs one iteration in normal operation. The loop has a
limit, and it reports an error instead of waiting forever.

### Returning the CPU

```c
static void proc_resume(pcb_t *p)
{
    if (sys_kill(p->pid, SIGCONT) != 0 && errno != ESRCH) ...
}
```

`ESRCH`, no such process, is not an error here. It means that the child ended
between the choice of the scheduler and the send of the signal. That sequence
is normal, and the next `SIGCHLD` completes the cleanup.

---

## 8. Blocking I/O

This system has two kinds of context switch. The difference between them is
the difference between cooperative and preemptive multitasking.

- **Involuntary.** The timer expires, the slice ends, and the kernel sends
  `SIGSTOP`. The process has no control over this.
- **Voluntary.** The process makes a blocking call and gives up the CPU before
  its slice ends, because it has no work until a device answers.

The second kind is the reason that time sharing works. A machine that serves
twenty people at terminals can use one CPU, because at any moment nineteen of
them wait for a disk or a keystroke.

### The side of the application

```c
/* worker.c */
static void request_io(int slot, pid_t kernel_pid)
{
    union sigval sv = { .sival_int = slot };
    v_io_done = 0;
    if (sigqueue(kernel_pid, SIG_IOREQ, sv) != 0) _exit(10);
    while (!v_io_done)
        sigsuspend(&g_wait_mask);
}
```

The process asks, then it sleeps. Inside `sigsuspend()` it **uses no CPU
time**. Linux removes it from the run queue, `/proc` reports it as `S`, and its
`CLOCK_PROCESS_CPUTIME_ID` does not advance. The call is a real blocking call.
For this reason the kernel does not need to stop a process that waits for I/O.

The code uses a `while` loop and not one call to `sigsuspend()`. Any caught
signal returns from `sigsuspend()`. Only the flag states that the wanted
signal arrived.

### The side of the kernel

The request path is short. The kernel marks the process `BLOCKED`, sets a
countdown in ticks, and frees the CPU at once instead of at the end of the
tick. The completion path needs more care.

```c
static void io_complete(pcb_t *p)
{
    if (!proc_stop(p)) return;                   /* stop it FIRST */
    if (sys_kill(p->pid, SIG_IODONE) != 0 ...)   /* then wake it  */
    p->state = P_READY;
    sched_requeue(p->slot);
}
```

**The order of these two calls is necessary.** The process sleeps in
`sigsuspend()`. A wakeup sent first starts the process **at once**, while
another process holds the CPU. Two children then become runnable, and the rule
breaks.

The kernel therefore stops the process first. The wakeup signal stays pending
while the process is stopped. Linux delivers it when the scheduler gives the
process the CPU again and sends `SIGCONT`. The process continues and treats
the data as present. A kernel exists to maintain that view.

---

## 9. Eleven race conditions

Signal-driven concurrency is the part of this kind of program that fails most
easily. Each fault below is a real risk in this design, and the code contains
the correction for each one.

### 1. A lost wakeup in the application

**The fault.** The child sends its I/O request, then calls `sigsuspend()`. If
the completion signal arrives between those two steps, Linux runs the handler,
the flag is set, and the child then sleeps and waits for a signal that has
already arrived. The child sleeps forever.

**The correction.** The child blocks `SIG_IODONE` at start-up and keeps it
blocked.

```c
sigemptyset(&only_iodone);
sigaddset(&only_iodone, SIG_IODONE);
sigprocmask(SIG_SETMASK, &only_iodone, NULL);   /* blocked permanently */
sigemptyset(&g_wait_mask);                      /* sigsuspend unblocks it */
```

Linux cannot deliver the signal outside `sigsuspend()`. An early signal stays
**pending**, and `sigsuspend()` unblocks and waits in one atomic step, so it
returns at once. The window does not exist.

### 2. A lost wakeup in the kernel

**The fault.** The same error one level higher. The kernel unblocks the
signals, then calls `pause()`. A signal that arrives in the gap runs its
handler, and the kernel then waits for the next signal.

**The correction.** `sigsuspend()`, as section 5 describes. The kernel never
unblocks and waits in two steps.

### 3. SIGCONT sent before the child stops itself

**The fault.** After `fork()` the child installs its handlers and calls
`raise(SIGSTOP)` to leave the CPU. If the kernel dispatches the child before
it reaches that line, Linux delivers `SIGCONT` to a process that is not
stopped. The signal has no effect and Linux discards it. The child then stops
itself, and nothing continues it.

**The correction.** Two independent measures.

A handshake through a pipe. The child writes one byte immediately before it
stops itself. The kernel reads that byte and blocks until it arrives, so
`fork()` does not report success until the child reaches that line.

```c
/* worker.c */
while (write(ready_fd, &ack, 1) < 0 && errno == EINTR) ;
close(ready_fd);
raise(SIGSTOP);
```

A confirmation, which closes the remaining nanoseconds between the `write()`
and the `raise()`.

```c
/* kernel.c, proc_spawn() */
return confirm_stopped(p);     /* wait for Linux to report the stop */
```

The kernel marks a process `READY` only after Linux confirms that it stopped.

### 4. Standard signals that merge

**The fault.** Linux merges two I/O requests into one, and one child sleeps
forever. Section 3 gives the detail.

**The correction.** A queued POSIX real-time signal, `SIGRTMIN`, with the slot
number in `si_value`.

### 5. A flood of SIGCHLD from the context switches of the kernel

**The fault.** Linux delivers `SIGCHLD` for stops as well as for deaths. This
kernel stops and continues processes continuously, so it would handle reports
about its own decisions.

**The correction.** `SA_NOCLDSTOP`.

### 6. Two consumers of one wait() status

**The fault.** A process consumes a `wait()` status once. `confirm_stopped()`
needs the stop reports. `reap_children()` needs the exit statuses. If the
reaper passed `WUNTRACED`, it could consume a stop report that
`confirm_stopped()` waits for. The kernel would then spin to its retry limit
on each switch.

**The correction.** Strict ownership, which the flags enforce.

```c
confirm_stopped:  sys_wait4(p->pid, &st, WUNTRACED | WNOHANG, &ru);  /* stops  */
reap_children:    sys_wait4(-1,     &st,             WNOHANG, &ru);  /* deaths */
```

Without `WUNTRACED`, `wait4()` ignores stopped children. Each `SIGSTOP` that
the kernel sends produces one stop report, and one function consumes it.

### 7. stdio buffers copied across fork()

**The fault.** `fork()` copies the stdio buffers of the parent. Text that
remains in a buffer exists twice and prints twice, once from the kernel and
once from each child. Redirected output then holds duplicate lines.

**The correction.** Flush before each fork, and never flush in the child.

```c
log_flush();          /* empty every buffer */
pid = fork();
```

The child leaves through `_exit()`, which does not flush, and not through
`exit()`, which does.

### 8. A child that inherits the signal state of the kernel

**The fault.** `fork()` gives the child the handlers **and** the blocked mask
of the parent. The child would start with the `SIGALRM` handler of the kernel
installed, and with `SIGINT` blocked. It would ignore Ctrl-C and survive the
shutdown.

**The correction.** `worker_main()` resets each inherited handler to `SIG_DFL`
and replaces the mask with `SIG_SETMASK`, which does not add to the old mask.

### 9. Lost timer ticks under load

**The fault.** A flag records that at least one tick arrived. Ticks that
arrive while the kernel is busy are lost, and simulated time falls behind real
time with no report.

**The correction.** Count the ticks with `v_ticks++`, and handle the backlog
in a loop. The reported drift shows the result.

```
simulated time  77 ticks = 1540 ms      wall clock 1543 ms (drift +3 ms)
```

Three milliseconds across 77 ticks is the cost of process creation and signal
delivery. It is not a set of lost interrupts.

### 10. A blocking call that arrives after the preemption

**The fault.** This sequence puts the kernel and the child into different
states.

1. The child is `RUNNING`. It makes a blocking call with
   `sigqueue(SIGRTMIN)`, then calls `sigsuspend()`. It is now asleep.
2. The kernel has interrupts masked, so the signal is only **pending**. The
   request is not in the ring.
3. `on_tick()` runs. The time slice ends, so the kernel preempts the process
   that it records as running. It sends `SIGSTOP` and sets the state to
   `READY`.
4. The kernel reads the request ring. It finds a request from a process whose
   state is `READY` and not `RUNNING`.

A kernel that treats step 4 as impossible discards the request.

```c
if (p->state != P_RUNNING) {
    log_event("ERROR", "I/O request from %s, which is %s", ...);
    continue;                 /* incorrect */
}
```

The child then stays asleep in `sigsuspend()` and waits for a completion that
nobody sends. The kernel continues to dispatch and preempt a process that
cannot run. The report shows the result as a very large CPU time against a
very small figure from `getrusage()`. One measured example credited a process
with 58.6 seconds of CPU time across 288 dispatches, while Linux reported
204 milliseconds.

**Frequency.** The window is the few microseconds that `on_tick()` needs to
run, so at the default 20 ms tick the window rarely opens. The `--verify`
option makes each tick more expensive, because it reads `/proc` for each
child. Measured directly, the sequence occurs **8 times in 25 runs at a 1 ms
tick** and **0 times in 10 runs at the default tick**.

**The correction.** The process made the call while it held the CPU, so the
kernel must serve the request.

```c
if (p->state == P_READY) {
    sched_remove(p->slot);        /* take it out of the ready queue */
    log_event("IO-REQ", "P%d %-14s had already been preempted when its "
                        "blocking call reached the kernel", ...);
}
p->state = P_BLOCKED;
```

The general rule: a guard that names a state as impossible is a risk. In
signal-driven code there is always a gap between an event and the observation
of that event. Any state that the kernel can hold inside that gap can occur.

### 11. A stop sent to a process that is already stopped

**The fault.** The correction for fault 10 creates this one. A process can now
reach the I/O completion path while Linux already holds it stopped, because
step 3 above preempted it. `io_complete()` stops it first.

```c
if (!proc_stop(p)) return;    /* stop it before the wakeup */
```

A second `SIGSTOP` to a stopped process has no effect, and Linux produces **no
second stop report**. `confirm_stopped()` would wait for a report that never
arrives, spin to its retry limit, report an error and return. The process
would stay `BLOCKED` forever. The first correction alone converts a rare
deadlock into a less rare deadlock.

**The correction.** Record what Linux has done, instead of a conclusion drawn
from the simulated process state.

```c
static bool proc_stop(pcb_t *p)
{
    if (p->os_stopped)
        return true;          /* already off the CPU */
    ...
}
```

`confirm_stopped()` sets `os_stopped` when Linux confirms a stop.
`proc_resume()` clears it when the kernel sends `SIGCONT`. The field separates
the model of the state of the **operating system** from the model of the state
of the **process**. These are two different things.

### A further safeguard

Each child calls `prctl`.

```c
prctl(PR_SET_PDEATHSIG, SIGKILL);
```

If something kills the kernel process, Linux sends `SIGKILL` to each child.
Without this call, `kill -9` on the parent leaves four processes in
`burn_until()` with no parent to stop them. The normal shutdown path,
`kill_survivors()`, covers the ordinary case.

---

## 10. The schedulers

The four policies are in `scheduler.c`, behind one interface. `kernel.c` calls
`sched_pick()` and does not learn which policy answered.

```c
void sched_admit(int slot);        /* arrived                               */
void sched_preempted(int slot);    /* used a whole slice, may be demoted    */
void sched_requeue(int slot);      /* gave up the CPU, keeps its level      */
int  sched_pick(void);             /* select and dequeue                    */
int  sched_peek(void);             /* report the selection, change nothing  */
bool sched_outranks(int a, int b); /* should a take the CPU from b?         */
long sched_quantum(int slot);      /* length of the slice to grant          */
void sched_tick(void);             /* periodic work                         */
```

### FCFS

This policy never preempts. `sched_quantum()` returns -1, which means no
limit, and `policy_is_preemptive()` returns false, so the timer never takes
the CPU back.

The project includes this policy because its results are poor in an
instructive way. On `workloads/interactive.txt` the average response time is
**645 ms**, against 20 ms for the feedback queue. `cpu-hog` arrives first, it
never blocks, and it therefore owns the machine until it ends. That one number
is the complete argument for a timer interrupt.

### Round robin

One FIFO. Each process receives `quantum` ticks in turn. For N runnable
processes the response time cannot exceed N multiplied by the quantum. That
limit is the guarantee that a time-sharing system gives to the people at the
terminals.

Round robin does not preempt a running process when another process wakes up.
`sched_outranks()` returns false for this policy. Its guarantee is that a
process keeps the CPU for a full quantum. A switch to a new arrival would make
the length of one round unpredictable.

### Static priority

The lowest `nice` value wins, FIFO order breaks a tie, and a higher-priority
arrival preempts at once.

This policy shows a failure mode. On `workloads/starvation.txt` the `nice 9`
process waits **1200 ms** for its first turn, because `nice 0` work keeps
arriving. With a continuous supply of such work it would never run.

### Multi-level feedback queue

Three queues, with strict priority between them. No process in L1 runs while
L0 holds work. Four rules do the work.

**Rule 1. Demote a process that uses its whole slice.**

```c
void sched_preempted(int slot) {
    if (g_cfg.policy == SCHED_MLFQ && p->level < MLFQ_LEVELS - 1)
        p->level++;
    q_push(&g_q[p->level], slot);
}
```

The kernel receives no statement about which programs are CPU-bound. It
measures their behavior. A process that uses a whole quantum without blocking
is CPU-bound by that measurement.

**Rule 2. A process that blocks early keeps its level.**

```c
void sched_requeue(int slot) {
    q_push(&g_q[p->level], slot);        /* no demotion */
}
```

This rule makes an MLFQ favor interactive work without any declaration from
the programs. A text editor computes for 20 ms and then waits for a keystroke.
It never uses a whole slice, so it never moves down, so it is always in the
top queue when the next keystroke arrives.

**Rule 3. Each lower level grants a longer slice.**

```c
case SCHED_MLFQ: return g_cfg.quantum << p->level;   /* 3, 6, 12 ticks */
```

A CPU-bound process moves to the lowest level, where the kernel switches it
less often and it causes less overhead. The measurement supports the rule. On
the interactive workload the feedback queue reaches a response time within
20 ms of static priority, and it uses **5 forced preemptions instead of 13**.

**Rule 4. The periodic boost, which corrects rule 1.**

```c
/* every boost_every ticks, each process returns to L0 */
for (l = 1; l < MLFQ_LEVELS; l++)
    while ((s = q_pop(&g_q[l])) >= 0) { proc_at(s)->level = 0; q_push(&g_q[0], s); }
```

Without this rule the strict priority between levels starves the lowest queue,
in the same way as static priority. With this rule the `nice 9` process that
waited 1200 ms under static priority reaches the CPU after **60 ms**.

This policy ignores `nice`. It ranks processes by their measured behavior and
not by a declared value.

### Preemption on a wakeup

Under the ranking policies, a process that becomes runnable can have a higher
priority than the process that holds the CPU. The kernel acts at once.

```c
static void preempt_for_rank(void)
{
    int best = sched_peek();
    if (best < 0 || !sched_outranks(best, g_running)) return;
    ...
    sched_requeue(cur->slot);     /* not demoted: it did not use its slice */
}
```

A wait for the end of the current slice would leave a high-priority process in
the queue for up to a full quantum. On the lowest MLFQ level a quantum is
240 ms, which a person can detect.

The process that loses the CPU here is **not** demoted. It did not use its
whole slice, so rule 1 does not apply to it. Without this rule the feedback
queue gives a worse result than round robin on the interactive workload, at
710 ms of average waiting time against 400 ms, because interactive processes
wait for the 240 ms bottom-level slice of `cpu-hog`. With this rule the
average waiting time is 160 ms.

---

## 11. Accounting and verification

The kernel holds per-process counters in ticks, and it reports the standard
measures.

| Measure | Definition |
|---|---|
| CPU time | ticks on which the process held the CPU |
| Waiting time | ticks in the READY state, waiting for the CPU |
| Turnaround | `t_finish - t_arrival` |
| Response time | `t_first - t_arrival` |
| CPU utilization | total CPU ticks divided by total ticks |

A simulator that agrees only with itself has proved nothing, so an external
source checks each layer.

**The view of the process.** Each child measures its progress with
`CLOCK_PROCESS_CPUTIME_ID`, a per-process CPU clock. The clock advances only
while that process runs. It does not advance while the process is stopped, and
it does not advance while the process sleeps in `sigsuspend()`. A workload
line that states `cpu_ms 180` therefore means 180 ms of computation, whatever
number of preemptions the process receives.

**The view of Linux.** `wait4()` returns a `struct rusage` for each reaped
child. The report prints the figure from the kernel beside the figure that
Linux charged.

```
scheduler accounting      1540 ms  vs 1541 ms charged by the real
                                    kernel to the children (getrusage)
```

**The exclusivity check.** The `--verify` option reads `/proc/PID/stat` for
each live child on each tick, and it counts the children that Linux reports as
runnable. The parser scans back from the **last** `)` in the file. Field 2
holds the command name in brackets, and a command name can contain brackets
and spaces. A split of the line on whitespace is a known way to misread
`/proc`.

```c
rp = strrchr(buf, ')');       /* field 2 can contain spaces and brackets */
rp++; while (*rp == ' ') rp++;
return *rp;                   /* field 3: R, S, T, Z, ... */
```

```
EXCLUSIVITY CHECK   77 tick(s) sampled from /proc, 0 violation(s)
```

`tsim` exits with status 2 after a failure of this check, so the test suite
detects it without a reader.

### The known limit of the accounting

The kernel charges CPU time in whole ticks. It does not charge a process for
part of a tick, so a process that blocks or ends inside a tick loses that
fraction. One process can differ from `getrusage()` by up to one tick. For
example, `cpu-hog` can appear with 740 ms of CPU time after a request for
700 ms, while `getrusage()` reports 700.3 ms.

The errors have no bias and they cancel across a batch, so the totals agree to
about 1%. A smaller `--tick` reduces the per-process error and increases the
time spent in signal delivery. This is a property of tick-driven accounting.
Real kernels had the same limit until high-resolution timers and tickless
operation replaced the fixed tick.

---

## 12. A trace, line by line

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

**tick 0, CREATE.** `pipe()`, then `fork()`. The child installs its handlers,
blocks `SIGUSR2`, writes its handshake byte and calls `raise(SIGSTOP)`. The
kernel reads the byte, waits for Linux to confirm the stop, marks the process
`READY` and puts it in L0.

**tick 0, BOOT.** `timer_create(CLOCK_MONOTONIC)` and `timer_settime()` with a
20 ms interval. From this point `SIGALRM` arrives 50 times each second,
whatever work the kernel performs.

**tick 0, DISPATCH.** `sched_pick()` returns P0 from L0. The state becomes
`RUNNING`, `quantum_left` becomes 3, and the kernel sends `SIGCONT`. The child
continues inside `burn_until()`.

**ticks 1, 2 and 3.** Three deliveries of `SIGALRM`. Each one increments
`v_ticks` in the handler, and the main loop handles them. Each tick charges one
tick of CPU time to P0 and decrements `quantum_left`.

**tick 3, PREEMPT.** `quantum_left` reaches zero. The kernel sends `SIGSTOP`
to P0 and waits for the confirmation. P0 used a whole slice without blocking,
so rule 1 moves it to L1. It will receive a 6-tick slice there, behind every
process in L0.

**tick 3, DISPATCH.** P1 from L0, with `SIGCONT`.

**tick 4, IO-REQ.** P1 computed for one tick and made a blocking call. It sent
`sigqueue(SIGRTMIN)` with its slot number, then it called `sigsuspend()`. The
handler of the kernel pushed the slot on the ring. The main loop read it,
compared `si_pid`, marked P1 `BLOCKED` with a 3-tick countdown, and freed the
CPU **at once** instead of at the end of the tick. P1 loses the rest of its
slice. It gave up the CPU, so rule 2 applies and it keeps level 0.

**tick 4, DISPATCH.** P2, the next process in L0. The CPU was never idle. The
kernel gives a voluntary release directly to the next process.

**tick 7, IO-DONE.** The countdown reaches zero. The kernel sends `SIGSTOP`
first and confirms it, then it sends `SIGUSR2`. That signal stays pending
until the next dispatch of P1. P1 goes to the back of L0 at level 0. It is
interactive, so it will be near the front of the queue at each wakeup.

Those eight lines hold a complete time-sharing system.

---

## 13. Rejected alternatives

| Alternative | Reason for the rejection |
|---|---|
| Entries in a table, with no `fork()` | Nothing real is scheduled. No check against `getrusage()` or `/proc` is possible. The signal races, the delivery latency and the stop confirmation all disappear, and they are the content of the exercise |
| POSIX threads instead of processes | Threads share one signal disposition table, and a thread cannot be stopped by itself. `SIGSTOP` acts on a whole process. The uncatchable-signal property that makes preemption real does not exist for threads |
| `setitimer()` or `alarm()` | `alarm()` has a resolution of one second. `setitimer()` gives one timer for each process and no choice of clock. `timer_create()` gives nanosecond resolution and `CLOCK_MONOTONIC` |
| `SIGUSR1` for the I/O request | Standard signals do not queue, so two requests merge and one process sleeps forever. `SIGRTMIN` queues and carries a payload |
| `pause()` in the main loop | It loses a signal that arrives between the unblock and the wait. `sigsuspend()` is atomic |
| `sleep()` or `usleep()` for the quantum | They measure wall-clock time and not CPU time, so a stopped process would appear to make progress. `CLOCK_PROCESS_CPUTIME_ID` advances only on the CPU |
| `printf()` inside the signal handlers | It is not async-signal-safe. The stream lock can deadlock against the main loop. The handlers set flags and the loop prints |
| `exit()` in the child | It flushes the stdio buffers inherited from the parent and prints the output of the kernel twice. `_exit()` does not flush |
| Dynamic allocation for the table and the queues | An allocation that fails during a context switch has no recovery path. The code uses fixed arrays and enforces the limit when it reads the workload |
| A custom system call compiled into the kernel | WSL 2 runs a real Linux kernel, so this is possible. It requires a kernel rebuild and installation. The assignment asks for a time-sharing simulation that uses signals and timers. The `--raw-syscalls` option shows the system call boundary without a kernel rebuild |

---

## 14. Appendix: system calls used

Each call below enters the real Linux kernel. The `-R` option sends `kill`,
`wait4` and `getpid` through `syscall(2)` instead of the glibc wrappers. Both
paths reach the same kernel routine. The option makes the boundary explicit in
the source, and `strace` shows the difference.

| System call | Purpose |
|---|---|
| `timer_create`, `timer_settime`, `timer_delete` | the timer |
| `sigaction` | installs the handlers, with `SA_SIGINFO`, `SA_NOCLDSTOP` and `SA_RESTART` |
| `sigprocmask` | disables and enables interrupts |
| `sigsuspend` | halts until the next interrupt, atomically |
| `kill` | sends `SIGSTOP`, `SIGCONT`, `SIGUSR2` and `SIGKILL`, which perform the context switch |
| `rt_sigqueueinfo`, through `sigqueue` | the I/O request, with a payload |
| `fork` | process creation |
| `wait4` | stop confirmation, reaping, and the `getrusage` accounting |
| `pipe`, `read`, `write`, `close` | the start-up handshake |
| `clock_gettime` | `CLOCK_MONOTONIC` for wall time, `CLOCK_PROCESS_CPUTIME_ID` for CPU time |
| `nanosleep` | the limited wait inside the stop confirmation |
| `prctl(PR_SET_PDEATHSIG)` | makes sure that no child outlives a killed kernel |
| `open` and `read` on `/proc/PID/stat` | the independent check of the single-CPU rule |

To watch the calls during a run:

```bash
sudo apt install strace
strace -e trace=timer_create,timer_settime,kill,rt_sigqueueinfo ./tsim -Q -m 30
```
