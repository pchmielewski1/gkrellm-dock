#include "headroom_stats.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
	while (p && *p && isspace((unsigned char)*p))
		p++;
	return p;
}

/* Find `"key"` then `:`; return pointer after colon (or NULL). */
static const char *find_key_colon(const char *json, const char *key)
{
	char needle[96];
	const char *p;
	size_t key_len;

	if (!json || !key)
		return NULL;
	key_len = strlen(key);
	if (key_len + 4 >= sizeof(needle))
		return NULL;
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return NULL;
	p += strlen(needle);
	p = skip_ws(p);
	if (!p || *p != ':')
		return NULL;
	return skip_ws(p + 1);
}

/* Bound of a JSON object starting at `{` (inclusive). Returns end `}` or NULL. */
static const char *object_end(const char *start)
{
	int depth = 0;
	int in_str = 0;
	int esc = 0;
	const char *p;

	if (!start || *start != '{')
		return NULL;
	for (p = start; *p; ++p) {
		if (in_str) {
			if (esc)
				esc = 0;
			else if (*p == '\\')
				esc = 1;
			else if (*p == '"')
				in_str = 0;
			continue;
		}
		if (*p == '"') {
			in_str = 1;
			continue;
		}
		if (*p == '{')
			++depth;
		else if (*p == '}') {
			--depth;
			if (depth == 0)
				return p;
		}
	}
	return NULL;
}

static int extract_number_after(const char *after_colon, double *out)
{
	if (!after_colon || !out)
		return -1;
	after_colon = skip_ws(after_colon);
	if (sscanf(after_colon, "%lf", out) == 1)
		return 0;
	return -1;
}

static int extract_number(const char *json, const char *key, double *out)
{
	const char *p = find_key_colon(json, key);
	return extract_number_after(p, out);
}

/*
 * Find object for `obj_key`, then first `field` number inside that object.
 * Avoids matching same field names in sibling objects (e.g. display_session).
 */
/* Find `"obj_key": { ... }` (skip scalar `"obj_key": 123`). */
static const char *find_object_value(const char *json, const char *obj_key)
{
	char needle[96];
	const char *p;
	size_t nlen;

	if (!json || !obj_key)
		return NULL;
	snprintf(needle, sizeof(needle), "\"%s\"", obj_key);
	nlen = strlen(needle);
	p = json;
	while ((p = strstr(p, needle)) != NULL) {
		const char *after = skip_ws(p + nlen);
		if (after && *after == ':') {
			after = skip_ws(after + 1);
			if (after && *after == '{')
				return after;
		}
		p += nlen;
	}
	return NULL;
}

static int extract_number_in_object(const char *json, const char *obj_key,
                                    const char *field, double *out)
{
	const char *p;
	const char *end;
	const char *hit;
	char needle[96];

	p = find_object_value(json, obj_key);
	if (!p)
		return -1;
	end = object_end(p);
	if (!end || end <= p)
		return -1;
	snprintf(needle, sizeof(needle), "\"%s\"", field);
	hit = strstr(p, needle);
	if (!hit || hit > end)
		return -1;
	hit = find_key_colon(hit, field);
	return extract_number_after(hit, out);
}

static int extract_string(const char *json, const char *key, char *out,
                          size_t out_sz)
{
	const char *p;
	size_t i;

	if (!out || out_sz == 0)
		return -1;
	out[0] = '\0';
	p = find_key_colon(json, key);
	if (!p)
		return -1;
	p = skip_ws(p);
	if (*p != '"')
		return -1;
	++p;
	for (i = 0; i + 1 < out_sz && *p && *p != '"'; ++p) {
		if (*p == '\\' && p[1]) {
			++p;
			out[i++] = *p;
			continue;
		}
		out[i++] = *p;
	}
	out[i] = '\0';
	return i > 0 ? 0 : -1;
}

int gk_headroom_parse_stats(const char *json, GkHeadroomStats *out)
{
	int got = 0;

	if (!json || !out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (extract_string(json, "primary_model", out->primary_model,
	                   sizeof(out->primary_model)) == 0)
		got = 1;

	if (extract_number_in_object(json, "tokens", "savings_percent",
	                            &out->savings_percent) == 0)
		got = 1;
	else if (extract_number(json, "savings_percent",
	                        &out->savings_percent) == 0)
		got = 1;

	if (extract_number_in_object(json, "tokens", "saved",
	                            &out->tokens_saved) == 0)
		got = 1;

	if (extract_number_in_object(json, "latency", "average_ms",
	                            &out->latency_avg_ms) == 0)
		got = 1;

	if (extract_number_in_object(json, "requests", "total",
	                            &out->requests_total) == 0)
		got = 1;
	if (extract_number_in_object(json, "requests", "failed",
	                            &out->requests_failed) == 0)
		got = 1;

	out->ok = got ? 1 : 0;
	return got ? 0 : -1;
}

int gk_headroom_parse_health(const char *json, GkHeadroomStats *out)
{
	char status[32];

	if (!json || !out)
		return -1;

	if (extract_string(json, "status", status, sizeof(status)) == 0) {
		if (!strcmp(status, "healthy") || !strcmp(status, "ok"))
			out->healthy = 1;
		else
			out->healthy = 0;
	}

	/* Prefer nested compression_executor fields when present. */
	if (extract_number_in_object(json, "compression_executor", "in_flight",
	                            &out->workers_in_flight) != 0)
		extract_number(json, "in_flight", &out->workers_in_flight);
	if (extract_number_in_object(json, "compression_executor", "queued",
	                            &out->workers_queued) != 0)
		extract_number(json, "queued", &out->workers_queued);

	return 0;
}
