# Video plan, 8 minutes

**COMP6697001 Operating Systems - AOL Case Study**

The assignment requires 5 to 10 minutes. The video must show **the program in
operation** and **an explanation of the code**.

Marks: the operating system installation 30%, the program 40%, the explanation
30%. Give the installation a full minute, and spend more than two minutes on
the code.

---

## Before you record

```bash
cd "/mnt/c/Users/thoma/Documents/Project OS"
make clean && make        # a fresh build looks better on camera than a stale one
make check                # confirm that all 38 tests pass
```

- Set the terminal font to 16 point or larger.
- Use a dark background with the default colors. The event types then stand
  out.
- Keep two windows open: the terminal, and the editor on `src/kernel.c`.
- `scripts/demo.sh` runs the whole sequence below and waits between the
  steps, so you can speak without typing. `./scripts/demo.sh --auto` plays
  through without input.

---

## 0:00 to 0:40. What the program is

Show `ls src/ include/`.

State these points.

- The program is a time-sharing operating system kernel, written in C for
  Linux.
- It is not a model of a scheduler. It is a scheduler, and it schedules real
  processes.
- The parent process is the kernel. Each program that it schedules is a real
  child process created by `fork()`.
- A POSIX interval timer sends `SIGALRM`, which acts as the timer interrupt.
  `SIGSTOP` and `SIGCONT` perform the context switch.
- One requirement defines a time-sharing system: the kernel can take the CPU
  from a program at a time that the program did not choose. That needs a clock
  that interrupts, and a method to suspend a process that the process cannot
  refuse. POSIX supplies both.

---

## 0:40 to 1:40. The operating system installation

This section carries the same weight as the explanation, so do not hurry it.

```powershell
wsl -l -v
```

- The machine runs Ubuntu 24.04 under WSL 2.
- The version column shows 2, and that matters. WSL 1 translated Linux calls
  into Windows calls. WSL 2 runs a real, unmodified Linux kernel.

```bash
lsb_release -a
uname -srm
cat /proc/version
```

- This is Linux 6.18, a real kernel with a real `/proc`.
- The project can therefore check itself against `/proc` and against
  `getrusage()` later in the video. POSIX timers, job control signals and
  queued real-time signals are genuine here, and not an emulation.

```bash
gcc --version
make --version
make clean && make
```

- The compiler is gcc 13.3 from `build-essential`.
- The build uses `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes` and
  produces no warning.

---

## 1:40 to 2:10. The architecture

Show the signal map table in `README.md`.

- Each mechanism that a time-sharing kernel needs has an equivalent here, and
  each equivalent is a signal or a timer.
- The timer interrupt is `SIGALRM` from `timer_create`.
- The context switch out is `SIGSTOP`. The reason for that choice is that a
  process cannot catch, block or ignore `SIGSTOP`. Most programs treat that
  property as a restriction. This design needs it, because it makes preemption
  involuntary. The real kernel enforces it, and the process can give no
  cooperation.
- A blocking I/O call is a queued real-time signal from the child to the
  kernel.
- `sigprocmask` disables interrupts. `sigsuspend` halts until the next
  interrupt.

---

## 2:10 to 4:20. How the code works

Four items in `src/kernel.c`. The 30% is awarded for this section, so give the
reason for each choice and not only the behavior.

### 1. The timer, in src/timer.c

```c
struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM };
timer_create(CLOCK_MONOTONIC, &sev, &g_timer);
its.it_interval = its.it_value;      /* repeating, not single-shot */
```

- `CLOCK_MONOTONIC` and not `CLOCK_REALTIME`. Monotonic time only moves
  forward. A scheduler must not stop or move backward after a time
  correction.
- `it_interval` makes the timer repeat. A single-shot timer gives one
  interrupt, and a kernel needs a steady tick.
- `timer_create` and not `alarm`, which has a resolution of one second, and
  not `setitimer`, which gives one timer and no choice of clock.

### 2. The handlers do almost nothing

```c
static void h_alrm(int sig) { (void)sig; v_ticks++; }
```

- Each handler increments a `volatile sig_atomic_t` and returns. It does
  nothing else.
- `volatile` stops the compiler from holding the variable in a register.
  Without it the optimizer can convert the main loop into an infinite loop at
  `-O2`.
- `sig_atomic_t` guarantees that a signal cannot interrupt the access in the
  middle.
- The reason for the rule: few functions are safe inside a handler. `printf`
  takes a lock on the stream. If a signal arrives while the main loop holds
  that lock, and the handler also calls `printf`, the process deadlocks
  against itself. The handler sets a flag and the loop prints. Real kernels
  make the same split between the top half and the bottom half.
- The handler counts the ticks and does not set a flag to one. If the kernel
  stays busy for longer than a tick, it still owes those ticks to a process. A
  counter keeps them. A flag loses them.

### 3. The main loop, critical sections and the halt

```c
sigprocmask(SIG_BLOCK, &block, &original);   /* interrupts OFF */
while (!done) {
    ... handle the flags, run the ticks, dispatch ...
    sigsuspend(&idle);                       /* interrupts ON, then halt */
}
```

- The loop runs with all five signals blocked, so interrupts are off. Each
  change to the process table therefore runs inside a critical section, and no
  handler can read a partly updated queue. This is `cli` and `sti` around a
  scheduler, written with `sigprocmask`.
- The last line needs the most care. The direct method is to unblock the
  signals and then call `pause()`. That method is incorrect. A signal that
  arrives between those two lines runs its handler and sets its flag, and
  `pause()` then waits for the **next** signal. Nothing reads the flag that
  was just set. If that was the last tick, the kernel waits forever.
  `sigsuspend` replaces the mask and waits as one atomic operation.

### 4. The context switch, and the order of the calls

```c
static bool proc_stop(pcb_t *p) {
    if (sys_kill(p->pid, SIGSTOP) != 0) { ... }
    return confirm_stopped(p);            /* wait for Linux to confirm */
}
```

- A successful `kill` means that the signal is pending. It does not mean that
  Linux delivered it. If the kernel gave the CPU to another process inside
  that window, two children would be runnable at the same time and the
  single-CPU rule would break. The kernel therefore waits for the stop report
  from `wait4` with `WUNTRACED`.

Then show `io_complete()`.

```c
if (!proc_stop(p)) return;                   /* stop it FIRST  */
if (sys_kill(p->pid, SIG_IODONE) != 0 ...)   /* then wake it   */
```

- The order is necessary. The process sleeps in `sigsuspend` and waits for its
  device. A wakeup sent first starts the process at once, while another
  process holds the CPU. The kernel therefore stops it first, and the wakeup
  stays pending until the scheduler gives the process the CPU again. The
  process then continues and treats the data as present. A kernel exists to
  maintain that view.

### If time allows, add the lost wakeup

- The hardest fault in the project is the lost wakeup. The child sends its I/O
  request and then calls `sigsuspend` to wait for the answer. If the
  completion signal arrives between those two steps, Linux runs the handler,
  and the child then sleeps and waits for a signal that has already arrived.
  It sleeps forever.
- The correction: the child blocks the completion signal at start-up and keeps
  it blocked. Linux cannot deliver it outside `sigsuspend`. An early signal
  stays pending, and `sigsuspend` unblocks and waits in one atomic step, so it
  returns at once. The window does not exist.
- `docs/DESIGN.md` lists eleven such faults and the correction for each one.

---

## 4:20 to 6:30. The demonstration

### First, with no timer

```bash
./tsim -p fcfs -w workloads/interactive.txt
```

- First come, first served, with no preemption. `cpu-hog` arrives first and
  never blocks, so it owns the machine until it ends.
- The average response time is 645 ms. A computer without a timer interrupt
  behaves in this way, and this is the reason for the rest of the program.
- Point at the chart. One long run of `#`, and a row of `-` for every other
  program.

### Then with the timer

```bash
./tsim -p rr -w workloads/interactive.txt
```

- Round robin, on the same programs. The timer now takes the CPU back every
  60 ms.
- The response time falls from 645 ms to 30 ms. The cost appears in the switch
  count: 17 forced preemptions instead of none.
- Point at the chart. The CPU now alternates between the programs.

### Then a scheduler that measures behavior

```bash
./tsim -p mlfq -w workloads/interactive.txt
```

- A multi-level feedback queue, the policy that early UNIX systems used.
- The kernel receives no statement about which programs are interactive. It
  measures their behavior. A process that uses a whole quantum moves to a
  lower queue with a longer slice. A process that blocks before its quantum
  ends keeps its level. That one rule makes the policy favor interactive work.
- Point at the trace. `cpu-hog` moves from level 0 to level 1 to level 2.
  `text-editor` stays at level 0 for the whole run.

### All four side by side

```bash
make compare
```

- The feedback queue reaches a response time within 20 ms of static priority,
  and it uses a third as many forced switches.

### Starvation, and the correction

```bash
./tsim -p prio -Q -w workloads/starvation.txt | grep -E 'SLOT|P4'
./tsim -p mlfq -Q -w workloads/starvation.txt | grep -E 'SLOT|P4'
```

- Static priority starves the `nice 9` process. It waits 1200 ms for its first
  turn on the CPU.
- The feedback queue has a periodic priority boost that returns each process
  to the top queue. The same process now reaches the CPU after 60 ms.
- The boost is necessary. Without it, the strict priority between the levels
  starves the lowest queue in the same way as static priority.

---

## 6:30 to 7:30. Proof that the program is correct

State that a simulator which agrees only with itself has proved nothing, then
show the three checks.

```bash
./tsim --verify --no-color | tail -20
```

1. The `--verify` option reads `/proc/PID/stat` for each child on each tick
   and counts the children that Linux reports as runnable. On a correct
   single-CPU simulation that count can never exceed one, and it never does.
2. The scheduler counts the ticks that it grants to each process. Linux
   charges each child for the CPU time that it used, through `getrusage`. The
   report prints both figures, and they agree to about one per cent.
3. Simulated time is the tick count multiplied by the tick period, printed
   beside the wall clock. The drift is 3 ms across 77 ticks, so the kernel
   loses no timer interrupt.

```bash
make check
```

- 38 tests in three suites. The tests do not check that the program compiles.
  Each one states a requirement about behavior: that FCFS never preempts and
  round robin does, that a shorter quantum causes more switches, that the
  feedback queue gives a better response time than FCFS, that the boost gives
  the starved process the CPU, that the run leaves no zombie and no running
  child, and that Ctrl-C stops the machine and still prints a report.
- The third suite searches for lost signals. It runs under timer pressure and
  checks that no process receives credit for CPU time that Linux does not
  report.

---

## 7:30 to 8:00. Close

- The result is a preemptive time-sharing kernel in about 2,400 lines of C.
- A POSIX interval timer acts as the timer interrupt. `SIGSTOP` and `SIGCONT`
  perform the context switch. A queued real-time signal acts as the blocking
  system call. The program has four scheduling policies, and it includes a
  multi-level feedback queue.
- The program verifies itself against `/proc` and `getrusage`, and not against
  its own records.
- The part to point at is the list of eleven race conditions in
  `docs/DESIGN.md`. A scheduler is not difficult to write. A scheduler that
  loses no signal, loses no wakeup, and never makes two processes runnable at
  the same moment is where the operating system content of this project sits.

---

## Questions to prepare for

| Question | Answer |
|---|---|
| Why signals and not threads? | Threads share one signal disposition table, and a thread cannot be stopped by itself. `SIGSTOP` acts on a whole process. The uncatchable-signal property that makes preemption real does not exist for threads. |
| Why `timer_create` and not `alarm`? | `alarm` has a resolution of one second. `setitimer` gives one timer for each process and no choice of clock. `timer_create` gives nanosecond resolution and `CLOCK_MONOTONIC`. |
| Why a real-time signal for the I/O request? | Standard signals do not queue. Two requests that arrive together merge into one, and one process sleeps forever. `SIGRTMIN` queues and carries a payload. |
| Why not call `printf` from a handler? | `printf` is not async-signal-safe. Its stream lock can deadlock against the main loop. The handlers set flags and the loop prints. |
| How do you know that only one process runs at a time? | The `--verify` option reads `/proc/PID/stat` on each tick and counts the runnable children. The count is never above one, and the program exits with status 2 after any violation. |
| Why is the CPU time above the requested value? | The kernel charges in whole ticks. A process that ends inside a tick is not charged for the fraction. The errors cancel across a batch, so the totals agree to about 1%. A smaller `--tick` reduces the error. |
| What does `SA_NOCLDSTOP` do? | Without it, Linux reports every `SIGSTOP` and `SIGCONT` that the kernel sends back to it as a `SIGCHLD`. The kernel would then handle reports about its own context switches. |
| What would you add next? | Multiprocessor scheduling with one run queue for each CPU, a cost model for a context switch, and a device model with a queue in place of a fixed countdown. |
