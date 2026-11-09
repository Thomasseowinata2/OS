/* worker.c - the inside of a simulated user process.
 *
 * After fork(2) the child runs only this code. The code is as simple as a
 * real application. It computes, it makes a blocking call from time to
 * time, and it holds no knowledge of the scheduler. The kernel can take the
 * CPU from it between any two instructions. The kernel performs all of the
 * multitasking. The process performs none of it.
 */
#include "worker.h"
#include "tsim.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t v_io_done;
static sigset_t              g_wait_mask;

/* This variable is volatile and the loop reads it back. The compiler
 * therefore cannot remove the arithmetic as unused work. */
static volatile double g_sink;

static void h_iodone(int sig)
{
    (void)sig;
    v_io_done = 1;        /* set a flag, do nothing else */
}

/* CLOCK_PROCESS_CPUTIME_ID is a per-process CPU clock. It advances only
 * while this process runs on a CPU. It does not advance while the kernel
 * holds the process stopped, and it does not advance inside sigsuspend().
 * A program that must perform a fixed amount of work needs this clock
 * rather than a wall-clock timer. */
static long cpu_used_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
        return 0;
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static void burn_until(long target_us)
{
    double acc = g_sink;
    int    i;

    while (cpu_used_us() < target_us) {
        /* A block of a few thousand operations limits the number of
         * clock_gettime(2) calls and keeps the resolution fine enough. */
        for (i = 0; i < 4000; i++)
            acc += (double)i * 1.0000001;
        g_sink = acc;
    }
}

/* A blocking system call, from the side of the application.
 *
 * The order of these steps prevents a lost wakeup. The process blocks
 * SIG_IODONE at start-up and keeps it blocked. No window therefore exists
 * between the request below and the sigsuspend() that waits for the answer.
 * Without the block, a fast device could answer before the process sleeps.
 * Linux would discard the completion signal and the process would wait
 * forever. sigsuspend() unblocks the signal and waits in one atomic step. */
static void request_io(int slot, pid_t kernel_pid)
{
    union sigval sv;

    sv.sival_int = slot;
    v_io_done = 0;
    if (sigqueue(kernel_pid, SIG_IOREQ, sv) != 0)
        _exit(10);                      /* the kernel went away */
    while (!v_io_done)
        sigsuspend(&g_wait_mask);
}

void worker_main(const pcb_t *spec, int ready_fd)
{
    struct sigaction sa;
    sigset_t         only_iodone;
    pid_t            kernel_pid = getppid();
    long             total_us;
    int              n, k;
    const char       ack = 1;

    /* Ask Linux to kill this process if the kernel process ends. Without
     * this, a kill of the kernel leaves a busy loop running forever. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    /* fork(2) gives the child the handlers and the blocked-signal mask of
     * the parent. The handlers of the kernel have no use here. The mask of
     * the kernel blocks SIGINT, so the child would not answer Ctrl-C. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM,   &sa, NULL);
    sigaction(SIGCHLD,   &sa, NULL);
    sigaction(SIG_IOREQ, &sa, NULL);
    sigaction(SIGINT,    &sa, NULL);
    sigaction(SIGTERM,   &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_iodone;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    /* no SA_RESTART, EINTR is wanted */
    sigaction(SIG_IODONE, &sa, NULL);

    sigemptyset(&only_iodone);
    sigaddset(&only_iodone, SIG_IODONE);
    sigprocmask(SIG_SETMASK, &only_iodone, NULL);
    sigemptyset(&g_wait_mask);          /* sigsuspend() unblocks it later */

    /* Report to the kernel that this process is ready, then leave the CPU.
     * A process cannot catch or ignore SIGSTOP. From this point the process
     * runs only when the scheduler sends SIGCONT. The handshake byte is
     * necessary. Without it the kernel could send SIGCONT before the
     * process reaches this line. Linux would discard that signal and the
     * process would never continue. */
    while (write(ready_fd, &ack, 1) < 0 && errno == EINTR)
        ;
    close(ready_fd);
    raise(SIGSTOP);

    /* ---- the program itself -----------------------------------------
     * The io_count blocking calls divide the CPU demand into io_count + 1
     * equal parts. */
    total_us = spec->cpu_ms * 1000L;
    n        = spec->io_count;

    for (k = 0; k <= n; k++) {
        long target = (n == 0)
                    ? total_us
                    : (long)((double)total_us * (k + 1) / (n + 1));
        burn_until(target);
        if (k < n)
            request_io(spec->slot, kernel_pid);
    }

    /* Call _exit(2) and not exit(3). The stdio buffers here are copies of
     * the buffers of the kernel. A flush would print the output twice. */
    _exit(0);
}
