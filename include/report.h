/* report.h - the per-tick recorder and the final report. */
#ifndef REPORT_H
#define REPORT_H

#include "tsim.h"

void report_record(long tick, int running);   /* record the state of each process */
void report_final(double wall_s, long switches, long idle_ticks);

#endif /* REPORT_H */
