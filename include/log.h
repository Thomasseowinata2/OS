/* log.h - the kernel console.
 *
 * No signal handler calls these functions. The handlers only write flags of
 * type volatile sig_atomic_t. The main loop prints. printf(3) is not
 * async-signal-safe. A handler that calls printf(3) can deadlock against the
 * main loop on the stream lock.
 */
#ifndef LOG_H
#define LOG_H

#include "tsim.h"

void log_open(void);
void log_close(void);
void log_flush(void);   /* empty every stdio buffer, call before fork(2) */

void log_event(const char *kind, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
void log_banner(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
void log_table(void);   /* print the process table */

#endif /* LOG_H */
