/* syscalls.c - the same kernel services through two different paths.
 *
 * Each function below has two bodies. The default body calls the glibc
 * wrapper. With --raw-syscalls the call uses syscall(2) and traps directly
 * into the Linux kernel with no library code between. Both paths reach the
 * same routine in the kernel. The option makes the system call boundary
 * visible in the source, and strace(1) shows the difference.
 *
 * syscall(2) does not set errno. On failure it returns the negative errno
 * value. The code below translates that value, so the caller can read errno
 * after either path.
 */
#include "syscalls.h"

#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Convert the kernel convention, a negative errno value, into the libc
 * convention, which is -1 plus errno. The caller then cannot tell the two
 * paths apart. */
static long fix(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int sys_kill(pid_t pid, int sig)
{
    if (g_cfg.raw_syscalls)
        return (int)fix(syscall(SYS_kill, (long)pid, (long)sig));
    return kill(pid, sig);
}

pid_t sys_wait4(pid_t pid, int *status, int options, struct rusage *ru)
{
    if (g_cfg.raw_syscalls)
        return (pid_t)fix(syscall(SYS_wait4, (long)pid, (long)status,
                                  (long)options, (long)ru));
    return wait4(pid, status, options, ru);
}

pid_t sys_getpid(void)
{
    if (g_cfg.raw_syscalls)
        return (pid_t)syscall(SYS_getpid);   /* getpid never fails */
    return getpid();
}

/* glibc added a wrapper for this call in version 2.30. Ubuntu 20.04 ships
 * an older glibc, so the raw system call is the only portable path. */
long sys_gettid(void)
{
    return syscall(SYS_gettid);
}

const char *sys_mode(void)
{
    return g_cfg.raw_syscalls ? "raw syscall(2) traps" : "glibc wrappers";
}
