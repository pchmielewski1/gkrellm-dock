/*****************************************************************************
 * net_clusters — one compact chart for many container interfaces
 *
 * The stock Net monitor draws one chart per interface. With dozens of Docker
 * containers (one host-side veth each) the dock grows by ~60 px per container.
 * This plugin folds every interface matching a regex (default "^veth") into a
 * single chart with two equal bands, like cpu_clusters does for CPU cores:
 *
 *   top band    "in"  = bytes entering the containers  (veth tx_bytes), cyan
 *   bottom band "out" = bytes leaving the containers    (veth rx_bytes), amber
 *
 * Note: container-to-container traffic on a Docker bridge is counted once as
 * "out" (sender veth) and once as "in" (receiver veth), so on a busy internal
 * network in and out are naturally close to equal.
 *
 * Interfaces are rescanned from /proc/net/dev every second, so containers that
 * start, stop or get recreated (new veth names) are picked up without
 * restarting GKrellM. Physical NICs and docker0 stay on the stock Net monitor.
 *
 * Config (user-config keyword "net_clusters"):
 *   net_clusters pattern <POSIX extended regex>   default ^veth
 *   net_clusters label   <panel title>            default Docker
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STYLE_NAME "net_clusters"
#define CONFIG_KEYWORD "net_clusters"
#define DEFAULT_PATTERN "^veth"
#define DEFAULT_LABEL "Docker"
#define CHART_H 48
#define MAX_IF 512
#define IF_NAME_MAX 32
#define N_BANDS 2

typedef struct {
	char name[IF_NAME_MAX];
	unsigned long long rx;
	unsigned long long tx;
	int seen;
} IfState;

static GkrellmMonitor *mon;
static gint style_id;

static GtkWidget *vbox_main;
static GkrellmChart *chart;
static GkrellmChartconfig *chart_config;
static GkrellmChartdata *cds[N_BANDS];
static GdkPixmap *data_pix[N_BANDS];
static GdkPixmap *grid_pix[N_BANDS];
static GdkColor color[N_BANDS];
static GdkColor grid_color[N_BANDS];

static char pattern_src[128] = DEFAULT_PATTERN;
static char panel_label[32] = DEFAULT_LABEL;
static regex_t pattern_re;
static int pattern_ok;

static IfState ifs[MAX_IF];
static int n_ifs;
static gint64 prev_us;
static int have_prev;

static int last_if_count;
static int last_active;
static gulong last_in, last_out;

static int compile_pattern(void)
{
	if (pattern_ok) {
		regfree(&pattern_re);
		pattern_ok = 0;
	}
	if (regcomp(&pattern_re, pattern_src, REG_EXTENDED | REG_NOSUB) != 0) {
		/* Bad regex from user-config: fall back to the default */
		g_strlcpy(pattern_src, DEFAULT_PATTERN, sizeof(pattern_src));
		if (regcomp(&pattern_re, pattern_src,
		            REG_EXTENDED | REG_NOSUB) != 0)
			return -1;
	}
	pattern_ok = 1;
	return 0;
}

static IfState *if_find_or_add(const char *name, int *is_new)
{
	int i;

	for (i = 0; i < n_ifs; ++i) {
		if (!strcmp(ifs[i].name, name)) {
			*is_new = 0;
			return &ifs[i];
		}
	}
	if (n_ifs >= MAX_IF)
		return NULL;
	g_strlcpy(ifs[n_ifs].name, name, sizeof(ifs[n_ifs].name));
	ifs[n_ifs].rx = ifs[n_ifs].tx = 0;
	ifs[n_ifs].seen = 0;
	*is_new = 1;
	return &ifs[n_ifs++];
}

static void if_prune_unseen(void)
{
	int i, j = 0;

	for (i = 0; i < n_ifs; ++i) {
		if (!ifs[i].seen)
			continue;
		if (i != j)
			ifs[j] = ifs[i];
		++j;
	}
	n_ifs = j;
}

/*
 * Only operationally-UP interfaces are folded in and counted. Docker keeps
 * creating and dropping short-lived veth pairs; ones that are down
 * (lowerlayerdown, notpresent, ...) or vanished between the /proc/net/dev
 * read and now must not inflate the interface count. "unknown" is what many
 * virtual devices report while being fully usable, so it counts as up.
 */
static int if_is_up(const char *name)
{
	char path[560], state[32];
	FILE *f;
	int up;

	if (strchr(name, '/'))
		return 0;
	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
	f = fopen(path, "r");
	if (!f)
		return 0;	/* gone already */
	if (!fgets(state, sizeof(state), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	up = !strncmp(state, "up", 2) || !strncmp(state, "unknown", 7);
	return up;
}

/*
 * One scan of /proc/net/dev. Sums per-second rates over every matching
 * UP interface. Container view: veth tx = traffic delivered *into* the
 * container.
 */
static int sample_ifaces(double *in_bps, double *out_bps, int *n_match,
                         int *n_active)
{
	FILE *f;
	char line[512];
	double dt = 0.0;
	gint64 now = g_get_monotonic_time();
	double in_sum = 0.0, out_sum = 0.0;
	int i, matched = 0, active = 0;

	*in_bps = *out_bps = 0.0;
	*n_match = *n_active = 0;
	if (!pattern_ok)
		return -1;
	f = fopen("/proc/net/dev", "r");
	if (!f)
		return -1;

	if (have_prev && now > prev_us)
		dt = (double)(now - prev_us) / 1e6;
	for (i = 0; i < n_ifs; ++i)
		ifs[i].seen = 0;

	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':');
		char *name;
		unsigned long long rx, tx;
		IfState *st;
		int is_new;

		if (!colon)
			continue;
		*colon = '\0';
		name = line;
		while (*name == ' ' || *name == '\t')
			++name;
		if (regexec(&pattern_re, name, 0, NULL, 0) != 0)
			continue;
		if (!if_is_up(name))
			continue;	/* not seen -> pruned, re-baselined if it returns */
		if (sscanf(colon + 1,
		           "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) != 2)
			continue;

		st = if_find_or_add(name, &is_new);
		if (!st)
			continue;
		++matched;
		st->seen = 1;
		/* New interface: baseline only, a delta against 0 would spike */
		if (!is_new && dt > 0.0) {
			double d_in = 0.0, d_out = 0.0;

			if (tx >= st->tx)
				d_in = (double)(tx - st->tx);
			if (rx >= st->rx)
				d_out = (double)(rx - st->rx);
			in_sum += d_in;
			out_sum += d_out;
			if (d_in > 0.0 || d_out > 0.0)
				++active;
		}
		st->rx = rx;
		st->tx = tx;
	}
	fclose(f);
	if_prune_unseen();

	if (dt > 0.0) {
		*in_bps = in_sum / dt;
		*out_bps = out_sum / dt;
	}
	prev_us = now;
	have_prev = 1;
	*n_match = matched;
	*n_active = active;
	return 0;
}

/* 1.2K, 34M, 512 — compact for the small chart label */
static void fmt_rate(char *out, size_t n, double bps)
{
	if (bps < 0.0)
		bps = 0.0;
	if (bps < 1000.0)
		snprintf(out, n, "%.0f", bps);
	else if (bps < 1000.0 * 1000.0)
		snprintf(out, n, "%.1fK", bps / 1000.0);
	else if (bps < 1000.0 * 1000.0 * 1000.0)
		snprintf(out, n, "%.1fM", bps / 1e6);
	else
		snprintf(out, n, "%.1fG", bps / 1e9);
}

static void make_colors(void)
{
	GdkColormap *cmap = gdk_colormap_get_system();

	/*
	 * Layer 0 = bottom band = out (warm amber, same family as cpu_clusters),
	 * layer 1 = top band = in (cool cyan). Later split layers stack on top.
	 */
	gdk_color_parse("#f0b060", &color[0]);
	gdk_color_parse("#ffe0b0", &grid_color[0]);
	gdk_color_parse("#5ec8e8", &color[1]);
	gdk_color_parse("#bde8f6", &grid_color[1]);
	gdk_colormap_alloc_color(cmap, &color[0], FALSE, TRUE);
	gdk_colormap_alloc_color(cmap, &grid_color[0], FALSE, TRUE);
	gdk_colormap_alloc_color(cmap, &color[1], FALSE, TRUE);
	gdk_colormap_alloc_color(cmap, &grid_color[1], FALSE, TRUE);
}

static void render_pixmaps(gint h)
{
	int i;

	if (h < 2)
		h = CHART_H;
	for (i = 0; i < N_BANDS; ++i) {
		gkrellm_render_data_pixmap(NULL, &data_pix[i], &color[i], h);
		gkrellm_render_data_grid_pixmap(NULL, &grid_pix[i], &grid_color[i]);
	}
}

static void refresh_chart(gpointer data)
{
	GkrellmChart *cp = (GkrellmChart *)data;
	char a[16], b[16], buf[64];

	if (!cp)
		return;
	gkrellm_draw_chartdata(cp);
	fmt_rate(a, sizeof(a), (double)last_in);
	fmt_rate(b, sizeof(b), (double)last_out);
	snprintf(buf, sizeof(buf), "%d  \xe2\x86\x93%s \xe2\x86\x91%s",
	         last_if_count, a, b);
	gkrellm_draw_chart_text(cp, style_id, buf);
	gkrellm_draw_chart_to_screen(cp);
}

static gint chart_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	if (chart && chart->drawing_area == widget && chart->pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                chart->pixmap, ev->area.x, ev->area.y, ev->area.x,
		                ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

static gint panel_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	GkrellmPanel *p = chart ? chart->panel : NULL;

	if (p && p->drawing_area == widget && p->pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                p->pixmap, ev->area.x, ev->area.y, ev->area.x,
		                ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

/* Two equal bands: in on top, out below (same trick as cpu_clusters) */
static void ensure_equal_splits(void)
{
	int k;
	gint h;
	int need_recompute = 0;

	if (!chart)
		return;
	for (k = 0; k < N_BANDS; ++k) {
		if (!cds[k])
			continue;
		cds[k]->hide = FALSE;
		if (k == 0) {
			cds[k]->split_chart = FALSE;
		} else {
			cds[k]->split_chart = TRUE;
			cds[k]->split_fraction = 1.0f / (gfloat)(N_BANDS - k + 1);
		}
	}
	h = chart->h > 0 ? chart->h : CHART_H;
	if (cds[0] && cds[1] && cds[0]->y == cds[1]->y)
		need_recompute = 1;
	if (need_recompute || h != CHART_H) {
		gkrellm_set_chart_height(chart, CHART_H + 1);
		gkrellm_set_chart_height(chart, CHART_H);
	}
}

static void update_plugin(void)
{
	double in_bps, out_bps;
	int n_match, n_active;
	GkrellmPanel *p;

	if (!GK.second_tick || !chart)
		return;
	if (sample_ifaces(&in_bps, &out_bps, &n_match, &n_active) != 0)
		return;

	last_if_count = n_match;
	last_active = n_active;
	last_in = (gulong)(in_bps + 0.5);
	last_out = (gulong)(out_bps + 0.5);

	ensure_equal_splits();
	/* layer order: 0 = bottom (out), 1 = top (in) */
	gkrellm_store_chartdata(chart, 0, last_out, last_in);
	refresh_chart(chart);

	p = chart->panel;
	if (p)
		gkrellm_draw_panel_layers(p);
}

static void create_plugin(GtkWidget *vbox, gint first_create)
{
	GkrellmStyle *panel_style;
	GkrellmPanel *p;
	int k;

	if (!pattern_ok)
		compile_pattern();

	gkrellm_spacers_set_types(mon, GKRELLM_SPACER_CHART, GKRELLM_SPACER_CHART);

	if (first_create) {
		vbox_main = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), vbox_main, FALSE, FALSE, 0);
		gtk_widget_show(vbox_main);
		chart = gkrellm_chart_new0();
		chart->panel = gkrellm_panel_new0();
		memset(cds, 0, sizeof(cds));
		memset(data_pix, 0, sizeof(data_pix));
		memset(grid_pix, 0, sizeof(grid_pix));
		have_prev = 0;
	}
	p = chart->panel;
	panel_style = gkrellm_panel_style(style_id);
	gkrellm_panel_configure(p, panel_label, panel_style);
	gkrellm_panel_create(vbox_main, mon, p);

	gkrellm_set_chart_height_default(chart, CHART_H);
	gkrellm_chart_create(vbox_main, mon, chart, &chart_config);
	gkrellm_set_draw_chart_function(chart, refresh_chart, chart);

	make_colors();
	render_pixmaps(CHART_H);

	for (k = 0; k < N_BANDS; ++k) {
		cds[k] = gkrellm_add_chartdata(chart, &data_pix[k], grid_pix[k],
		                               k == 0 ? "out" : "in");
		gkrellm_monotonic_chartdata(cds[k], FALSE);
		gkrellm_set_chartdata_draw_style_default(cds[k], CHARTDATA_IMPULSE);
		gkrellm_set_chartdata_draw_style(cds[k], CHARTDATA_IMPULSE);
		gkrellm_set_chartdata_flags(cds[k], CHARTDATA_NO_CONFIG);
		cds[k]->hide = FALSE;
		if (k >= 1) {
			cds[k]->split_chart = TRUE;
			cds[k]->split_fraction = 1.0f / (gfloat)(N_BANDS - k + 1);
		}
	}

	/* Auto-scale (rates span bytes/s to hundreds of MB/s) */
	gkrellm_set_chartconfig_auto_grid_resolution(chart->config, TRUE);
	gkrellm_set_chartconfig_fixed_grids(chart->config, 0);
	gkrellm_alloc_chartdata(chart);
	gkrellm_set_chart_height(chart, CHART_H);
	for (k = 0; k < N_BANDS; ++k) {
		if (!cds[k])
			continue;
		cds[k]->hide = FALSE;
		gkrellm_render_data_pixmap(NULL, &data_pix[k], &color[k],
		                           chart->h > 0 ? chart->h : CHART_H);
	}
	gkrellm_set_chart_height(chart, CHART_H + 1);
	gkrellm_set_chart_height(chart, CHART_H);

	if (first_create) {
		g_signal_connect(G_OBJECT(chart->drawing_area), "expose_event",
		                 G_CALLBACK(chart_expose), NULL);
		g_signal_connect(G_OBJECT(p->drawing_area), "expose_event",
		                 G_CALLBACK(panel_expose), NULL);
	} else {
		refresh_chart(chart);
	}
}

static void save_config(FILE *f)
{
	fprintf(f, "%s pattern %s\n", CONFIG_KEYWORD, pattern_src);
	fprintf(f, "%s label %s\n", CONFIG_KEYWORD, panel_label);
}

static void load_config(gchar *arg)
{
	gchar key[32], val[128];

	if (!arg)
		return;
	if (sscanf(arg, "%31s %127[^\n]", key, val) < 2)
		return;
	if (!strcmp(key, "pattern")) {
		g_strlcpy(pattern_src, val, sizeof(pattern_src));
		compile_pattern();
	} else if (!strcmp(key, "label")) {
		g_strlcpy(panel_label, val, sizeof(panel_label));
	}
}

static GkrellmMonitor plugin_mon = {
	N_("Net Clusters"),
	0,
	create_plugin,
	update_plugin,
	NULL,
	NULL,
	save_config,
	load_config,
	CONFIG_KEYWORD,
	NULL,
	NULL,
	NULL,
	MON_NET | MON_INSERT_AFTER,
	NULL,
	NULL
};

GkrellmMonitor *gkrellm_init_plugin(void)
{
	style_id = gkrellm_add_chart_style(&plugin_mon, STYLE_NAME);
	mon = &plugin_mon;
	compile_pattern();
	return &plugin_mon;
}
