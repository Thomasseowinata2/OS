/* queue.h - a fixed-size FIFO of process table slot numbers.
 *
 * The ready queues of the scheduler use these. The queue allocates no memory
 * at run time. An allocation that fails during a context switch has no safe
 * recovery path, so the code avoids one.
 */
#ifndef QUEUE_H
#define QUEUE_H

#include "tsim.h"

typedef struct {
    int slot[MAX_PROCS + 1];
    int head, tail, count;
} iqueue_t;

void q_init(iqueue_t *q);
bool q_empty(const iqueue_t *q);
int  q_count(const iqueue_t *q);
bool q_push(iqueue_t *q, int slot);       /* add at the tail                 */
int  q_pop(iqueue_t *q);                  /* remove the head, -1 if empty    */
int  q_peek_at(const iqueue_t *q, int i); /* entry i in FIFO order           */
bool q_remove(iqueue_t *q, int slot);     /* remove one entry from the middle */
bool q_contains(const iqueue_t *q, int slot);

#endif /* QUEUE_H */
