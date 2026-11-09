/* worker.h - the code that runs inside a simulated user process. */
#ifndef WORKER_H
#define WORKER_H

#include "pcb.h"

void worker_main(const pcb_t *spec, int ready_fd) __attribute__((noreturn));

#endif /* WORKER_H */
