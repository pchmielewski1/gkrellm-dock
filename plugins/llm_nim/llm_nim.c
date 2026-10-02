/*****************************************************************************
 * llm_nim - NIM/vLLM status for GKrellM
 *
 * Per Display bit: label|value strip + optional LINE chart
 * (X = time scrolling horizontally, Y = value).
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STYLE_NAME "llm_nim"
#define CONFIG_KEYWORD "llm_nim"
#define ROW_CHART_H 40
#define N_FEATS 27
#define DEFAULT_URL "http://127.0.0.1:8000"
#define DEFAULT_FEATURES 15u
#define DEFAULT_CHART_MAX_TPS 50
#define DEFAULT_CHART_MAX_PREFILL 1000
#define DEFAULT_TIMEOUT_MS 500
#define DEFAULT_DOCS_RELEASE "2.0.10"
#define HELPER_LOGIN_TIMEOUT_SEC 30
#define HELPER_CATALOG_TIMEOUT_SEC 20
#define HELPER_DOCS_SYNC_TIMEOUT_SEC 60
#define HELPER_PULL_TIMEOUT_SEC 45
#define HELPER_IMAGES_TIMEOUT_SEC 20
#define HELPER_PROFILES_TIMEOUT_SEC 120
#define HELPER_RECIPES_TIMEOUT_SEC 20
#define HELPER_INSTANCES_TIMEOUT_SEC 20
#define HELPER_START_TIMEOUT_SEC 60
#define HELPER_STOP_TIMEOUT_SEC 45
#define HELPER_OUT_MAX 65536
/* Prefill rate is NOT held: vllm:prompt_tokens_total jumps once when
 * prefill is accounted (often after multi-second work). Holding the rate
 * made Pre look active for several seconds while `in` correctly added only
 * that single Δ — reported as a summing bug. */
#define PREFILL_ADAPT_MIN 500U
#define PREFILL_ADAPT_MAX 100000U

#define FEAT_TPS (1u << 0)
#define FEAT_KV (1u << 1)
#define FEAT_QUEUE (1u << 2)
#define FEAT_PREFILL (1u << 3)
#define FEAT_TTFT (1u << 4)
#define FEAT_SPEC (1u << 5)
#define FEAT_PREFIX (1u << 6)
#define FEAT_ITL (1u << 7)
#define FEAT_E2E (1u << 8)
#define FEAT_TPOT (1u << 9)
#define FEAT_PREEMPT (1u << 10)
#define FEAT_REQ_RATE (1u << 11)
#define FEAT_RSS (1u << 12)
#define FEAT_Q_WAIT (1u << 13)
#define FEAT_PREFILL_T (1u << 14)
#define FEAT_DECODE_T (1u << 15)
#define FEAT_PROMPT_CACHE (1u << 16)
#define FEAT_HTTP_ERR (1u << 17)
#define FEAT_CPU (1u << 18)
#define FEAT_Q_REASON (1u << 19)
#define FEAT_ENGINE (1u << 20)
#define FEAT_MEAN_PROMPT (1u << 21)
#define FEAT_MEAN_GEN (1u << 22)
#define FEAT_ITER_TOK (1u << 23)
#define FEAT_EXT_PREFIX (1u << 24)
#define FEAT_HTTP_RATE (1u << 25)
#define FEAT_INFER_T (1u << 26)

#define MAX_DOCK_SLOTS 4
#define COMPACT_FEATURES (FEAT_TPS | FEAT_KV | FEAT_QUEUE | FEAT_ENGINE)

typedef enum {
	MK_TEXT = 0,
	MK_KRELL_PCT,   /* value already 0–100 */
	MK_KRELL_RATE,  /* value / full → krell */
	MK_KRELL_LAT    /* seconds / lat_full → krell */
} MetricKind;

typedef struct {
	guint bit;
	MetricKind kind;
	const char *tag;
	const char *cfg_label;
	double lat_full; /* MK_KRELL_LAT full-scale seconds */
} MetricDef;

typedef struct {
	double sum;
	double count;
} HistPair;

typedef struct {
	char model_short[48];
	double gen_tokens;
	double prompt_tokens;
	double prompt_cached;
	double kv_pct;
	double running;
	double waiting;
	double wait_capacity;
	double wait_deferred;
	double prefix_hits;
	double prefix_queries;
	double ext_prefix_hits;
	double ext_prefix_queries;
	double spec_accepted;
	double spec_draft;
	double preempt;
	double success;
	double rss_bytes;
	double cpu_seconds;
	double http_total;
	double http_5xx;
	double engine_awake;
	double engine_w1;
	double engine_w2;
	HistPair ttft, itl, tpot, e2e, q_wait, prefill_t, decode_t, infer_t;
	HistPair mean_prompt, mean_gen, iter_tok;
	double sglang_gen_tps;
	int backend_sglang;
	int backend_tensorfold;
	int ok;
} MetricsSnap;

/* Extra dock targets (slots 1..3); slot 0 mirrors P.base_url / display_name */
typedef struct {
	char name[64];
	char base_url[256];
	char display_name[64];
	int enabled;
	MetricsSnap last, prev;
	struct timespec prev_ts;
	int have_prev;
	double tps_decode, tps_prefill;
	GkrellmPanel *header;
	GkrellmDecal *title_decal;
	GkrellmDecal *lamp_decal;
	GkrellmPanel *info_panel;
	GkrellmDecal *info_decal;
	int ui_alive;
} DockSlot;

typedef struct {
	int alive;
	GkrellmPanel *panel;
	GkrellmDecal *label_decal;
	GkrellmDecal *value_decal;
	/* Horizontal history: X=time (scroll), Y=value — standard GKrellM chart */
	GkrellmChart *chart;
	GkrellmChartconfig *chart_cfg;
	GkrellmChartdata *cd;
} MetricRow;

typedef struct {
	GtkWidget *main_vbox;
	GkrellmMonitor *monitor;
	GkrellmPanel *header;
	GkrellmDecal *title_decal;
	GkrellmDecal *lamp_decal;
	GdkPixmap *lamp_pm;
	/* Session token totals: in=prefill, out=decode (each own strip) */
	GkrellmPanel *sess_in_panel;
	GkrellmDecal *sess_in_label;
	GkrellmDecal *sess_in_value;
	GkrellmPanel *sess_out_panel;
	GkrellmDecal *sess_out_label;
	GkrellmDecal *sess_out_value;
	MetricRow rows[N_FEATS];
	gint style_id;
	gint chart_style_id;

	char base_url[256];
	char display_name[64];
	char docs_release[32];
	guint features;
	guint chart_max_tps;
	guint chart_max_prefill;
	guint timeout_ms;
	int airgap;

	MetricsSnap last;
	MetricsSnap prev;
	struct timespec prev_ts;
	int have_prev;

	double tps_decode;
	double tps_prefill;
	guint prefill_scale_adapt;

	double d_ttft, d_itl, d_tpot, d_e2e, d_q_wait;
	double d_prefill_t, d_decode_t, d_infer_t;
	double d_mean_prompt, d_mean_gen, d_iter_tok;
	double d_prefix_pct, d_ext_prefix_pct, d_spec_pct, d_prompt_cache_pct;
	double d_req_rate, d_http_rate, d_http_err_pct, d_cpu_pct;

	char shown_model[48];
	int ui_built;

	/* Option B: tokens since this dock/plugin session (sum of scrape Δ) */
	int session_have_base;
	double session_prompt;
	double session_gen;

	/* slots[0] mirrors base_url / display_name; 1..3 are compact extras */
	DockSlot slots[MAX_DOCK_SLOTS];
	int dock_slots_hint;
} Plugin;

static Plugin P;

static GtkWidget *cfg_url_entry;
static GtkWidget *cfg_display_name_entry;
static GtkWidget *cfg_ngc_entry;
static GtkWidget *cfg_hf_entry;
static GtkWidget *cfg_ngc_status_label;
static GtkWidget *cfg_hf_status_label;
static GtkWidget *cfg_conn_link_label;
static GtkWidget *cfg_docs_release_entry;
static GtkWidget *cfg_docs_release_combo;
static GtkWidget *cfg_airgap_btn;
static GtkWidget *cfg_catalog_view;
static GtkWidget *cfg_catalog_combo;
static GtkWidget *cfg_catalog_tag_combo;
static GtkWidget *cfg_catalog_image_entry;
static GtkWidget *cfg_catalog_sync_label;
static GPtrArray *cfg_catalog_images; /* gchar* image refs parallel to combo */
static int cfg_catalog_busy;
static GtkWidget *cfg_local_view;
static GtkWidget *cfg_local_combo;
static GtkWidget *cfg_local_image_entry;
static GPtrArray *cfg_local_images; /* gchar* image refs parallel to local combo */
static GtkWidget *cfg_recipes_view;
static GtkWidget *cfg_recipe_combo;
static GtkWidget *cfg_recipe_name_entry;
static GtkWidget *cfg_recipe_profile_entry;
static GtkWidget *cfg_profile_combo;
static GPtrArray *cfg_profile_ids; /* gchar* profile ids parallel to profile combo */
static int cfg_recipes_busy; /* suppress combo change recursion */
static GtkWidget *cfg_instances_view;
static GtkWidget *cfg_instance_combo;
static GtkWidget *cfg_instance_name_entry;
static GPtrArray *cfg_instance_names; /* gchar* names parallel to instance combo */
static int cfg_instances_busy;
static guint cfg_logs_live_id; /* g_timeout source; 0 = off */
static char cfg_logs_live_name[160];
static GHashTable *cfg_logs_seen; /* exact log lines already shown (live mode) */
static int cfg_logs_live_primed; /* 0 = next fetch paints full buffer once */
static int cfg_logs_follow; /* 1 = follow running (independent of source id) */
static GtkWidget *cfg_adopt_name_entry;
static GtkWidget *cfg_orphan_name_entry;
static GtkWidget *cfg_spin_tps;
static GtkWidget *cfg_spin_prefill;
static GtkWidget *cfg_spin_timeout;
static GtkWidget *cfg_feat_btn[N_FEATS];

static int json_extract_string_after(const char *json, const char *after,
                                     const char *key, char *out, size_t out_sz);
static void format_docs_sync_dialog(const char *json, char *msg, size_t msg_sz);
static void cb_recipes_refresh(GtkWidget *button, gpointer data);
static void recipe_combo_select_name(const char *want);
static void recipes_load_named(const char *name);
static int profile_id_is_placeholder(const char *pid);
static void recipes_autoload_profiles_cached(const char *image);
static void instance_combo_select_name(const char *want);
static void cb_instances_refresh(GtkWidget *button, gpointer data);
static void cb_refresh_secrets_status(GtkWidget *button, gpointer data);
static void logs_live_stop(void);
static int instance_logs_fetch(const char *name, int quiet);

/* Dock order = bit index; Settings uses same defs */
static const MetricDef METRICS[N_FEATS] = {
	{ FEAT_TPS, MK_KRELL_RATE, "Dec", "Decode t/s chart (TX)", 0 },
	{ FEAT_KV, MK_KRELL_PCT, "KV", "KV cache %", 0 },
	{ FEAT_QUEUE, MK_TEXT, "Q", "Queue R/W", 0 },
	{ FEAT_PREFILL, MK_KRELL_RATE, "Pre", "Prefill t/s chart (RX)", 0 },
	{ FEAT_TTFT, MK_KRELL_LAT, "TFT", "TTFT (time to first token)", 5.0 },
	{ FEAT_SPEC, MK_KRELL_PCT, "Sp", "Spec-decode accept %", 0 },
	{ FEAT_PREFIX, MK_KRELL_PCT, "Px", "Prefix cache hit %", 0 },
	{ FEAT_ITL, MK_KRELL_LAT, "ITL", "ITL (inter-token latency)", 0.5 },
	{ FEAT_E2E, MK_KRELL_LAT, "E2E", "E2E request latency", 30.0 },
	{ FEAT_TPOT, MK_KRELL_LAT, "TP", "TPOT (time per output token)", 0.5 },
	{ FEAT_PREEMPT, MK_TEXT, "Pr", "Preemptions (total)", 0 },
	{ FEAT_REQ_RATE, MK_KRELL_RATE, "Req", "Successful request rate", 0 },
	{ FEAT_RSS, MK_TEXT, "RSS", "Process RSS", 0 },
	{ FEAT_Q_WAIT, MK_KRELL_LAT, "Qw", "Queue wait time", 5.0 },
	{ FEAT_PREFILL_T, MK_KRELL_LAT, "Pf", "Prefill phase time", 5.0 },
	{ FEAT_DECODE_T, MK_KRELL_LAT, "Dc", "Decode phase time", 30.0 },
	{ FEAT_PROMPT_CACHE, MK_KRELL_PCT, "Pc", "Prompt tokens cached %", 0 },
	{ FEAT_HTTP_ERR, MK_KRELL_PCT, "5xx", "HTTP 5xx %", 0 },
	{ FEAT_CPU, MK_KRELL_PCT, "CPU", "Process CPU %", 0 },
	{ FEAT_Q_REASON, MK_TEXT, "Wr", "Wait by reason (Wc/Wd)", 0 },
	{ FEAT_ENGINE, MK_TEXT, "Eng", "Engine status lamp (header)", 0 },
	{ FEAT_MEAN_PROMPT, MK_TEXT, "Pm", "Mean prompt tokens", 0 },
	{ FEAT_MEAN_GEN, MK_TEXT, "Gm", "Mean generation tokens", 0 },
	{ FEAT_ITER_TOK, MK_TEXT, "Bt", "Tokens per engine step", 0 },
	{ FEAT_EXT_PREFIX, MK_KRELL_PCT, "Xp", "External prefix hit %", 0 },
	{ FEAT_HTTP_RATE, MK_KRELL_RATE, "Htt", "HTTP request rate", 0 },
	{ FEAT_INFER_T, MK_KRELL_LAT, "Rn", "Inference phase time", 30.0 },
};

static void rebuild_llm_ui(void);
static void refresh_row_chart(gpointer data);
static void update_compact_slots(void);
static void sync_slot0_from_primary(void);

typedef struct {
	char *data;
	size_t len;
} CurlBuf;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	CurlBuf *b = (CurlBuf *)userdata;
	size_t n = size * nmemb;
	char *p = realloc(b->data, b->len + n + 1);
	if (!p)
		return 0;
	b->data = p;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
	return n;
}

static int http_get(const char *url, char **out_body)
{
	CURL *curl;
	CURLcode rc;
	CurlBuf buf = {0};
	long code = 0;
	long tmo = (long)(P.timeout_ms ? P.timeout_ms : DEFAULT_TIMEOUT_MS);

	*out_body = NULL;
	curl = curl_easy_init();
	if (!curl)
		return -1;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, tmo);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
	                 tmo > 200 ? 200L : tmo);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "gkrellm-llm_nim/1.2");
	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK || code != 200L || !buf.data) {
		free(buf.data);
		return -1;
	}
	*out_body = buf.data;
	return 0;
}

static void shorten_model(const char *full, char *out, size_t out_sz)
{
	const char *slash;
	const char *rest;
	char tmp[128];
	size_t n;

	if (!full || !*full) {
		snprintf(out, out_sz, "LLM");
		return;
	}
	slash = strrchr(full, '/');
	snprintf(tmp, sizeof(tmp), "%s", slash ? slash + 1 : full);
	/* nemotron-3-nano → Nemotron Nano; nemotron-3-super-… → Nemotron Super */
	if (strncmp(tmp, "nemotron", 8) == 0) {
		rest = tmp + 8;
		while (*rest == '-' || *rest == '_')
			++rest;
		while (*rest &&
		       (isdigit((unsigned char)*rest) || *rest == '-' || *rest == '_'))
			++rest;
		if (strncmp(rest, "nano", 4) == 0) {
			snprintf(out, out_sz, "Nemotron Nano");
			return;
		}
		if (strncmp(rest, "super", 5) == 0) {
			snprintf(out, out_sz, "Nemotron Super");
			return;
		}
		snprintf(out, out_sz, "Nemotron");
		return;
	}
	n = strcspn(tmp, "-_");
	if (n >= out_sz)
		n = out_sz - 1;
	memcpy(out, tmp, n);
	out[n] = '\0';
	if (out[0])
		out[0] = (char)toupper((unsigned char)out[0]);
}

static int metric_double(const char *body, const char *name, double *out)
{
	const char *p = body;
	size_t nlen = strlen(name);

	while ((p = strstr(p, name)) != NULL) {
		const char *q;
		if (p > body && p[-1] != '\n' && p[-1] != '\r') {
			p += nlen;
			continue;
		}
		q = p + nlen;
		if (*q == '{' || *q == ' ' || *q == '\t') {
			while (*q && *q != ' ' && *q != '\t' && *q != '\n')
				++q;
			while (*q == ' ' || *q == '\t')
				++q;
			if (sscanf(q, "%lf", out) == 1)
				return 0;
		}
		p += nlen;
	}
	return -1;
}

static int metric_sum_all(const char *body, const char *name, double *out)
{
	const char *p = body;
	size_t nlen = strlen(name);
	double total = 0;
	int found = 0;

	while ((p = strstr(p, name)) != NULL) {
		const char *q;
		double v;
		if (p > body && p[-1] != '\n' && p[-1] != '\r') {
			p += nlen;
			continue;
		}
		q = p + nlen;
		if (*q != '{' && *q != ' ' && *q != '\t') {
			p += nlen;
			continue;
		}
		while (*q && *q != ' ' && *q != '\t' && *q != '\n')
			++q;
		while (*q == ' ' || *q == '\t')
			++q;
		if (sscanf(q, "%lf", &v) == 1) {
			total += v;
			found = 1;
		}
		p += nlen;
	}
	if (!found)
		return -1;
	*out = total;
	return 0;
}

static int metric_sum_with(const char *body, const char *name, const char *needle,
                           double *out)
{
	const char *p = body;
	size_t nlen = strlen(name);
	double total = 0;
	int found = 0;

	while ((p = strstr(p, name)) != NULL) {
		const char *q;
		const char *endl;
		double v;
		if (p > body && p[-1] != '\n' && p[-1] != '\r') {
			p += nlen;
			continue;
		}
		q = p + nlen;
		if (*q != '{' && *q != ' ' && *q != '\t') {
			p += nlen;
			continue;
		}
		endl = strchr(q, '\n');
		if (!endl)
			endl = q + strlen(q);
		{
			size_t linelen = (size_t)(endl - p);
			char line[512];
			if (linelen >= sizeof(line))
				linelen = sizeof(line) - 1;
			memcpy(line, p, linelen);
			line[linelen] = '\0';
			if (needle && !strstr(line, needle)) {
				p += nlen;
				continue;
			}
			q = line + nlen;
			while (*q && *q != ' ' && *q != '\t')
				++q;
			while (*q == ' ' || *q == '\t')
				++q;
			if (sscanf(q, "%lf", &v) == 1) {
				total += v;
				found = 1;
			}
		}
		p += nlen;
	}
	if (!found)
		return -1;
	*out = total;
	return 0;
}

static int metric_model_label(const char *body, char *out, size_t out_sz)
{
	const char *p = strstr(body, "model_name=\"");
	const char *e;
	size_t n;

	if (!p)
		return -1;
	p += strlen("model_name=\"");
	e = strchr(p, '"');
	if (!e || e <= p)
		return -1;
	n = (size_t)(e - p);
	if (n >= out_sz)
		n = out_sz - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 0;
}

static int fetch_model_from_v1(const char *base_url, char *full, size_t full_sz)
{
	char url[320];
	char *body = NULL;
	const char *p, *e;
	size_t n;

	if (!base_url || !*base_url)
		return -1;
	snprintf(url, sizeof(url), "%s/v1/models", base_url);
	if (http_get(url, &body) != 0)
		return -1;
	p = strstr(body, "\"id\"");
	if (!p) {
		free(body);
		return -1;
	}
	p = strchr(p + 4, '"');
	if (!p) {
		free(body);
		return -1;
	}
	++p;
	e = strchr(p, '"');
	if (!e || e <= p) {
		free(body);
		return -1;
	}
	n = (size_t)(e - p);
	if (n >= full_sz)
		n = full_sz - 1;
	memcpy(full, p, n);
	full[n] = '\0';
	free(body);
	return 0;
}

static void hist_load(const char *body, const char *base, HistPair *h)
{
	char nsum[128], ncnt[128];

	h->sum = 0;
	h->count = 0;
	snprintf(nsum, sizeof(nsum), "%s_sum", base);
	snprintf(ncnt, sizeof(ncnt), "%s_count", base);
	metric_double(body, nsum, &h->sum);
	metric_double(body, ncnt, &h->count);
}

/* Sum _sum/_count across all Prometheus label variants on one line each. */
static void hist_load_all(const char *body, const char *base, HistPair *h)
{
	char nsum[128], ncnt[128];

	h->sum = 0;
	h->count = 0;
	snprintf(nsum, sizeof(nsum), "%s_sum", base);
	snprintf(ncnt, sizeof(ncnt), "%s_count", base);
	if (metric_sum_all(body, nsum, &h->sum) != 0)
		metric_double(body, nsum, &h->sum);
	if (metric_sum_all(body, ncnt, &h->count) != 0)
		metric_double(body, ncnt, &h->count);
}

static void hist_load_labeled(const char *body, const char *base,
                              const char *needle, HistPair *h)
{
	char nsum[128], ncnt[128];

	h->sum = 0;
	h->count = 0;
	snprintf(nsum, sizeof(nsum), "%s_sum", base);
	snprintf(ncnt, sizeof(ncnt), "%s_count", base);
	metric_sum_with(body, nsum, needle, &h->sum);
	metric_sum_with(body, ncnt, needle, &h->count);
}

static int metrics_body_has_sglang(const char *body)
{
	if (!body)
		return 0;
	return strstr(body, "sglang:gen_throughput") != NULL ||
	       strstr(body, "sglang:token_usage") != NULL ||
	       strstr(body, "sglang:prompt_tokens_total") != NULL;
}

/* Average every sample of a Prometheus gauge (handles labeled streams). */
static int metric_mean_all(const char *body, const char *name, double *out)
{
	const char *p = body;
	size_t nlen = strlen(name);
	double total = 0;
	int n = 0;

	if (!body || !name || !out)
		return -1;
	while ((p = strstr(p, name)) != NULL) {
		const char *q;
		double v;

		if (p > body && p[-1] != '\n' && p[-1] != '\r') {
			p += nlen;
			continue;
		}
		q = p + nlen;
		if (*q != '{' && *q != ' ' && *q != '\t') {
			p += nlen;
			continue;
		}
		while (*q && *q != ' ' && *q != '\t' && *q != '\n')
			++q;
		while (*q == ' ' || *q == '\t')
			++q;
		if (sscanf(q, "%lf", &v) == 1) {
			total += v;
			++n;
		}
		p += nlen;
	}
	if (n <= 0)
		return -1;
	*out = total / (double)n;
	return 0;
}

static int metrics_body_has_tensorfold(const char *body)
{
	if (!body)
		return 0;
	return strstr(body, "tensorfold:") != NULL;
}

/* Simple JSON number extract: "key": <number> (first match). */
static int json_extract_number(const char *json, const char *key, double *out)
{
	char needle[96];
	const char *p;
	size_t key_len;

	if (!json || !key || !out)
		return -1;
	key_len = strlen(key);
	if (key_len + 4 >= sizeof(needle))
		return -1;
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return -1;
	p += strlen(needle);
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (*p != ':')
		return -1;
	p++;
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (sscanf(p, "%lf", out) == 1)
		return 0;
	return -1;
}

/*
 * TensorFold /metrics token counters are finished-request only (HELP text).
 * Live Dec/Pre need /health: completion_tokens_total grows during decode.
 */
static void overlay_tensorfold_health(const char *base_url, MetricsSnap *s)
{
	char url[320];
	char *body = NULL;
	double v;

	if (!base_url || !*base_url || !s)
		return;
	snprintf(url, sizeof(url), "%s/health", base_url);
	if (http_get(url, &body) != 0 || !body)
		return;

	if (json_extract_number(body, "completion_tokens_total", &v) == 0)
		s->gen_tokens = v;
	if (json_extract_number(body, "prompt_tokens_total", &v) == 0)
		s->prompt_tokens = v;
	if (json_extract_number(body, "cached_tokens_total", &v) == 0)
		s->prompt_cached = v;
	if (json_extract_number(body, "requests_running", &v) == 0)
		s->running = v;
	if (json_extract_number(body, "accepted_total", &v) == 0)
		s->spec_accepted = v;
	if (json_extract_number(body, "drafted_total", &v) == 0)
		s->spec_draft = v;
	/* prefill/decode_seconds_total stay finished-only on current TF — skip. */

	free(body);
}

/*
 * TensorFold (MiaAI Flash-Next recipe, TF >= 0.6.1): /metrics is Prometheus
 * text with a tensorfold: prefix. vLLM-shaped mirrors keep that prefix too
 * (tensorfold:generation_tokens_total, …), so the vLLM scrape path above
 * stays empty and we overlay here — same pattern as SGLang.
 */
static void fetch_tensorfold_metrics(const char *base_url, const char *body,
                                     MetricsSnap *s)
{
	double v;

	s->backend_tensorfold = 1;
	s->backend_sglang = 0;

	metric_double(body, "tensorfold:generation_tokens_total", &s->gen_tokens);
	metric_double(body, "tensorfold:prompt_tokens_total", &s->prompt_tokens);
	metric_double(body, "tensorfold:num_requests_running", &s->running);
	if (s->running == 0.0)
		metric_double(body, "tensorfold:requests_running", &s->running);
	metric_double(body, "tensorfold:num_requests_waiting", &s->waiting);
	if (s->waiting == 0.0)
		metric_double(body, "tensorfold:requests_waiting", &s->waiting);

	/* Named *_perc but values are 0–1 occupancy ratios per stream. */
	if (metric_mean_all(body, "tensorfold:kv_cache_usage_perc", &v) == 0 ||
	    metric_mean_all(body, "tensorfold:kv_cache_usage_ratio", &v) == 0) {
		s->kv_pct = (v <= 1.0) ? (v * 100.0) : v;
	}

	if (metric_double(body, "tensorfold:spec_decode_num_accepted_tokens_total",
	                  &s->spec_accepted) != 0)
		metric_double(body, "tensorfold:mtp_accepted_total",
		              &s->spec_accepted);
	if (metric_double(body, "tensorfold:spec_decode_num_draft_tokens_total",
	                  &s->spec_draft) != 0)
		metric_double(body, "tensorfold:mtp_drafted_total",
		              &s->spec_draft);

	metric_double(body, "tensorfold:preemptions_total", &s->preempt);

	hist_load(body, "tensorfold:time_to_first_token_seconds", &s->ttft);
	hist_load(body, "tensorfold:e2e_request_latency_seconds", &s->e2e);
	if (s->e2e.count <= 0.0)
		hist_load(body, "tensorfold:request_latency_seconds", &s->e2e);

	s->engine_awake = 1.0;

	/* Live token counters — /metrics only updates on finished requests. */
	overlay_tensorfold_health(base_url, s);
}

static void fetch_sglang_metrics(const char *body, MetricsSnap *s)
{
	double v, mem_gb = 0;

	s->backend_sglang = 1;

	metric_double(body, "sglang:gen_throughput", &s->sglang_gen_tps);
	metric_sum_all(body, "sglang:prompt_tokens_total", &s->prompt_tokens);
	metric_sum_all(body, "sglang:generation_tokens_total", &s->gen_tokens);
	metric_sum_all(body, "sglang:cached_tokens_total", &s->prompt_cached);

	if (metric_double(body, "sglang:token_usage", &v) == 0)
		s->kv_pct = v * 100.0;
	metric_double(body, "sglang:num_running_reqs", &s->running);
	metric_double(body, "sglang:num_queue_reqs", &s->waiting);

	if (metric_double(body, "sglang:num_prefill_bootstrap_queue_reqs", &v) == 0)
		s->wait_capacity += v;
	if (metric_double(body, "sglang:num_prefill_inflight_queue_reqs", &v) == 0)
		s->wait_capacity += v;
	if (metric_double(body, "sglang:num_decode_prealloc_queue_reqs", &v) == 0)
		s->wait_deferred += v;
	if (metric_double(body, "sglang:num_decode_transfer_queue_reqs", &v) == 0)
		s->wait_deferred += v;

	if (metric_double(body, "sglang:cache_hit_rate", &v) == 0) {
		s->prefix_hits = v;
		s->prefix_queries = 1.0;
	}
	if (metric_double(body, "sglang:spec_accept_rate", &v) == 0) {
		s->spec_accepted = v;
		s->spec_draft = 1.0;
	}

	metric_sum_all(body, "sglang:num_requests_total", &s->success);
	metric_sum_all(body, "sglang:num_aborted_requests_total", &s->preempt);
	metric_sum_all(body, "sglang:http_requests_total", &s->http_total);
	metric_sum_with(body, "sglang:http_responses_total", "status_code=\"5",
	                &s->http_5xx);
	metric_sum_all(body, "sglang:process_cpu_seconds_total", &s->cpu_seconds);

	if (metric_double(body, "sglang:weight_memory_usage_gb", &v) == 0)
		mem_gb += v;
	if (metric_double(body, "sglang:kv_cache_memory_usage_gb", &v) == 0)
		mem_gb += v;
	if (metric_sum_all(body, "sglang:graph_memory_usage_gb", &v) == 0)
		mem_gb += v;
	if (mem_gb > 0.0)
		s->rss_bytes = mem_gb * 1024.0 * 1024.0 * 1024.0;

	hist_load_all(body, "sglang:time_to_first_token_seconds", &s->ttft);
	hist_load_all(body, "sglang:inter_token_latency_seconds", &s->itl);
	hist_load_all(body, "sglang:inter_token_latency_seconds", &s->tpot);
	hist_load_all(body, "sglang:e2e_request_latency_seconds", &s->e2e);
	hist_load_all(body, "sglang:queue_time_seconds", &s->q_wait);
	hist_load_labeled(body, "sglang:per_stage_req_latency_seconds",
	                  "stage=\"prefill_forward\"", &s->prefill_t);

	hist_load_all(body, "sglang:prompt_tokens_histogram", &s->mean_prompt);
	hist_load_all(body, "sglang:generation_tokens_histogram", &s->mean_gen);

	s->engine_awake = 1.0;
}

static double hist_window_mean(const HistPair *cur, const HistPair *prev)
{
	double ds = cur->sum - prev->sum;
	double dc = cur->count - prev->count;

	/* Only recent window — lifetime mean invents huge stale latencies */
	if (dc > 0.0 && ds >= 0.0)
		return ds / dc;
	return -1.0;
}

static double ratio_pct(double num, double den, double prev_num, double prev_den)
{
	double dn = num - prev_num;
	double dd = den - prev_den;

	if (dd > 0.0 && dn >= 0.0)
		return 100.0 * dn / dd;
	if (den > 0.0)
		return 100.0 * num / den;
	return -1.0;
}

static int fetch_metrics(const char *base_url, MetricsSnap *s)
{
	char url[320];
	char *body = NULL;
	char full[128];

	memset(s, 0, sizeof(*s));
	if (!base_url || !*base_url)
		return -1;
	snprintf(s->model_short, sizeof(s->model_short), "LLM");

	snprintf(url, sizeof(url), "%s/metrics", base_url);
	if (http_get(url, &body) != 0)
		return -1;

	if (metric_model_label(body, full, sizeof(full)) == 0 ||
	    fetch_model_from_v1(base_url, full, sizeof(full)) == 0)
		shorten_model(full, s->model_short, sizeof(s->model_short));

	metric_double(body, "vllm:generation_tokens_total", &s->gen_tokens);
	if (metric_double(body, "vllm:prompt_tokens_total", &s->prompt_tokens) != 0)
		metric_double(body, "vllm:prompt_tokens_by_source_total",
		              &s->prompt_tokens);
	metric_double(body, "vllm:prompt_tokens_cached_total", &s->prompt_cached);
	if (metric_double(body, "vllm:kv_cache_usage_perc", &s->kv_pct) == 0)
		s->kv_pct *= 100.0;
	metric_double(body, "vllm:num_requests_running", &s->running);
	metric_double(body, "vllm:num_requests_waiting", &s->waiting);
	metric_sum_with(body, "vllm:num_requests_waiting_by_reason",
	                "reason=\"capacity\"", &s->wait_capacity);
	metric_sum_with(body, "vllm:num_requests_waiting_by_reason",
	                "reason=\"deferred\"", &s->wait_deferred);
	metric_double(body, "vllm:prefix_cache_hits_total", &s->prefix_hits);
	metric_double(body, "vllm:prefix_cache_queries_total", &s->prefix_queries);
	metric_double(body, "vllm:external_prefix_cache_hits_total",
	              &s->ext_prefix_hits);
	metric_double(body, "vllm:external_prefix_cache_queries_total",
	              &s->ext_prefix_queries);
	metric_double(body, "vllm:spec_decode_num_accepted_tokens_total",
	              &s->spec_accepted);
	metric_double(body, "vllm:spec_decode_num_draft_tokens_total",
	              &s->spec_draft);
	metric_double(body, "vllm:num_preemptions_total", &s->preempt);
	metric_sum_all(body, "vllm:request_success_total", &s->success);
	metric_double(body, "process_resident_memory_bytes", &s->rss_bytes);
	metric_double(body, "process_cpu_seconds_total", &s->cpu_seconds);
	metric_sum_all(body, "http_requests_total", &s->http_total);
	metric_sum_with(body, "http_requests_total", "status=\"5", &s->http_5xx);
	metric_sum_with(body, "vllm:engine_sleep_state", "sleep_state=\"awake\"",
	                &s->engine_awake);
	metric_sum_with(body, "vllm:engine_sleep_state",
	                "sleep_state=\"weights_offloaded\"", &s->engine_w1);
	metric_sum_with(body, "vllm:engine_sleep_state",
	                "sleep_state=\"discard_all\"", &s->engine_w2);
	hist_load(body, "vllm:time_to_first_token_seconds", &s->ttft);
	hist_load(body, "vllm:inter_token_latency_seconds", &s->itl);
	hist_load(body, "vllm:request_time_per_output_token_seconds", &s->tpot);
	hist_load(body, "vllm:e2e_request_latency_seconds", &s->e2e);
	hist_load(body, "vllm:request_queue_time_seconds", &s->q_wait);
	hist_load(body, "vllm:request_prefill_time_seconds", &s->prefill_t);
	hist_load(body, "vllm:request_decode_time_seconds", &s->decode_t);
	hist_load(body, "vllm:request_inference_time_seconds", &s->infer_t);
	hist_load(body, "vllm:request_prompt_tokens", &s->mean_prompt);
	hist_load(body, "vllm:request_generation_tokens", &s->mean_gen);
	hist_load(body, "vllm:iteration_tokens_total", &s->iter_tok);

	/* Auto-detect SGLang (/metrics uses sglang:*). vLLM path above stays intact. */
	if (metrics_body_has_sglang(body))
		fetch_sglang_metrics(body, s);
	/* TensorFold (/metrics uses tensorfold:* including vLLM-shaped mirrors). */
	else if (metrics_body_has_tensorfold(body))
		fetch_tensorfold_metrics(base_url, body, s);

	s->ok = 1;
	free(body);
	return 0;
}

static double decode_tps_from_snap(const MetricsSnap *snap,
                                   const MetricsSnap *prev, double dt)
{
	if (snap->backend_sglang && snap->sglang_gen_tps >= 0.0)
		return snap->sglang_gen_tps;
	if (prev && snap->gen_tokens >= prev->gen_tokens && dt > 0.0)
		return (snap->gen_tokens - prev->gen_tokens) / dt;
	return 0.0;
}

static int scrape_metrics(MetricsSnap *s)
{
	if (fetch_metrics(P.base_url, s) != 0)
		return -1;
	if (P.shown_model[0] && !strcmp(s->model_short, "LLM"))
		snprintf(s->model_short, sizeof(s->model_short), "%s",
		         P.shown_model);
	return 0;
}

static double timespec_delta(const struct timespec *a, const struct timespec *b)
{
	return (double)(a->tv_sec - b->tv_sec) +
	       (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static void derive_from_snap(const MetricsSnap *snap, double dt)
{
	long ncpu;

	P.d_ttft = hist_window_mean(&snap->ttft, &P.prev.ttft);
	P.d_itl = hist_window_mean(&snap->itl, &P.prev.itl);
	P.d_tpot = hist_window_mean(&snap->tpot, &P.prev.tpot);
	P.d_e2e = hist_window_mean(&snap->e2e, &P.prev.e2e);
	P.d_q_wait = hist_window_mean(&snap->q_wait, &P.prev.q_wait);
	P.d_prefill_t = hist_window_mean(&snap->prefill_t, &P.prev.prefill_t);
	P.d_decode_t = hist_window_mean(&snap->decode_t, &P.prev.decode_t);
	P.d_infer_t = hist_window_mean(&snap->infer_t, &P.prev.infer_t);
	P.d_mean_prompt = hist_window_mean(&snap->mean_prompt, &P.prev.mean_prompt);
	P.d_mean_gen = hist_window_mean(&snap->mean_gen, &P.prev.mean_gen);
	P.d_iter_tok = hist_window_mean(&snap->iter_tok, &P.prev.iter_tok);
	P.d_prefix_pct = ratio_pct(snap->prefix_hits, snap->prefix_queries,
	                           P.prev.prefix_hits, P.prev.prefix_queries);
	P.d_ext_prefix_pct =
	    ratio_pct(snap->ext_prefix_hits, snap->ext_prefix_queries,
	              P.prev.ext_prefix_hits, P.prev.ext_prefix_queries);
	P.d_spec_pct = ratio_pct(snap->spec_accepted, snap->spec_draft,
	                         P.prev.spec_accepted, P.prev.spec_draft);
	P.d_prompt_cache_pct =
	    ratio_pct(snap->prompt_cached, snap->prompt_tokens,
	              P.prev.prompt_cached, P.prev.prompt_tokens);
	if (dt > 0 && snap->success >= P.prev.success)
		P.d_req_rate = (snap->success - P.prev.success) / dt;
	else
		P.d_req_rate = -1.0;
	if (dt > 0 && snap->http_total >= P.prev.http_total)
		P.d_http_rate = (snap->http_total - P.prev.http_total) / dt;
	else
		P.d_http_rate = -1.0;
	P.d_http_err_pct = ratio_pct(snap->http_5xx, snap->http_total,
	                             P.prev.http_5xx, P.prev.http_total);
	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1)
		ncpu = 1;
	if (dt > 0 && snap->cpu_seconds >= P.prev.cpu_seconds)
		P.d_cpu_pct = 100.0 * (snap->cpu_seconds - P.prev.cpu_seconds) /
		              dt / (double)ncpu;
	else
		P.d_cpu_pct = -1.0;
}

enum {
	LAMP_AWAKE = 0,
	LAMP_W1,
	LAMP_W2,
	LAMP_DOWN,
	LAMP_FRAMES
};

#define LAMP_SZ 9

static GdkPixmap *make_lamp_pixmap(void)
{
	GdkPixmap *pm;
	GdkGC *gc;
	GdkColormap *cmap;
	GdkColor bg, c[LAMP_FRAMES];
	int i, sz = LAMP_SZ;

	pm = gdk_pixmap_new(NULL, sz, sz * LAMP_FRAMES,
	                    gdk_visual_get_system()->depth);
	gc = gdk_gc_new(pm);
	cmap = gdk_colormap_get_system();

	gdk_color_parse("#152033", &bg);
	gdk_colormap_alloc_color(cmap, &bg, FALSE, TRUE);
	gdk_color_parse("#3ddc84", &c[LAMP_AWAKE]); /* awake */
	gdk_color_parse("#f0c040", &c[LAMP_W1]);    /* weights_offloaded */
	gdk_color_parse("#ff5555", &c[LAMP_W2]);    /* discard_all */
	gdk_color_parse("#6a7180", &c[LAMP_DOWN]);  /* down / unknown */
	for (i = 0; i < LAMP_FRAMES; ++i)
		gdk_colormap_alloc_color(cmap, &c[i], FALSE, TRUE);

	for (i = 0; i < LAMP_FRAMES; ++i) {
		gdk_gc_set_foreground(gc, &bg);
		gdk_draw_rectangle(pm, gc, TRUE, 0, i * sz, sz, sz);
		gdk_gc_set_foreground(gc, &c[i]);
		gdk_draw_arc(pm, gc, TRUE, 1, i * sz + 1, sz - 3, sz - 3, 0,
		             360 * 64);
	}
	g_object_unref(gc);
	return pm;
}

static int engine_lamp_frame_snap(const MetricsSnap *snap)
{
	if (!snap || !snap->ok)
		return LAMP_DOWN;
	if (snap->engine_w2 >= 0.5)
		return LAMP_W2;
	if (snap->engine_w1 >= 0.5)
		return LAMP_W1;
	if (snap->engine_awake >= 0.5)
		return LAMP_AWAKE;
	return LAMP_DOWN;
}

static int engine_lamp_frame(void)
{
	return engine_lamp_frame_snap(&P.last);
}

static void fmt_tok_n(char *out, size_t n, double v)
{
	if (v < 0)
		v = 0;
	/* Compact for large session totals (many millions+) */
	if (v >= 1e9)
		snprintf(out, n, "%.2fB", v / 1e9);
	else if (v >= 100e6)
		snprintf(out, n, "%.0fM", v / 1e6);
	else if (v >= 1e6)
		snprintf(out, n, "%.1fM", v / 1e6);
	else if (v >= 1000)
		snprintf(out, n, "%.1fk", v / 1000.0);
	else
		snprintf(out, n, "%.0f", v);
}

static void update_session_totals(const MetricsSnap *snap)
{
	if (!snap || !snap->ok)
		return;
	if (!P.session_have_base) {
		P.session_have_base = 1;
		P.session_prompt = 0;
		P.session_gen = 0;
		return;
	}
	/* Sum each scrape Δ (same source as Pre/Dec t/s). Skip regress ticks. */
	if (P.have_prev) {
		if (snap->prompt_tokens >= P.prev.prompt_tokens)
			P.session_prompt +=
			    snap->prompt_tokens - P.prev.prompt_tokens;
		if (snap->gen_tokens >= P.prev.gen_tokens)
			P.session_gen += snap->gen_tokens - P.prev.gen_tokens;
	}
}

static void fmt_lat(char *out, size_t n, double sec)
{
	if (sec < 0) {
		snprintf(out, n, "-");
		return;
	}
	if (sec < 1.0)
		snprintf(out, n, "%.0fms", sec * 1000.0);
	else
		snprintf(out, n, "%.1fs", sec);
}

/* Fill value text + numeric for krell/mini (raw units depend on kind). */
static void metric_value(int idx, char *buf, size_t buflen, double *raw,
                         double *full)
{
	const MetricDef *d = &METRICS[idx];
	*raw = 0;
	*full = 100;

	if (!P.last.ok) {
		snprintf(buf, buflen, "down");
		return;
	}

	switch (d->bit) {
	case FEAT_TPS:
		*raw = P.tps_decode;
		*full = P.chart_max_tps ? P.chart_max_tps : DEFAULT_CHART_MAX_TPS;
		snprintf(buf, buflen, "%.0f/s", P.tps_decode);
		break;
	case FEAT_PREFILL:
		*raw = P.tps_prefill;
		*full = P.prefill_scale_adapt ? P.prefill_scale_adapt
		                             : (P.chart_max_prefill
		                                    ? P.chart_max_prefill
		                                    : DEFAULT_CHART_MAX_PREFILL);
		snprintf(buf, buflen, "%.0f/s", P.tps_prefill);
		break;
	case FEAT_KV:
		*raw = P.last.kv_pct;
		snprintf(buf, buflen, "%.0f%%", P.last.kv_pct);
		break;
	case FEAT_QUEUE:
		snprintf(buf, buflen, "R%.0f/W%.0f", P.last.running, P.last.waiting);
		break;
	case FEAT_Q_REASON:
		snprintf(buf, buflen, "Wc%.0f/Wd%.0f", P.last.wait_capacity,
		         P.last.wait_deferred);
		break;
	case FEAT_TTFT:
		*raw = P.d_ttft;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_ttft);
		if (P.d_ttft < 0)
			*raw = -1;
		break;
	case FEAT_ITL:
		*raw = P.d_itl;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_itl);
		if (P.d_itl < 0)
			*raw = -1;
		break;
	case FEAT_TPOT:
		*raw = P.d_tpot;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_tpot);
		if (P.d_tpot < 0)
			*raw = -1;
		break;
	case FEAT_E2E:
		*raw = P.d_e2e;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_e2e);
		if (P.d_e2e < 0)
			*raw = -1;
		break;
	case FEAT_Q_WAIT:
		*raw = P.d_q_wait;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_q_wait);
		if (P.d_q_wait < 0)
			*raw = -1;
		break;
	case FEAT_PREFILL_T:
		*raw = P.d_prefill_t;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_prefill_t);
		if (P.d_prefill_t < 0)
			*raw = -1;
		break;
	case FEAT_DECODE_T:
		*raw = P.d_decode_t;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_decode_t);
		if (P.d_decode_t < 0)
			*raw = -1;
		break;
	case FEAT_INFER_T:
		*raw = P.d_infer_t;
		*full = d->lat_full;
		fmt_lat(buf, buflen, P.d_infer_t);
		if (P.d_infer_t < 0)
			*raw = -1;
		break;
	case FEAT_PREFIX:
		*raw = P.d_prefix_pct >= 0 ? P.d_prefix_pct : 0;
		if (P.d_prefix_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_prefix_pct);
		break;
	case FEAT_EXT_PREFIX:
		*raw = P.d_ext_prefix_pct >= 0 ? P.d_ext_prefix_pct : 0;
		if (P.d_ext_prefix_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_ext_prefix_pct);
		break;
	case FEAT_SPEC:
		*raw = P.d_spec_pct >= 0 ? P.d_spec_pct : 0;
		if (P.d_spec_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_spec_pct);
		break;
	case FEAT_PROMPT_CACHE:
		*raw = P.d_prompt_cache_pct >= 0 ? P.d_prompt_cache_pct : 0;
		if (P.d_prompt_cache_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_prompt_cache_pct);
		break;
	case FEAT_HTTP_ERR:
		*raw = P.d_http_err_pct >= 0 ? P.d_http_err_pct : 0;
		if (P.d_http_err_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_http_err_pct);
		break;
	case FEAT_CPU:
		*raw = P.d_cpu_pct >= 0 ? P.d_cpu_pct : 0;
		if (P.d_cpu_pct < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f%%", P.d_cpu_pct);
		break;
	case FEAT_PREEMPT:
		snprintf(buf, buflen, "%.0f", P.last.preempt);
		break;
	case FEAT_REQ_RATE:
		*raw = P.d_req_rate >= 0 ? P.d_req_rate : 0;
		*full = 10.0;
		if (P.d_req_rate < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.1f/s", P.d_req_rate);
		break;
	case FEAT_HTTP_RATE:
		*raw = P.d_http_rate >= 0 ? P.d_http_rate : 0;
		*full = 50.0;
		if (P.d_http_rate < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f/s", P.d_http_rate);
		break;
	case FEAT_RSS:
		snprintf(buf, buflen, "%.0fG",
		         P.last.rss_bytes / (1024.0 * 1024.0 * 1024.0));
		break;
	case FEAT_ENGINE:
		if (P.last.engine_w2 >= 0.5)
			snprintf(buf, buflen, "slp2");
		else if (P.last.engine_w1 >= 0.5)
			snprintf(buf, buflen, "slp1");
		else if (P.last.engine_awake >= 0.5)
			snprintf(buf, buflen, "wake");
		else
			snprintf(buf, buflen, "zzz");
		break;
	case FEAT_MEAN_PROMPT:
		if (P.d_mean_prompt < 0)
			snprintf(buf, buflen, "-");
		else if (P.d_mean_prompt >= 1000.0)
			snprintf(buf, buflen, "%.1fk", P.d_mean_prompt / 1000.0);
		else
			snprintf(buf, buflen, "%.0f", P.d_mean_prompt);
		break;
	case FEAT_MEAN_GEN:
		if (P.d_mean_gen < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f", P.d_mean_gen);
		break;
	case FEAT_ITER_TOK:
		if (P.d_iter_tok < 0)
			snprintf(buf, buflen, "-");
		else
			snprintf(buf, buflen, "%.0f", P.d_iter_tok);
		break;
	default:
		snprintf(buf, buflen, "?");
		break;
	}
}

static gulong krell_units(const MetricDef *d, double raw, double full)
{
	gulong u;

	if (raw < 0)
		return 0;
	if (d->kind == MK_KRELL_PCT) {
		u = (gulong)(raw + 0.5);
		if (u > 100)
			u = 100;
		return u;
	}
	if (full <= 0)
		return 0;
	u = (gulong)(raw * 100.0 / full + 0.5);
	if (u > 100)
		u = 100;
	return u;
}

static void refresh_row_chart(gpointer data)
{
	int idx = GPOINTER_TO_INT(data);
	MetricRow *r;
	char buf[40];
	double raw, full;

	if (idx < 0 || idx >= N_FEATS)
		return;
	r = &P.rows[idx];
	if (!r->chart)
		return;
	gkrellm_draw_chartdata(r->chart);
	metric_value(idx, buf, sizeof(buf), &raw, &full);
	gkrellm_draw_chart_text(r->chart, P.chart_style_id, buf);
	gkrellm_draw_chart_to_screen(r->chart);
}

static gint any_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	int i;
	GdkPixmap *pixmap = NULL;

	if (P.header && P.header->drawing_area == widget)
		pixmap = P.header->pixmap;
	else if (P.sess_in_panel && P.sess_in_panel->drawing_area == widget)
		pixmap = P.sess_in_panel->pixmap;
	else if (P.sess_out_panel && P.sess_out_panel->drawing_area == widget)
		pixmap = P.sess_out_panel->pixmap;
	for (i = 0; !pixmap && i < N_FEATS; ++i) {
		MetricRow *r = &P.rows[i];
		if (!r->alive)
			continue;
		if (r->panel && r->panel->drawing_area == widget)
			pixmap = r->panel->pixmap;
		else if (r->chart && r->chart->drawing_area == widget)
			pixmap = r->chart->pixmap;
	}
	if (pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                pixmap, ev->area.x, ev->area.y, ev->area.x,
		                ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

static void destroy_compact_slot_ui(DockSlot *s)
{
	if (!s)
		return;
	if (s->info_panel) {
		gkrellm_panel_destroy(s->info_panel);
		s->info_panel = NULL;
		s->info_decal = NULL;
	}
	if (s->header) {
		gkrellm_panel_destroy(s->header);
		s->header = NULL;
		s->title_decal = NULL;
		s->lamp_decal = NULL;
	}
	s->ui_alive = 0;
}

static void destroy_ui_widgets(void)
{
	int i;

	for (i = 1; i < MAX_DOCK_SLOTS; ++i)
		destroy_compact_slot_ui(&P.slots[i]);

	for (i = 0; i < N_FEATS; ++i) {
		MetricRow *r = &P.rows[i];
		if (r->chart) {
			gkrellm_chart_destroy(r->chart);
			r->chart = NULL;
			r->chart_cfg = NULL;
			r->cd = NULL;
		}
		if (r->panel) {
			gkrellm_panel_destroy(r->panel);
			r->panel = NULL;
		}
		r->label_decal = NULL;
		r->value_decal = NULL;
		r->alive = 0;
	}
	if (P.sess_in_panel) {
		gkrellm_panel_destroy(P.sess_in_panel);
		P.sess_in_panel = NULL;
		P.sess_in_label = P.sess_in_value = NULL;
	}
	if (P.sess_out_panel) {
		gkrellm_panel_destroy(P.sess_out_panel);
		P.sess_out_panel = NULL;
		P.sess_out_label = P.sess_out_value = NULL;
	}
	if (P.header) {
		gkrellm_panel_destroy(P.header);
		P.header = NULL;
		P.title_decal = NULL;
		P.lamp_decal = NULL;
	}
	P.ui_built = 0;
}

static void create_session_row(GkrellmPanel **panel, GkrellmDecal **label,
                               GkrellmDecal **value, const char *tag)
{
	GkrellmStyle *style;
	GkrellmTextstyle *ts;

	*panel = gkrellm_panel_new0();
	style = gkrellm_meter_style(P.style_id);
	ts = gkrellm_meter_textstyle(P.style_id);
	*label = gkrellm_create_decal_text(*panel, (gchar *)tag, ts, style, -1,
	                                  -1, -1);
	*value = gkrellm_create_decal_text(*panel, "999.99B", ts, style, -1,
	                                  (*label)->y, -1);
	gkrellm_panel_configure(*panel, NULL, style);
	gkrellm_panel_create(P.main_vbox, P.monitor, *panel);
	g_signal_connect(G_OBJECT((*panel)->drawing_area), "expose_event",
	                 G_CALLBACK(any_expose), NULL);
}

static void create_metric_row(int idx)
{
	const MetricDef *d = &METRICS[idx];
	MetricRow *r = &P.rows[idx];
	GkrellmStyle *style;
	GkrellmTextstyle *ts;
	GkrellmPanel *p;
	GkrellmChart *cp;

	r->panel = gkrellm_panel_new0();
	p = r->panel;
	style = gkrellm_meter_style(P.style_id);
	ts = gkrellm_meter_textstyle(P.style_id);

	r->label_decal =
	    gkrellm_create_decal_text(p, (gchar *)d->tag, ts, style, -1, -1, -1);
	r->value_decal =
	    gkrellm_create_decal_text(p, "9999.9ms", ts, style, -1,
	                              r->label_decal->y, -1);
	gkrellm_panel_configure(p, NULL, style);
	gkrellm_panel_create(P.main_vbox, P.monitor, p);
	g_signal_connect(G_OBJECT(p->drawing_area), "expose_event",
	                 G_CALLBACK(any_expose), NULL);

	/* Non-text: horizontal time history (X=time scroll, Y=value) */
	if (d->kind != MK_TEXT) {
		r->chart = gkrellm_chart_new0();
		r->chart->panel = NULL;
		cp = r->chart;
		gkrellm_set_chart_height_default(cp, ROW_CHART_H);
		gkrellm_chart_create(P.main_vbox, P.monitor, cp, &r->chart_cfg);
		gkrellm_set_draw_chart_function(cp, refresh_row_chart,
		                                GINT_TO_POINTER(idx));
		r->cd = gkrellm_add_default_chartdata(cp, (gchar *)d->tag);
		gkrellm_monotonic_chartdata(r->cd, FALSE);
		gkrellm_set_chartdata_draw_style_default(r->cd, CHARTDATA_LINE);
		gkrellm_set_chartdata_draw_style(r->cd, CHARTDATA_LINE);
		gkrellm_set_chartdata_flags(r->cd, CHARTDATA_NO_CONFIG);
		gkrellm_set_chartconfig_auto_grid_resolution(cp->config, FALSE);
		gkrellm_set_chartconfig_fixed_grids(cp->config, 2);
		gkrellm_set_chartconfig_grid_resolution(cp->config, 50);
		gkrellm_alloc_chartdata(cp);
		gkrellm_set_chart_height(cp, ROW_CHART_H);
		g_signal_connect(G_OBJECT(cp->drawing_area), "expose_event",
		                 G_CALLBACK(any_expose), NULL);
	}
	r->alive = 1;
}

static void sync_slot0_from_primary(void)
{
	g_strlcpy(P.slots[0].base_url, P.base_url, sizeof(P.slots[0].base_url));
	g_strlcpy(P.slots[0].display_name, P.display_name,
	          sizeof(P.slots[0].display_name));
	if (!P.slots[0].name[0] && P.display_name[0])
		g_strlcpy(P.slots[0].name, P.display_name,
		          sizeof(P.slots[0].name));
	P.slots[0].enabled = P.base_url[0] != '\0';
}

static void strip_url_inplace(char *url)
{
	size_t n;

	if (!url)
		return;
	n = strlen(url);
	while (n > 0 && url[n - 1] == '/')
		url[--n] = '\0';
}

/* Host bind 0.0.0.0 / :: is not a client address — scrape via loopback. */
static void normalize_loopback_url(char *url)
{
	char *p;

	if (!url || !*url)
		return;
	strip_url_inplace(url);
	if ((p = strstr(url, "://0.0.0.0:")) != NULL) {
		memmove(p + 3 + 9, p + 3 + 7, strlen(p + 3 + 7) + 1);
		memcpy(p + 3, "127.0.0.1", 9);
	} else if ((p = strstr(url, "://[::]:")) != NULL) {
		memmove(p + 3 + 5, p + 3 + 4, strlen(p + 3 + 4) + 1);
		memcpy(p + 3, "[::1]", 5);
	}
}

static void create_compact_slot_ui(DockSlot *s)
{
	GkrellmStyle *style;
	GkrellmTextstyle *ts;

	if (!s || !s->enabled || !s->base_url[0] || !P.main_vbox || !P.monitor)
		return;

	destroy_compact_slot_ui(s);

	s->header = gkrellm_panel_new0();
	style = gkrellm_panel_style(P.style_id);
	ts = gkrellm_meter_textstyle(P.style_id);
	s->title_decal = gkrellm_create_decal_text(s->header, "NemotronXXXX", ts,
	                                           style, -1, -1, -1);
	if (!P.lamp_pm)
		P.lamp_pm = make_lamp_pixmap();
	s->lamp_decal = gkrellm_create_decal_pixmap(s->header, P.lamp_pm, NULL,
	                                            LAMP_FRAMES, style, -1,
	                                            s->title_decal->y);
	gkrellm_panel_configure(s->header, NULL, style);
	gkrellm_panel_create(P.main_vbox, P.monitor, s->header);
	g_signal_connect(G_OBJECT(s->header->drawing_area), "expose_event",
	                 G_CALLBACK(any_expose), NULL);

	s->info_panel = gkrellm_panel_new0();
	style = gkrellm_meter_style(P.style_id);
	ts = gkrellm_meter_textstyle(P.style_id);
	s->info_decal = gkrellm_create_decal_text(s->info_panel,
	                                          "9999/s kv=100% q=99", ts,
	                                          style, -1, -1, -1);
	gkrellm_panel_configure(s->info_panel, NULL, style);
	gkrellm_panel_create(P.main_vbox, P.monitor, s->info_panel);
	g_signal_connect(G_OBJECT(s->info_panel->drawing_area), "expose_event",
	                 G_CALLBACK(any_expose), NULL);
	s->ui_alive = 1;
}

static void rebuild_llm_ui(void)
{
	int i;
	GkrellmStyle *style;
	GkrellmTextstyle *ts;

	if (!P.main_vbox || !P.monitor)
		return;

	destroy_ui_widgets();
	sync_slot0_from_primary();

	P.header = gkrellm_panel_new0();
	style = gkrellm_panel_style(P.style_id);
	ts = gkrellm_meter_textstyle(P.style_id);
	P.title_decal = gkrellm_create_decal_text(P.header, "NemotronXXXX", ts,
	                                          style, -1, -1, -1);
	if (P.features & FEAT_ENGINE) {
		if (!P.lamp_pm)
			P.lamp_pm = make_lamp_pixmap();
		P.lamp_decal = gkrellm_create_decal_pixmap(P.header, P.lamp_pm, NULL,
		                                           LAMP_FRAMES, style, -1,
		                                           P.title_decal->y);
	}
	gkrellm_panel_configure(P.header, NULL, style);
	gkrellm_panel_create(P.main_vbox, P.monitor, P.header);
	g_signal_connect(G_OBJECT(P.header->drawing_area), "expose_event",
	                 G_CALLBACK(any_expose), NULL);

	/* Session totals since dock start: in=prefill, out=decode */
	create_session_row(&P.sess_in_panel, &P.sess_in_label, &P.sess_in_value,
	                   "in");
	create_session_row(&P.sess_out_panel, &P.sess_out_label, &P.sess_out_value,
	                   "out");

	for (i = 0; i < N_FEATS; ++i) {
		/* Engine bit drives header lamp only — no Eng tile */
		if (METRICS[i].bit == FEAT_ENGINE)
			continue;
		if (P.features & METRICS[i].bit)
			create_metric_row(i);
	}

	for (i = 1; i < MAX_DOCK_SLOTS; ++i) {
		P.slots[i].enabled = P.slots[i].base_url[0] != '\0';
		if (P.slots[i].enabled)
			create_compact_slot_ui(&P.slots[i]);
	}

	P.ui_built = 1;
	gkrellm_spacers_set_types(P.monitor, GKRELLM_SPACER_CHART,
	                          GKRELLM_SPACER_CHART);
}

static void update_plugin(void)
{
	MetricsSnap snap;
	struct timespec now;
	double dt;
	int i;
	GkrellmStyle *style;
	GkrellmMargin *m;

	if (!GK.second_tick)
		return;

	if (scrape_metrics(&snap) != 0) {
		P.last.ok = 0;
		P.tps_decode = P.tps_prefill = 0;
	} else {
		clock_gettime(CLOCK_MONOTONIC, &now);
		dt = 1.0;
		if (P.have_prev) {
			dt = timespec_delta(&now, &P.prev_ts);
			if (dt < 0.2)
				dt = 0.2;
			P.tps_decode = decode_tps_from_snap(&snap, &P.prev, dt);
			if (snap.prompt_tokens >= P.prev.prompt_tokens)
				P.tps_prefill =
				    (snap.prompt_tokens - P.prev.prompt_tokens) /
				    dt;
			else
				P.tps_prefill = 0;
			derive_from_snap(&snap, dt);
		} else {
			P.tps_decode = P.tps_prefill = 0;
			derive_from_snap(&snap, 0);
		}
		/* Adapt Pre chart full-scale to recent peaks; do not hold rate. */
		if (P.tps_prefill > 0.5) {
			guint need = (guint)(P.tps_prefill * 1.25 + 0.5);
			if (need < PREFILL_ADAPT_MIN)
				need = PREFILL_ADAPT_MIN;
			if (need > PREFILL_ADAPT_MAX)
				need = PREFILL_ADAPT_MAX;
			if (need > P.prefill_scale_adapt)
				P.prefill_scale_adapt = need;
		} else if (P.prefill_scale_adapt > P.chart_max_prefill) {
			P.prefill_scale_adapt =
			    P.prefill_scale_adapt -
			    (P.prefill_scale_adapt - P.chart_max_prefill) / 8;
		}
		update_session_totals(&snap);
		P.prev = snap;
		P.prev_ts = now;
		P.have_prev = 1;
		P.last = snap;
	}

	if (P.header && P.title_decal) {
		const char *title;
		GkrellmStyle *hstyle = gkrellm_panel_style(P.style_id);
		GkrellmMargin *hm = gkrellm_get_style_margins(hstyle);
		int w = gkrellm_chart_width();

		if (P.last.ok && P.last.model_short[0]) {
			strncpy(P.shown_model, P.last.model_short,
			        sizeof(P.shown_model) - 1);
			P.shown_model[sizeof(P.shown_model) - 1] = '\0';
		}
		if (P.display_name[0])
			title = P.display_name;
		else
			title = P.last.ok ? P.last.model_short
			                 : (P.shown_model[0] ? P.shown_model : "LLM");

		P.title_decal->x = hm->left;
		gkrellm_draw_decal_text(P.header, P.title_decal, (gchar *)title,
		                        -1);
		if (P.lamp_decal) {
			P.lamp_decal->x = w - hm->left - hm->right - LAMP_SZ - 1;
			if (P.lamp_decal->x < hm->left + 40)
				P.lamp_decal->x = hm->left + 40;
			P.lamp_decal->y = P.title_decal->y +
			                  (P.title_decal->h - LAMP_SZ) / 2;
			if (P.lamp_decal->y < 0)
				P.lamp_decal->y = P.title_decal->y;
			gkrellm_draw_decal_pixmap(P.header, P.lamp_decal,
			                          engine_lamp_frame());
		}
		gkrellm_draw_panel_layers(P.header);
	}

	style = gkrellm_meter_style(P.style_id);
	m = gkrellm_get_style_margins(style);

	if (P.sess_in_panel && P.sess_in_value) {
		char tbuf[24];
		int w, w_text;

		fmt_tok_n(tbuf, sizeof(tbuf), P.session_prompt);
		w = gkrellm_chart_width();
		P.sess_in_label->x = m->left;
		gkrellm_draw_decal_text(P.sess_in_panel, P.sess_in_label, "in", -1);
		w_text = gkrellm_gdk_string_width(
		    P.sess_in_value->text_style.font, tbuf);
		P.sess_in_value->x = w - m->left - m->right - w_text - 1;
		if (P.sess_in_value->x < m->left + 24)
			P.sess_in_value->x = m->left + 24;
		gkrellm_draw_decal_text(P.sess_in_panel, P.sess_in_value, tbuf, -1);
		gkrellm_draw_panel_layers(P.sess_in_panel);
	}
	if (P.sess_out_panel && P.sess_out_value) {
		char tbuf[24];
		int w, w_text;

		fmt_tok_n(tbuf, sizeof(tbuf), P.session_gen);
		w = gkrellm_chart_width();
		P.sess_out_label->x = m->left;
		gkrellm_draw_decal_text(P.sess_out_panel, P.sess_out_label, "out",
		                        -1);
		w_text = gkrellm_gdk_string_width(
		    P.sess_out_value->text_style.font, tbuf);
		P.sess_out_value->x = w - m->left - m->right - w_text - 1;
		if (P.sess_out_value->x < m->left + 24)
			P.sess_out_value->x = m->left + 24;
		gkrellm_draw_decal_text(P.sess_out_panel, P.sess_out_value, tbuf,
		                        -1);
		gkrellm_draw_panel_layers(P.sess_out_panel);
	}

	for (i = 0; i < N_FEATS; ++i) {
		MetricRow *r = &P.rows[i];
		const MetricDef *d = &METRICS[i];
		char buf[48];
		double raw, full;
		gulong yu;
		int w, w_text;

		if (!r->alive || !r->panel)
			continue;
		metric_value(i, buf, sizeof(buf), &raw, &full);
		w = gkrellm_chart_width();
		if (r->label_decal) {
			r->label_decal->x = m->left;
			gkrellm_draw_decal_text(r->panel, r->label_decal,
			                        (gchar *)d->tag, -1);
		}
		if (r->value_decal) {
			w_text = gkrellm_gdk_string_width(
			    r->value_decal->text_style.font, buf);
			r->value_decal->x =
			    w - m->left - m->right - w_text - 1;
			if (r->value_decal->x < m->left + 24)
				r->value_decal->x = m->left + 24;
			gkrellm_draw_decal_text(r->panel, r->value_decal, buf,
			                        -1);
		}
		gkrellm_draw_panel_layers(r->panel);

		/* Y = value (0..100), X = time via scrolling chart history */
		if (r->chart && r->cd) {
			yu = krell_units(d, raw, full);
			gkrellm_store_chartdata(r->chart, 100UL, yu);
			refresh_row_chart(GINT_TO_POINTER(i));
		}
	}

	update_compact_slots();
}

static void update_compact_slot(int idx)
{
	DockSlot *s;
	MetricsSnap snap;
	struct timespec now;
	double dt;
	GkrellmStyle *hstyle, *style;
	GkrellmMargin *hm, *m;
	const char *title;
	char info[64];
	int w;

	if (idx < 1 || idx >= MAX_DOCK_SLOTS)
		return;
	s = &P.slots[idx];
	if (!s->enabled || !s->base_url[0])
		return;

	if (fetch_metrics(s->base_url, &snap) != 0) {
		s->last.ok = 0;
		s->tps_decode = s->tps_prefill = 0;
	} else {
		clock_gettime(CLOCK_MONOTONIC, &now);
		dt = 1.0;
		if (s->have_prev) {
			dt = timespec_delta(&now, &s->prev_ts);
			if (dt < 0.2)
				dt = 0.2;
			s->tps_decode = decode_tps_from_snap(&snap, &s->prev, dt);
			if (snap.prompt_tokens >= s->prev.prompt_tokens)
				s->tps_prefill =
				    (snap.prompt_tokens - s->prev.prompt_tokens) /
				    dt;
			else
				s->tps_prefill = 0;
		} else {
			s->tps_decode = s->tps_prefill = 0;
		}
		s->prev = snap;
		s->prev_ts = now;
		s->have_prev = 1;
		s->last = snap;
	}

	if (!s->ui_alive || !s->header || !s->title_decal)
		return;

	hstyle = gkrellm_panel_style(P.style_id);
	hm = gkrellm_get_style_margins(hstyle);
	w = gkrellm_chart_width();

	if (s->display_name[0])
		title = s->display_name;
	else if (s->name[0])
		title = s->name;
	else
		title = s->last.ok ? s->last.model_short : "LLM";

	s->title_decal->x = hm->left;
	gkrellm_draw_decal_text(s->header, s->title_decal, (gchar *)title, -1);
	if (s->lamp_decal) {
		s->lamp_decal->x = w - hm->left - hm->right - LAMP_SZ - 1;
		if (s->lamp_decal->x < hm->left + 40)
			s->lamp_decal->x = hm->left + 40;
		s->lamp_decal->y =
		    s->title_decal->y + (s->title_decal->h - LAMP_SZ) / 2;
		if (s->lamp_decal->y < 0)
			s->lamp_decal->y = s->title_decal->y;
		gkrellm_draw_decal_pixmap(s->header, s->lamp_decal,
		                          engine_lamp_frame_snap(&s->last));
	}
	gkrellm_draw_panel_layers(s->header);

	if (!s->info_panel || !s->info_decal)
		return;
	style = gkrellm_meter_style(P.style_id);
	m = gkrellm_get_style_margins(style);
	if (s->last.ok)
		snprintf(info, sizeof(info), "%.0f/s kv=%.0f%% q=%.0f",
		         s->tps_decode, s->last.kv_pct, s->last.waiting);
	else
		snprintf(info, sizeof(info), "down");
	s->info_decal->x = m->left;
	gkrellm_draw_decal_text(s->info_panel, s->info_decal, info, -1);
	gkrellm_draw_panel_layers(s->info_panel);
}

static void update_compact_slots(void)
{
	int i;

	for (i = 1; i < MAX_DOCK_SLOTS; ++i)
		update_compact_slot(i);
}

static void create_plugin(GtkWidget *vbox, gint first_create)
{
	if (first_create) {
		P.main_vbox = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), P.main_vbox, FALSE, FALSE, 0);
		gtk_widget_show(P.main_vbox);
		P.have_prev = 0;
	} else {
		destroy_ui_widgets();
	}
	rebuild_llm_ui();
}

static void strip_url_slash(void)
{
	normalize_loopback_url(P.base_url);
	sync_slot0_from_primary();
}

static void load_config(gchar *arg)
{
	gchar key[64], val[256];
	int slot;

	if (!arg)
		return;
	if (sscanf(arg, "%63s %255[^\n]", key, val) < 2)
		return;
	if (!strcmp(key, "url")) {
		strncpy(P.base_url, val, sizeof(P.base_url) - 1);
		P.base_url[sizeof(P.base_url) - 1] = '\0';
		strip_url_slash();
	} else if (!strcmp(key, "display_name")) {
		strncpy(P.display_name, val, sizeof(P.display_name) - 1);
		P.display_name[sizeof(P.display_name) - 1] = '\0';
		sync_slot0_from_primary();
	} else if (!strcmp(key, "dock_slots")) {
		P.dock_slots_hint = atoi(val);
		if (P.dock_slots_hint < 0)
			P.dock_slots_hint = 0;
		if (P.dock_slots_hint > MAX_DOCK_SLOTS)
			P.dock_slots_hint = MAX_DOCK_SLOTS;
	} else if (!strncmp(key, "slot", 4) &&
	           (key[4] == '1' || key[4] == '2' || key[4] == '3') &&
	           key[5] == '_') {
		slot = key[4] - '0';
		if (!strcmp(key + 5, "url")) {
			g_strlcpy(P.slots[slot].base_url, val,
			          sizeof(P.slots[slot].base_url));
			strip_url_inplace(P.slots[slot].base_url);
			P.slots[slot].enabled = P.slots[slot].base_url[0] != '\0';
			P.slots[slot].have_prev = 0;
		} else if (!strcmp(key + 5, "name")) {
			g_strlcpy(P.slots[slot].display_name, val,
			          sizeof(P.slots[slot].display_name));
			g_strlcpy(P.slots[slot].name, val,
			          sizeof(P.slots[slot].name));
		}
	} else if (!strcmp(key, "docs_release")) {
		strncpy(P.docs_release, val, sizeof(P.docs_release) - 1);
		P.docs_release[sizeof(P.docs_release) - 1] = '\0';
	} else if (!strcmp(key, "airgap")) {
		P.airgap = atoi(val) ? 1 : 0;
	} else if (!strcmp(key, "features")) {
		P.features = (guint)strtoul(val, NULL, 10);
	} else if (!strcmp(key, "chart_max_tps")) {
		P.chart_max_tps = (guint)atoi(val);
		if (P.chart_max_tps < 5)
			P.chart_max_tps = 5;
	} else if (!strcmp(key, "chart_max_prefill")) {
		P.chart_max_prefill = (guint)atoi(val);
		if (P.chart_max_prefill < 50)
			P.chart_max_prefill = 50;
	} else if (!strcmp(key, "timeout_ms")) {
		P.timeout_ms = (guint)atoi(val);
		if (P.timeout_ms < 100)
			P.timeout_ms = 100;
		if (P.timeout_ms > 5000)
			P.timeout_ms = 5000;
	}
}

static int count_enabled_dock_slots(void)
{
	int i, n = 0;

	sync_slot0_from_primary();
	for (i = 0; i < MAX_DOCK_SLOTS; ++i) {
		if (i == 0) {
			if (P.base_url[0])
				++n;
		} else if (P.slots[i].base_url[0]) {
			++n;
		}
	}
	return n;
}

static void save_config(FILE *f)
{
	int i;

	sync_slot0_from_primary();
	fprintf(f, "%s url %s\n", CONFIG_KEYWORD, P.base_url);
	fprintf(f, "%s display_name %s\n", CONFIG_KEYWORD, P.display_name);
	fprintf(f, "%s dock_slots %d\n", CONFIG_KEYWORD,
	        count_enabled_dock_slots());
	for (i = 1; i < MAX_DOCK_SLOTS; ++i) {
		if (!P.slots[i].base_url[0])
			continue;
		fprintf(f, "%s slot%d_url %s\n", CONFIG_KEYWORD, i,
		        P.slots[i].base_url);
		fprintf(f, "%s slot%d_name %s\n", CONFIG_KEYWORD, i,
		        P.slots[i].display_name[0] ? P.slots[i].display_name
		                                   : P.slots[i].name);
	}
	fprintf(f, "%s docs_release %s\n", CONFIG_KEYWORD, P.docs_release);
	fprintf(f, "%s airgap %d\n", CONFIG_KEYWORD, P.airgap ? 1 : 0);
	fprintf(f, "%s features %u\n", CONFIG_KEYWORD, P.features);
	fprintf(f, "%s chart_max_tps %u\n", CONFIG_KEYWORD, P.chart_max_tps);
	fprintf(f, "%s chart_max_prefill %u\n", CONFIG_KEYWORD,
	        P.chart_max_prefill);
	fprintf(f, "%s timeout_ms %u\n", CONFIG_KEYWORD, P.timeout_ms);
}

static void cfg_add_entry(GtkWidget *box, GtkWidget **entry, const char *text,
                          const char *label)
{
	GtkWidget *l = gtk_label_new(label);
	GtkWidget *e = gtk_entry_new();
	GtkWidget *h = gtk_hbox_new(FALSE, 4);

	gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(h), e, TRUE, TRUE, 4);
	if (text)
		gtk_entry_set_text(GTK_ENTRY(e), text);
	gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 2);
	*entry = e;
}

static void cfg_tip(GtkWidget *w, const char *tip)
{
	if (w && tip)
		gtk_widget_set_tooltip_text(w, tip);
}

static void cfg_add_password_entry(GtkWidget *box, GtkWidget **entry,
                                   const char *label)
{
	cfg_add_entry(box, entry, "", label);
	gtk_entry_set_visibility(GTK_ENTRY(*entry), FALSE);
}

static int find_gkrellm_nim(char *out, size_t out_sz)
{
	gchar *found;
	const char *home;
	char candidate[512];

	found = g_find_program_in_path("gkrellm-nim");
	if (found) {
		g_strlcpy(out, found, out_sz);
		g_free(found);
		return 0;
	}
	home = getenv("HOME");
	if (home && *home) {
		snprintf(candidate, sizeof(candidate), "%s/.local/bin/gkrellm-nim",
		         home);
		if (access(candidate, X_OK) == 0) {
			g_strlcpy(out, candidate, out_sz);
			return 0;
		}
	}
	return -1;
}

static int ensure_config_dir(char *dir_out, size_t dir_sz)
{
	const char *home = getenv("HOME");
	struct stat st;

	if (!home || !*home)
		return -1;
	snprintf(dir_out, dir_sz, "%s/.config/gkrellm-dock", home);
	if (stat(dir_out, &st) == 0) {
		if (!S_ISDIR(st.st_mode))
			return -1;
		return 0;
	}
	snprintf(dir_out, dir_sz, "%s/.config", home);
	if (mkdir(dir_out, 0700) < 0 && errno != EEXIST)
		return -1;
	snprintf(dir_out, dir_sz, "%s/.config/gkrellm-dock", home);
	if (mkdir(dir_out, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_secret_file(const char *filename, const char *value)
{
	char dir[512], path[576];
	int fd;
	size_t n;
	ssize_t wr;

	if (!value)
		return -1;
	if (ensure_config_dir(dir, sizeof(dir)) < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s", dir, filename);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0600) < 0) {
		close(fd);
		return -1;
	}
	n = strlen(value);
	wr = write(fd, value, n);
	if (wr < 0 || (size_t)wr != n) {
		close(fd);
		return -1;
	}
	if (close(fd) < 0)
		return -1;
	return 0;
}

/* Run helper; optional stdin. Returns exit status, or -1 on spawn/timeout.
 * If kill_on_timeout is 0, a timed-out child is left running (return -1). */
static int run_helper_ex(char *const argv[], const char *stdin_data,
                         int timeout_sec, int kill_on_timeout, char *out_buf,
                         size_t out_sz)
{
	int in_pipe[2] = { -1, -1 };
	int out_pipe[2] = { -1, -1 };
	pid_t pid;
	int status = -1;
	size_t out_len = 0;
	size_t in_off = 0;
	size_t in_len = stdin_data ? strlen(stdin_data) : 0;
	time_t deadline;
	int alive = 1;

	if (out_buf && out_sz)
		out_buf[0] = '\0';
	if (pipe(out_pipe) < 0)
		return -1;
	if (stdin_data && pipe(in_pipe) < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		if (in_pipe[0] >= 0) {
			close(in_pipe[0]);
			close(in_pipe[1]);
		}
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		if (in_pipe[0] >= 0) {
			dup2(in_pipe[0], STDIN_FILENO);
			close(in_pipe[0]);
			close(in_pipe[1]);
		} else {
			int devnull = open("/dev/null", O_RDONLY);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				close(devnull);
			}
		}
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(out_pipe[1], STDERR_FILENO);
		close(out_pipe[0]);
		close(out_pipe[1]);
		execv(argv[0], argv);
		_exit(127);
	}

	if (in_pipe[0] >= 0)
		close(in_pipe[0]);
	close(out_pipe[1]);
	if (in_pipe[1] >= 0)
		fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
	fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

	deadline = time(NULL) + (timeout_sec > 0 ? timeout_sec : 1);
	while (alive || (in_pipe[1] >= 0 && in_off < in_len) || out_pipe[0] >= 0) {
		fd_set rfds, wfds;
		struct timeval tv;
		int maxfd = -1;
		int nready;
		time_t now = time(NULL);

		if (now >= deadline) {
			if (kill_on_timeout)
				kill(pid, SIGKILL);
			if (kill_on_timeout)
				waitpid(pid, &status, 0);
			else
				waitpid(pid, &status, WNOHANG);
			if (out_buf && out_sz)
				g_strlcpy(out_buf, "timed out waiting for gkrellm-nim",
				          out_sz);
			if (in_pipe[1] >= 0)
				close(in_pipe[1]);
			if (out_pipe[0] >= 0)
				close(out_pipe[0]);
			return -1;
		}

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000;

		if (out_pipe[0] >= 0) {
			FD_SET(out_pipe[0], &rfds);
			maxfd = out_pipe[0];
		}
		if (in_pipe[1] >= 0 && in_off < in_len) {
			FD_SET(in_pipe[1], &wfds);
			if (in_pipe[1] > maxfd)
				maxfd = in_pipe[1];
		}

		nready = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
		if (nready < 0 && errno == EINTR)
			continue;

		/* Keep Settings UI responsive during long helper jobs (profiles/pull). */
		while (gtk_events_pending())
			gtk_main_iteration();

		if (in_pipe[1] >= 0 && in_off < in_len &&
		    FD_ISSET(in_pipe[1], &wfds)) {
			ssize_t wr = write(in_pipe[1], stdin_data + in_off,
			                   in_len - in_off);
			if (wr > 0)
				in_off += (size_t)wr;
			else if (wr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				close(in_pipe[1]);
				in_pipe[1] = -1;
			}
			if (in_off >= in_len && in_pipe[1] >= 0) {
				close(in_pipe[1]);
				in_pipe[1] = -1;
			}
		}

		if (out_pipe[0] >= 0 && FD_ISSET(out_pipe[0], &rfds)) {
			char tmp[256];
			ssize_t rd = read(out_pipe[0], tmp, sizeof(tmp));
			if (rd > 0) {
				if (out_buf && out_sz > 1 && out_len + 1 < out_sz) {
					size_t copy = (size_t)rd;
					if (copy > out_sz - 1 - out_len)
						copy = out_sz - 1 - out_len;
					memcpy(out_buf + out_len, tmp, copy);
					out_len += copy;
					out_buf[out_len] = '\0';
				}
			} else if (rd == 0 ||
			           (rd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
				close(out_pipe[0]);
				out_pipe[0] = -1;
			}
		}

		if (alive) {
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid) {
				alive = 0;
				if (in_pipe[1] >= 0) {
					close(in_pipe[1]);
					in_pipe[1] = -1;
				}
			}
		}
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

static int run_helper(char *const argv[], const char *stdin_data,
                      int timeout_sec, char *out_buf, size_t out_sz)
{
	return run_helper_ex(argv, stdin_data, timeout_sec, 1, out_buf, out_sz);
}

static void text_view_set(GtkWidget *view, const char *text)
{
	GtkTextBuffer *buf;

	if (!view)
		return;
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	gtk_text_buffer_set_text(buf, text ? text : "", -1);
}

static gboolean text_view_scroll_end_idle(gpointer data)
{
	GtkWidget *view = GTK_WIDGET(data);
	GtkTextBuffer *buf;
	GtkTextIter end;
	GtkTextMark *mark;

	if (!view || !GTK_IS_TEXT_VIEW(view))
		return FALSE;
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	gtk_text_buffer_get_end_iter(buf, &end);
	mark = gtk_text_buffer_get_mark(buf, "llm_nim_tail");
	if (!mark)
		mark = gtk_text_buffer_create_mark(buf, "llm_nim_tail", &end, FALSE);
	else
		gtk_text_buffer_move_mark(buf, mark, &end);
	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view), mark, 0.0, TRUE, 0.0,
	                             1.0);
	return FALSE;
}

static void text_view_set_tail(GtkWidget *view, const char *text)
{
	text_view_set(view, text);
	if (!view)
		return;
	g_idle_add(text_view_scroll_end_idle, view);
}

/* Append without rewriting the buffer — used for live logs (no flicker). */
static void text_view_append_scroll(GtkWidget *view, const char *chunk)
{
	GtkTextBuffer *buf;
	GtkTextIter end;

	if (!view || !chunk || !*chunk)
		return;
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	gtk_text_buffer_get_end_iter(buf, &end);
	gtk_text_buffer_insert(buf, &end, chunk, -1);
	g_idle_add(text_view_scroll_end_idle, view);
}

static void logs_seen_clear(void)
{
	if (cfg_logs_seen) {
		g_hash_table_destroy(cfg_logs_seen);
		cfg_logs_seen = NULL;
	}
	cfg_logs_live_primed = 0;
}

static void logs_live_stop(void)
{
	cfg_logs_follow = 0;
	if (cfg_logs_live_id) {
		g_source_remove(cfg_logs_live_id);
		cfg_logs_live_id = 0;
	}
	cfg_logs_live_name[0] = '\0';
	logs_seen_clear();
}

static void logs_seen_ensure(void)
{
	if (!cfg_logs_seen)
		cfg_logs_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                      NULL);
}

/* Mark every line as seen (first paint). */
static void logs_seen_ingest_all(const char *body)
{
	char **lines;
	int i;

	logs_seen_ensure();
	g_hash_table_remove_all(cfg_logs_seen);
	lines = g_strsplit(body ? body : "", "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		if (!lines[i][0])
			continue;
		g_hash_table_insert(cfg_logs_seen, g_strdup(lines[i]),
		                    GINT_TO_POINTER(1));
	}
	g_strfreev(lines);
}

/*
 * Append only lines not yet shown. docker --tail is a sliding window — never
 * rewrite the whole TextView (that causes jump-to-top flicker).
 */
static int logs_append_unseen(GtkWidget *view, const char *body)
{
	char **lines;
	int i;
	int added = 0;
	GString *chunk;

	if (!view || !body)
		return 0;
	logs_seen_ensure();
	chunk = g_string_new(NULL);
	lines = g_strsplit(body, "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		if (!lines[i][0])
			continue;
		if (g_hash_table_lookup(cfg_logs_seen, lines[i]))
			continue;
		g_hash_table_insert(cfg_logs_seen, g_strdup(lines[i]),
		                    GINT_TO_POINTER(1));
		g_string_append_c(chunk, '\n');
		g_string_append(chunk, lines[i]);
		added++;
	}
	g_strfreev(lines);
	if (added)
		text_view_append_scroll(view, chunk->str);
	g_string_free(chunk, TRUE);
	return added;
}

static gchar *text_view_get(GtkWidget *view)
{
	GtkTextBuffer *buf;
	GtkTextIter start, end;

	if (!view)
		return g_strdup("");
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	gtk_text_buffer_get_bounds(buf, &start, &end);
	return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

static GtkWidget *cfg_add_scrolled_text_ex(GtkWidget *box, GtkWidget **view_out,
                                           int height, int editable)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *tv = gtk_text_view_new();

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
	                                    GTK_SHADOW_IN);
	gtk_widget_set_size_request(sw, -1, height);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), editable ? TRUE : FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv),
	                                 editable ? TRUE : FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
	gtk_container_add(GTK_CONTAINER(sw), tv);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 2);
	*view_out = tv;
	return sw;
}

static GtkWidget *cfg_add_scrolled_text(GtkWidget *box, GtkWidget **view_out,
                                        int height)
{
	return cfg_add_scrolled_text_ex(box, view_out, height, 0);
}

static int helper_missing_dialog(const char *title)
{
	char helper[512];
	gchar *title_copy;

	if (find_gkrellm_nim(helper, sizeof(helper)) == 0)
		return 0;
	title_copy = g_strdup(title ? title : "gkrellm-nim");
	gkrellm_config_message_dialog(
	    title_copy,
	    _("gkrellm-nim not found on PATH or ~/.local/bin.\n"
	      "Install with ./scripts/install.sh (or build bin/gkrellm-nim)."));
	g_free(title_copy);
	return -1;
}

static void cfg_docs_release_text(char *out, size_t out_sz)
{
	const gchar *s = NULL;

	if (cfg_docs_release_entry)
		s = gtk_entry_get_text(GTK_ENTRY(cfg_docs_release_entry));
	if (s && *s)
		g_strlcpy(out, s, out_sz);
	else if (P.docs_release[0])
		g_strlcpy(out, P.docs_release, out_sz);
	else
		g_strlcpy(out, DEFAULT_DOCS_RELEASE, out_sz);
}

static int cfg_airgap_active(void)
{
	if (cfg_airgap_btn)
		return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cfg_airgap_btn))
		           ? 1
		           : 0;
	return P.airgap ? 1 : 0;
}

static void catalog_images_clear(void)
{
	guint i;

	if (!cfg_catalog_images)
		return;
	for (i = 0; i < cfg_catalog_images->len; ++i)
		g_free(g_ptr_array_index(cfg_catalog_images, i));
	g_ptr_array_set_size(cfg_catalog_images, 0);
}

static void split_image_repo_tag(const char *image, char *repo, size_t repo_sz,
                                 char *tag, size_t tag_sz)
{
	const char *colon;
	const char *slash;
	size_t n;

	if (repo && repo_sz)
		repo[0] = '\0';
	if (tag && tag_sz) {
		g_strlcpy(tag, "latest", tag_sz);
	}
	if (!image || !*image)
		return;
	slash = strrchr(image, '/');
	colon = strrchr(image, ':');
	if (colon && (!slash || colon > slash)) {
		n = (size_t)(colon - image);
		if (repo && repo_sz) {
			if (n >= repo_sz)
				n = repo_sz - 1;
			memcpy(repo, image, n);
			repo[n] = '\0';
		}
		if (tag && tag_sz)
			g_strlcpy(tag, colon + 1, tag_sz);
	} else if (repo && repo_sz) {
		g_strlcpy(repo, image, repo_sz);
	}
}

static void catalog_set_image_entry(const char *repo, const char *tag)
{
	char buf[640];

	if (!cfg_catalog_image_entry || !repo || !*repo)
		return;
	snprintf(buf, sizeof(buf), "%s:%s", repo, (tag && *tag) ? tag : "latest");
	gtk_entry_set_text(GTK_ENTRY(cfg_catalog_image_entry), buf);
}

static void catalog_sync_label_set(const char *iso_or_text)
{
	char pretty[128];
	const char *p;

	if (!cfg_catalog_sync_label)
		return;
	if (!iso_or_text || !*iso_or_text) {
		gtk_label_set_text(GTK_LABEL(cfg_catalog_sync_label),
		                   _("Last sync: never"));
		return;
	}
	p = iso_or_text;
	if (strlen(p) >= 16 && p[4] == '-' && p[10] == 'T') {
		snprintf(pretty, sizeof(pretty), "Last sync: %.10s %.5s", p, p + 11);
		gtk_label_set_text(GTK_LABEL(cfg_catalog_sync_label), pretty);
	} else {
		snprintf(pretty, sizeof(pretty), "Last sync: %.96s", p);
		gtk_label_set_text(GTK_LABEL(cfg_catalog_sync_label), pretty);
	}
}

static void catalog_tag_combo_select(const char *want)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gint i = 0;

	if (!cfg_catalog_tag_combo || !want || !*want)
		return;
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_catalog_tag_combo));
	if (!model || !gtk_tree_model_get_iter_first(model, &iter))
		return;
	do {
		gchar *txt = NULL;
		gtk_tree_model_get(model, &iter, 0, &txt, -1);
		if (txt && strcmp(txt, want) == 0) {
			cfg_catalog_busy = 1;
			gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_catalog_tag_combo), i);
			cfg_catalog_busy = 0;
			g_free(txt);
			return;
		}
		g_free(txt);
		i++;
	} while (gtk_tree_model_iter_next(model, &iter));
}

static void catalog_load_tags_for_image(const char *image, int refresh)
{
	char helper[512];
	char *out;
	char *argv[8];
	int argc = 0;
	int rc;
	char **lines;
	int i;
	int count = 0;
	char repo[400];
	char cur_tag[128];
	char sync_buf[96];

	if (!cfg_catalog_tag_combo || !image || !*image)
		return;
	split_image_repo_tag(image, repo, sizeof(repo), cur_tag, sizeof(cur_tag));
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0)
		return;
	out = g_malloc(HELPER_OUT_MAX);
	argv[argc++] = helper;
	argv[argc++] = "catalog-tags";
	argv[argc++] = "--image";
	argv[argc++] = (char *)image;
	if (refresh)
		argv[argc++] = "--refresh";
	argv[argc++] = "--pick";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC + (refresh ? 40 : 0),
	                out, HELPER_OUT_MAX);

	cfg_catalog_busy = 1;
	gtk_list_store_clear(GTK_LIST_STORE(
	    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_catalog_tag_combo))));
	if (rc == 0 && out[0]) {
		lines = g_strsplit(out, "\n", -1);
		for (i = 0; lines && lines[i]; ++i) {
			g_strstrip(lines[i]);
			if (!lines[i][0])
				continue;
			gtk_combo_box_text_append_text(
			    GTK_COMBO_BOX_TEXT(cfg_catalog_tag_combo), lines[i]);
			count++;
		}
		g_strfreev(lines);
	}
	if (count == 0) {
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cfg_catalog_tag_combo),
		                               "latest");
		if (cur_tag[0] && strcmp(cur_tag, "latest") != 0)
			gtk_combo_box_text_append_text(
			    GTK_COMBO_BOX_TEXT(cfg_catalog_tag_combo), cur_tag);
	}
	cfg_catalog_busy = 0;
	catalog_tag_combo_select(cur_tag[0] ? cur_tag : "latest");
	{
		gchar *sel = gtk_combo_box_text_get_active_text(
		    GTK_COMBO_BOX_TEXT(cfg_catalog_tag_combo));
		catalog_set_image_entry(repo, sel && *sel ? sel : cur_tag);
		g_free(sel);
	}
	g_free(out);

	/* Refresh last-sync label from tags JSON. */
	out = g_malloc(HELPER_OUT_MAX);
	argc = 0;
	argv[argc++] = helper;
	argv[argc++] = "catalog-tags";
	argv[argc++] = "--image";
	argv[argc++] = (char *)image;
	argv[argc++] = "--json";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC, out, HELPER_OUT_MAX);
	sync_buf[0] = '\0';
	if (rc == 0 && out[0] == '{') {
		json_extract_string_after(out, NULL, "last_sync", sync_buf,
		                          sizeof(sync_buf));
		if (!sync_buf[0])
			json_extract_string_after(out, NULL, "synced_at", sync_buf,
			                          sizeof(sync_buf));
	}
	catalog_sync_label_set(sync_buf[0] ? sync_buf : NULL);
	g_free(out);
}

static const char *catalog_selected_image(void)
{
	gint idx;
	const gchar *custom = NULL;

	if (cfg_catalog_image_entry)
		custom = gtk_entry_get_text(GTK_ENTRY(cfg_catalog_image_entry));
	if (custom && *custom)
		return custom;

	if (!cfg_catalog_combo || !cfg_catalog_images)
		return "";
	idx = gtk_combo_box_get_active(GTK_COMBO_BOX(cfg_catalog_combo));
	if (idx < 0 || (guint)idx >= cfg_catalog_images->len)
		return "";
	return (const char *)g_ptr_array_index(cfg_catalog_images, idx);
}

static void cb_catalog_tag_changed(GtkComboBox *combo, gpointer data)
{
	gchar *tag;
	char repo[400];
	char old_tag[128];
	const char *image;

	(void)data;
	if (cfg_catalog_busy || !combo)
		return;
	image = catalog_selected_image();
	if (!image || !*image)
		return;
	split_image_repo_tag(image, repo, sizeof(repo), old_tag, sizeof(old_tag));
	tag = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	if (tag && *tag && repo[0])
		catalog_set_image_entry(repo, tag);
	g_free(tag);
}

static void cb_catalog_combo_changed(GtkComboBox *combo, gpointer data)
{
	gint idx;
	const char *image;

	(void)data;
	if (cfg_catalog_busy || !cfg_catalog_image_entry || !cfg_catalog_images)
		return;
	idx = gtk_combo_box_get_active(combo);
	if (idx < 0 || (guint)idx >= cfg_catalog_images->len)
		return;
	image = (const char *)g_ptr_array_index(cfg_catalog_images, idx);
	if (!image)
		return;
	gtk_entry_set_text(GTK_ENTRY(cfg_catalog_image_entry), image);
	catalog_load_tags_for_image(image, 0);
}

static void catalog_combo_populate_from_pick(const char *pick_text,
                                             const char *prefer_image)
{
	char **lines;
	int i;
	GString *summary;
	int count = 0;
	int prefer_idx = -1;

	if (!cfg_catalog_combo)
		return;
	if (!cfg_catalog_images)
		cfg_catalog_images = g_ptr_array_new();

	catalog_images_clear();
	cfg_catalog_busy = 1;
	gtk_list_store_clear(
	    GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_catalog_combo))));

	summary = g_string_new("");
	lines = g_strsplit(pick_text ? pick_text : "", "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		char **fields;
		const char *name;
		const char *image;
		const char *source;
		char label[192];

		g_strstrip(lines[i]);
		if (!lines[i][0])
			continue;
		fields = g_strsplit(lines[i], "\t", 3);
		if (!fields || !fields[0] || !fields[1] || !fields[1][0]) {
			g_strfreev(fields);
			continue;
		}
		name = fields[0];
		image = fields[1];
		source = fields[2] ? fields[2] : "?";
		g_snprintf(label, sizeof(label), "%s", name);
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cfg_catalog_combo),
		                               label);
		if (prefer_image && *prefer_image && strcmp(image, prefer_image) == 0)
			prefer_idx = count;
		g_ptr_array_add(cfg_catalog_images, g_strdup(image));
		g_string_append_printf(summary, "%s  [%s]\n  %s\n\n", name, source,
		                       image);
		count++;
		g_strfreev(fields);
	}
	g_strfreev(lines);
	/* Keep busy through set_active so changed-handler does not race tag load. */
	if (count > 0)
		gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_catalog_combo),
		                         prefer_idx >= 0 ? prefer_idx : 0);
	cfg_catalog_busy = 0;

	if (count > 0) {
		const char *image = catalog_selected_image();
		if (image && *image)
			gtk_entry_set_text(GTK_ENTRY(cfg_catalog_image_entry), image);
	}
	if (cfg_catalog_view) {
		if (count == 0)
			text_view_set(cfg_catalog_view, _("(empty catalog)"));
		else
			text_view_set(cfg_catalog_view, summary->str);
	}
	g_string_free(summary, TRUE);
}

/* Fast path: local catalog cache only (safe to call when opening GUI). */
static void catalog_reload_list(void)
{
	char helper[512];
	char *out;
	char *argv[5];
	int rc;
	char dialog_msg[512];
	const char *image;
	char sync_buf[96];
	char keep_image[512];

	keep_image[0] = '\0';
	image = catalog_selected_image();
	if (image && *image)
		g_strlcpy(keep_image, image, sizeof(keep_image));

	if (find_gkrellm_nim(helper, sizeof(helper)) < 0)
		return;
	out = g_malloc(HELPER_OUT_MAX);

	argv[0] = helper;
	argv[1] = "catalog-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "catalog-list failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		text_view_set(cfg_catalog_view, dialog_msg);
	} else if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "catalog-list exit %d:\n%s", rc, out[0] ? out : "");
		text_view_set(cfg_catalog_view, out[0] ? out : dialog_msg);
	} else {
		catalog_combo_populate_from_pick(out, keep_image[0] ? keep_image : NULL);
		image = catalog_selected_image();
		if (image && *image)
			catalog_load_tags_for_image(image, 0);
	}

	/* Best-effort last_sync label (local JSON, no NGC). */
	argv[1] = "catalog-list";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	sync_buf[0] = '\0';
	if (rc == 0 && out[0] == '{')
		json_extract_string_after(out, NULL, "last_sync", sync_buf,
		                          sizeof(sync_buf));
	catalog_sync_label_set(sync_buf[0] ? sync_buf : NULL);
	g_free(out);
}

/* Explicit Refresh: NGC sync + reload list + tags for selected model only. */
static void cb_catalog_refresh(GtkWidget *button, gpointer data)
{
	char helper[512];
	char *out;
	char *argv[6];
	int rc;
	char dialog_msg[512];
	char sync_buf[96];
	char msg_buf[256];

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Catalog")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);

	argv[0] = helper;
	argv[1] = "catalog-sync";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC + 120, out,
	                HELPER_OUT_MAX);
	sync_buf[0] = '\0';
	msg_buf[0] = '\0';
	if (rc == 0 && out[0] == '{') {
		json_extract_string_after(out, NULL, "last_sync", sync_buf,
		                          sizeof(sync_buf));
		catalog_sync_label_set(sync_buf[0] ? sync_buf : NULL);
	} else if (rc != 0) {
		if (out[0] == '{')
			json_extract_string_after(out, NULL, "msg", msg_buf,
			                          sizeof(msg_buf));
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "catalog-sync failed%s%s",
		         msg_buf[0] ? ":\n" : " (set NGC token in Connection)\n",
		         msg_buf[0] ? msg_buf : "");
		text_view_set(cfg_catalog_view, dialog_msg);
	}
	g_free(out);

	catalog_reload_list();
}

static void cb_catalog_add(GtkWidget *button, gpointer data)
{
	const gchar *image = "";
	char helper[512];
	char out[2048];
	char *argv[6];
	int rc;
	char dialog_msg[2200];
	char msg_buf[256];

	(void)button;
	(void)data;
	image = catalog_selected_image();
	if (!image || !*image) {
		gkrellm_config_message_dialog(_("Catalog Add"),
		                              _("Select or enter an image reference first."));
		return;
	}
	if (helper_missing_dialog(_("Catalog Add")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "catalog-add";
	argv[2] = "--image";
	argv[3] = (char *)image;
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "catalog-add failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Catalog Add"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "catalog-add exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Catalog Add"), dialog_msg);
		return;
	}
	msg_buf[0] = '\0';
	if (out[0] == '{')
		json_extract_string_after(out, NULL, "msg", msg_buf, sizeof(msg_buf));
	if (strstr(out, "\"already_present\": true") ||
	    strstr(out, "\"already_present\":true")) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "%s\n\n%s",
		         msg_buf[0] ? msg_buf
		                   : _("Already in Catalog — use Pull to download to Local"),
		         image);
	} else {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         _("Saved to Catalog cache (not a docker pull).\n"
		           "Use Pull to download → Local.\n%s"),
		         image);
	}
	gkrellm_config_message_dialog(_("Catalog Add"), dialog_msg);
	catalog_reload_list();
}

static void cb_docs_sync(GtkWidget *button, gpointer data)
{
	char helper[512];
	char release[64];
	char out[4096];
	char *argv[8];
	int argc = 0;
	int rc;
	int airgap;
	char dialog_msg[4300];

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Sync docs")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	cfg_docs_release_text(release, sizeof(release));
	airgap = cfg_airgap_active();

	argv[argc++] = helper;
	argv[argc++] = "docs-sync";
	argv[argc++] = "--release";
	argv[argc++] = release;
	if (airgap)
		argv[argc++] = "--airgap";
	argv[argc++] = "--json";
	argv[argc] = NULL;

	rc = run_helper(argv, NULL, HELPER_DOCS_SYNC_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "docs-sync failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Sync docs"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "docs-sync exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Sync docs"), dialog_msg);
		return;
	}
	format_docs_sync_dialog(out, dialog_msg, sizeof(dialog_msg));
	if (airgap) {
		size_t n = strlen(dialog_msg);

		if (n + 32 < sizeof(dialog_msg))
			snprintf(dialog_msg + n, sizeof(dialog_msg) - n, "\n%s",
			         _("Air-gap mode (vendored schema only)."));
	}
	gkrellm_config_message_dialog(_("Sync docs"), dialog_msg);
}

static void cb_catalog_pull(GtkWidget *button, gpointer data)
{
	const gchar *image = "";
	char helper[512];
	char out[4096];
	char *argv[6];
	int rc;
	char dialog_msg[4300];

	(void)button;
	(void)data;
	image = catalog_selected_image();
	if (!image || !*image) {
		gkrellm_config_message_dialog(
		    _("Pull"),
		    _("Select a catalog model or enter an image reference first."));
		return;
	}
	if (helper_missing_dialog(_("Pull")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "pull";
	argv[2] = "--image";
	argv[3] = (char *)image;
	argv[4] = "--json";
	argv[5] = NULL;
	/* Do not kill on timeout — pull may continue; point user at events. */
	rc = run_helper_ex(argv, NULL, HELPER_PULL_TIMEOUT_SEC, 0, out,
	                   sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "Pull timed out or failed to start for:\n%s\n\n"
		         "%s\n\n"
		         "If docker pull is still running, check:\n"
		         "  gkrellm-nim events-tail",
		         image, out[0] ? out : "(no output)");
		gkrellm_config_message_dialog(_("Pull"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "pull exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Pull"), dialog_msg);
		return;
	}
	snprintf(dialog_msg, sizeof(dialog_msg), "Pull OK.\n%s",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Pull"), dialog_msg);
}

static void local_images_clear(void)
{
	guint i;

	if (!cfg_local_images)
		return;
	for (i = 0; i < cfg_local_images->len; ++i)
		g_free(g_ptr_array_index(cfg_local_images, i));
	g_ptr_array_set_size(cfg_local_images, 0);
}

static const char *local_selected_image(void)
{
	gint idx;
	const gchar *custom = NULL;

	if (cfg_local_image_entry)
		custom = gtk_entry_get_text(GTK_ENTRY(cfg_local_image_entry));
	if (custom && *custom)
		return custom;

	if (!cfg_local_combo || !cfg_local_images)
		return "";
	idx = gtk_combo_box_get_active(GTK_COMBO_BOX(cfg_local_combo));
	if (idx < 0 || (guint)idx >= cfg_local_images->len)
		return "";
	return (const char *)g_ptr_array_index(cfg_local_images, idx);
}

static void cb_local_combo_changed(GtkComboBox *combo, gpointer data)
{
	gint idx;
	const char *image;

	(void)data;
	if (!cfg_local_image_entry || !cfg_local_images)
		return;
	idx = gtk_combo_box_get_active(combo);
	if (idx < 0 || (guint)idx >= cfg_local_images->len)
		return;
	image = (const char *)g_ptr_array_index(cfg_local_images, idx);
	if (image)
		gtk_entry_set_text(GTK_ENTRY(cfg_local_image_entry), image);
}

static void local_combo_populate_from_pick(const char *pick_text)
{
	char **lines;
	int i;
	GString *summary;
	int count = 0;

	if (!cfg_local_combo)
		return;
	if (!cfg_local_images)
		cfg_local_images = g_ptr_array_new();

	local_images_clear();
	gtk_list_store_clear(
	    GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_local_combo))));

	summary = g_string_new("");
	lines = g_strsplit(pick_text ? pick_text : "", "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		char **fields;
		const char *label;
		const char *image;

		g_strstrip(lines[i]);
		if (!lines[i][0])
			continue;
		fields = g_strsplit(lines[i], "\t", 2);
		if (!fields || !fields[0] || !fields[1] || !fields[1][0]) {
			g_strfreev(fields);
			continue;
		}
		label = fields[0];
		image = fields[1];
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cfg_local_combo),
		                               label);
		g_ptr_array_add(cfg_local_images, g_strdup(image));
		g_string_append_printf(summary, "%s\n  %s\n\n", label, image);
		count++;
		g_strfreev(fields);
	}
	g_strfreev(lines);

	if (count > 0) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_local_combo), 0);
		cb_local_combo_changed(GTK_COMBO_BOX(cfg_local_combo), NULL);
	}
	if (cfg_local_view) {
		if (count == 0)
			text_view_set(cfg_local_view, _("(no local NIM images)"));
		else
			text_view_set(cfg_local_view, summary->str);
	}
	g_string_free(summary, TRUE);
}

static void cb_local_refresh(GtkWidget *button, gpointer data)
{
	char helper[512];
	char *out;
	char *argv[5];
	int rc;
	char dialog_msg[512];

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Local images")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "images-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_IMAGES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "images-list failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		text_view_set(cfg_local_view, dialog_msg);
		gkrellm_config_message_dialog(_("Local images"), dialog_msg);
	} else if (rc != 0) {
		text_view_set(cfg_local_view, out[0] ? out : "(error)");
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "images-list exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Local images"), dialog_msg);
	} else {
		local_combo_populate_from_pick(out);
	}
	g_free(out);
}

static void cb_local_profiles(GtkWidget *button, gpointer data)
{
	const gchar *image = "";
	char helper[512];
	char *out;
	char *argv[6];
	int rc;
	char dialog_msg[512];

	(void)button;
	(void)data;
	image = local_selected_image();
	if (!image || !*image) {
		gkrellm_config_message_dialog(
		    _("Profiles"),
		    _("Select a local image from the list (or enter a ref) first."));
		return;
	}
	gkrellm_config_message_dialog(
	    _("Profiles"),
	    _("Will run: docker … list-model-profiles on this image.\n"
	      "This can take 1–2 minutes. The window may feel slow — "
	      "wait for the result dialog.\n\n"
	      "Copy a profile ID from the result into Recipes → Profile, "
	      "then Start."));
	if (helper_missing_dialog(_("Profiles")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "profiles-list";
	argv[2] = "--image";
	argv[3] = (char *)image;
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper(argv, NULL, HELPER_PROFILES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "profiles-list failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Profiles"), dialog_msg);
		g_free(out);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "profiles-list exit %d:\n%s", rc,
		         out[0] ? out : "");
		gkrellm_config_message_dialog(_("Profiles"),
		                              dialog_msg[0] ? dialog_msg : out);
		g_free(out);
		return;
	}
	/* Prefer full helper JSON in the dialog when short enough. */
	if (strlen(out) < 3500)
		gkrellm_config_message_dialog(_("Profiles"), out[0] ? out : "(none)");
	else {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "profiles-list OK (%lu bytes). Output truncated; "
		         "re-run: gkrellm-nim profiles-list --image … --json",
		         (unsigned long)strlen(out));
		gkrellm_config_message_dialog(_("Profiles"), dialog_msg);
	}
	g_free(out);
}

static void cb_local_make_recipe(GtkWidget *button, gpointer data)
{
	const gchar *image = "";
	const gchar *profile = "";
	char helper[512];
	char out[4096];
	char *argv[10];
	int argc = 0;
	int rc;
	char dialog_msg[4300];
	char name_buf[128];

	(void)button;
	(void)data;
	image = local_selected_image();
	if (!image || !*image) {
		gkrellm_config_message_dialog(
		    _("Make recipe"),
		    _("Select a local image first."));
		return;
	}
	if (cfg_recipe_profile_entry)
		profile = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_profile_entry));
	if (helper_missing_dialog(_("Make recipe")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[argc++] = helper;
	argv[argc++] = "recipes-from-image";
	argv[argc++] = "--image";
	argv[argc++] = (char *)image;
	/* Never bake the Spark demo hash into a new recipe. */
	if (profile && *profile && !profile_id_is_placeholder(profile)) {
		argv[argc++] = "--profile";
		argv[argc++] = (char *)profile;
	}
	argv[argc++] = "--json";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out, sizeof(out));
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-from-image failed (exit %d):\n%s", rc,
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Make recipe"), dialog_msg);
		return;
	}
	name_buf[0] = '\0';
	json_extract_string_after(out, NULL, "name", name_buf, sizeof(name_buf));
	if (name_buf[0] && cfg_recipe_name_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_recipe_name_entry), name_buf);
	cb_recipes_refresh(NULL, NULL);
	if (name_buf[0])
		recipe_combo_select_name(name_buf);
	snprintf(dialog_msg, sizeof(dialog_msg),
	         "Recipe saved%s%s and opened in the editor below.\n\n"
	         "Next:\n"
	         "1) Edit JSON if needed\n"
	         "2) Load profiles → pick a profile\n"
	         "3) Save\n"
	         "4) Start (only this recipe runs)\n"
	         "5) Instances → Use on dock\n\n%s",
	         name_buf[0] ? ": " : "", name_buf[0] ? name_buf : "",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Make recipe"), dialog_msg);
}

/* Extract JSON string value for key after optional search start (simple, not a parser). */
static int json_extract_string_after(const char *json, const char *after,
                                     const char *key, char *out, size_t out_sz)
{
	const char *base;
	const char *p;
	const char *q;
	char needle[96];
	size_t key_len;
	size_t n;

	if (!json || !key || !out || out_sz < 2)
		return -1;
	base = after && after >= json ? after : json;
	key_len = strlen(key);
	if (key_len + 4 >= sizeof(needle))
		return -1;
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(base, needle);
	if (!p)
		return -1;
	p += strlen(needle);
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (*p != ':')
		return -1;
	p++;
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (*p != '"')
		return -1;
	p++;
	q = p;
	while (*q && *q != '"' && *q != '\n') {
		if (*q == '\\' && q[1])
			q += 2;
		else
			q++;
	}
	if (*q != '"')
		return -1;
	n = (size_t)(q - p);
	if (n >= out_sz)
		n = out_sz - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 0;
}

/* Human-readable Sync docs success text (never dump raw JSON). */
static void format_docs_sync_dialog(const char *json, char *msg, size_t msg_sz)
{
	char release[64];
	char source[64];
	char sync_err[512];
	char synced_at[80];
	const char *p;
	const char *q;
	int env_count = 0;
	size_t n;

	if (!json || !msg || msg_sz < 32)
		return;
	release[0] = source[0] = sync_err[0] = synced_at[0] = '\0';
	json_extract_string_after(json, NULL, "release", release, sizeof(release));
	json_extract_string_after(json, NULL, "source", source, sizeof(source));
	json_extract_string_after(json, NULL, "sync_error", sync_err, sizeof(sync_err));
	json_extract_string_after(json, NULL, "synced_at", synced_at, sizeof(synced_at));
	p = strstr(json, "\"env\"");
	if (p) {
		for (q = p; (q = strstr(q, "\"help\"")) != NULL; ) {
			env_count++;
			q++;
		}
	}
	n = (size_t)snprintf(msg, msg_sz,
	                     _("Docs schema %s synced.\n"
	                       "Source: %s\n"
	                       "Environment variables: %d"),
	                     release[0] ? release : "?",
	                     source[0] ? source
	                               : _("live NVIDIA docs"),
	                     env_count);
	if (synced_at[0] && n < msg_sz - 1)
		n += (size_t)snprintf(msg + n, msg_sz - n, "\nTimestamp: %s",
		                      synced_at);
	if (sync_err[0] && n < msg_sz - 1) {
		if (!strcmp(source, "last_good"))
			n += (size_t)snprintf(
			    msg + n, msg_sz - n,
			    "\n\n%s",
			    _("Used vendored schema seed — live docs HTML did not "
			      "parse (NVIDIA may have changed the page layout)."));
		n += (size_t)snprintf(msg + n, msg_sz - n, "\n%s: %s",
		                      _("Detail"), sync_err);
	}
}

/* Set/replace a top-level JSON string field. Returns newly allocated JSON. */
static gchar *json_set_string_field(const char *json, const char *key,
                                    const char *value)
{
	const char *p;
	const char *val_start;
	const char *val_end;
	char needle[96];
	GString *out;
	size_t key_len;

	if (!json || !key || !value)
		return g_strdup(json ? json : "");
	key_len = strlen(key);
	if (key_len + 4 >= sizeof(needle))
		return g_strdup(json);
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (p) {
		const char *colon = p + strlen(needle);
		while (*colon && (*colon == ' ' || *colon == '\t' || *colon == '\n' ||
		                  *colon == '\r'))
			colon++;
		if (*colon != ':')
			return g_strdup(json);
		colon++;
		while (*colon && (*colon == ' ' || *colon == '\t' || *colon == '\n' ||
		                  *colon == '\r'))
			colon++;
		if (*colon != '"')
			return g_strdup(json);
		val_start = colon + 1;
		val_end = val_start;
		while (*val_end && *val_end != '"' && *val_end != '\n') {
			if (*val_end == '\\' && val_end[1])
				val_end += 2;
			else
				val_end++;
		}
		if (*val_end != '"')
			return g_strdup(json);
		out = g_string_new_len(json, (gssize)(val_start - json));
		g_string_append(out, value);
		g_string_append(out, val_end);
		return g_string_free(out, FALSE);
	}

	/* Insert after opening '{'. */
	p = strchr(json, '{');
	if (!p)
		return g_strdup(json);
	out = g_string_new_len(json, (gssize)(p - json + 1));
	g_string_append_printf(out, "\n  \"%s\": \"%s\",", key, value);
	g_string_append(out, p + 1);
	return g_string_free(out, FALSE);
}

static int find_instance_fields(const char *json, const char *name,
                                char *scrape_out, size_t scrape_sz,
                                char *disp_out, size_t disp_sz)
{
	const char *p;
	char found_name[128];
	const char *name_at;
	const char *next_name;
	const char *scrape_at;
	char scrape_tmp[256];

	if (!json || !name || !*name || !scrape_out || scrape_sz < 8)
		return -1;
	scrape_out[0] = '\0';
	if (disp_out && disp_sz)
		disp_out[0] = '\0';

	p = json;
	while ((p = strstr(p, "\"name\"")) != NULL) {
		name_at = p;
		if (json_extract_string_after(p, p, "name", found_name,
		                              sizeof(found_name)) < 0) {
			p += 6;
			continue;
		}
		if (strcmp(found_name, name) != 0) {
			p += 6;
			continue;
		}
		next_name = strstr(name_at + 6, "\"name\"");
		scrape_at = strstr(name_at, "\"scrape_url\"");
		if (!scrape_at || (next_name && scrape_at > next_name)) {
			p += 6;
			continue;
		}
		if (json_extract_string_after(scrape_at, scrape_at, "scrape_url",
		                              scrape_tmp, sizeof(scrape_tmp)) < 0) {
			p += 6;
			continue;
		}
		g_strlcpy(scrape_out, scrape_tmp, scrape_sz);
		if (disp_out && disp_sz) {
			const char *disp_at = strstr(name_at, "\"display_name\"");
			if (disp_at && (!next_name || disp_at < next_name))
				json_extract_string_after(disp_at, disp_at,
				                          "display_name", disp_out,
				                          disp_sz);
		}
		return 0;
	}
	return -1;
}

static void profile_ids_clear(void)
{
	guint i;

	if (!cfg_profile_ids)
		return;
	for (i = 0; i < cfg_profile_ids->len; ++i)
		g_free(g_ptr_array_index(cfg_profile_ids, i));
	g_ptr_array_set_size(cfg_profile_ids, 0);
}

/* Active Profile combo → hash id (NULL if none). Do not free. */
static const char *profile_id_from_combo(void)
{
	gint idx;

	if (!cfg_profile_combo || !cfg_profile_ids || cfg_profile_ids->len == 0)
		return NULL;
	idx = gtk_combo_box_get_active(GTK_COMBO_BOX(cfg_profile_combo));
	if (idx < 0 || (guint)idx >= cfg_profile_ids->len)
		return NULL;
	return (const char *)g_ptr_array_index(cfg_profile_ids, idx);
}

static void profile_entry_sync_from_combo(void)
{
	const char *pid = profile_id_from_combo();

	if (pid && *pid && cfg_recipe_profile_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry), pid);
}

/* Prefer helper JSON "msg"; else truncate raw output for short dialogs. */
static void helper_short_dialog(const char *title, const char *lead,
                                const char *helper_out)
{
	char msg_buf[400];
	char dialog[700];
	const char *body;

	msg_buf[0] = '\0';
	if (helper_out && helper_out[0] == '{')
		json_extract_string_after(helper_out, NULL, "msg", msg_buf,
		                          sizeof(msg_buf));
	if (msg_buf[0])
		body = msg_buf;
	else if (helper_out && helper_out[0])
		body = helper_out;
	else
		body = "(no details)";

	if (lead && *lead)
		snprintf(dialog, sizeof(dialog), "%s\n\n%s", lead, body);
	else
		snprintf(dialog, sizeof(dialog), "%s", body);
	if (strlen(dialog) > 480) {
		dialog[477] = '.';
		dialog[478] = '.';
		dialog[479] = '.';
		dialog[480] = '\0';
	}
	gkrellm_config_message_dialog((gchar *)title, dialog);
}

static void recipes_load_named(const char *name)
{
	char helper[512];
	char *out;
	char *argv[5];
	int rc;
	char name_buf[128];
	char profile_buf[128];
	char image_buf[320];

	if (!name || !*name)
		return;
	if (helper_missing_dialog(_("Load recipe")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "recipes-get";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0 || !out[0] || out[0] != '{') {
		gkrellm_config_message_dialog(
		    _("Load recipe"),
		    out[0] ? out : "recipes-get failed");
		g_free(out);
		return;
	}
	text_view_set(cfg_recipes_view, out);
	name_buf[0] = profile_buf[0] = image_buf[0] = '\0';
	json_extract_string_after(out, NULL, "name", name_buf, sizeof(name_buf));
	json_extract_string_after(out, NULL, "profile", profile_buf,
	                          sizeof(profile_buf));
	json_extract_string_after(out, NULL, "image", image_buf, sizeof(image_buf));
	if (name_buf[0] && cfg_recipe_name_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_recipe_name_entry), name_buf);
	if (cfg_recipe_profile_entry) {
		if (profile_buf[0] && !profile_id_is_placeholder(profile_buf))
			gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry),
			                   profile_buf);
		else
			gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry), "");
		gtk_widget_set_sensitive(cfg_recipe_profile_entry, TRUE);
	}
	if (cfg_profile_combo)
		gtk_widget_set_sensitive(cfg_profile_combo, TRUE);
	g_free(out);
	if (image_buf[0])
		recipes_autoload_profiles_cached(image_buf);
}

static void cb_recipe_combo_changed(GtkComboBox *combo, gpointer data)
{
	gchar *name;

	(void)data;
	if (cfg_recipes_busy || !combo)
		return;
	name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	if (!name || !*name) {
		g_free(name);
		return;
	}
	if (cfg_recipe_name_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_recipe_name_entry), name);
	recipes_load_named(name);
	g_free(name);
}

static void recipe_combo_select_name(const char *want)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gint i = 0;

	if (!cfg_recipe_combo || !want || !*want)
		return;
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_recipe_combo));
	if (!model || !gtk_tree_model_get_iter_first(model, &iter))
		return;
	do {
		gchar *txt = NULL;
		gtk_tree_model_get(model, &iter, 0, &txt, -1);
		if (txt && strcmp(txt, want) == 0) {
			cfg_recipes_busy = 1;
			gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_recipe_combo), i);
			cfg_recipes_busy = 0;
			g_free(txt);
			recipes_load_named(want);
			return;
		}
		g_free(txt);
		i++;
	} while (gtk_tree_model_iter_next(model, &iter));
}

static int profile_id_is_placeholder(const char *pid)
{
	if (!pid || !*pid)
		return 1;
	if (strncmp(pid, "0123456789abcdef", 16) == 0)
		return 1;
	return 0;
}

static void profile_combo_populate_from_pick(const char *pick_text);

static void cb_profile_combo_changed(GtkComboBox *combo, gpointer data)
{
	(void)combo;
	(void)data;
	if (cfg_recipes_busy)
		return;
	profile_entry_sync_from_combo();
}

/* Prefer Spark-friendly nvfp4 runnable when present. */
static int profile_combo_prefer_default(void)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gint i = 0;
	gint best = -1;
	gint first_runnable = -1;

	if (!cfg_profile_combo)
		return -1;
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_profile_combo));
	if (!model || !gtk_tree_model_get_iter_first(model, &iter))
		return -1;
	do {
		gchar *txt = NULL;
		gtk_tree_model_get(model, &iter, 0, &txt, -1);
		if (txt) {
			if (first_runnable < 0 && strstr(txt, "[runnable]"))
				first_runnable = i;
			if (strstr(txt, "[runnable]") && strstr(txt, "nvfp4")) {
				best = i;
				g_free(txt);
				break;
			}
		}
		g_free(txt);
		i++;
	} while (gtk_tree_model_iter_next(model, &iter));
	if (best < 0)
		best = first_runnable;
	if (best < 0)
		best = 0;
	cfg_recipes_busy = 1;
	gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_profile_combo), best);
	cfg_recipes_busy = 0;
	cb_profile_combo_changed(GTK_COMBO_BOX(cfg_profile_combo), NULL);
	return best;
}

static void profile_combo_clear(void)
{
	if (!cfg_profile_combo)
		return;
	if (!cfg_profile_ids)
		cfg_profile_ids = g_ptr_array_new();
	profile_ids_clear();
	cfg_recipes_busy = 1;
	gtk_list_store_clear(GTK_LIST_STORE(
	    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_profile_combo))));
	cfg_recipes_busy = 0;
}

/* Select combo row matching profile hash; keep entry text if no match. */
static int profile_combo_select_id(const char *want)
{
	guint i;

	if (!want || !*want || !cfg_profile_combo || !cfg_profile_ids)
		return -1;
	for (i = 0; i < cfg_profile_ids->len; ++i) {
		const char *pid = (const char *)g_ptr_array_index(cfg_profile_ids, i);
		if (pid && strcmp(pid, want) == 0) {
			cfg_recipes_busy = 1;
			gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_profile_combo),
			                         (gint)i);
			cfg_recipes_busy = 0;
			return (int)i;
		}
	}
	return -1;
}

static void recipes_autoload_profiles_cached(const char *image)
{
	char helper[512];
	char *out;
	char *argv[8];
	int argc = 0;
	int rc;
	char keep_profile[128];

	if (!image || !*image || !cfg_profile_combo)
		return;
	keep_profile[0] = '\0';
	if (cfg_recipe_profile_entry) {
		const gchar *cur =
		    gtk_entry_get_text(GTK_ENTRY(cfg_recipe_profile_entry));
		if (cur && *cur && !profile_id_is_placeholder(cur))
			g_strlcpy(keep_profile, cur, sizeof(keep_profile));
	}
	/* Always drop previous recipe's profiles before cache load. */
	profile_combo_clear();
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0)
		return;
	out = g_malloc(HELPER_OUT_MAX);
	argv[argc++] = helper;
	argv[argc++] = "profiles-list";
	argv[argc++] = "--image";
	argv[argc++] = (char *)image;
	argv[argc++] = "--cached";
	argv[argc++] = "--pick";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc == 0 && out[0]) {
		profile_combo_populate_from_pick(out);
		gtk_widget_set_sensitive(cfg_profile_combo, TRUE);
		if (cfg_recipe_profile_entry)
			gtk_widget_set_sensitive(cfg_recipe_profile_entry, TRUE);
		/* Prefer recipe's saved profile over nvfp4 heuristic. */
		if (keep_profile[0] && profile_combo_select_id(keep_profile) >= 0) {
			if (cfg_recipe_profile_entry)
				gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry),
				                   keep_profile);
		} else if (keep_profile[0] && cfg_recipe_profile_entry) {
			/* Keep recipe id even if not in (stale) list. */
			gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry),
			                   keep_profile);
		}
	} else {
		gtk_widget_set_sensitive(cfg_profile_combo, TRUE);
		if (cfg_recipe_profile_entry) {
			gtk_widget_set_sensitive(cfg_recipe_profile_entry, TRUE);
			if (keep_profile[0])
				gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry),
				                   keep_profile);
			else if (profile_id_is_placeholder(gtk_entry_get_text(
			             GTK_ENTRY(cfg_recipe_profile_entry))))
				gtk_entry_set_text(GTK_ENTRY(cfg_recipe_profile_entry),
				                   "");
		}
	}
	g_free(out);
}


static void profile_combo_populate_from_pick(const char *pick_text)
{
	char **lines;
	int i;
	int count = 0;

	if (!cfg_profile_combo)
		return;
	if (!cfg_profile_ids)
		cfg_profile_ids = g_ptr_array_new();
	profile_ids_clear();
	gtk_list_store_clear(GTK_LIST_STORE(
	    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_profile_combo))));

	lines = g_strsplit(pick_text ? pick_text : "", "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		char **fields;
		const char *label;
		const char *pid;

		g_strstrip(lines[i]);
		if (!lines[i][0])
			continue;
		fields = g_strsplit(lines[i], "\t", 2);
		if (!fields || !fields[0] || !fields[1] || !fields[1][0]) {
			g_strfreev(fields);
			continue;
		}
		label = fields[0];
		pid = fields[1];
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cfg_profile_combo),
		                               label);
		g_ptr_array_add(cfg_profile_ids, g_strdup(pid));
		count++;
		g_strfreev(fields);
	}
	g_strfreev(lines);
	if (count > 0) {
		/* Caller may re-select a saved recipe profile id afterwards. */
		profile_combo_prefer_default();
	}
}

static void cb_recipes_refresh(GtkWidget *button, gpointer data)
{
	char helper[512];
	char *out;
	char *argv[4];
	int rc;
	char dialog_msg[512];
	char **lines;
	int i;
	int count = 0;
	char *prev = NULL;

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Recipes")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "recipes-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-list failed (exit %d):\n%s", rc,
		         out[0] ? out : "");
		gkrellm_config_message_dialog(_("Recipes"), dialog_msg);
		g_free(out);
		return;
	}

	if (cfg_recipe_combo) {
		if (cfg_recipe_name_entry)
			prev = g_strdup(
			    gtk_entry_get_text(GTK_ENTRY(cfg_recipe_name_entry)));
		cfg_recipes_busy = 1;
		gtk_list_store_clear(GTK_LIST_STORE(
		    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_recipe_combo))));
		lines = g_strsplit(out, "\n", -1);
		for (i = 0; lines && lines[i]; ++i) {
			g_strstrip(lines[i]);
			if (!lines[i][0])
				continue;
			gtk_combo_box_text_append_text(
			    GTK_COMBO_BOX_TEXT(cfg_recipe_combo), lines[i]);
			count++;
		}
		g_strfreev(lines);
		cfg_recipes_busy = 0;
		if (count > 0) {
			if (prev && *prev)
				recipe_combo_select_name(prev);
			else {
				gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_recipe_combo),
				                         0);
				cb_recipe_combo_changed(GTK_COMBO_BOX(cfg_recipe_combo),
				                        NULL);
			}
		}
		g_free(prev);
	}
	/* Do NOT overwrite the editable JSON editor with the list. */
	g_free(out);
}

static void cb_recipes_load_profiles(GtkWidget *button, gpointer data)
{
	char helper[512];
	char image[320];
	char *out;
	char *argv[8];
	int argc;
	int rc;
	gchar *body;

	(void)button;
	(void)data;
	image[0] = '\0';
	body = text_view_get(cfg_recipes_view);
	if (body && body[0] == '{')
		json_extract_string_after(body, NULL, "image", image, sizeof(image));
	g_free(body);
	if (!image[0] && cfg_local_image_entry) {
		const gchar *loc =
		    gtk_entry_get_text(GTK_ENTRY(cfg_local_image_entry));
		if (loc && *loc)
			g_strlcpy(image, loc, sizeof(image));
	}
	if (!image[0]) {
		gkrellm_config_message_dialog(
		    _("Load profiles"),
		    _("No image in the recipe editor.\n"
		      "Select a recipe (or Make recipe / Load) first."));
		return;
	}
	if (helper_missing_dialog(_("Load profiles")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);

	/* Fast path: cached profiles from Local → Profiles. */
	argc = 0;
	argv[argc++] = helper;
	argv[argc++] = "profiles-list";
	argv[argc++] = "--image";
	argv[argc++] = image;
	argv[argc++] = "--cached";
	argv[argc++] = "--pick";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc == 0 && out[0]) {
		profile_combo_populate_from_pick(out);
		g_free(out);
		gkrellm_config_message_dialog(
		    _("Load profiles"),
		    _("Loaded cached profiles into the Profile list.\n"
		      "Pick one — Start will use it.\n"
		      "(If empty/wrong, re-run Local → Profiles first.)"));
		return;
	}

	gkrellm_config_message_dialog(
	    _("Load profiles"),
	    _("No cached profiles — will run list-model-profiles now "
	      "(1–2 minutes). Wait for the next dialog."));
	argc = 0;
	argv[argc++] = helper;
	argv[argc++] = "profiles-list";
	argv[argc++] = "--image";
	argv[argc++] = image;
	argv[argc++] = "--pick";
	argv[argc] = NULL;
	rc = run_helper(argv, NULL, HELPER_PROFILES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0) {
		gkrellm_config_message_dialog(
		    _("Load profiles"),
		    out[0] ? out : "profiles-list failed");
		g_free(out);
		return;
	}
	profile_combo_populate_from_pick(out);
	g_free(out);
	gkrellm_config_message_dialog(
	    _("Load profiles"),
	    _("Profiles loaded into the list. Pick one, Save if you want it "
	      "in JSON, then Start."));
}

static void cb_recipes_preset(GtkWidget *button, gpointer data)
{
	char helper[512];
	char out[4096];
	char *argv[6];
	int rc;
	char dialog_msg[4300];
	char name_buf[128];

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Install Spark preset")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "recipes-preset-install";
	argv[2] = "--preset";
	argv[3] = "nemotron_spark";
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-preset-install failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Install Spark preset"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-preset-install exit %d:\n%s", rc,
		         out[0] ? out : "");
		gkrellm_config_message_dialog(_("Install Spark preset"), dialog_msg);
		return;
	}
	name_buf[0] = '\0';
	json_extract_string_after(out, NULL, "name", name_buf, sizeof(name_buf));
	cb_recipes_refresh(NULL, NULL);
	if (name_buf[0])
		recipe_combo_select_name(name_buf);
	snprintf(dialog_msg, sizeof(dialog_msg),
	         "Installed example recipe%s%s into the list and editor.\n\n"
	         "This does NOT start a container.\n"
	         "Edit JSON / pick Profile → Save → Start to run it.\n\n%s",
	         name_buf[0] ? " " : "", name_buf[0] ? name_buf : "",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Install Spark preset"), dialog_msg);
}

static void cb_recipes_get(GtkWidget *button, gpointer data)
{
	const gchar *name = "";

	(void)button;
	(void)data;
	if (cfg_recipe_combo) {
		gchar *sel =
		    gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cfg_recipe_combo));
		if (sel && *sel) {
			recipes_load_named(sel);
			g_free(sel);
			return;
		}
		g_free(sel);
	}
	if (cfg_recipe_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_name_entry));
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Load recipe"),
		    _("Select a recipe from the list first."));
		return;
	}
	recipes_load_named(name);
}

static void cb_recipes_save_editor(GtkWidget *button, gpointer data)
{
	char helper[512];
	char out[4096];
	char *argv[5];
	int rc;
	char dialog_msg[4300];
	gchar *body;
	gchar *merged = NULL;
	char name_buf[128];
	const gchar *profile = "";

	(void)button;
	(void)data;
	/* Profile combo/entry → top-level "profile" in JSON before save. */
	profile_entry_sync_from_combo();
	if (cfg_recipe_profile_entry)
		profile = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_profile_entry));
	if ((!profile || !*profile) && profile_id_from_combo())
		profile = profile_id_from_combo();

	body = text_view_get(cfg_recipes_view);
	if (!body || !g_strstrip(body)[0] || body[0] != '{') {
		gkrellm_config_message_dialog(
		    _("Save recipe"),
		    _("Editor must contain a recipe JSON object.\n"
		      "Load a recipe first (or Make recipe), then edit fields "
		      "like env, args, host_port, profile, shm_size."));
		g_free(body);
		return;
	}
	if (profile && *profile && !profile_id_is_placeholder(profile)) {
		merged = json_set_string_field(body, "profile", profile);
		g_free(body);
		body = merged;
		text_view_set(cfg_recipes_view, body);
	}
	if (helper_missing_dialog(_("Save recipe")) < 0) {
		g_free(body);
		return;
	}
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "recipes-save";
	argv[2] = "--stdin";
	argv[3] = "--json";
	argv[4] = NULL;
	rc = run_helper(argv, body, HELPER_RECIPES_TIMEOUT_SEC, out, sizeof(out));
	g_free(body);
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-save failed (exit %d):\n%s\n\n"
		         "Fix JSON (name, image, host_port required) and try again.",
		         rc, out[0] ? out : "spawn/timeout/invalid JSON");
		gkrellm_config_message_dialog(_("Save recipe"), dialog_msg);
		return;
	}
	name_buf[0] = '\0';
	json_extract_string_after(out, NULL, "name", name_buf, sizeof(name_buf));
	if (name_buf[0] && cfg_recipe_name_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_recipe_name_entry), name_buf);
	snprintf(dialog_msg, sizeof(dialog_msg),
	         "Recipe saved%s%s.\n"
	         "Profile is stored as top-level \"profile\" "
	         "(→ NIM_MODEL_PROFILE on Start).\n"
	         "You can Start it now.\n\n%s",
	         name_buf[0] ? ": " : "", name_buf[0] ? name_buf : "",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Save recipe"), dialog_msg);
	cb_recipes_refresh(NULL, NULL);
	if (name_buf[0])
		recipes_load_named(name_buf);
}

static void cb_recipes_export(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	char helper[512];
	char *out;
	char *argv[6];
	int rc;
	char dialog_msg[512];

	(void)button;
	(void)data;
	if (cfg_recipe_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_name_entry));
	if (!name || !*name) {
		gkrellm_config_message_dialog(_("Export preview"),
		                              _("Enter a recipe name first."));
		return;
	}
	if (helper_missing_dialog(_("Export preview")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "recipes-export";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-export failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		text_view_set(cfg_recipes_view, dialog_msg);
		gkrellm_config_message_dialog(_("Export preview"), dialog_msg);
	} else if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "recipes-export exit %d:\n%s", rc, out[0] ? out : "");
		text_view_set(cfg_recipes_view, out[0] ? out : dialog_msg);
		gkrellm_config_message_dialog(_("Export preview"), dialog_msg);
	} else {
		text_view_set(cfg_recipes_view,
		              out[0] ? out : "(empty export)");
	}
	g_free(out);
}

static void cb_recipes_start(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	const gchar *profile = "";
	char helper[512];
	char out[4096];
	char *argv[10];
	int argc = 0;
	int rc;

	(void)button;
	(void)data;
	if (cfg_recipe_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_name_entry));
	/* Prefer Profile id entry; fall back to combo selection. */
	profile_entry_sync_from_combo();
	if (cfg_recipe_profile_entry)
		profile = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_profile_entry));
	if ((!profile || !*profile) && profile_id_from_combo())
		profile = profile_id_from_combo();
	if (!name || !*name) {
		gkrellm_config_message_dialog(_("Start"),
		                              _("Select a recipe first."));
		return;
	}
	if (!profile || !*profile || profile_id_is_placeholder(profile)) {
		gkrellm_config_message_dialog(
		    _("Start"),
		    _("Pick a real Profile from the list first.\n"
		      "(01234567… is only a demo placeholder — not a NIM profile.)\n"
		      "If the list is empty: Load profiles."));
		return;
	}
	if (helper_missing_dialog(_("Start")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[argc++] = helper;
	argv[argc++] = "start";
	argv[argc++] = "--recipe";
	argv[argc++] = (char *)name;
	argv[argc++] = "--profile";
	argv[argc++] = (char *)profile;
	argv[argc++] = "--json";
	argv[argc] = NULL;
	rc = run_helper_ex(argv, NULL, HELPER_START_TIMEOUT_SEC, 0, out,
	                   sizeof(out));
	if (rc < 0) {
		helper_short_dialog(
		    _("Start"),
		    _("Start timed out — container may still be starting.\n"
		      "Instances → Refresh / Logs"),
		    out);
		return;
	}
	if (rc != 0) {
		helper_short_dialog(_("Start"), _("Start failed:"), out);
		return;
	}
	if (cfg_instance_name_entry && name && *name)
		gtk_entry_set_text(GTK_ENTRY(cfg_instance_name_entry), name);
	cb_instances_refresh(NULL, NULL);
	if (name && *name)
		instance_combo_select_name(name);
	gkrellm_config_message_dialog(
	    _("Start"),
	    _("Container started (uses local image if already pulled — "
	      "no docker pull).\n\n"
	      "Warming / Logs = model weights download or engine boot.\n"
	      "Instances → Logs until Ready, then Use on dock."));
}

static void instance_names_clear(void)
{
	guint i;

	if (!cfg_instance_names)
		return;
	for (i = 0; i < cfg_instance_names->len; ++i)
		g_free(g_ptr_array_index(cfg_instance_names, i));
	g_ptr_array_set_size(cfg_instance_names, 0);
}

static const char *instance_selected_name(void)
{
	gint idx;
	const gchar *from_entry = NULL;

	if (cfg_instance_combo && cfg_instance_names && cfg_instance_names->len) {
		idx = gtk_combo_box_get_active(GTK_COMBO_BOX(cfg_instance_combo));
		if (idx >= 0 && (guint)idx < cfg_instance_names->len)
			return (const char *)g_ptr_array_index(cfg_instance_names, idx);
	}
	if (cfg_instance_name_entry)
		from_entry = gtk_entry_get_text(GTK_ENTRY(cfg_instance_name_entry));
	if (from_entry && *from_entry)
		return from_entry;
	return "";
}

static void instance_combo_select_name(const char *want)
{
	guint i;

	if (!cfg_instance_combo || !cfg_instance_names || !want || !*want)
		return;
	for (i = 0; i < cfg_instance_names->len; ++i) {
		const char *n = (const char *)g_ptr_array_index(cfg_instance_names, i);
		if (n && strcmp(n, want) == 0) {
			cfg_instances_busy = 1;
			gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_instance_combo),
			                         (gint)i);
			cfg_instances_busy = 0;
			if (cfg_instance_name_entry)
				gtk_entry_set_text(GTK_ENTRY(cfg_instance_name_entry),
				                   want);
			return;
		}
	}
}

static void cb_instance_combo_changed(GtkComboBox *combo, gpointer data)
{
	const char *name;

	(void)combo;
	(void)data;
	if (cfg_instances_busy)
		return;
	name = instance_selected_name();
	if (name && *name && cfg_instance_name_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_instance_name_entry), name);
	/* If live logs were running, switch follow target and repaint once. */
	if (cfg_logs_follow && name && *name) {
		if (strcmp(cfg_logs_live_name, name) != 0) {
			logs_seen_clear();
			g_strlcpy(cfg_logs_live_name, name, sizeof(cfg_logs_live_name));
			instance_logs_fetch(name, 1);
		}
	}
}

static void instance_combo_populate_from_pick(const char *pick_text,
                                             const char *prefer)
{
	char **lines;
	int i;
	int count = 0;

	if (!cfg_instance_combo)
		return;
	if (!cfg_instance_names)
		cfg_instance_names = g_ptr_array_new();
	instance_names_clear();
	cfg_instances_busy = 1;
	gtk_list_store_clear(GTK_LIST_STORE(
	    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_instance_combo))));

	lines = g_strsplit(pick_text ? pick_text : "", "\n", -1);
	for (i = 0; lines && lines[i]; ++i) {
		char **fields;
		const char *name;
		const char *state;
		char label[256];

		g_strstrip(lines[i]);
		if (!lines[i][0])
			continue;
		fields = g_strsplit(lines[i], "\t", 2);
		if (!fields || !fields[0] || !fields[0][0]) {
			g_strfreev(fields);
			continue;
		}
		name = fields[0];
		state = fields[1] ? fields[1] : "?";
		snprintf(label, sizeof(label), "%s  [%s]", name, state);
		gtk_combo_box_text_append_text(
		    GTK_COMBO_BOX_TEXT(cfg_instance_combo), label);
		g_ptr_array_add(cfg_instance_names, g_strdup(name));
		count++;
		g_strfreev(fields);
	}
	g_strfreev(lines);
	cfg_instances_busy = 0;

	if (count > 0) {
		if (prefer && *prefer)
			instance_combo_select_name(prefer);
		else {
			gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_instance_combo), 0);
			cb_instance_combo_changed(GTK_COMBO_BOX(cfg_instance_combo),
			                          NULL);
		}
	} else if (cfg_instance_name_entry) {
		gtk_entry_set_text(GTK_ENTRY(cfg_instance_name_entry), "");
	}
}

/* Quiet: refresh registry state + combo labels (no dialogs). Used by live logs. */
static void instance_state_and_combo_quiet(const char *name)
{
	char helper[512];
	char *out;
	char *argv[6];
	char prefer[160];

	if (!name || !*name)
		return;
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0)
		return;
	g_strlcpy(prefer, name, sizeof(prefer));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instance-refresh";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = "--json";
	argv[5] = NULL;
	(void)run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                 HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instances-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	if (run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	               HELPER_OUT_MAX) == 0)
		instance_combo_populate_from_pick(out, prefer);
	g_free(out);
}

static void cb_instances_refresh(GtkWidget *button, gpointer data)
{
	char helper[512];
	char *out;
	char *out_pick;
	char *argv[4];
	int rc;
	char dialog_msg[512];
	char prefer[160];

	(void)button;
	(void)data;
	logs_live_stop();
	if (helper_missing_dialog(_("Instances")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	prefer[0] = '\0';
	{
		const char *cur = instance_selected_name();
		if (cur && *cur)
			g_strlcpy(prefer, cur, sizeof(prefer));
	}

	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instances-list";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "instances-list failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		text_view_set(cfg_instances_view, dialog_msg);
		gkrellm_config_message_dialog(_("Instances"), dialog_msg);
		g_free(out);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "instances-list exit %d:\n%s", rc, out[0] ? out : "");
		text_view_set(cfg_instances_view, out[0] ? out : dialog_msg);
		gkrellm_config_message_dialog(_("Instances"), dialog_msg);
		g_free(out);
		return;
	}
	text_view_set(cfg_instances_view,
	              out[0] ? out : "(no instances registered)");
	g_free(out);

	out_pick = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instances-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out_pick,
	                HELPER_OUT_MAX);
	if (rc == 0)
		instance_combo_populate_from_pick(out_pick, prefer);
	g_free(out_pick);
}

static void cb_instance_named_op(const char *title, const char *subcommand,
                                 int timeout_sec, int kill_on_timeout)
{
	const gchar *name = "";
	char helper[512];
	char out[4096];
	char *argv[6];
	int rc;
	char dialog_msg[4300];
	gchar *title_copy;

	title_copy = g_strdup(title ? title : "instance");
	name = instance_selected_name();
	if (!name || !*name) {
		gkrellm_config_message_dialog(title_copy,
		                              _("Select an instance from the list."));
		g_free(title_copy);
		return;
	}
	if (helper_missing_dialog(title_copy) < 0) {
		g_free(title_copy);
		return;
	}
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = (char *)subcommand;
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper_ex(argv, NULL, timeout_sec, kill_on_timeout, out,
	                   sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "%s failed:\n%s", subcommand,
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(title_copy, dialog_msg);
		g_free(title_copy);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "%s exit %d:\n%s", subcommand, rc,
		         out[0] ? out : "");
		gkrellm_config_message_dialog(title_copy, dialog_msg);
		g_free(title_copy);
		return;
	}
	snprintf(dialog_msg, sizeof(dialog_msg), "%s OK.\n%s", subcommand,
	         out[0] ? out : "");
	gkrellm_config_message_dialog(title_copy, dialog_msg);
	g_free(title_copy);
	cb_instances_refresh(NULL, NULL);
}

static void cb_instance_stop(GtkWidget *button, gpointer data)
{
	(void)button;
	(void)data;
	cb_instance_named_op(_("Stop"), "stop", HELPER_STOP_TIMEOUT_SEC, 1);
}

static void cb_instance_restart(GtkWidget *button, gpointer data)
{
	(void)button;
	(void)data;
	cb_instance_named_op(_("Restart"), "restart", HELPER_START_TIMEOUT_SEC, 0);
}

static void cb_instance_refresh_state(GtkWidget *button, gpointer data)
{
	(void)button;
	(void)data;
	cb_instance_named_op(_("Refresh state"), "instance-refresh",
	                     HELPER_INSTANCES_TIMEOUT_SEC, 1);
}

static void cb_instance_recreate(GtkWidget *button, gpointer data)
{
	(void)button;
	(void)data;
	/* Stop+rm container, then start again from the linked recipe (new args/env). */
	cb_instance_named_op(_("Recreate"), "recreate", HELPER_START_TIMEOUT_SEC, 0);
}

static int instance_logs_fetch(const char *name, int quiet)
{
	char helper[512];
	char *out;
	char *argv[8];
	int rc;
	char header[220];

	if (!name || !*name)
		return -1;
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0) {
		if (!quiet)
			helper_missing_dialog(_("Logs"));
		return -1;
	}
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "logs";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = "--tail";
	argv[5] = "200";
	argv[6] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0) {
		if (!quiet)
			helper_short_dialog(_("Logs"), _("Could not fetch logs:"), out);
		g_free(out);
		return -1;
	}

	if (!cfg_logs_live_primed) {
		snprintf(header, sizeof(header),
		         _("—— live logs: %s (follow 1s; Refresh stops) ——\n"), name);
		{
			GString *body = g_string_new(header);
			g_string_append(body, out[0] ? out : "(empty)");
			text_view_set_tail(cfg_instances_view, body->str);
			g_string_free(body, TRUE);
		}
		logs_seen_ingest_all(out);
		cfg_logs_live_primed = 1;
	} else {
		/* Append-only — never set_text (avoids scroll jump / flicker). */
		logs_append_unseen(cfg_instances_view, out);
	}
	g_free(out);
	return 0;
}

static gboolean cb_logs_live_tick(gpointer data)
{
	(void)data;
	/* Prefer follow flag — do not die if source id was rewritten. */
	if (!cfg_logs_follow || !cfg_logs_live_name[0])
		return FALSE;
	if (!cfg_instances_view)
		return FALSE;
	/* Do not stop on !mapped — notebook pages / scrolled windows flake on GTK2. */
	instance_logs_fetch(cfg_logs_live_name, 1);
	return TRUE; /* keep following */
}

static void logs_live_start(const char *name)
{
	if (!name || !*name)
		return;
	/* Do not clear seen/primed — caller already painted the first frame. */
	if (cfg_logs_live_id) {
		g_source_remove(cfg_logs_live_id);
		cfg_logs_live_id = 0;
	}
	g_strlcpy(cfg_logs_live_name, name, sizeof(cfg_logs_live_name));
	cfg_logs_follow = 1;
	/* 1s poll — g_timeout_add is more reliable than add_seconds on some GTK2. */
	cfg_logs_live_id = g_timeout_add(1000, cb_logs_live_tick, NULL);
}

static void cb_instance_logs(GtkWidget *button, gpointer data)
{
	const gchar *name = "";

	(void)button;
	(void)data;
	name = instance_selected_name();
	if ((!name || !*name) && cfg_recipe_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_recipe_name_entry));
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Logs"),
		    _("Select an instance from the list."));
		return;
	}
	logs_live_stop();
	if (instance_logs_fetch(name, 0) == 0) {
		logs_live_start(name);
		/* One-shot state update — never from the follow tick (rebuilds combo). */
		instance_state_and_combo_quiet(name);
	}
}

static void cb_instance_adopt(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	char helper[512];
	char out[4096];
	char *argv[6];
	int rc;
	char dialog_msg[4300];

	(void)button;
	(void)data;
	if (cfg_adopt_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_adopt_name_entry));
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Adopt"),
		    _("Enter a container name or id to adopt."));
		return;
	}
	if (helper_missing_dialog(_("Adopt")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "adopt";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "adopt failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Adopt"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "adopt exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Adopt"), dialog_msg);
		return;
	}
	snprintf(dialog_msg, sizeof(dialog_msg), "Adopted.\n%s",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Adopt"), dialog_msg);
	cb_instances_refresh(NULL, NULL);
}

static void cb_orphans_list(GtkWidget *button, gpointer data)
{
	char helper[512];
	char *out;
	char *argv[4];
	int rc;
	char dialog_msg[512];

	(void)button;
	(void)data;
	if (helper_missing_dialog(_("Orphans")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "orphans-list";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "orphans-list failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Orphans"), dialog_msg);
		g_free(out);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "orphans-list exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Orphans"),
		                              dialog_msg[0] ? dialog_msg : out);
		g_free(out);
		return;
	}
	if (strlen(out) < 3500)
		gkrellm_config_message_dialog(_("Orphans"),
		                              out[0] ? out : "(no orphans)");
	else {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "orphans-list OK (%lu bytes). Truncated; re-run:\n"
		         "  gkrellm-nim orphans-list --json",
		         (unsigned long)strlen(out));
		gkrellm_config_message_dialog(_("Orphans"), dialog_msg);
	}
	g_free(out);
}

static void cb_orphans_reclaim(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	char helper[512];
	char out[4096];
	char *argv[6];
	int rc;
	char dialog_msg[4300];

	(void)button;
	(void)data;
	if (cfg_orphan_name_entry)
		name = gtk_entry_get_text(GTK_ENTRY(cfg_orphan_name_entry));
	if ((!name || !*name))
		name = instance_selected_name();
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Reclaim orphan"),
		    _("Enter an orphan container name to reclaim."));
		return;
	}
	if (helper_missing_dialog(_("Reclaim orphan")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	argv[0] = helper;
	argv[1] = "orphans-reclaim";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = "--json";
	argv[5] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "orphans-reclaim failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Reclaim orphan"), dialog_msg);
		return;
	}
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "orphans-reclaim exit %d:\n%s", rc, out[0] ? out : "");
		gkrellm_config_message_dialog(_("Reclaim orphan"), dialog_msg);
		return;
	}
	snprintf(dialog_msg, sizeof(dialog_msg), "Reclaimed.\n%s",
	         out[0] ? out : "");
	gkrellm_config_message_dialog(_("Reclaim orphan"), dialog_msg);
}

static void best_effort_dock_show(const char *name)
{
	char helper[512];
	char out[512];
	char *argv[5];

	if (!name || !*name)
		return;
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0)
		return;
	argv[0] = helper;
	argv[1] = "dock-show";
	argv[2] = "--name";
	argv[3] = (char *)name;
	argv[4] = NULL;
	(void)run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                 sizeof(out));
}

static void cb_instance_use_on_dock(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	char helper[512];
	char *out;
	char *argv[4];
	int rc;
	char scrape[256];
	char disp[64];
	char dialog_msg[512];

	(void)button;
	(void)data;
	name = instance_selected_name();
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Use on dock"),
		    _("Select an instance from the list."));
		return;
	}
	if (helper_missing_dialog(_("Use on dock")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instances-list";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "instances-list failed (exit %d):\n%s", rc,
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Use on dock"), dialog_msg);
		g_free(out);
		return;
	}
	scrape[0] = disp[0] = '\0';
	if (find_instance_fields(out, name, scrape, sizeof(scrape), disp,
	                         sizeof(disp)) < 0 ||
	    !scrape[0]) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "No scrape_url found for instance '%s'.\n"
		         "Refresh the list and check the name.",
		         name);
		gkrellm_config_message_dialog(_("Use on dock"), dialog_msg);
		g_free(out);
		return;
	}
	g_strlcpy(P.base_url, scrape, sizeof(P.base_url));
	normalize_loopback_url(P.base_url);
	if (cfg_url_entry)
		gtk_entry_set_text(GTK_ENTRY(cfg_url_entry), P.base_url);
	if (!disp[0]) {
		char full[128];
		if (fetch_model_from_v1(P.base_url, full, sizeof(full)) == 0)
			shorten_model(full, disp, sizeof(disp));
		else if (strstr(name, "nano"))
			g_strlcpy(disp, "Nemotron Nano", sizeof(disp));
		else if (strstr(name, "super"))
			g_strlcpy(disp, "Nemotron Super", sizeof(disp));
	}
	if (disp[0]) {
		g_strlcpy(P.display_name, disp, sizeof(P.display_name));
		if (cfg_display_name_entry)
			gtk_entry_set_text(GTK_ENTRY(cfg_display_name_entry),
			                   P.display_name);
	}
	g_strlcpy(P.slots[0].name, name, sizeof(P.slots[0].name));
	sync_slot0_from_primary();
	best_effort_dock_show(name);
	if (cfg_conn_link_label)
		cb_refresh_secrets_status(NULL, NULL);
	snprintf(dialog_msg, sizeof(dialog_msg),
	         "Dock scrape URL set to:\n%s\nDisplay name: %s",
	         P.base_url,
	         P.display_name[0] ? P.display_name : "(unchanged)");
	gkrellm_config_message_dialog(_("Use on dock"), dialog_msg);
	g_free(out);
}

static void cb_instance_add_to_dock(GtkWidget *button, gpointer data)
{
	const gchar *name = "";
	char helper[512];
	char *out;
	char *argv[4];
	int rc, slot = -1, i;
	char scrape[256];
	char disp[64];
	char dialog_msg[512];

	(void)button;
	(void)data;
	name = instance_selected_name();
	if (!name || !*name) {
		gkrellm_config_message_dialog(
		    _("Add to dock"),
		    _("Select an instance from the list."));
		return;
	}
	if (helper_missing_dialog(_("Add to dock")) < 0)
		return;
	find_gkrellm_nim(helper, sizeof(helper));
	out = g_malloc(HELPER_OUT_MAX);
	argv[0] = helper;
	argv[1] = "instances-list";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_INSTANCES_TIMEOUT_SEC, out,
	                HELPER_OUT_MAX);
	if (rc != 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "instances-list failed (exit %d):\n%s", rc,
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Add to dock"), dialog_msg);
		g_free(out);
		return;
	}
	scrape[0] = disp[0] = '\0';
	if (find_instance_fields(out, name, scrape, sizeof(scrape), disp,
	                         sizeof(disp)) < 0 ||
	    !scrape[0]) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "No scrape_url found for instance '%s'.\n"
		         "Refresh the list and check the name.",
		         name);
		gkrellm_config_message_dialog(_("Add to dock"), dialog_msg);
		g_free(out);
		return;
	}
	for (i = 1; i < MAX_DOCK_SLOTS; ++i) {
		if (!P.slots[i].base_url[0]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		gkrellm_config_message_dialog(
		    _("Add to dock"),
		    _("No free dock slots (1–3). Clear extra slots first."));
		g_free(out);
		return;
	}
	g_strlcpy(P.slots[slot].base_url, scrape,
	          sizeof(P.slots[slot].base_url));
	normalize_loopback_url(P.slots[slot].base_url);
	g_strlcpy(P.slots[slot].name, name, sizeof(P.slots[slot].name));
	if (!disp[0]) {
		char full[128];
		if (fetch_model_from_v1(P.slots[slot].base_url, full,
		                        sizeof(full)) == 0)
			shorten_model(full, disp, sizeof(disp));
		else if (strstr(name, "nano"))
			g_strlcpy(disp, "Nemotron Nano", sizeof(disp));
		else if (strstr(name, "super"))
			g_strlcpy(disp, "Nemotron Super", sizeof(disp));
	}
	if (disp[0])
		g_strlcpy(P.slots[slot].display_name, disp,
		          sizeof(P.slots[slot].display_name));
	else
		g_strlcpy(P.slots[slot].display_name, name,
		          sizeof(P.slots[slot].display_name));
	P.slots[slot].enabled = 1;
	P.slots[slot].have_prev = 0;
	best_effort_dock_show(name);
	if (P.ui_built && P.main_vbox) {
		rebuild_llm_ui();
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "Added '%s' to dock slot %d:\n%s", name, slot,
		         P.slots[slot].base_url);
	} else {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "Saved '%s' to dock slot %d:\n%s\n"
		         "Apply config or restart gkrellm to show panels.",
		         name, slot, P.slots[slot].base_url);
	}
	gkrellm_config_message_dialog(_("Add to dock"), dialog_msg);
	g_free(out);
}

static void cb_clear_extra_slots(GtkWidget *button, gpointer data)
{
	int i;

	(void)button;
	(void)data;
	for (i = 1; i < MAX_DOCK_SLOTS; ++i) {
		destroy_compact_slot_ui(&P.slots[i]);
		P.slots[i].base_url[0] = '\0';
		P.slots[i].display_name[0] = '\0';
		P.slots[i].name[0] = '\0';
		P.slots[i].enabled = 0;
		P.slots[i].have_prev = 0;
		memset(&P.slots[i].last, 0, sizeof(P.slots[i].last));
		memset(&P.slots[i].prev, 0, sizeof(P.slots[i].prev));
	}
	if (P.ui_built && P.main_vbox)
		rebuild_llm_ui();
	gkrellm_config_message_dialog(
	    _("Clear extra slots"),
	    _("Cleared dock slots 1–3. Slot 0 (Connection URL) unchanged."));
}

static int save_token_via_helper_or_file(const char *subcommand,
                                         const char *filename,
                                         const char *value, char *msg,
                                         size_t msg_sz)
{
	char helper[512];
	char out[512];
	char *argv[4];
	int rc;

	if (!value || !*value) {
		g_strlcpy(msg, "empty token skipped", msg_sz);
		return 0;
	}

	if (find_gkrellm_nim(helper, sizeof(helper)) == 0) {
		argv[0] = helper;
		argv[1] = (char *)subcommand;
		argv[2] = NULL;
		rc = run_helper(argv, value, 10, out, sizeof(out));
		if (rc == 0) {
			snprintf(msg, msg_sz, "%s: saved via gkrellm-nim", filename);
			return 0;
		}
		/* fall through to direct write */
	}

	if (write_secret_file(filename, value) == 0) {
		snprintf(msg, msg_sz, "%s: written to ~/.config/gkrellm-dock/",
		         filename);
		return 0;
	}
	snprintf(msg, msg_sz, "%s: failed to write secret (%s)", filename,
	         strerror(errno));
	return -1;
}

static void cb_refresh_secrets_status(GtkWidget *button, gpointer data)
{
	char helper[512];
	char out[2048];
	char *argv[4];
	int rc;
	char ngc_mask[64];
	char hf_mask[64];
	char label[220];
	int ngc_set = 0;
	int hf_set = 0;
	char model_full[128];
	char model_short[64];
	const gchar *url_txt;

	(void)button;
	(void)data;
	if (cfg_url_entry) {
		url_txt = gtk_entry_get_text(GTK_ENTRY(cfg_url_entry));
		if (url_txt && *url_txt) {
			g_strlcpy(P.base_url, url_txt, sizeof(P.base_url));
			normalize_loopback_url(P.base_url);
			gtk_entry_set_text(GTK_ENTRY(cfg_url_entry), P.base_url);
		}
	}
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0) {
		if (cfg_ngc_status_label)
			gtk_label_set_text(GTK_LABEL(cfg_ngc_status_label),
			                   _("NGC: (helper missing)"));
		if (cfg_hf_status_label)
			gtk_label_set_text(GTK_LABEL(cfg_hf_status_label),
			                   _("HF: (helper missing)"));
		if (cfg_conn_link_label)
			gtk_label_set_text(GTK_LABEL(cfg_conn_link_label),
			                   _("Link: (helper missing)"));
		return;
	}
	argv[0] = helper;
	argv[1] = "secrets-status";
	argv[2] = "--json";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_RECIPES_TIMEOUT_SEC, out, sizeof(out));
	ngc_mask[0] = hf_mask[0] = '\0';
	if (rc == 0 && out[0] == '{') {
		json_extract_string_after(out, NULL, "ngc_api_key", ngc_mask,
		                          sizeof(ngc_mask));
		json_extract_string_after(out, NULL, "hf_token", hf_mask,
		                          sizeof(hf_mask));
		/* booleans as true/false in JSON — crude detect */
		if (strstr(out, "\"ngc_api_key_set\": true") ||
		    strstr(out, "\"ngc_api_key_set\":true"))
			ngc_set = 1;
		if (strstr(out, "\"hf_token_set\": true") ||
		    strstr(out, "\"hf_token_set\":true"))
			hf_set = 1;
		if (!ngc_set && ngc_mask[0])
			ngc_set = 1;
		if (!hf_set && hf_mask[0])
			hf_set = 1;
	}
	if (cfg_ngc_status_label) {
		if (ngc_set)
			snprintf(label, sizeof(label), _("NGC: saved %s"),
			         ngc_mask[0] ? ngc_mask : "••••");
		else
			snprintf(label, sizeof(label), _("NGC: not set (required for pull)"));
		gtk_label_set_text(GTK_LABEL(cfg_ngc_status_label), label);
	}
	if (cfg_hf_status_label) {
		if (hf_set)
			snprintf(label, sizeof(label), _("HF: saved %s (optional)"),
			         hf_mask[0] ? hf_mask : "••••");
		else
			snprintf(label, sizeof(label),
			         _("HF: not set (optional — only if image needs HF)"));
		gtk_label_set_text(GTK_LABEL(cfg_hf_status_label), label);
	}
	if (cfg_conn_link_label) {
		model_full[0] = model_short[0] = '\0';
		if (P.base_url[0] &&
		    fetch_model_from_v1(P.base_url, model_full,
		                       sizeof(model_full)) == 0) {
			shorten_model(model_full, model_short, sizeof(model_short));
			snprintf(label, sizeof(label),
			         _("Link: %s → %s (%s)"), P.base_url, model_full,
			         model_short);
			if (!P.display_name[0] && model_short[0]) {
				g_strlcpy(P.display_name, model_short,
				          sizeof(P.display_name));
				if (cfg_display_name_entry)
					gtk_entry_set_text(GTK_ENTRY(cfg_display_name_entry),
					                   P.display_name);
			}
		} else if (P.base_url[0]) {
			snprintf(label, sizeof(label),
			         _("Link: %s → (unreachable — Start instance / "
			           "Use on dock)"),
			         P.base_url);
		} else {
			snprintf(label, sizeof(label),
			         _("Link: (no Base URL)"));
		}
		gtk_label_set_text(GTK_LABEL(cfg_conn_link_label), label);
	}
}

static void docs_release_combo_populate(void)
{
	char helper[512];
	char out[2048];
	char *argv[4];
	int rc;
	char **lines;
	int i;
	const char *want;

	if (!cfg_docs_release_combo)
		return;
	want = P.docs_release[0] ? P.docs_release : DEFAULT_DOCS_RELEASE;
	if (find_gkrellm_nim(helper, sizeof(helper)) < 0) {
		gtk_combo_box_text_append_text(
		    GTK_COMBO_BOX_TEXT(cfg_docs_release_combo), want);
		gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_docs_release_combo), 0);
		return;
	}
	argv[0] = helper;
	argv[1] = "docs-seeds-list";
	argv[2] = "--pick";
	argv[3] = NULL;
	rc = run_helper(argv, NULL, HELPER_CATALOG_TIMEOUT_SEC, out, sizeof(out));
	gtk_list_store_clear(GTK_LIST_STORE(
	    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_docs_release_combo))));
	if (rc == 0 && out[0]) {
		lines = g_strsplit(out, "\n", -1);
		for (i = 0; lines && lines[i]; ++i) {
			g_strstrip(lines[i]);
			if (!lines[i][0])
				continue;
			gtk_combo_box_text_append_text(
			    GTK_COMBO_BOX_TEXT(cfg_docs_release_combo), lines[i]);
		}
		g_strfreev(lines);
	}
	if (!gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_docs_release_combo)) ||
	    !gtk_tree_model_iter_n_children(
	        gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_docs_release_combo)),
	        NULL)) {
		gtk_combo_box_text_append_text(
		    GTK_COMBO_BOX_TEXT(cfg_docs_release_combo), want);
	}
	/* select want */
	{
		GtkTreeModel *model =
		    gtk_combo_box_get_model(GTK_COMBO_BOX(cfg_docs_release_combo));
		GtkTreeIter iter;
		gint idx = 0;
		gint found = -1;
		if (model && gtk_tree_model_get_iter_first(model, &iter)) {
			do {
				gchar *txt = NULL;
				gtk_tree_model_get(model, &iter, 0, &txt, -1);
				if (txt && strcmp(txt, want) == 0)
					found = idx;
				g_free(txt);
				idx++;
			} while (found < 0 && gtk_tree_model_iter_next(model, &iter));
		}
		gtk_combo_box_set_active(GTK_COMBO_BOX(cfg_docs_release_combo),
		                         found >= 0 ? found : 0);
	}
	if (cfg_docs_release_entry) {
		gchar *sel = gtk_combo_box_text_get_active_text(
		    GTK_COMBO_BOX_TEXT(cfg_docs_release_combo));
		if (sel && *sel)
			gtk_entry_set_text(GTK_ENTRY(cfg_docs_release_entry), sel);
		g_free(sel);
	}
}

static void cb_docs_release_combo_changed(GtkComboBox *combo, gpointer data)
{
	gchar *sel;

	(void)data;
	if (!cfg_docs_release_entry || !combo)
		return;
	sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	if (sel && *sel)
		gtk_entry_set_text(GTK_ENTRY(cfg_docs_release_entry), sel);
	g_free(sel);
}

static void cb_save_tokens(GtkWidget *button, gpointer data)
{
	const gchar *ngc = "";
	const gchar *hf = "";
	char msg_ngc[256], msg_hf[256], combined[640];
	int rc = 0;

	(void)button;
	(void)data;
	if (cfg_ngc_entry)
		ngc = gtk_entry_get_text(GTK_ENTRY(cfg_ngc_entry));
	if (cfg_hf_entry)
		hf = gtk_entry_get_text(GTK_ENTRY(cfg_hf_entry));

	msg_ngc[0] = msg_hf[0] = '\0';
	if (ngc && *ngc) {
		if (save_token_via_helper_or_file("secrets-set-ngc", "ngc_api_key",
		                                 ngc, msg_ngc, sizeof(msg_ngc)) < 0)
			rc = -1;
	} else {
		g_strlcpy(msg_ngc, "NGC: (empty — left unchanged)", sizeof(msg_ngc));
	}
	if (hf && *hf) {
		if (save_token_via_helper_or_file("secrets-set-hf", "hf_token", hf,
		                                 msg_hf, sizeof(msg_hf)) < 0)
			rc = -1;
	} else {
		g_strlcpy(msg_hf, "HF: (empty — left unchanged)", sizeof(msg_hf));
	}

	snprintf(combined, sizeof(combined), "%s\n%s", msg_ngc, msg_hf);
	gkrellm_config_message_dialog(rc == 0 ? _("Save tokens")
	                                      : _("Save tokens — error"),
	                              combined);
	if (rc == 0) {
		if (cfg_ngc_entry && ngc && *ngc)
			gtk_entry_set_text(GTK_ENTRY(cfg_ngc_entry), "");
		if (cfg_hf_entry && hf && *hf)
			gtk_entry_set_text(GTK_ENTRY(cfg_hf_entry), "");
		cb_refresh_secrets_status(NULL, NULL);
	}
}

static void cb_test_login(GtkWidget *button, gpointer data)
{
	char helper[512];
	char out[1024];
	char *argv[3];
	int rc;
	char dialog_msg[1200];

	(void)button;
	(void)data;

	if (find_gkrellm_nim(helper, sizeof(helper)) < 0) {
		gkrellm_config_message_dialog(
		    _("Test NGC login"),
		    _("gkrellm-nim not found on PATH or ~/.local/bin.\n"
		      "Install with ./scripts/install.sh (or build bin/gkrellm-nim)."));
		return;
	}

	argv[0] = helper;
	argv[1] = "login-test";
	argv[2] = NULL;
	rc = run_helper(argv, NULL, HELPER_LOGIN_TIMEOUT_SEC, out, sizeof(out));
	if (rc < 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "gkrellm-nim login-test failed:\n%s",
		         out[0] ? out : "spawn/timeout error");
		gkrellm_config_message_dialog(_("Test NGC login"), dialog_msg);
		return;
	}
	if (rc == 0) {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "NGC login OK.\n%s", out[0] ? out : "");
		gkrellm_config_message_dialog(_("Test NGC login"), dialog_msg);
	} else {
		snprintf(dialog_msg, sizeof(dialog_msg),
		         "NGC login failed (exit %d).\n%s", rc,
		         out[0] ? out : "");
		gkrellm_config_message_dialog(_("Test NGC login"), dialog_msg);
	}
}

static void cb_feat_toggle(GtkWidget *button, gpointer data)
{
	guint bit = GPOINTER_TO_UINT(data);
	gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));

	if (on)
		P.features |= bit;
	else
		P.features &= ~bit;
	rebuild_llm_ui();
}

static void create_plugin_tab(GtkWidget *tab_vbox)
{
	GtkWidget *tabs, *conn, *catalog, *local, *recipes, *instances;
	GtkWidget *opt, *disp, *frame, *btn, *spin;
	GtkWidget *hbox;
	int i;
	int thr[] = {0, 1, 2, 3};
	int cache[] = {6, 5, 16, 24, 10, 19, 20};
	int lat[] = {4, 7, 9, 8, 13, 14, 15, 26};
	int host[] = {11, 12, 17, 18, 25, 21, 22, 23};
	int j, n;

	tabs = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tabs), GTK_POS_TOP);
	gtk_box_pack_start(GTK_BOX(tab_vbox), tabs, TRUE, TRUE, 0);

	conn = gkrellm_gtk_framed_notebook_page(tabs, _(" Connection "));
	cfg_add_entry(conn, &cfg_url_entry, P.base_url, _("Base URL"));
	cfg_add_entry(conn, &cfg_display_name_entry, P.display_name,
	              _("Display name"));
	cfg_conn_link_label = gtk_label_new(_("Link: …"));
	gtk_misc_set_alignment(GTK_MISC(cfg_conn_link_label), 0.0, 0.5);
	gtk_label_set_line_wrap(GTK_LABEL(cfg_conn_link_label), TRUE);
	gtk_box_pack_start(GTK_BOX(conn), cfg_conn_link_label, FALSE, FALSE, 2);
	cfg_add_password_entry(conn, &cfg_ngc_entry,
	                       _("NGC token (paste to update, then Save)"));
	cfg_ngc_status_label = gtk_label_new(_("NGC: …"));
	gtk_misc_set_alignment(GTK_MISC(cfg_ngc_status_label), 0.0, 0.5);
	gtk_box_pack_start(GTK_BOX(conn), cfg_ngc_status_label, FALSE, FALSE, 2);
	cfg_add_password_entry(conn, &cfg_hf_entry,
	                       _("HF token (optional)"));
	cfg_hf_status_label = gtk_label_new(_("HF: …"));
	gtk_misc_set_alignment(GTK_MISC(cfg_hf_status_label), 0.0, 0.5);
	gtk_box_pack_start(GTK_BOX(conn), cfg_hf_status_label, FALSE, FALSE, 2);
	{
		GtkWidget *l = gtk_label_new(_("Docs schema pin"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_docs_release_combo = gtk_combo_box_text_new();
		cfg_docs_release_entry = gtk_entry_new();
		gtk_widget_set_no_show_all(cfg_docs_release_entry, TRUE);
		gtk_widget_hide(cfg_docs_release_entry);
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_docs_release_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(conn), h, FALSE, FALSE, 2);
		gtk_box_pack_start(GTK_BOX(conn), cfg_docs_release_entry, FALSE, FALSE,
		                   0);
		g_signal_connect(G_OBJECT(cfg_docs_release_combo), "changed",
		                 G_CALLBACK(cb_docs_release_combo_changed), NULL);
		cfg_tip(cfg_docs_release_combo,
		        _("Vendored NIM Environment Variables schema version for "
		          "Catalog → Sync docs. Independent of image tag."));
	}
	docs_release_combo_populate();
	gkrellm_gtk_check_button_connected(
	    conn, &cfg_airgap_btn, P.airgap != 0, FALSE, FALSE, 0, NULL, NULL,
	    _("Air-gap / offline (use vendored schema seed)"));
	gkrellm_gtk_button_connected(conn, &btn, FALSE, FALSE, 4, cb_save_tokens,
	                             NULL, _("Save tokens"));
	gkrellm_gtk_button_connected(conn, &btn, FALSE, FALSE, 4, cb_test_login,
	                             NULL, _("Test NGC login"));
	gkrellm_gtk_button_connected(conn, &btn, FALSE, FALSE, 4,
	                             cb_refresh_secrets_status, NULL,
	                             _("Refresh status"));
	cb_refresh_secrets_status(NULL, NULL);

	catalog = gkrellm_gtk_framed_notebook_page(tabs, _(" Catalog "));
	{
		GtkWidget *head = gtk_hbox_new(FALSE, 4);
		cfg_catalog_sync_label = gtk_label_new(_("Last sync: never"));
		gtk_misc_set_alignment(GTK_MISC(cfg_catalog_sync_label), 1.0, 0.5);
		gtk_box_pack_start(GTK_BOX(head), cfg_catalog_sync_label, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(catalog), head, FALSE, FALSE, 2);
	}
	cfg_add_scrolled_text(catalog, &cfg_catalog_view, 100);
	{
		GtkWidget *l = gtk_label_new(_("Model"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_catalog_combo = gtk_combo_box_text_new();
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_catalog_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(catalog), h, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(cfg_catalog_combo), "changed",
		                 G_CALLBACK(cb_catalog_combo_changed), NULL);
	}
	{
		GtkWidget *l = gtk_label_new(_("Version"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_catalog_tag_combo = gtk_combo_box_text_new();
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_catalog_tag_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(catalog), h, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(cfg_catalog_tag_combo), "changed",
		                 G_CALLBACK(cb_catalog_tag_changed), NULL);
		cfg_tip(cfg_catalog_tag_combo,
		        _("Image tag (latest by default). Refresh updates tags for "
		          "the selected model from registry/cache."));
	}
	cfg_add_entry(catalog, &cfg_catalog_image_entry, "",
	              _("Image ref (model+version, or paste custom)"));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(catalog), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_catalog_refresh, NULL, _("Refresh"));
	cfg_tip(btn,
	        _("Sync NIM images from NGC into catalog cache, then reload list."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_catalog_add,
	                             NULL, _("Add"));
	cfg_tip(btn,
	        _("Paste a custom nvcr.io image into Catalog cache. "
	          "Models already listed: use Pull (not Add)."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_docs_sync,
	                             NULL, _("Sync docs"));
	cfg_tip(btn,
	        _("Fetch/cache NIM docs schema for Docs schema pin "
	          "(Connection). Does not pull model images."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_catalog_pull,
	                             NULL, _("Pull"));
	cfg_tip(btn,
	        _("docker pull selected Image ref → appears under Local."));
	catalog_reload_list();

	local = gkrellm_gtk_framed_notebook_page(tabs, _(" Local "));
	cfg_add_scrolled_text(local, &cfg_local_view, 120);
	{
		GtkWidget *l = gtk_label_new(_("Local image"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_local_combo = gtk_combo_box_text_new();
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_local_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(local), h, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(cfg_local_combo), "changed",
		                 G_CALLBACK(cb_local_combo_changed), NULL);
	}
	cfg_add_entry(local, &cfg_local_image_entry, "",
	              _("Image ref (auto from list, or paste custom)"));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(local), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_local_refresh,
	                             NULL, _("Refresh"));
	cfg_tip(btn, _("List local nvcr.io/nim (and related) Docker images."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_local_profiles, NULL, _("Profiles"));
	cfg_tip(btn,
	        _("Run list-model-profiles inside the image (1–2 min). "
	          "Copy a Compatible profile ID into Recipes → Profile."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_local_make_recipe, NULL,
	                             _("Make recipe"));
	cfg_tip(btn,
	        _("Create a launch recipe for this image (Spark defaults, "
	          "bind 127.0.0.1). Then Recipes → Profile → Start."));
	cb_local_refresh(NULL, NULL);

	recipes = gkrellm_gtk_framed_notebook_page(tabs, _(" Recipes "));
	{
		GtkWidget *l = gtk_label_new(_("Recipe"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_recipe_combo = gtk_combo_box_text_new();
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_recipe_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(recipes), h, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(cfg_recipe_combo), "changed",
		                 G_CALLBACK(cb_recipe_combo_changed), NULL);
	}
	cfg_add_scrolled_text_ex(recipes, &cfg_recipes_view, 160, 1);
	cfg_add_entry(recipes, &cfg_recipe_name_entry, "", _("Recipe name"));
	{
		GtkWidget *l = gtk_label_new(_("Profile"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_profile_combo = gtk_combo_box_text_new();
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_profile_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(recipes), h, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(cfg_profile_combo), "changed",
		                 G_CALLBACK(cb_profile_combo_changed), NULL);
	}
	cfg_add_entry(recipes, &cfg_recipe_profile_entry, "",
	              _("Profile id (filled from list; used by Start)"));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(recipes), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_refresh, NULL, _("Refresh"));
	cfg_tip(btn, _("Reload recipe names into the Recipe list (keeps editor)."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_recipes_get,
	                             NULL, _("Load"));
	cfg_tip(btn, _("Reload selected recipe JSON into the editor."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_save_editor, NULL, _("Save"));
	cfg_tip(btn, _("Save edited JSON. Current Profile selection is written "
	               "into top-level \"profile\" (NIM_MODEL_PROFILE)."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_load_profiles, NULL,
	                             _("Load profiles"));
	cfg_tip(btn,
	        _("Fill Profile list from cached Local→Profiles (or discover). "
	          "Then pick a profile before Start."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_start, NULL, _("Start"));
	cfg_tip(btn,
	        _("docker run THIS selected recipe. Not Install Spark preset."));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(recipes), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_preset, NULL,
	                             _("Install Spark preset"));
	cfg_tip(btn,
	        _("Only adds/updates example recipe nemotron-nim. Does not start."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_recipes_export, NULL, _("Export preview"));
	cfg_tip(btn, _("Preview docker run script (no secrets)."));
	cb_recipes_refresh(NULL, NULL);

	instances = gkrellm_gtk_framed_notebook_page(tabs, _(" Instances "));
	cfg_add_scrolled_text(instances, &cfg_instances_view, 100);
	{
		GtkWidget *l = gtk_label_new(_("Instance"));
		GtkWidget *h = gtk_hbox_new(FALSE, 4);
		cfg_instance_combo = gtk_combo_box_text_new();
		cfg_instance_name_entry = gtk_entry_new();
		gtk_widget_set_no_show_all(cfg_instance_name_entry, TRUE);
		gtk_widget_hide(cfg_instance_name_entry);
		gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
		gtk_box_pack_start(GTK_BOX(h), cfg_instance_combo, TRUE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(instances), h, FALSE, FALSE, 2);
		gtk_box_pack_start(GTK_BOX(instances), cfg_instance_name_entry,
		                   FALSE, FALSE, 0);
		g_signal_connect(G_OBJECT(cfg_instance_combo), "changed",
		                 G_CALLBACK(cb_instance_combo_changed), NULL);
	}
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(instances), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instances_refresh, NULL, _("Refresh"));
	cfg_tip(btn, _("List managed NIM instances and fill the Instance list."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_logs, NULL, _("Logs"));
	cfg_tip(btn,
	        _("Follow new log lines only (like tail -f, no flicker). "
	          "Refresh stops live mode."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_stop, NULL, _("Stop"));
	cfg_tip(btn, _("Stop the named instance container."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_restart, NULL, _("Restart"));
	cfg_tip(btn, _("Restart the named instance."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_recreate, NULL, _("Recreate"));
	cfg_tip(btn,
	        _("Force-remove container and start from recipe using the local "
	          "Docker image (--pull=never) and ~/.cache/nim weights. "
	          "Warming = loading shards to GPU, not re-downloading. "
	          "Use Catalog → Pull only when you want a newer image tag."));

	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(instances), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_refresh_state, NULL,
	                             _("Refresh state"));
	cfg_tip(btn, _("Probe logs/health and update instance state."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_use_on_dock, NULL,
	                             _("Use on dock"));
	cfg_tip(btn, _("Point the main dock LLM panel at this instance scrape URL."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_add_to_dock, NULL,
	                             _("Add to dock"));
	cfg_tip(btn, _("Add this instance as compact dock slot 1–3."));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_clear_extra_slots, NULL,
	                             _("Clear extra slots"));
	cb_instances_refresh(NULL, NULL);
	cfg_add_entry(instances, &cfg_adopt_name_entry, "",
	              _("Adopt container name"));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(instances), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_instance_adopt, NULL, _("Adopt"));
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4, cb_orphans_list,
	                             NULL, _("Orphans"));
	cfg_add_entry(instances, &cfg_orphan_name_entry, "",
	              _("Orphan name (reclaim)"));
	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(instances), hbox, FALSE, FALSE, 2);
	gkrellm_gtk_button_connected(hbox, &btn, FALSE, FALSE, 4,
	                             cb_orphans_reclaim, NULL, _("Reclaim"));

	opt = gkrellm_gtk_framed_notebook_page(tabs, _(" Options "));
	gkrellm_gtk_spin_button(opt, &spin, (gfloat)P.chart_max_tps, 5.0, 5000.0,
	                        1.0, 10.0, 0, 70, NULL, NULL, FALSE,
	                        _("Decode chart max (t/s)"));
	cfg_spin_tps = spin;
	gkrellm_gtk_spin_button(opt, &spin, (gfloat)P.chart_max_prefill, 50.0,
	                        100000.0, 10.0, 100.0, 0, 70, NULL, NULL, FALSE,
	                        _("Prefill chart max (t/s)"));
	cfg_spin_prefill = spin;
	gkrellm_gtk_spin_button(opt, &spin, (gfloat)P.timeout_ms, 100.0, 5000.0,
	                        50.0, 100.0, 0, 70, NULL, NULL, FALSE,
	                        _("Scrape timeout (ms)"));
	cfg_spin_timeout = spin;

	disp = gkrellm_gtk_framed_notebook_page(tabs, _(" Display "));
	memset(cfg_feat_btn, 0, sizeof(cfg_feat_btn));

	frame = gkrellm_gtk_framed_vbox(disp, _(" Throughput "), 2, TRUE, 4, 4);
	n = (int)(sizeof(thr) / sizeof(thr[0]));
	for (j = 0; j < n; ++j) {
		i = thr[j];
		gkrellm_gtk_check_button_connected(
		    frame, &btn, (P.features & METRICS[i].bit) != 0, FALSE, FALSE,
		    0, cb_feat_toggle, GUINT_TO_POINTER(METRICS[i].bit),
		    (gchar *)METRICS[i].cfg_label);
		cfg_feat_btn[i] = btn;
	}
	frame = gkrellm_gtk_framed_vbox(disp, _(" Cache / queue "), 2, TRUE, 4, 4);
	n = (int)(sizeof(cache) / sizeof(cache[0]));
	for (j = 0; j < n; ++j) {
		i = cache[j];
		gkrellm_gtk_check_button_connected(
		    frame, &btn, (P.features & METRICS[i].bit) != 0, FALSE, FALSE,
		    0, cb_feat_toggle, GUINT_TO_POINTER(METRICS[i].bit),
		    (gchar *)METRICS[i].cfg_label);
		cfg_feat_btn[i] = btn;
	}
	frame = gkrellm_gtk_framed_vbox(disp, _(" Latency "), 2, TRUE, 4, 4);
	n = (int)(sizeof(lat) / sizeof(lat[0]));
	for (j = 0; j < n; ++j) {
		i = lat[j];
		gkrellm_gtk_check_button_connected(
		    frame, &btn, (P.features & METRICS[i].bit) != 0, FALSE, FALSE,
		    0, cb_feat_toggle, GUINT_TO_POINTER(METRICS[i].bit),
		    (gchar *)METRICS[i].cfg_label);
		cfg_feat_btn[i] = btn;
	}
	frame = gkrellm_gtk_framed_vbox(disp, _(" Host / HTTP / sizes "), 2, TRUE,
	                               4, 4);
	n = (int)(sizeof(host) / sizeof(host[0]));
	for (j = 0; j < n; ++j) {
		i = host[j];
		gkrellm_gtk_check_button_connected(
		    frame, &btn, (P.features & METRICS[i].bit) != 0, FALSE, FALSE,
		    0, cb_feat_toggle, GUINT_TO_POINTER(METRICS[i].bit),
		    (gchar *)METRICS[i].cfg_label);
		cfg_feat_btn[i] = btn;
	}
}

static void apply_plugin_config(void)
{
	const gchar *url;
	const gchar *s;
	guint feats = 0;
	int i;
	gdouble v;

	if (cfg_url_entry) {
		url = gtk_entry_get_text(GTK_ENTRY(cfg_url_entry));
		if (url && *url) {
			strncpy(P.base_url, url, sizeof(P.base_url) - 1);
			P.base_url[sizeof(P.base_url) - 1] = '\0';
			strip_url_slash();
		}
	}
	if (cfg_display_name_entry) {
		s = gtk_entry_get_text(GTK_ENTRY(cfg_display_name_entry));
		if (s)
			g_strlcpy(P.display_name, s, sizeof(P.display_name));
		else
			P.display_name[0] = '\0';
	}
	sync_slot0_from_primary();
	if (cfg_docs_release_entry) {
		s = gtk_entry_get_text(GTK_ENTRY(cfg_docs_release_entry));
		if (s && *s)
			g_strlcpy(P.docs_release, s, sizeof(P.docs_release));
		else
			g_strlcpy(P.docs_release, DEFAULT_DOCS_RELEASE,
			          sizeof(P.docs_release));
	}
	if (cfg_airgap_btn)
		P.airgap = gtk_toggle_button_get_active(
		               GTK_TOGGLE_BUTTON(cfg_airgap_btn))
		               ? 1
		               : 0;
	if (cfg_spin_tps) {
		v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cfg_spin_tps));
		P.chart_max_tps = (guint)v;
		if (P.chart_max_tps < 5)
			P.chart_max_tps = 5;
	}
	if (cfg_spin_prefill) {
		v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cfg_spin_prefill));
		P.chart_max_prefill = (guint)v;
		if (P.chart_max_prefill < 50)
			P.chart_max_prefill = 50;
		if (P.prefill_scale_adapt < P.chart_max_prefill)
			P.prefill_scale_adapt = P.chart_max_prefill;
	}
	if (cfg_spin_timeout) {
		v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cfg_spin_timeout));
		P.timeout_ms = (guint)v;
		if (P.timeout_ms < 100)
			P.timeout_ms = 100;
	}
	for (i = 0; i < N_FEATS; ++i) {
		if (cfg_feat_btn[i] &&
		    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cfg_feat_btn[i])))
			feats |= METRICS[i].bit;
	}
	if (feats != P.features) {
		P.features = feats;
		rebuild_llm_ui();
	}
}

static GkrellmMonitor plugin_mon = {
	N_("LLM NIM"),
	0,
	create_plugin,
	update_plugin,
	create_plugin_tab,
	apply_plugin_config,
	save_config,
	load_config,
	CONFIG_KEYWORD,
	NULL,
	NULL,
	NULL,
	MON_CPU | MON_INSERT_AFTER,
	NULL,
	NULL
};

GkrellmMonitor *gkrellm_init_plugin(void)
{
	static int curl_ready;

	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = 1;
	}

	memset(&P, 0, sizeof(P));
	strncpy(P.base_url, DEFAULT_URL, sizeof(P.base_url) - 1);
	strncpy(P.docs_release, DEFAULT_DOCS_RELEASE, sizeof(P.docs_release) - 1);
	P.airgap = 0;
	P.features = DEFAULT_FEATURES;
	P.chart_max_tps = DEFAULT_CHART_MAX_TPS;
	P.chart_max_prefill = DEFAULT_CHART_MAX_PREFILL;
	P.timeout_ms = DEFAULT_TIMEOUT_MS;
	P.prefill_scale_adapt = DEFAULT_CHART_MAX_PREFILL;
	P.style_id = gkrellm_add_meter_style(&plugin_mon, STYLE_NAME);
	P.chart_style_id = gkrellm_add_chart_style(&plugin_mon, STYLE_NAME);
	P.monitor = &plugin_mon;
	return &plugin_mon;
}
