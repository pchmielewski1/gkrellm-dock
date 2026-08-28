#define _DEFAULT_SOURCE
#include "thermal_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *k_wanted[] = {
	"TSOC", "TGPU", "TS0E", "TS0P", "TS1E", "TS1P", "TUNC", NULL
};

int gk_thermal_map_from_text(const char *text, GkThermalMap *out)
{
	const char *p;
	if (!text || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	p = text;
	while (*p && out->count < GK_THERMAL_MAX) {
		int zone = -1, temp = 0;
		char typ[32], name[16];
		if (sscanf(p, "%d\t%31s\t%15s\t%d", &zone, typ, name, &temp) == 4) {
			GkThermalZone *z = &out->zones[out->count++];
			memset(z, 0, sizeof(*z));
			strncpy(z->name, name, sizeof(z->name) - 1);
			z->zone = zone;
			z->temp_mC = temp;
		}
		p = strchr(p, '\n');
		if (!p)
			break;
		++p;
	}
	return out->count > 0 ? 0 : -1;
}

static int read_int_file(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", out) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int read_acpi_short_name(int zone, char *name, size_t n)
{
	char path[256], link[256], apath[256];
	char *base;
	ssize_t r;
	FILE *f;

	snprintf(path, sizeof(path),
	         "/sys/class/thermal/thermal_zone%d/device", zone);
	r = readlink(path, link, sizeof(link) - 1);
	if (r < 0)
		return -1;
	link[r] = '\0';
	base = strrchr(link, '/');
	base = base ? base + 1 : link;
	snprintf(apath, sizeof(apath), "/sys/bus/acpi/devices/%s/path", base);
	f = fopen(apath, "r");
	if (!f)
		return -1;
	if (!fgets(path, sizeof(path), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	/* trim */
	base = strrchr(path, '.');
	base = base ? base + 1 : path;
	while (*base == ' ' || *base == '\t')
		++base;
	/* strip newline */
	{
		char *nl = strchr(base, '\n');
		if (nl)
			*nl = '\0';
	}
	strncpy(name, base, n - 1);
	name[n - 1] = '\0';
	return 0;
}

int gk_thermal_map_scan_sysfs(GkThermalMap *out)
{
	int z;
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	for (z = 0; z < 32 && out->count < GK_THERMAL_MAX; ++z) {
		char tpath[128], name[16];
		int temp = 0;
		snprintf(tpath, sizeof(tpath),
		         "/sys/class/thermal/thermal_zone%d/temp", z);
		if (read_int_file(tpath, &temp) != 0)
			continue;
		if (read_acpi_short_name(z, name, sizeof(name)) != 0)
			snprintf(name, sizeof(name), "Z%d", z);
		{
			GkThermalZone *tz = &out->zones[out->count++];
			memset(tz, 0, sizeof(*tz));
			strncpy(tz->name, name, sizeof(tz->name) - 1);
			tz->zone = z;
			tz->temp_mC = temp;
		}
	}
	(void)k_wanted;
	return out->count > 0 ? 0 : -1;
}

int gk_thermal_temp_by_name(const GkThermalMap *map, const char *name)
{
	int i;
	if (!map || !name)
		return -1;
	for (i = 0; i < map->count; ++i)
		if (strcmp(map->zones[i].name, name) == 0)
			return map->zones[i].temp_mC;
	return -1;
}
