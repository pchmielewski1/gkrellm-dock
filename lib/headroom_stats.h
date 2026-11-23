#ifndef GK_HEADROOM_STATS_H
#define GK_HEADROOM_STATS_H

#include <stddef.h>

typedef struct {
	int ok; /* 1 when parse found usable fields */
	int healthy; /* /health status == healthy or ready */
	char primary_model[64];
	double savings_percent; /* tokens.savings_percent */
	double latency_avg_ms; /* latency.average_ms */
	double tokens_saved; /* tokens.saved */
	double requests_total; /* requests.total */
	double requests_failed; /* requests.failed */
	double workers_in_flight; /* runtime.compression_executor.in_flight */
	double workers_queued; /* runtime.compression_executor.queued */
} GkHeadroomStats;

/* Parse Headroom /stats JSON body into out. Returns 0 on success. */
int gk_headroom_parse_stats(const char *json, GkHeadroomStats *out);

/* Overlay /health JSON (status + compression_executor queue). Returns 0 on success. */
int gk_headroom_parse_health(const char *json, GkHeadroomStats *out);

#endif
