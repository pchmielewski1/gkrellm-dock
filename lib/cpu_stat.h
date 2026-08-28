#ifndef GK_CPU_STAT_H
#define GK_CPU_STAT_H

#include "cpu_map.h"

typedef struct {
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} GkCpuTimes;

typedef struct {
	GkCpuTimes prev[GK_CPU_MAX];
	int have_prev;
} GkCpuStatState;

/* Parse one /proc/stat blob; fill times[cpu] for present cpus. */
int gk_cpu_stat_parse(const char *text, GkCpuTimes times[GK_CPU_MAX], int *max_cpu);

/* Aggregate busy percent 0..100 for a cluster between two samples. */
double gk_cpu_stat_busy_pct(const GkCpuList *list,
                            const GkCpuTimes *prev,
                            const GkCpuTimes *cur);

/* Single-CPU busy percent 0..100 between two samples. */
double gk_cpu_stat_core_busy_pct(const GkCpuTimes *prev,
                                 const GkCpuTimes *cur);

/* Absolute cumulative counters from one /proc/stat sample (for chart store). */
unsigned long long gk_cpu_busy_abs(const GkCpuTimes *t);
unsigned long long gk_cpu_total_abs(const GkCpuTimes *t);

/* Read /proc/stat and update state; returns busy% for each cluster.
 * If core_busy_out != NULL, also fills per-CPU busy% (0..100) for this interval.
 */
int gk_cpu_stat_sample(GkCpuStatState *st,
                       const GkCpuList clusters[GK_CLUSTER_COUNT],
                       double busy_out[GK_CLUSTER_COUNT],
                       double core_busy_out[GK_CPU_MAX]);

#endif
