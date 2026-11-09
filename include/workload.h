/* workload.h - read the list of programs to simulate. */
#ifndef WORKLOAD_H
#define WORKLOAD_H

#include "tsim.h"

int  workload_load(const char *path);  /* processes created, or -1 on error */
int  workload_builtin(void);
void workload_print_example(void);

#endif /* WORKLOAD_H */
