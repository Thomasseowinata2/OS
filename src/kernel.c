/* kernel.c - the dispatcher of the simulated operating system.
 *
 * This file is the kernel. It owns the process table and the timer. It is
 * the only code that changes the state of a process. Four events drive it,
 * and all four arrive as signals.
 *
 *   SIGALRM    The timer tick. Charge the CPU to the running process, bill
 *              the waiting processes, advance the I/O devices, and take the
 *              CPU back if the time slice has ended.
 *   SIG_IOREQ  A process made a blocking call and gave up the CPU.
 *   SIGCHLD    A process terminated.
 *   SIGINT     The operator wants the machine to stop.
 *
 * The code follows two rules everywhere. A real kernel follows the same two
 * rules for the same reasons.
 *
 * 1. A signal handler does the least work possible. It increments a counter
 *    or sets a flag of type volatile sig_atomic_t, then returns. It does not
 *    touch the process table. It does not call printf(3). It does not
 *    allocate memory. The main loop does the work, in a known order. This
 *    matches the split in Linux between an interrupt service routine and the
 *    deferred work that the routine queues.
 *
 * 2. The main loop runs with the four signals blocked. Interrupts are off.
 *    The loop opens the window only inside sigsuspend() at the bottom. Every
 *    update to the process table therefore happens in a critical section,
 *    and no handler can read a partly updated queue. sigsuspend() unblocks
 *    and waits in one atomic step. An unblock call followed by pause(2)
 *    would leave a race open between the two calls.
 */
#include "kernel.h"

#include "log.h"
#include "pcb.h"
#include "report.h"
#include "scheduler.h"
#include "syscalls.h"
#include "timer.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 *  Interrupt flags. The handlers write them and the main loop reads
 *  them. volatile stops the compiler from holding them in a register
 *  across the loop. sig_atomic_t makes sure that a signal cannot
 *  interrupt a read or a write in the middle.
 * ------------------------------------------------------------------ */
static volatile sig_atomic_t v_ticks;      /* timer ticks not yet handled */
static volatile sig_atomic_t v_sigchld;
static volatile sig_atomic_t v_shutdown;

#define IORING 64
static volatile sig_atomic_t v_io_slot[IORING];
static volatile sig_atomic_t v_io_pid[IORING];
static volatile sig_atomic_t v_io_head;
static volatile sig_atomic_t v_io_tail;
static volatile sig_atomic_t v_io_lost;

/* ------------------------------------------------------------------ *
 *  Kernel state
 * ------------------------------------------------------------------ */
static int  g_running = -1;      /* slot that holds the CPU, -1 if idle */
static long g_switches;
static long g_idle_ticks;
static bool g_was_idle;
static long g_verify_ok;
static long g_verify_fail;

/* confirm_stopped() calls this function before its definition. */
static void proc_mark_exit(pcb_t *p, int status, const struct rusage *ru);

/* ================================================================== *
 *  Signal handlers
 * ================================================================== */

static void h_alrm(int sig)
{
    (void)sig;
    /* Count the ticks. Do not set a flag. If the kernel stays busy for
     * longer than one tick, it still owes the missed ticks to a process.
     * A flag loses them and the accounting drifts. */
    v_ticks++;
}

static void h_chld(int sig)
{
    (void)sig;
    v_sigchld = 1;
}

static void h_stop(int sig)
{
    (void)sig;
    v_shutdown = 1;
}

/* A process made a blocking call. sigqueue(3) carries the slot number in
 * si_value. Linux fills in si_pid. The main loop compares si_pid with the
 * recorded pid, so one process cannot request I/O for another. */
static void h_ioreq(int sig, siginfo_t *si, void *uc)
{
    sig_atomic_t next;

    (void)sig;
    (void)uc;
    next = (v_io_head + 1) % IORING;
    if (next == v_io_tail) {
        v_io_lost++;
        return;
    }
    v_io_slot[v_io_head] = si->si_value.sival_int;
    v_io_pid[v_io_head]  = si->si_pid;
    v_io_head = next;
}

static bool install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_alrm;
    sigfillset(&sa.sa_mask);      /* block all signals inside the handler */
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGALRM, &sa, NULL) != 0)
        return false;

    /* SA_NOCLDSTOP is necessary here. Without it, Linux reports every
     * SIGSTOP and SIGCONT that this kernel sends back to it as a SIGCHLD.
     * The kernel would then spend its time handling reports about its own
     * context switches. It only needs reports about processes that end. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_chld;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return false;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h_ioreq;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;   /* SA_SIGINFO gives si_value */
    if (sigaction(SIG_IOREQ, &sa, NULL) != 0)
        return false;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_stop;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return false;
    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return false;

    return true;
}

/* ================================================================== *
 *  Context switching
 * ================================================================== */

/* Wait until Linux confirms that a process has stopped.
 *
 * A successful kill(2) means that the signal is pending. It does not mean
 * that Linux has delivered it. If the kernel gives the CPU to another
 * process inside that window, two processes become runnable at the same
 * time and the single-CPU rule breaks. The kernel therefore waits for the
 * stop report before it continues.
 *
 * waitpid(2) with WUNTRACED reports a stopped child once for each stop. The
 * reaper below never passes WUNTRACED. Stop reports are therefore consumed
 * here and nowhere else, one for each SIGSTOP that the kernel sends. A
 * process can also exit inside this window instead of stopping, so the code
 * handles the exit case as well. */
static bool confirm_stopped(pcb_t *p)
{
    int spin;

    for (spin = 0; spin < 20000; spin++) {
        struct rusage ru;
        int           st;
        pid_t         r = sys_wait4(p->pid, &st, WUNTRACED | WNOHANG, &ru);

        if (r == p->pid) {
            if (WIFSTOPPED(st)) {
                p->os_stopped = true;
                return true;
            }
            if (WIFEXITED(st) || WIFSIGNALED(st)) {
                proc_mark_exit(p, st, &ru);
                return false;
            }
        } else if (r < 0) {
            return false;                 /* the process has gone */
        }
        {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = 50000L;          /* 50 us */
            nanosleep(&ts, NULL);
        }
    }
    log_event("ERROR", "%s did not stop", p->name);
    return false;
}

/* Take the CPU from a process. A process cannot catch, block or ignore
 * SIGSTOP. The child has no control over this context switch. */
static bool proc_stop(pcb_t *p)
{
    /* A second SIGSTOP to a stopped process produces no second stop report.
     * confirm_stopped() below would then wait for a report that never
     * arrives. This case occurs in normal operation. The kernel can preempt
     * a process and then find that the same process made a blocking call,
     * and the completion path tries to stop it again. */
    if (p->os_stopped)
        return true;

    if (sys_kill(p->pid, SIGSTOP) != 0) {
        if (errno != ESRCH)
            log_event("ERROR", "SIGSTOP %s: %s", p->name, strerror(errno));
        return false;
    }
    return confirm_stopped(p);
}

/* Return the CPU to a process. */
static void proc_resume(pcb_t *p)
{
    p->os_stopped = false;
    if (sys_kill(p->pid, SIGCONT) != 0 && errno != ESRCH)
        log_event("ERROR", "SIGCONT %s: %s", p->name, strerror(errno));
}

static void proc_mark_exit(pcb_t *p, int status, const struct rusage *ru)
{
    if (p->state == P_TERMINATED)
        return;
    if (g_running == p->slot)
        g_running = -1;
    sched_remove(p->slot);
    p->state    = P_TERMINATED;
    p->t_finish = g_tick;
    p->exit_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : -WTERMSIG(status);
    if (ru)
        p->os_cpu_us = ru->ru_utime.tv_sec * 1000000L + ru->ru_utime.tv_usec
                     + ru->ru_stime.tv_sec * 1000000L + ru->ru_stime.tv_usec;

    log_event("EXIT", "P%d %-14s status=%d  granted %ld ms of CPU in %ld tick(s)",
              p->slot, p->name, p->exit_status,
              ticks_to_ms(p->cpu_ticks), p->cpu_ticks);
}

/* Collect the processes that have ended. This function must not pass
 * WUNTRACED. See the comment on confirm_stopped(). */
static void reap_children(void)
{
    for (;;) {
        struct rusage ru;
        int           st;
        pid_t         pid = sys_wait4(-1, &st, WNOHANG, &ru);
        pcb_t        *p;

        if (pid <= 0)
            break;
        p = proc_by_pid(pid);
        if (p)
            proc_mark_exit(p, st, &ru);
    }
}

/* ================================================================== *
 *  Process creation
 * ================================================================== */

static bool proc_spawn(pcb_t *p)
{
    int   fds[2];
    pid_t pid;
    char  ack;
    ssize_t r;

    /* The pipe is a start-up handshake. The child writes one byte after it
     * installs its handlers. The kernel therefore never sends SIGCONT to a
     * child that has not yet stopped itself. */
    if (pipe(fds) != 0) {
        log_event("ERROR", "pipe: %s", strerror(errno));
        return false;
    }

    log_flush();                 /* empty the buffers before fork(2) */
    pid = fork();
    if (pid < 0) {
        log_event("ERROR", "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        worker_main(p, fds[1]);  /* does not return */
    }

    close(fds[1]);
    p->pid = pid;
    do {
        r = read(fds[0], &ack, 1);
    } while (r < 0 && errno == EINTR);
    close(fds[0]);

    if (r != 1) {
        log_event("ERROR", "%s failed to start", p->name);
        return false;
    }
    /* The child stops itself with raise(SIGSTOP). Consume that stop report
     * here, so that each stop report matches one SIGSTOP. */
    return confirm_stopped(p);
}

/* Create each program at its arrival time rather than at the start of the
 * run. The simulation can then show what happens when new work arrives on a
 * busy machine. */
static void admit_arrivals(void)
{
    int i;

    for (i = 0; i < proc_count(); i++) {
        pcb_t *p = proc_at(i);

        if (p->state != P_NEW)
            continue;
        if (ms_to_ticks(p->arrival_ms) > g_tick)
            continue;

        if (!proc_spawn(p)) {
            p->state    = P_TERMINATED;
            p->t_finish = g_tick;
            continue;
        }
        p->state     = P_READY;
        p->t_arrival = g_tick;
        sched_admit(p->slot);
        log_event("CREATE",
                  "P%d %-14s pid=%ld  needs %ld ms CPU, %d x %ld ms I/O, nice %d",
                  p->slot, p->name, (long)p->pid, p->cpu_ms, p->io_count,
                  p->io_ms, p->nice);
    }
}

/* ================================================================== *
 *  Scheduling decisions
 * ================================================================== */

static void dispatch(void)
{
    int    slot = sched_pick();
    pcb_t *p;

    if (slot < 0) {
        if (!g_was_idle) {
            g_was_idle = true;
            log_event("IDLE", "nothing is runnable - the CPU has no work");
        }
        return;
    }
    g_was_idle = false;

    p = proc_at(slot);
    p->state        = P_RUNNING;
    p->quantum_left = sched_quantum(slot);
    if (p->t_first < 0)
        p->t_first = g_tick;
    p->n_dispatch++;
    g_switches++;
    g_running = slot;

    proc_resume(p);

    if (p->quantum_left < 0)
        log_event("DISPATCH", "P%d %-14s pid=%ld  no time limit (FCFS)",
                  p->slot, p->name, (long)p->pid);
    else
        log_event("DISPATCH",
                  "P%d %-14s pid=%ld  level %d  time slice %ld tick(s) = %ld ms",
                  p->slot, p->name, (long)p->pid, p->level, p->quantum_left,
                  p->quantum_left * g_cfg.tick_ms);
}

/* The time slice has ended. A system without a timer cannot do this, and
 * that is why a time-sharing kernel needs a timer. */
static void preempt(pcb_t *p)
{
    int before = p->level;

    p->n_preempt++;
    if (!proc_stop(p))
        return;                     /* the process exited, already recorded */

    p->state  = P_READY;
    g_running = -1;
    sched_preempted(p->slot);

    if (p->level != before)
        log_event("PREEMPT",
                  "P%d %-14s used its whole slice -> demoted to level %d",
                  p->slot, p->name, p->level);
    else
        log_event("PREEMPT",
                  "P%d %-14s used its whole slice -> back of the ready queue",
                  p->slot, p->name);
}

/* Serve the blocking calls in the request ring.
 *
 * The process already sleeps inside sigsuspend(). It uses no CPU time, so
 * the kernel has nothing to stop. This is a voluntary context switch, and
 * the kernel can give the CPU away at once instead of at the end of the
 * tick. */
static void drain_io_requests(void)
{
    while (v_io_tail != v_io_head) {
        int    slot = (int)v_io_slot[v_io_tail];
        pid_t  from = (pid_t)v_io_pid[v_io_tail];
        pcb_t *p;

        v_io_tail = (v_io_tail + 1) % IORING;

        p = proc_at(slot);
        if (!p || p->pid != from) {
            log_event("ERROR", "I/O request for slot %d did not come from it "
                               "(pid %ld)", slot, (long)from);
            continue;
        }
        if (p->state == P_TERMINATED)
            continue;                  /* it ended before the kernel read the ring */
        if (p->state == P_BLOCKED) {
            log_event("ERROR", "a second I/O request from %s, which is "
                               "already blocked", p->name);
            continue;
        }

        /* A request can arrive from a process that the kernel has already
         * taken off the CPU. The child makes the call. The signal stays
         * pending while the kernel masks interrupts. The time slice then
         * ends and the kernel preempts the process. The state is READY by
         * the time the kernel reads the ring.
         *
         * The process made the call while it held the CPU, so the kernel
         * must serve the request. If the kernel discards it, the child
         * sleeps in sigsuspend() forever and waits for a completion that
         * nobody sends. The kernel would continue to dispatch and preempt a
         * process that cannot run. */
        if (p->state == P_READY) {
            sched_remove(p->slot);
            log_event("IO-REQ", "P%d %-14s had already been preempted when its "
                                "blocking call reached the kernel",
                      p->slot, p->name);
        }

        p->state   = P_BLOCKED;
        p->io_left = ms_to_ticks(p->io_ms);
        if (p->io_left < 1)
            p->io_left = 1;        /* no device answers inside zero ticks */
        p->n_io++;
        p->quantum_left = 0;       /* the process loses the rest of its slice */
        if (g_running == slot)
            g_running = -1;

        log_event("IO-REQ",
                  "P%d %-14s blocking call, %ld ms (%ld tick(s)) - CPU given up early",
                  p->slot, p->name, p->io_ms, p->io_left);
    }

    if (v_io_lost) {
        log_event("ERROR", "%d I/O request(s) were lost", (int)v_io_lost);
        v_io_lost = 0;
    }
}

/* The device has finished. Stop the process before you wake it. The process
 * sleeps in sigsuspend() and starts to run as soon as the completion signal
 * arrives. Two processes would then run on a one-CPU machine. The kernel
 * stops the process first, so the wakeup stays pending until the scheduler
 * gives the process the CPU again. */
static void io_complete(pcb_t *p)
{
    if (!proc_stop(p))
        return;
    if (sys_kill(p->pid, SIG_IODONE) != 0 && errno != ESRCH)
        log_event("ERROR", "wakeup %s: %s", p->name, strerror(errno));

    p->state = P_READY;
    sched_requeue(p->slot);
    log_event("IO-DONE", "P%d %-14s transfer complete -> READY (level %d)",
              p->slot, p->name, p->level);
}

/* A process that has become runnable can have a higher priority than the
 * process that holds the CPU. Under a ranking policy the kernel acts at
 * once. It does not wait for the time slice to end. A slice on the lowest
 * MLFQ level lasts several hundred milliseconds, and an interactive process
 * must not wait that long.
 *
 * The process that loses the CPU here did not use its whole slice, so the
 * kernel does not demote it. It returns to its own queue and receives a new
 * slice at its next turn. */
static void preempt_for_rank(void)
{
    pcb_t *cur, *cand;
    int    best;

    if (g_running < 0)
        return;
    best = sched_peek();
    if (best < 0 || !sched_outranks(best, g_running))
        return;

    cur  = proc_at(g_running);
    cand = proc_at(best);
    if (!proc_stop(cur))
        return;

    cur->state = P_READY;
    g_running  = -1;
    sched_requeue(cur->slot);
    log_event("PREEMPT", "P%d %-14s loses the CPU to %s, which ranks higher",
              cur->slot, cur->name, cand->name);
}

/* ================================================================== *
 *  Independent check of the single-CPU rule
 *
 *  This check does not read the state that the kernel records. It reads
 *  the state that Linux reports for each child in /proc/PID/stat. The
 *  letter T means stopped. S means asleep, which here means blocked in
 *  sigsuspend() and waiting for I/O. R means runnable. No more than one
 *  child may be R at any time.
 * ================================================================== */

static char os_state_of(pid_t pid)
{
    char    path[64], buf[512], *rp;
    int     fd;
    ssize_t n;

    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    /* Field 2 holds the command name in brackets and can contain spaces.
     * Scan back from the last ')' to find field 3. */
    rp = strrchr(buf, ')');
    if (!rp)
        return '?';
    rp++;
    while (*rp == ' ')
        rp++;
    return *rp;
}

static void verify_exclusivity(void)
{
    int i, runnable = 0;

    for (i = 0; i < proc_count(); i++) {
        const pcb_t *p = proc_at(i);
        if (p->state == P_NEW || p->state == P_TERMINATED || p->pid <= 0)
            continue;
        if (os_state_of(p->pid) == 'R')
            runnable++;
    }
    if (runnable > 1) {
        g_verify_fail++;
        log_event("VERIFY", "BROKEN: %d processes were runnable at once",
                  runnable);
    } else {
        g_verify_ok++;
    }
}

/* ================================================================== *
 *  The timer tick: the periodic work of the kernel
 * ================================================================== */

static void on_tick(void)
{
    int ran, i;

    g_tick++;
    ran = g_running;

    /* 1. Record which process held the CPU during the tick that has just
     *    ended. Do this before the steps below change the state. */
    report_record(g_tick - 1, ran);

    /* 2. Charge the tick to the process that held the CPU. */
    if (ran >= 0)
        proc_at(ran)->cpu_ticks++;
    else
        g_idle_ticks++;

    /* 3. Bill the processes that wanted the CPU and did not get it, and the
     *    processes that wait for a device. Do this before steps 4 and 5. A
     *    process that wakes or arrives during this tick did not wait in the
     *    ready queue for it. */
    for (i = 0; i < proc_count(); i++) {
        pcb_t *p = proc_at(i);
        if (p->state == P_READY)
            p->ready_ticks++;
        else if (p->state == P_BLOCKED)
            p->blocked_ticks++;
    }

    /* 4. The I/O subsystem. A finished transfer raises a completion
     *    interrupt and the process becomes runnable again. */
    for (i = 0; i < proc_count(); i++) {
        pcb_t *p = proc_at(i);
        if (p->state == P_BLOCKED && --p->io_left <= 0)
            io_complete(p);
    }

    /* 4b. Read the I/O request ring before step 6 decides anything about
     *     the running process. The process can have made a blocking call
     *     during the tick that has just ended. It is already asleep, and a
     *     preemption would have no meaning. */
    drain_io_requests();

    /* 5. Create the programs that arrive at this time. */
    admit_arrivals();

    /* 6. Take the CPU back when the time slice ends. */
    if (g_running >= 0 && policy_is_preemptive(g_cfg.policy)) {
        pcb_t *p = proc_at(g_running);
        if (p->quantum_left > 0 && --p->quantum_left == 0)
            preempt(p);
    }

    /* 6b. A wakeup or an arrival can produce a higher-priority candidate. */
    preempt_for_rank();

    /* 7. Run the periodic work of the policy, then fill the CPU. */
    sched_tick();
    if (g_running < 0)
        dispatch();

    if (g_cfg.table_every > 0 && g_tick % g_cfg.table_every == 0)
        log_table();
    if (g_cfg.verify)
        verify_exclusivity();
}

/* ================================================================== *
 *  Start, run, stop
 * ================================================================== */

static void kill_survivors(void)
{
    int i;

    for (i = 0; i < proc_count(); i++) {
        pcb_t *p = proc_at(i);
        if (p->state == P_NEW || p->state == P_TERMINATED || p->pid <= 0)
            continue;
        /* Send SIGKILL first and SIGCONT second. A stopped process must
         * continue before it can end. A process cannot catch SIGKILL, so
         * this order does not let it run any of its own code again. */
        sys_kill(p->pid, SIGKILL);
        sys_kill(p->pid, SIGCONT);
    }
    for (;;) {
        struct rusage ru;
        int           st;
        pid_t         pid = sys_wait4(-1, &st, 0, &ru);
        pcb_t        *p;

        if (pid <= 0)
            break;
        p = proc_by_pid(pid);
        if (p)
            proc_mark_exit(p, st, &ru);
    }
}

/* Detect a deadlock. Correct code never reaches this state. The check
 * reports the fault instead of leaving the kernel to hang. */
static bool machine_stalled(void)
{
    int i;

    if (g_running >= 0 || sched_ready_count() > 0)
        return false;
    for (i = 0; i < proc_count(); i++) {
        pstate_t s = proc_at(i)->state;
        if (s == P_BLOCKED || s == P_NEW)
            return false;
    }
    return !proc_all_done();
}

int kernel_run(void)
{
    sigset_t        block, original, idle;
    struct timespec t0, t1;
    double          wall;
    bool            done = false;

    sched_init();
    log_open();

    /* Install the handlers before the timer starts. The default action for
     * SIGALRM ends the process. */
    if (!install_handlers()) {
        fprintf(stderr, "%s: cannot install signal handlers: %s\n",
                TSIM_NAME, strerror(errno));
        return 1;
    }

    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigaddset(&block, SIGCHLD);
    sigaddset(&block, SIG_IOREQ);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigemptyset(&idle);

    /* Interrupts off. For the rest of the run, the kernel can be
     * interrupted only inside sigsuspend() at the bottom of the loop. Every
     * update below therefore runs in a critical section. */
    sigprocmask(SIG_BLOCK, &block, &original);

    log_banner("\n  %s %s - a time-sharing system built from signals and timers\n",
               TSIM_NAME, TSIM_VERSION);
    log_banner("  kernel pid %ld, thread %ld, using %s\n",
               (long)sys_getpid(), sys_gettid(), sys_mode());
    log_banner("  policy %s, tick %ld ms, base quantum %ld tick(s) = %ld ms\n\n",
               policy_name(g_cfg.policy), g_cfg.tick_ms, g_cfg.quantum,
               g_cfg.quantum * g_cfg.tick_ms);

    admit_arrivals();                   /* programs that arrive at time zero */

    if (!timer_arm(g_cfg.tick_ms)) {
        fprintf(stderr, "%s: cannot arm the timer\n", TSIM_NAME);
        kill_survivors();
        return 1;
    }
    log_event("BOOT", "interval timer armed: SIGALRM every %ld ms", g_cfg.tick_ms);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!done) {
        if (v_shutdown) {
            log_event("HALT", "operator interrupt - shutting the machine down");
            break;
        }
        if (v_sigchld) {
            v_sigchld = 0;
            reap_children();
        }
        drain_io_requests();

        /* Handle every tick that the handler counted. */
        while (v_ticks > 0 && !done) {
            v_ticks--;
            on_tick();
            if (proc_all_done())
                done = true;
        }
        if (done)
            break;

        /* An exit or a blocking call can free the CPU between two ticks.
         * Fill it now instead of at the next interrupt. */
        if (g_running < 0)
            dispatch();

        if (proc_all_done())
            break;
        if (machine_stalled()) {
            log_event("ERROR", "the machine is stalled - no runnable and no "
                               "blocked processes left");
            break;
        }
        if (g_cfg.max_ticks > 0 && g_tick >= g_cfg.max_ticks) {
            log_event("HALT", "watchdog: %ld ticks reached", g_tick);
            break;
        }

        /* Interrupts on, then wait for the next one. pause(2) here would be
         * a fault. A signal that arrives between the unblock and the pause
         * runs its handler, and pause(2) then waits for the next signal. */
        sigsuspend(&idle);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    timer_disarm();
    if (proc_all_done())
        log_event("HALT", "all %d process(es) finished", proc_count());
    else
        log_event("HALT", "stopping with %d process(es) still unfinished",
                  proc_count() - proc_count_state(P_TERMINATED));
    kill_survivors();

    sigprocmask(SIG_SETMASK, &original, NULL);

    wall = (double)(t1.tv_sec - t0.tv_sec)
         + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    report_final(wall, g_switches, g_idle_ticks);

    if (g_cfg.verify)
        printf("  EXCLUSIVITY CHECK   %ld tick(s) sampled from /proc, "
               "%ld violation(s)\n\n", g_verify_ok + g_verify_fail,
               g_verify_fail);

    log_close();
    return g_verify_fail > 0 ? 2 : 0;
}
