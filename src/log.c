/* log.c - the kernel console and the optional CSV trace. */
#include "log.h"
#include "pcb.h"
#include "scheduler.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE *g_csv;

/* ANSI colors, so that a demonstration stays readable on a projector. */
static const char *color_of(const char *kind)
{
    if (!strcmp(kind, "DISPATCH")) return "\033[1;32m";
    if (!strcmp(kind, "PREEMPT"))  return "\033[1;33m";
    if (!strcmp(kind, "IO-REQ"))   return "\033[1;36m";
    if (!strcmp(kind, "IO-DONE"))  return "\033[0;36m";
    if (!strcmp(kind, "CREATE"))   return "\033[1;34m";
    if (!strcmp(kind, "EXIT"))     return "\033[1;35m";
    if (!strcmp(kind, "BOOST"))    return "\033[1;37m";
    if (!strcmp(kind, "IDLE"))     return "\033[0;90m";
    if (!strcmp(kind, "ERROR"))    return "\033[1;31m";
    if (!strcmp(kind, "VERIFY"))   return "\033[1;31m";
    return "\033[0m";
}

void log_open(void)
{
    if (!g_cfg.csv_path)
        return;
    g_csv = fopen(g_cfg.csv_path, "w");
    if (!g_csv) {
        fprintf(stderr, "%s: cannot write %s\n", TSIM_NAME, g_cfg.csv_path);
        return;
    }
    fprintf(g_csv, "tick,ms,event,detail\n");
}

void log_close(void)
{
    if (g_csv) {
        fclose(g_csv);
        g_csv = NULL;
    }
}

/* A child inherits the stdio buffers of the parent across fork(2). Any text
 * left in a buffer would print twice, once from the kernel and once from
 * the child. The child leaves through _exit(2), which does not flush, but
 * the kernel still empties the buffers before each fork(2). The code must
 * not depend on the behavior of _exit(2) alone. */
void log_flush(void)
{
    fflush(stdout);
    if (g_csv)
        fflush(g_csv);
}

void log_event(const char *kind, const char *fmt, ...)
{
    char    msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (g_csv)
        fprintf(g_csv, "%ld,%ld,%s,\"%s\"\n",
                g_tick, ticks_to_ms(g_tick), kind, msg);

    if (g_cfg.quiet)
        return;

    printf("[%7ld ms | tick %5ld ]  %s%-9s%s %s\n",
           ticks_to_ms(g_tick), g_tick,
           g_cfg.color ? color_of(kind) : "", kind,
           g_cfg.color ? "\033[0m" : "", msg);
    fflush(stdout);
}

void log_banner(const char *fmt, ...)
{
    va_list ap;

    if (g_cfg.quiet)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void log_table(void)
{
    char queues[256];
    int  i;

    if (g_cfg.quiet)
        return;

    sched_queues_str(queues, sizeof queues);
    printf("\n     process table at tick %ld (%ld ms)   ready queues: %s\n",
           g_tick, ticks_to_ms(g_tick), queues);
    printf("     %-4s %-14s %8s %-11s %3s %7s %7s %7s %4s\n",
           "SLOT", "NAME", "PID", "STATE", "LVL", "CPU(ms)", "RDY(ms)",
           "IO(ms)", "SWI");
    for (i = 0; i < proc_count(); i++) {
        const pcb_t *p = proc_at(i);
        printf("     P%-3d %-14s %8ld %-11s %3d %7ld %7ld %7ld %4ld\n",
               p->slot, p->name, (long)p->pid, state_name(p->state), p->level,
               ticks_to_ms(p->cpu_ticks), ticks_to_ms(p->ready_ticks),
               ticks_to_ms(p->blocked_ticks), p->n_dispatch);
    }
    putchar('\n');
    fflush(stdout);
}
