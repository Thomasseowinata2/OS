/* workload.c - the list of programs that the simulated machine runs.
 *
 * The file holds one line for each program.
 *
 *   name  cpu_ms  io_count  io_ms  nice  arrival_ms
 *
 * A line must give the name and cpu_ms. The other fields default to zero.
 * A line states a requirement. The child process in worker.c uses the CPU
 * time and makes the blocking calls.
 */
#include "workload.h"
#include "pcb.h"

#include <stdio.h>
#include <string.h>

static bool add(const char *name, long cpu_ms, int io_count, long io_ms,
                int nice, long arrival_ms)
{
    pcb_t *p = proc_alloc();

    if (!p) {
        fprintf(stderr, "%s: process table full (max %d)\n",
                TSIM_NAME, MAX_PROCS);
        return false;
    }
    snprintf(p->name, sizeof p->name, "%s", name);
    p->cpu_ms     = cpu_ms;
    p->io_count   = io_count;
    p->io_ms      = io_ms;
    p->nice       = nice;
    p->arrival_ms = arrival_ms;
    return true;
}

int workload_load(const char *path)
{
    char  line[256];
    FILE *f;
    int   lineno = 0, n = 0;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: cannot open workload %s\n", TSIM_NAME, path);
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        char  name[64];
        long  cpu_ms = 0, io_ms = 0, arrival_ms = 0;
        int   io_count = 0, nice = 0, got;
        char *hash;

        lineno++;
        hash = strchr(line, '#');
        if (hash)
            *hash = '\0';

        got = sscanf(line, "%63s %ld %d %ld %d %ld",
                     name, &cpu_ms, &io_count, &io_ms, &nice, &arrival_ms);
        if (got <= 0)
            continue;                        /* an empty or comment line */
        if (got < 2) {
            fprintf(stderr, "%s:%d: expected at least a name and cpu_ms\n",
                    path, lineno);
            fclose(f);
            return -1;
        }
        if (cpu_ms <= 0) {
            fprintf(stderr, "%s:%d: cpu_ms must be positive\n", path, lineno);
            fclose(f);
            return -1;
        }
        if (io_count < 0 || nice < 0 || nice > 9 || arrival_ms < 0) {
            fprintf(stderr, "%s:%d: io_count/arrival must be >= 0 and "
                            "nice must be 0..9\n", path, lineno);
            fclose(f);
            return -1;
        }
        if (io_count > 0 && io_ms <= 0)
            io_ms = 50;                      /* default device service time */

        if (!add(name, cpu_ms, io_count, io_ms, nice, arrival_ms)) {
            fclose(f);
            return -1;
        }
        n++;
    }

    fclose(f);
    if (n == 0)
        fprintf(stderr, "%s: %s describes no processes\n", TSIM_NAME, path);
    return n;
}

/* A mixed set of programs. One program only computes. One program spends
 * most of its life in I/O. Two programs fall between the two extremes. This
 * mixture shows the difference between the four policies in one run. */
int workload_builtin(void)
{
    struct { const char *name; long cpu; int ion; long ioms; int nice; long arr; }
    w[] = {
        { "cpu-hog",      700, 0,   0, 2,   0 },
        { "text-editor",  180, 6,  60, 0,   0 },
        { "compiler",     420, 2,  80, 1,   0 },
        { "log-writer",   240, 4,  40, 1, 120 },
    };
    size_t i;

    for (i = 0; i < sizeof w / sizeof w[0]; i++)
        if (!add(w[i].name, w[i].cpu, w[i].ion, w[i].ioms, w[i].nice, w[i].arr))
            return -1;
    return (int)(sizeof w / sizeof w[0]);
}

void workload_print_example(void)
{
    printf("# name            cpu_ms  io_count  io_ms  nice  arrival_ms\n"
           "cpu-hog              700         0      0     2           0\n"
           "text-editor          180         6     60     0           0\n"
           "compiler             420         2     80     1           0\n"
           "log-writer           240         4     40     1         120\n");
}
