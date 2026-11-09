/* report.c - the per-tick recorder and the final report.
 *
 * The kernel calls the recorder once for each timer interrupt, before it
 * changes any state. The chart therefore shows the state of the machine
 * during that tick, and not the result of the scheduling decisions that
 * follow the tick.
 */
#include "report.h"
#include "pcb.h"

#include <stdio.h>
#include <string.h>

#define GANTT_WIDTH 72

static char g_trace[MAX_PROCS][MAX_TICKS_TRACE];
static long g_trace_ticks;

void report_record(long tick, int running)
{
    int i;

    if (tick < 0 || tick >= MAX_TICKS_TRACE)
        return;

    for (i = 0; i < proc_count(); i++) {
        const pcb_t *p = proc_at(i);
        char         c;

        if (i == running && p->state == P_RUNNING)
            c = '#';                     /* holds the CPU                  */
        else if (p->state == P_READY)
            c = '-';                     /* wants the CPU, cannot have it  */
        else if (p->state == P_BLOCKED)
            c = '~';                     /* waits for a device             */
        else if (p->state == P_TERMINATED)
            c = '.';                     /* has ended                      */
        else
            c = ' ';                     /* not yet submitted              */
        g_trace[i][tick] = c;
    }
    g_trace_ticks = tick + 1;
}

static void rule(char c)
{
    int i;
    putchar(' ');
    putchar(' ');
    for (i = 0; i < 86; i++)
        putchar(c);
    putchar('\n');
}

/* Rank the states for a column that holds more than one tick. The chart
 * shows the highest rank. Running outranks blocked. Blocked outranks ready.
 * Ready outranks ended, and ended outranks absent. */
static int rank_of(char c)
{
    switch (c) {
    case '#': return 4;
    case '~': return 3;
    case '-': return 2;
    case '.': return 1;
    default:  return 0;
    }
}

static void gantt(void)
{
    long per, n = g_trace_ticks;
    int  cols, i, c;

    if (n <= 0)
        return;

    per  = (n + GANTT_WIDTH - 1) / GANTT_WIDTH;
    if (per < 1)
        per = 1;
    cols = (int)((n + per - 1) / per);

    printf("\n  EXECUTION TIMELINE   one column = %ld tick(s) = %ld ms\n",
           per, per * g_cfg.tick_ms);
    printf("  legend:  # on the CPU    - ready and waiting    "
           "~ blocked on I/O    . finished\n\n");

    for (i = 0; i < proc_count(); i++) {
        const pcb_t *p = proc_at(i);
        printf("  %-14s |", p->name);
        for (c = 0; c < cols; c++) {
            char best = ' ';
            int  rb = 0;
            long t;
            for (t = (long)c * per; t < (long)(c + 1) * per && t < n; t++) {
                int r = rank_of(g_trace[i][t]);
                if (r > rb) {
                    rb = r;
                    best = g_trace[i][t];
                }
                if (r == 4)
                    break;
            }
            putchar(best);
        }
        printf("|\n");
    }

    /* A time axis with a label at every tenth column. */
    printf("  %-14s +", "");
    for (c = 0; c < cols; c++)
        putchar(c % 10 == 0 ? '+' : '-');
    printf("+\n");
    printf("  %-14s  ", "");
    for (c = 0; c < cols; c += 10) {
        char label[16];
        snprintf(label, sizeof label, "%ld", (long)c * per * g_cfg.tick_ms);
        printf("%-10s", label);
    }
    printf("  ms\n");
}

void report_final(double wall_s, long switches, long idle_ticks)
{
    long   tot_cpu = 0, tot_wait = 0, tot_turn = 0, tot_resp = 0;
    long   tot_os_us = 0, tot_io = 0, tot_preempt = 0;
    int    i, counted = 0;

    putchar('\n');
    rule('=');
    printf("  SIMULATION REPORT\n");
    rule('=');
    printf("  scheduling policy   %s\n", policy_name(g_cfg.policy));
    printf("  timer tick          %ld ms\n", g_cfg.tick_ms);
    printf("  base time slice     %ld tick(s) = %ld ms\n",
           g_cfg.quantum, g_cfg.quantum * g_cfg.tick_ms);
    printf("  simulated time      %ld ticks = %ld ms      "
           "wall clock %.0f ms (drift %+.0f ms)\n",
           g_trace_ticks, ticks_to_ms(g_trace_ticks), wall_s * 1000.0,
           wall_s * 1000.0 - (double)ticks_to_ms(g_trace_ticks));

    printf("\n  %-4s %-14s %7s %3s %8s %8s %8s %8s %4s %4s %3s %9s\n",
           "SLOT", "NAME", "PID", "EXI", "CPU(ms)", "WAIT(ms)", "TURN(ms)",
           "RESP(ms)", "DSP", "PRE", "IO", "OS-CPU(ms)");
    rule('-');

    for (i = 0; i < proc_count(); i++) {
        const pcb_t *p = proc_at(i);
        long turn = (p->t_finish >= 0 && p->t_arrival >= 0)
                  ? p->t_finish - p->t_arrival : -1;
        long resp = (p->t_first >= 0 && p->t_arrival >= 0)
                  ? p->t_first - p->t_arrival : -1;

        printf("  P%-3d %-14s %7ld ", p->slot, p->name, (long)p->pid);
        if (p->t_arrival >= 0)          /* otherwise it was never created */
            printf("%3d ", p->exit_status);
        else
            printf("%3s ", "-");
        printf("%8ld %8ld ",
               ticks_to_ms(p->cpu_ticks), ticks_to_ms(p->ready_ticks));
        if (turn >= 0)
            printf("%8ld ", ticks_to_ms(turn));
        else
            printf("%8s ", "-");
        if (resp >= 0)
            printf("%8ld ", ticks_to_ms(resp));
        else
            printf("%8s ", "-");
        printf("%4ld %4ld %3ld %9.1f\n",
               p->n_dispatch, p->n_preempt, p->n_io, p->os_cpu_us / 1000.0);

        tot_cpu     += p->cpu_ticks;
        tot_wait    += p->ready_ticks;
        tot_io      += p->n_io;
        tot_preempt += p->n_preempt;
        tot_os_us   += p->os_cpu_us;
        if (turn >= 0 && resp >= 0) {
            tot_turn += turn;
            tot_resp += resp;
            counted++;
        }
    }
    rule('-');

    printf("\n  AVERAGES over %d completed process(es)\n", counted);
    if (counted > 0) {
        printf("    waiting time    %8.1f ms     (time spent READY, wanting the CPU)\n",
               (double)ticks_to_ms(tot_wait) / counted);
        printf("    turnaround time %8.1f ms     (submission to termination)\n",
               (double)ticks_to_ms(tot_turn) / counted);
        printf("    response time   %8.1f ms     (submission to first time on the CPU)\n",
               (double)ticks_to_ms(tot_resp) / counted);
    }

    printf("\n  MACHINE\n");
    printf("    context switches        %6ld     (%ld of them forced by the timer)\n",
           switches, tot_preempt);
    printf("    blocking I/O calls      %6ld\n", tot_io);
    printf("    CPU busy                %6ld ms  (%.1f%% utilization)\n",
           ticks_to_ms(tot_cpu),
           g_trace_ticks ? 100.0 * (double)tot_cpu / (double)g_trace_ticks : 0.0);
    printf("    CPU idle                %6ld ms  (nothing runnable: all waiting on I/O)\n",
           ticks_to_ms(idle_ticks));
    printf("    throughput              %8.2f processes/second\n",
           wall_s > 0 ? counted / wall_s : 0.0);
    printf("    scheduler accounting    %6ld ms  vs %.0f ms charged by the real\n",
           ticks_to_ms(tot_cpu), tot_os_us / 1000.0);
    printf("                                        kernel to the children "
           "(getrusage)\n");

    gantt();
    putchar('\n');
}
