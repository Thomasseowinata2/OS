/* scheduler.h - the scheduling policies.
 *
 * kernel.c decides when the kernel must make a scheduling decision.
 * scheduler.c decides which process runs next. This split lets four
 * policies share one dispatcher.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "tsim.h"

void sched_init(void);

void sched_admit(int slot);      /* a new process arrived                   */
void sched_preempted(int slot);  /* its time slice ended, it may be demoted */
void sched_requeue(int slot);    /* requeue it and keep its level           */
void sched_remove(int slot);     /* it terminated, remove all record of it  */

int  sched_pick(void);           /* select the next process and dequeue it  */
int  sched_peek(void);           /* report the next process, change nothing */
bool sched_outranks(int a, int b); /* true if a has priority over b         */
long sched_quantum(int slot);    /* time slice to grant, in ticks           */
void sched_tick(void);           /* periodic work, such as the MLFQ boost   */

int  sched_ready_count(void);
void sched_queues_str(char *buf, size_t n);

#endif /* SCHEDULER_H */
