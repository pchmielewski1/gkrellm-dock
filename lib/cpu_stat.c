#include "cpu_stat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gk_cpu_stat_parse(const char *text, GkCpuTimes times[GK_CPU_MAX], int *max_cpu)
{
	const char *p;
	int max = -1;

	if (!text || !times)
		return -1;
	memset(times, 0, sizeof(GkCpuTimes) * GK_CPU_MAX);
	p = text;
	while (*p) {
		if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u' &&
		    p[3] >= '0' && p[3] <= '9') {
			int cpu = (int)strtol(p + 3, (char **)&p, 10);
			GkCpuTimes t;
			memset(&t, 0, sizeof(t));
			if (cpu >= 0 && cpu < GK_CPU_MAX &&
			    sscanf(p,
			           " %llu %llu %llu %llu %llu %llu %llu %llu",
			           &t.user, &t.nice, &t.system, &t.idle,
			           &t.iowait, &t.irq, &t.softirq, &t.steal) >= 4) {
				times[cpu] = t;
				if (cpu > max)
					max = cpu;
			}
		}
		p = strchr(p, '\n');
		if (!p)
			break;
		++p;
	}
	if (max_cpu)
		*max_cpu = max;
	return (max >= 0) ? 0 : -1;
}

static unsigned long long busy_sum(const GkCpuTimes *t)
{
	return t->user + t->nice + t->system + t->irq + t->softirq + t->steal;
}

static unsigned long long total_sum(const GkCpuTimes *t)
{
	return busy_sum(t) + t->idle + t->iowait;
}

unsigned long long gk_cpu_busy_abs(const GkCpuTimes *t)
{
	return t ? busy_sum(t) : 0;
}

unsigned long long gk_cpu_total_abs(const GkCpuTimes *t)
{
	return t ? total_sum(t) : 0;
}

double gk_cpu_stat_core_busy_pct(const GkCpuTimes *prev, const GkCpuTimes *cur)
{
	unsigned long long b0, b1, t0, t1;

	if (!prev || !cur)
		return 0.0;
	b0 = busy_sum(prev);
	b1 = busy_sum(cur);
	t0 = total_sum(prev);
	t1 = total_sum(cur);
	if (t1 <= t0)
		return 0.0;
	return 100.0 * (double)(b1 - b0) / (double)(t1 - t0);
}

double gk_cpu_stat_busy_pct(const GkCpuList *list,
                            const GkCpuTimes *prev,
                            const GkCpuTimes *cur)
{
	unsigned long long db = 0, dt = 0;
	int i;

	if (!list || !prev || !cur || list->count == 0)
		return 0.0;
	for (i = 0; i < list->count; ++i) {
		int c = list->cpus[i];
		unsigned long long b0, b1, t0, t1;
		if (c < 0 || c >= GK_CPU_MAX)
			continue;
		b0 = busy_sum(&prev[c]);
		b1 = busy_sum(&cur[c]);
		t0 = total_sum(&prev[c]);
		t1 = total_sum(&cur[c]);
		if (t1 > t0) {
			db += (b1 - b0);
			dt += (t1 - t0);
		}
	}
	if (dt == 0)
		return 0.0;
	return 100.0 * (double)db / (double)dt;
}

int gk_cpu_stat_sample(GkCpuStatState *st,
                       const GkCpuList clusters[GK_CLUSTER_COUNT],
                       double busy_out[GK_CLUSTER_COUNT],
                       double core_busy_out[GK_CPU_MAX])
{
	char *buf = NULL;
	size_t cap = 0, len = 0;
	char chunk[1024];
	FILE *f;
	GkCpuTimes cur[GK_CPU_MAX];
	int i, max_cpu = -1;

	if (!st || !clusters || !busy_out)
		return -1;

	f = fopen("/proc/stat", "r");
	if (!f)
		return -1;
	while (fgets(chunk, sizeof(chunk), f)) {
		size_t cl = strlen(chunk);
		if (len + cl + 1 > cap) {
			cap = cap ? cap * 2 : 8192;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, chunk, cl);
		len += cl;
		buf[len] = '\0';
	}
	fclose(f);

	if (gk_cpu_stat_parse(buf, cur, &max_cpu) != 0) {
		free(buf);
		return -1;
	}
	free(buf);

	for (i = 0; i < GK_CLUSTER_COUNT; ++i) {
		if (!st->have_prev)
			busy_out[i] = 0.0;
		else
			busy_out[i] = gk_cpu_stat_busy_pct(&clusters[i],
			                                   st->prev, cur);
	}
	if (core_busy_out) {
		for (i = 0; i < GK_CPU_MAX; ++i) {
			if (!st->have_prev)
				core_busy_out[i] = 0.0;
			else
				core_busy_out[i] =
				    gk_cpu_stat_core_busy_pct(&st->prev[i], &cur[i]);
		}
	}
	memcpy(st->prev, cur, sizeof(cur));
	st->have_prev = 1;
	return 0;
}
