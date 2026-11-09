/* main.c - configuration, the command line, and the global state.
 *
 *   COMP6697001 Operating Systems - AOL Case Study
 *   A time-sharing system simulated with POSIX signals and timers.
 */
#include "kernel.h"
#include "pcb.h"
#include "tsim.h"
#include "workload.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* These defaults keep a run short enough to watch and slow enough to read.
 * A 20 ms tick with a 3-tick quantum gives one context switch about every
 * 60 ms. A desktop kernel uses a similar rate. */
config_t g_cfg = {
    .tick_ms      = 20,
    .quantum      = 3,
    .policy       = SCHED_MLFQ,
    .boost_every  = 40,
    .max_ticks    = 3000,
    .table_every  = 0,
    .color        = true,
    .verify       = false,
    .quiet        = false,
    .raw_syscalls = false,
    .csv_path     = NULL,
    .workload     = NULL,
};

long g_tick = 0;

const char *policy_name(policy_t p)
{
    switch (p) {
    case SCHED_FCFS: return "FCFS (no preemption)";
    case SCHED_RR:   return "round robin";
    case SCHED_PRIO: return "static priority";
    case SCHED_MLFQ: return "multi-level feedback queue";
    default:         return "?";
    }
}

bool policy_is_preemptive(policy_t p)
{
    return p != SCHED_FCFS;
}

long ms_to_ticks(long ms)
{
    return ms / g_cfg.tick_ms;
}

long ticks_to_ms(long ticks)
{
    return ticks * g_cfg.tick_ms;
}

static void usage(FILE *out)
{
    fprintf(out,
"%s %s - a time-sharing operating system simulated with signals and timers\n"
"\n"
"usage: %s [options]\n"
"\n"
"  -w, --workload FILE   read the batch of programs to run from FILE\n"
"                        (default: a built-in mixed batch of four)\n"
"  -p, --policy NAME     fcfs | rr | prio | mlfq            (default mlfq)\n"
"  -t, --tick MS         timer interrupt period in ms       (default 20)\n"
"  -q, --quantum TICKS   base time slice in ticks           (default 3)\n"
"  -b, --boost TICKS     MLFQ anti-starvation period        (default 40)\n"
"  -T, --table TICKS     dump the process table every TICKS ticks\n"
"  -c, --csv FILE        also write the event trace to FILE as CSV\n"
"  -m, --max-ticks N     watchdog limit                     (default 3000)\n"
"  -V, --verify          cross-check the single-CPU rule against /proc\n"
"  -R, --raw-syscalls    use syscall(2) instead of the glibc wrappers\n"
"  -C, --no-color        plain output, no ANSI escapes\n"
"  -Q, --quiet           no event trace, only the final report\n"
"  -E, --example         print an example workload file and exit\n"
"  -h, --help            this text\n"
"  -v, --version         version information\n"
"\n"
"workload file format, one program per line:\n"
"    name  cpu_ms  io_count  io_ms  nice  arrival_ms\n"
"  name        label used in the trace\n"
"  cpu_ms      total CPU time the program needs to finish\n"
"  io_count    how many blocking I/O calls it makes along the way\n"
"  io_ms       how long each of those calls takes to complete\n"
"  nice        static priority, 0 is the highest (used by -p prio)\n"
"  arrival_ms  when the program is submitted to the machine\n"
"\n"
"examples:\n"
"  %s                                     the built-in batch under an MLFQ\n"
"  %s -p fcfs                             the same batch with no preemption\n"
"  %s -p rr -q 1 -t 10                    aggressive round robin\n"
"  %s -w workloads/interactive.txt -V     a workload file, with verification\n"
"  %s -Q -p rr -c trace.csv               quiet run, trace to a CSV file\n",
        TSIM_NAME, TSIM_VERSION, TSIM_NAME,
        TSIM_NAME, TSIM_NAME, TSIM_NAME, TSIM_NAME, TSIM_NAME);
}

static bool parse_policy(const char *s, policy_t *out)
{
    if (!strcmp(s, "fcfs")) { *out = SCHED_FCFS; return true; }
    if (!strcmp(s, "rr"))   { *out = SCHED_RR;   return true; }
    if (!strcmp(s, "prio")) { *out = SCHED_PRIO; return true; }
    if (!strcmp(s, "mlfq")) { *out = SCHED_MLFQ; return true; }
    return false;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "workload",     required_argument, NULL, 'w' },
        { "policy",       required_argument, NULL, 'p' },
        { "tick",         required_argument, NULL, 't' },
        { "quantum",      required_argument, NULL, 'q' },
        { "boost",        required_argument, NULL, 'b' },
        { "table",        required_argument, NULL, 'T' },
        { "csv",          required_argument, NULL, 'c' },
        { "max-ticks",    required_argument, NULL, 'm' },
        { "verify",       no_argument,       NULL, 'V' },
        { "raw-syscalls", no_argument,       NULL, 'R' },
        { "no-color",     no_argument,       NULL, 'C' },
        { "quiet",        no_argument,       NULL, 'Q' },
        { "example",      no_argument,       NULL, 'E' },
        { "help",         no_argument,       NULL, 'h' },
        { "version",      no_argument,       NULL, 'v' },
        { NULL, 0, NULL, 0 }
    };
    int opt, n;

    /* Use color only on a terminal. Redirected output stays plain text. */
    if (!isatty(STDOUT_FILENO))
        g_cfg.color = false;

    while ((opt = getopt_long(argc, argv, "w:p:t:q:b:T:c:m:VRCQEhv",
                              opts, NULL)) != -1) {
        switch (opt) {
        case 'w': g_cfg.workload = optarg; break;
        case 'p':
            if (!parse_policy(optarg, &g_cfg.policy)) {
                fprintf(stderr, "%s: unknown policy '%s' "
                                "(fcfs, rr, prio, mlfq)\n", TSIM_NAME, optarg);
                return 1;
            }
            break;
        case 't': g_cfg.tick_ms     = atol(optarg); break;
        case 'q': g_cfg.quantum     = atol(optarg); break;
        case 'b': g_cfg.boost_every = atol(optarg); break;
        case 'T': g_cfg.table_every = atol(optarg); break;
        case 'c': g_cfg.csv_path    = optarg;       break;
        case 'm': g_cfg.max_ticks   = atol(optarg); break;
        case 'V': g_cfg.verify      = true;  break;
        case 'R': g_cfg.raw_syscalls = true; break;
        case 'C': g_cfg.color       = false; break;
        case 'Q': g_cfg.quiet       = true;  break;
        case 'E': workload_print_example(); return 0;
        case 'h': usage(stdout); return 0;
        case 'v':
            printf("%s %s\n", TSIM_NAME, TSIM_VERSION);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    if (g_cfg.tick_ms < 1 || g_cfg.tick_ms > 1000) {
        fprintf(stderr, "%s: tick must be between 1 and 1000 ms\n", TSIM_NAME);
        return 1;
    }
    if (g_cfg.quantum < 1) {
        fprintf(stderr, "%s: quantum must be at least one tick\n", TSIM_NAME);
        return 1;
    }

    proc_table_init();
    n = g_cfg.workload ? workload_load(g_cfg.workload) : workload_builtin();
    if (n <= 0)
        return 1;

    return kernel_run();
}
