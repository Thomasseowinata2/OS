/* syscalls.h - the same kernel services through two different paths.
 *
 * By default these functions call the glibc wrappers. The --raw-syscalls
 * option makes them call syscall(2) instead. Both paths reach the same
 * routine in the Linux kernel. The option makes the system call boundary
 * explicit in the source, and strace(1) shows the difference.
 */
#ifndef SYSCALLS_H
#define SYSCALLS_H

#include "tsim.h"
#include <sys/resource.h>
#include <sys/wait.h>

int   sys_kill(pid_t pid, int sig);
pid_t sys_wait4(pid_t pid, int *status, int options, struct rusage *ru);
pid_t sys_getpid(void);
long  sys_gettid(void);
const char *sys_mode(void);

#endif /* SYSCALLS_H */
