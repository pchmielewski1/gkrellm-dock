#ifndef GK_THERMAL_MAP_H
#define GK_THERMAL_MAP_H

#define GK_THERMAL_MAX 16

typedef struct {
	char name[16];   /* TSOC, TGPU, ... */
	int zone;        /* thermal_zoneN index, -1 if missing */
	int temp_mC;     /* millidegree C */
} GkThermalZone;

typedef struct {
	int count;
	GkThermalZone zones[GK_THERMAL_MAX];
} GkThermalMap;

/* Parse fixture lines: "N\\ttype\\tNAME\\ttemp_mC" or scan /sys */
int gk_thermal_map_from_text(const char *text, GkThermalMap *out);
int gk_thermal_map_scan_sysfs(GkThermalMap *out);

/* Lookup by ACPI short name; returns millidegree C or -1 */
int gk_thermal_temp_by_name(const GkThermalMap *map, const char *name);

#endif
