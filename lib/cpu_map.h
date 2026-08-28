#ifndef GK_CPU_MAP_H
#define GK_CPU_MAP_H

#include <stddef.h>

#define GK_CPU_MAX 64
#define GK_CPU_PART_X925 0xd85
#define GK_CPU_PART_A725 0xd87

typedef enum {
	GK_CLUSTER_X925 = 0,
	GK_CLUSTER_A725 = 1,
	GK_CLUSTER_COUNT = 2
} GkClusterId;

typedef struct {
	int count;
	int cpus[GK_CPU_MAX];
} GkCpuList;

/* Parse /proc/cpuinfo text. Returns 0 on success. */
int gk_cpu_map_from_cpuinfo(const char *text, GkCpuList out[GK_CLUSTER_COUNT]);

/* Convenience: read path (usually "/proc/cpuinfo"). */
int gk_cpu_map_load(const char *path, GkCpuList out[GK_CLUSTER_COUNT]);

const char *gk_cluster_name(GkClusterId id);

#endif
