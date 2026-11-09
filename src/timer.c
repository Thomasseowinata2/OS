/* timer.c - the timer of the simulated machine.
 *
 * The code requests a POSIX per-process interval timer. It makes three
 * choices.
 *
 *   CLOCK_MONOTONIC   This clock only moves forward. CLOCK_REALTIME can
 *                     move backward after a time correction or a time zone
 *                     change. A scheduler must not depend on such a clock.
 *
 *   SIGEV_SIGNAL      Linux reports each expiry as a signal. A signal is
 *                     the nearest equivalent in user space to the
 *                     asynchronous interrupt from a real timer.
 *
 *   timer_create(2)   alarm(2) has a resolution of one second, which is too
 *                     coarse. setitimer(2) gives one timer for each process
 *                     and no choice of clock. timer_create(2) gives
 *                     nanosecond resolution, several timers, and a choice
 *                     of clock.
 */
#include "timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static timer_t g_timer;
static bool    g_armed;

bool timer_arm(long tick_ms)
{
    struct sigevent   sev;
    struct itimerspec its;

    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo  = SIGALRM;

    if (timer_create(CLOCK_MONOTONIC, &sev, &g_timer) == -1) {
        perror("timer_create");
        return false;
    }

    /* it_value starts the timer. it_interval repeats it. A single-shot timer
     * would deliver one interrupt, and the kernel needs a steady tick. */
    its.it_value.tv_sec     = tick_ms / 1000;
    its.it_value.tv_nsec    = (tick_ms % 1000) * 1000000L;
    its.it_interval.tv_sec  = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timer_settime(g_timer, 0, &its, NULL) == -1) {
        perror("timer_settime");
        timer_delete(g_timer);
        return false;
    }

    g_armed = true;
    return true;
}

void timer_disarm(void)
{
    struct itimerspec zero;

    if (!g_armed)
        return;
    memset(&zero, 0, sizeof zero);   /* an all-zero itimerspec stops it */
    timer_settime(g_timer, 0, &zero, NULL);
    timer_delete(g_timer);
    g_armed = false;
}
