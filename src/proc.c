/* proc.c - the process table.
 *
 * A real kernel allocates process control blocks at run time. This table is
 * a fixed array. The simulator therefore has no allocation failure path,
 * and each slot number stays valid for the whole run. The scheduler queues,
 * the trace recorder and the I/O signals all name a process by its slot
 * number, so a slot number must never change.
 */
#include "pcb.h"

#include <string.h>

static pcb_t g_table[MAX_PROCS];
static int   g_nproc;

const char *state_name(pstate_t s)
{
    static const char *n[] = {
        "UNUSED", "NEW", "READY", "RUNNING", "BLOCKED", "TERMINATED"
    };
    return (s >= P_UNUSED && s <= P_TERMINATED) ? n[s] : "??";
}

char state_char(pstate_t s)
{
    switch (s) {
    case P_NEW:        return 'n';
    case P_READY:      return 'R';
    case P_RUNNING:    return '>';
    case P_BLOCKED:    return 'B';
    case P_TERMINATED: return 'x';
    default:           return '-';
    }
}

void proc_table_init(void)
{
    memset(g_table, 0, sizeof g_table);
    g_nproc = 0;
}

pcb_t *proc_alloc(void)
{
    pcb_t *p;
    if (g_nproc >= MAX_PROCS)
        return NULL;
    p = &g_table[g_nproc];
    memset(p, 0, sizeof *p);
    p->slot      = g_nproc;
    p->state     = P_NEW;
    p->t_arrival = -1;
    p->t_first   = -1;
    p->t_finish  = -1;
    p->level     = 0;
    g_nproc++;
    return p;
}

pcb_t *proc_at(int slot)
{
    return (slot >= 0 && slot < g_nproc) ? &g_table[slot] : NULL;
}

pcb_t *proc_by_pid(pid_t pid)
{
    int i;
    if (pid <= 0)
        return NULL;
    for (i = 0; i < g_nproc; i++)
        if (g_table[i].pid == pid)
            return &g_table[i];
    return NULL;
}

int proc_count(void)
{
    return g_nproc;
}

int proc_count_state(pstate_t s)
{
    int i, n = 0;
    for (i = 0; i < g_nproc; i++)
        if (g_table[i].state == s)
            n++;
    return n;
}

bool proc_all_done(void)
{
    int i;
    for (i = 0; i < g_nproc; i++)
        if (g_table[i].state != P_TERMINATED)
            return false;
    return g_nproc > 0;
}
