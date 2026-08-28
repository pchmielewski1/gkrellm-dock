#include "cpu_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *gk_cluster_name(GkClusterId id)
{
	switch (id) {
	case GK_CLUSTER_X925: return "X925";
	case GK_CLUSTER_A725: return "A725";
	default: return "?";
	}
}

static void push_cpu(GkCpuList *list, int cpu)
{
	int i;
	if (cpu < 0 || list->count >= GK_CPU_MAX)
		return;
	for (i = 0; i < list->count; ++i)
		if (list->cpus[i] == cpu)
			return;
	list->cpus[list->count++] = cpu;
}

static void apply_fallback(GkCpuList out[GK_CLUSTER_COUNT])
{
	static const int x925[] = {5, 6, 7, 8, 9, 15, 16, 17, 18, 19};
	static const int a725[] = {0, 1, 2, 3, 4, 10, 11, 12, 13, 14};
	size_t i;
	memset(out, 0, sizeof(GkCpuList) * GK_CLUSTER_COUNT);
	for (i = 0; i < sizeof(x925) / sizeof(x925[0]); ++i)
		push_cpu(&out[GK_CLUSTER_X925], x925[i]);
	for (i = 0; i < sizeof(a725) / sizeof(a725[0]); ++i)
		push_cpu(&out[GK_CLUSTER_A725], a725[i]);
}

int gk_cpu_map_from_cpuinfo(const char *text, GkCpuList out[GK_CLUSTER_COUNT])
{
	const char *p = text;
	int cpu = -1;
	int found = 0;

	if (!text || !out)
		return -1;
	memset(out, 0, sizeof(GkCpuList) * GK_CLUSTER_COUNT);

	while (*p) {
		if (strncmp(p, "processor", 9) == 0) {
			const char *c = strchr(p, ':');
			if (c)
				cpu = (int)strtol(c + 1, NULL, 10);
		} else if (strncmp(p, "CPU part", 8) == 0 && cpu >= 0) {
			const char *c = strchr(p, ':');
			unsigned long part = 0;
			if (c) {
				while (*c == ':' || *c == ' ' || *c == '\t')
					++c;
				part = strtoul(c, NULL, 0);
			}
			if (part == GK_CPU_PART_X925) {
				push_cpu(&out[GK_CLUSTER_X925], cpu);
				found = 1;
			} else if (part == GK_CPU_PART_A725) {
				push_cpu(&out[GK_CLUSTER_A725], cpu);
				found = 1;
			}
			cpu = -1;
		}
		p = strchr(p, '\n');
		if (!p)
			break;
		++p;
	}

	if (!found || out[GK_CLUSTER_X925].count == 0 ||
	    out[GK_CLUSTER_A725].count == 0) {
		apply_fallback(out);
	}
	return 0;
}

int gk_cpu_map_load(const char *path, GkCpuList out[GK_CLUSTER_COUNT])
{
	FILE *f;
	char *buf = NULL;
	size_t n = 0;
	long sz;
	int rc;

	f = fopen(path ? path : "/proc/cpuinfo", "r");
	if (!f) {
		apply_fallback(out);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) == 0) {
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0) {
			buf = malloc((size_t)sz + 1);
			if (buf) {
				n = fread(buf, 1, (size_t)sz, f);
				buf[n] = '\0';
			}
		}
	}
	if (!buf) {
		/* streaming fallback */
		char chunk[4096];
		size_t cap = 0, len = 0;
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
	}
	fclose(f);
	rc = gk_cpu_map_from_cpuinfo(buf ? buf : "", out);
	free(buf);
	return rc;
}
