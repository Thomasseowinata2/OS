/* timer.h - the timer of the simulated machine.
 *
 * timer_create(2) supplies an interval timer that sends SIGALRM once per
 * tick. The kernel needs this timer to take the CPU back from a running
 * process. Without it the system can only switch processes when a process
 * gives up the CPU by itself.
 */
#ifndef TIMER_H
#define TIMER_H

#include "tsim.h"

bool timer_arm(long tick_ms);  /* create and start the interval timer */
void timer_disarm(void);       /* stop and delete the interval timer  */

#endif /* TIMER_H */
