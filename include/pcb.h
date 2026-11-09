/* pcb.h - the process control block and the process table.
 *
 * The kernel holds one pcb_t for each simulated process. The child process
 * does not read or write this structure after fork(2).
 */
#ifndef PCB_H
#define PCB_H

#include "tsim.h"

/* The five process states. P_NEW means that the workload file lists the
 * program, but its arrival time has not come. fork(2) has not run yet. */
typedef enum {
    P_UNUSED = 0,
    P_NEW,
    P_READY,
    P_RUNNING,
    P_BLOCKED,
    P_TERMINATED
} pstate_t;

typedef struct {
    int      slot;
    pid_t    pid;
    char     name[MAX_NAME];
    pstate_t state;

    /* ---- values read from the workload file -------------------------- */
    long cpu_ms;      /* CPU time that this program needs to finish        */
    int  io_count;    /* number of blocking I/O calls that it makes        */
    long io_ms;       /* service time of one I/O request                   */
    int  nice;        /* static priority, 0 is the highest                 */
    long arrival_ms;  /* time at which the program is submitted            */

    /* ---- scheduling state -------------------------------------------- */
    int  level;        /* current MLFQ queue                               */
    long quantum_left; /* ticks left in the current time slice             */
    long io_left;      /* ticks left on the open I/O request               */
    bool os_stopped;   /* true while Linux holds this process stopped      */

    /* ---- accounting, in ticks unless the comment says otherwise ------ */
    long t_arrival;    /* tick at which it first became READY              */
    long t_first;      /* tick of its first dispatch, or -1                */
    long t_finish;     /* tick at which it terminated                      */
    long cpu_ticks;    /* ticks that it held the CPU                       */
    long ready_ticks;  /* ticks that it waited in a ready queue            */
    long blocked_ticks;/* ticks that it waited for I/O                     */
    long n_dispatch;   /* context switches into this process               */
    long n_preempt;    /* times that the timer took the CPU from it        */
    long n_io;         /* blocking I/O calls served                        */

    int  exit_status;
    long os_cpu_us;    /* CPU time that Linux charged it, from getrusage   */
} pcb_t;

const char *state_name(pstate_t s);
char        state_char(pstate_t s);

void   proc_table_init(void);
pcb_t *proc_alloc(void);
pcb_t *proc_at(int slot);
pcb_t *proc_by_pid(pid_t pid);
int    proc_count(void);
int    proc_count_state(pstate_t s);
bool   proc_all_done(void);

#endif /* PCB_H */
