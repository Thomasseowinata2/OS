/* scheduler.c - four scheduling policies behind one interface.
 *
 * FCFS  First come, first served, with no preemption. Run the simulator
 *       with -p fcfs and the first CPU-bound program holds the machine
 *       until it ends. The interactive programs wait. Time sharing exists
 *       to solve this problem.
 *
 * RR    Round robin. Each runnable process receives the same quantum in
 *       turn. For N processes the response time cannot exceed N * quantum.
 *
 * PRIO  Static priority, with round robin inside one priority. This policy
 *       shows starvation. A steady supply of high-priority work stops the
 *       low-priority process from ever running.
 *
 * MLFQ  Multi-level feedback queue. Early UNIX systems used this policy.
 *       The kernel does not know whether a program is interactive or
 *       CPU-bound, so it measures the behavior of the program.
 *         - A process that uses its whole quantum is CPU-bound. The kernel
 *           demotes it to a lower queue with a longer quantum.
 *         - A process that blocks before its quantum ends is interactive.
 *           It keeps its level and stays responsive.
 *         - At each boost period the kernel returns every process to the
 *           top queue. This prevents the starvation that PRIO allows.
 */
#include "scheduler.h"
#include "log.h"
#include "pcb.h"
#include "queue.h"

#include <stdio.h>
#include <string.h>

static iqueue_t g_q[MLFQ_LEVELS];
static long     g_last_boost;

void sched_init(void)
{
    int l;
    for (l = 0; l < MLFQ_LEVELS; l++)
        q_init(&g_q[l]);
    g_last_boost = 0;
}

/* Each policy except MLFQ uses one ready queue. */
static int levels_in_use(void)
{
    return (g_cfg.policy == SCHED_MLFQ) ? MLFQ_LEVELS : 1;
}

void sched_admit(int slot)
{
    pcb_t *p = proc_at(slot);
    p->level = 0;                /* new work starts in the top queue */
    q_push(&g_q[0], slot);
}

void sched_preempted(int slot)
{
    pcb_t *p = proc_at(slot);
    /* The process used a whole time slice and did not block, so the kernel
     * treats it as CPU-bound. Move it down one level. It then receives a
     * longer slice, but it waits behind the higher levels. */
    if (g_cfg.policy == SCHED_MLFQ && p->level < MLFQ_LEVELS - 1)
        p->level++;
    q_push(&g_q[p->level], slot);
}

/* The kernel calls this when an I/O request finishes, and when a
 * higher-priority process takes the CPU. In both cases the process did not
 * use a whole time slice, so it keeps its level. This rule makes an MLFQ
 * favor interactive programs without any declaration from the programs. */
void sched_requeue(int slot)
{
    pcb_t *p = proc_at(slot);
    q_push(&g_q[p->level], slot);
}

void sched_remove(int slot)
{
    int l;
    for (l = 0; l < MLFQ_LEVELS; l++)
        q_remove(&g_q[l], slot);
}

int sched_peek(void)
{
    int l, i, best = -1;

    if (g_cfg.policy == SCHED_PRIO) {
        /* Scan the queue in arrival order and select the highest priority.
         * A scan in FIFO order breaks ties between equal priorities by
         * arrival time. */
        for (i = 0; i < q_count(&g_q[0]); i++) {
            int s = q_peek_at(&g_q[0], i);
            if (best < 0 || proc_at(s)->nice < proc_at(best)->nice)
                best = s;
        }
        return best;
    }

    /* The levels have strict priority. No process in level 1 runs while
     * level 0 holds work. The periodic boost is necessary for this reason. */
    for (l = 0; l < levels_in_use(); l++)
        if (q_count(&g_q[l]) > 0)
            return q_peek_at(&g_q[l], 0);
    return -1;
}

int sched_pick(void)
{
    int slot = sched_peek();

    if (slot >= 0)
        sched_remove(slot);
    return slot;
}

/* Does process a have priority over process b?
 *
 * Only the policies that rank processes return true here. Round robin
 * returns false. Round robin guarantees that a process keeps the CPU for a
 * full quantum. A switch to a new arrival would break that guarantee and
 * make the length of one round unpredictable.
 *
 * A priority scheduler makes the opposite guarantee and must act at once. A
 * wait for the end of the current slice would leave a high-priority process
 * in the queue for up to a full quantum. On the lowest MLFQ level that
 * quantum is long. */
bool sched_outranks(int a, int b)
{
    const pcb_t *pa = proc_at(a);
    const pcb_t *pb = proc_at(b);

    if (!pa || !pb)
        return false;
    switch (g_cfg.policy) {
    case SCHED_MLFQ: return pa->level < pb->level;
    case SCHED_PRIO: return pa->nice  < pb->nice;
    default:         return false;
    }
}

long sched_quantum(int slot)
{
    const pcb_t *p = proc_at(slot);

    switch (g_cfg.policy) {
    case SCHED_FCFS:
        return -1;                      /* no limit, run until it blocks or ends */
    case SCHED_MLFQ:
        /* Each lower queue grants a slice twice as long. A CPU-bound program
         * moves to the lowest level, where the kernel switches it less
         * often. This reduces the context-switch overhead that it causes. */
        return g_cfg.quantum << p->level;
    case SCHED_RR:
    case SCHED_PRIO:
    default:
        return g_cfg.quantum;
    }
}

void sched_tick(void)
{
    int moved = 0, l, i, s;

    if (g_cfg.policy != SCHED_MLFQ || g_cfg.boost_every <= 0)
        return;
    if (g_tick - g_last_boost < g_cfg.boost_every)
        return;
    g_last_boost = g_tick;

    for (l = 1; l < MLFQ_LEVELS; l++) {
        while ((s = q_pop(&g_q[l])) >= 0) {
            proc_at(s)->level = 0;
            q_push(&g_q[0], s);
            moved++;
        }
    }
    /* The process on the CPU and the processes that wait for I/O also hold
     * a level. A boost of the queues alone would let them return to the
     * lower levels at once. */
    for (i = 0; i < proc_count(); i++) {
        pcb_t *p = proc_at(i);
        if ((p->state == P_RUNNING || p->state == P_BLOCKED) && p->level != 0) {
            p->level = 0;
            moved++;
        }
    }
    if (moved)
        log_event("BOOST", "anti-starvation boost: %d process(es) back to level 0",
                  moved);
}

int sched_ready_count(void)
{
    int l, n = 0;
    for (l = 0; l < MLFQ_LEVELS; l++)
        n += q_count(&g_q[l]);
    return n;
}

void sched_queues_str(char *buf, size_t n)
{
    size_t used = 0;
    int    l, i;

    buf[0] = '\0';
    for (l = 0; l < levels_in_use(); l++) {
        used += (size_t)snprintf(buf + used, n > used ? n - used : 0,
                                 "L%d[", l);
        for (i = 0; i < q_count(&g_q[l]); i++) {
            const pcb_t *p = proc_at(q_peek_at(&g_q[l], i));
            used += (size_t)snprintf(buf + used, n > used ? n - used : 0,
                                     "%s%s", i ? " " : "", p ? p->name : "?");
        }
        used += (size_t)snprintf(buf + used, n > used ? n - used : 0, "] ");
        if (used >= n)
            break;
    }
}
