/* tsim.h - configuration and the signal map.
 *
 * tsim simulates a single-CPU preemptive time-sharing system on Linux.
 * The parent process acts as the kernel. Each simulated user program is a
 * real child process created by fork(2). Only one child runs at a time.
 * The kernel keeps every other child stopped with SIGSTOP. A POSIX interval
 * timer supplies the timer interrupt.
 */
#ifndef TSIM_H
#define TSIM_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define TSIM_NAME    "tsim"
#define TSIM_VERSION "1.0.0"

#define MAX_PROCS       32    /* size of the process table                  */
#define MAX_NAME        17    /* process name, including the NUL            */
#define MAX_TICKS_TRACE 4000  /* ticks that the trace recorder keeps        */
#define MLFQ_LEVELS     3     /* number of multi-level feedback queues      */

/* ------------------------------------------------------------------ *
 *  Signal map. Each signal stands for one event that a real kernel
 *  must handle.
 *
 *    SIGALRM     timer interrupt           OS timer  -> kernel
 *    SIGCHLD     process exit trap         OS        -> kernel
 *    SIG_IOREQ   blocking I/O system call  process   -> kernel
 *    SIG_IODONE  I/O completion interrupt  kernel    -> process
 *    SIGSTOP     context switch out        kernel    -> process
 *    SIGCONT     context switch in         kernel    -> process
 *    SIGINT/TERM operator shutdown         user      -> kernel
 *
 *  SIG_IOREQ must be a POSIX real-time signal. Standard signals do not
 *  queue. If two standard signals become pending while the kernel masks
 *  interrupts, Linux merges the second into the first. One I/O request
 *  would then be lost. Real-time signals queue, and sigqueue(3) adds a
 *  payload. The payload carries the process slot number.
 * ------------------------------------------------------------------ */
#define SIG_IOREQ  (SIGRTMIN + 0)
#define SIG_IODONE SIGUSR2

typedef enum {
    SCHED_FCFS = 0,  /* run to completion, no preemption                   */
    SCHED_RR,        /* round robin time sharing                           */
    SCHED_PRIO,      /* static priority, round robin inside a priority     */
    SCHED_MLFQ       /* multi-level feedback queue                         */
} policy_t;

typedef struct {
    long     tick_ms;      /* period of the timer interrupt                 */
    long     quantum;      /* base scheduling quantum, in ticks             */
    policy_t policy;
    long     boost_every;  /* MLFQ anti-starvation boost period, in ticks   */
    long     max_ticks;    /* watchdog limit, in ticks                      */
    long     table_every;  /* print the process table every N ticks         */
    bool     color;
    bool     verify;       /* check CPU exclusivity against /proc           */
    bool     quiet;        /* do not print the event trace                  */
    bool     raw_syscalls; /* use syscall(2) instead of the libc wrappers   */
    const char *csv_path;
    const char *workload;
} config_t;

extern config_t g_cfg;   /* run-time configuration                          */
extern long     g_tick;  /* timer interrupts since the timer was armed       */

const char *policy_name(policy_t p);
bool        policy_is_preemptive(policy_t p);
long        ms_to_ticks(long ms);
long        ticks_to_ms(long ticks);

#endif /* TSIM_H */
