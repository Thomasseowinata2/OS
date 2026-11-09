/* queue.c - fixed-size FIFO of process table slot numbers. */
#include "queue.h"

#define CAP (MAX_PROCS + 1)

void q_init(iqueue_t *q)
{
    q->head = q->tail = q->count = 0;
}

bool q_empty(const iqueue_t *q)
{
    return q->count == 0;
}

int q_count(const iqueue_t *q)
{
    return q->count;
}

bool q_push(iqueue_t *q, int slot)
{
    if (q->count >= CAP)
        return false;
    q->slot[q->tail] = slot;
    q->tail = (q->tail + 1) % CAP;
    q->count++;
    return true;
}

int q_pop(iqueue_t *q)
{
    int s;
    if (q->count == 0)
        return -1;
    s = q->slot[q->head];
    q->head = (q->head + 1) % CAP;
    q->count--;
    return s;
}

int q_peek_at(const iqueue_t *q, int i)
{
    if (i < 0 || i >= q->count)
        return -1;
    return q->slot[(q->head + i) % CAP];
}

bool q_contains(const iqueue_t *q, int slot)
{
    int i;
    for (i = 0; i < q->count; i++)
        if (q->slot[(q->head + i) % CAP] == slot)
            return true;
    return false;
}

/* A plain FIFO does not remove an entry from the middle, but the scheduler
 * needs this operation. A process can end while it waits in a ready queue,
 * and the kernel must not give the CPU to a process that has ended. */
bool q_remove(iqueue_t *q, int slot)
{
    int i, j;
    for (i = 0; i < q->count; i++) {
        if (q->slot[(q->head + i) % CAP] != slot)
            continue;
        for (j = i; j < q->count - 1; j++)
            q->slot[(q->head + j) % CAP] = q->slot[(q->head + j + 1) % CAP];
        q->tail = (q->tail - 1 + CAP) % CAP;
        q->count--;
        return true;
    }
    return false;
}
