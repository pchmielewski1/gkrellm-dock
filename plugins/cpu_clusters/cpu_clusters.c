/*****************************************************************************
 * cpu_clusters — Cortex-X925 vs Cortex-A725 charts
 *
 * Ten equal mini-CPU impulse bands per cluster (one per core). Label + krell
 * show the cluster average busy % (all cores), not a single core.
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <string.h>

#include "../../lib/cpu_map.h"
#include "../../lib/cpu_stat.h"

#define STYLE_NAME "cpu_clusters"
/* ~11 px data + separators for 10 cores */
#define CHART_H 120
#define CORE_MAX GK_CPU_MAX
#define CORE_STORE_MAX 10

typedef struct {
	GtkWidget *vbox;
	GkrellmChart *chart;
	GkrellmChartconfig *chart_config;
	GkrellmChartdata *cds[CORE_MAX];
	GdkPixmap *data_pix[CORE_MAX];
	GdkPixmap *grid_pix[CORE_MAX];
	GdkColor color[CORE_MAX];
	GdkColor grid_color[CORE_MAX];
	GkrellmKrell *krell;
	GkClusterId id;
	char label[16];
	gulong last_pct;
	int ncores;
	int cpu_ids[CORE_MAX];
} ClusterMon;

static GkrellmMonitor *mon;
static gint style_id;
static GkCpuList clusters[GK_CLUSTER_COUNT];
static GkCpuStatState stat_st;
static ClusterMon mons[GK_CLUSTER_COUNT];

/* Warm amber ramp around theme #f0b060 — darker → brighter per core index. */
static void amber_shade(int idx, int n, GdkColor *fill, GdkColor *grid)
{
	GdkColormap *cmap = gdk_colormap_get_system();
	char hex[8];
	double t;
	int r, g, b;

	if (n < 1)
		n = 1;
	t = (n == 1) ? 0.55 : (double)idx / (double)(n - 1);
	r = (int)(130 + t * 125);
	g = (int)(70 + t * 150);
	b = (int)(28 + t * 100);
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
	gdk_color_parse(hex, fill);
	gdk_colormap_alloc_color(cmap, fill, FALSE, TRUE);
	r = (int)(210 + t * 45);
	g = (int)(170 + t * 70);
	b = (int)(110 + t * 90);
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
	gdk_color_parse(hex, grid);
	gdk_colormap_alloc_color(cmap, grid, FALSE, TRUE);
}

static void render_core_pixmaps(ClusterMon *cm, gint h)
{
	int i;

	if (h < 2)
		h = CHART_H;
	for (i = 0; i < cm->ncores; ++i) {
		gkrellm_render_data_pixmap(NULL, &cm->data_pix[i], &cm->color[i], h);
		gkrellm_render_data_grid_pixmap(NULL, &cm->grid_pix[i],
		                                &cm->grid_color[i]);
	}
}

static void refresh_chart(gpointer data)
{
	ClusterMon *cm = (ClusterMon *)data;
	GkrellmChart *cp = cm->chart;
	char buf[16];

	gkrellm_draw_chartdata(cp);
	snprintf(buf, sizeof(buf), "%lu%%", cm->last_pct);
	gkrellm_draw_chart_text(cp, style_id, buf);
	gkrellm_draw_chart_to_screen(cp);
}

static gint chart_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	GdkPixmap *pixmap = NULL;
	GkrellmChart *cp;
	int i;
	(void)widget;
	for (i = 0; i < GK_CLUSTER_COUNT; ++i) {
		cp = mons[i].chart;
		if (cp && cp->drawing_area == widget) {
			pixmap = cp->pixmap;
			break;
		}
	}
	if (pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                pixmap, ev->area.x, ev->area.y,
		                ev->area.x, ev->area.y,
		                ev->area.width, ev->area.height);
	return FALSE;
}

static gint panel_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	int i;
	for (i = 0; i < GK_CLUSTER_COUNT; ++i) {
		GkrellmPanel *p = mons[i].chart ? mons[i].chart->panel : NULL;
		if (p && p->drawing_area == widget && p->pixmap) {
			gdk_draw_pixmap(widget->window,
			                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
			                p->pixmap, ev->area.x, ev->area.y,
			                ev->area.x, ev->area.y,
			                ev->area.width, ev->area.height);
			return FALSE;
		}
	}
	return FALSE;
}

static void ensure_equal_splits(ClusterMon *cm)
{
	int k, n = cm->ncores;
	gint h;
	int need_recompute = 0;

	if (n < 1 || !cm->chart)
		return;
	for (k = 0; k < n; ++k) {
		if (!cm->cds[k])
			continue;
		cm->cds[k]->hide = FALSE;
		if (k == 0) {
			cm->cds[k]->split_chart = FALSE;
		} else {
			cm->cds[k]->split_chart = TRUE;
			cm->cds[k]->split_fraction = 1.0f / (gfloat)(n - k + 1);
		}
	}
	h = cm->chart->h > 0 ? cm->chart->h : CHART_H;
	/* Overlap on last two bands ⇒ split heights were computed too early */
	if (n >= 2 && cm->cds[n - 1] && cm->cds[n - 2] &&
	    cm->cds[n - 1]->y == cm->cds[n - 2]->y)
		need_recompute = 1;
	if (need_recompute || cm->chart->h != CHART_H) {
		/*
		 * set_chart_height() no-ops when h is unchanged; nudge to force
		 * set_chartdata_split_heights() after split flags are correct.
		 */
		gkrellm_set_chart_height(cm->chart, CHART_H + 1);
		gkrellm_set_chart_height(cm->chart, CHART_H);
	}
}

static void store_core_layers(GkrellmChart *cp, gulong total, gulong *v, int n)
{
	switch (n) {
	case 0:
		return;
	case 1:
		gkrellm_store_chartdata(cp, total, v[0]);
		break;
	case 2:
		gkrellm_store_chartdata(cp, total, v[0], v[1]);
		break;
	case 3:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2]);
		break;
	case 4:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3]);
		break;
	case 5:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4]);
		break;
	case 6:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4],
		                        v[5]);
		break;
	case 7:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4],
		                        v[5], v[6]);
		break;
	case 8:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4],
		                        v[5], v[6], v[7]);
		break;
	case 9:
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4],
		                        v[5], v[6], v[7], v[8]);
		break;
	default:
		/* Exactly 10 cores per GB10 cluster */
		gkrellm_store_chartdata(cp, total, v[0], v[1], v[2], v[3], v[4],
		                        v[5], v[6], v[7], v[8], v[9]);
		break;
	}
}

static void update_plugin(void)
{
	double busy[GK_CLUSTER_COUNT];
	double core_busy[GK_CPU_MAX];
	int i, k;

	if (!GK.second_tick)
		return;
	if (gk_cpu_stat_sample(&stat_st, clusters, busy, core_busy) != 0)
		return;

	for (i = 0; i < GK_CLUSTER_COUNT; ++i) {
		ClusterMon *cm = &mons[i];
		GkrellmPanel *p;
		gulong pct;
		gulong vals[CORE_STORE_MAX];
		int nstore;

		/* Label / krell = average across all cores in the cluster */
		pct = (gulong)(busy[i] + 0.5);
		if (pct > 100)
			pct = 100;
		cm->last_pct = pct;

		/*
		 * Each mini-band is that core's busy % on a fixed 0–100 scale so a
		 * hot core fills its own strip (not scaled to the first core).
		 */
		nstore = cm->ncores;
		if (nstore > CORE_STORE_MAX)
			nstore = CORE_STORE_MAX;
		for (k = 0; k < nstore; ++k) {
			int cpu = cm->cpu_ids[k];
			double p = 0.0;

			if (cpu >= 0 && cpu < GK_CPU_MAX)
				p = core_busy[cpu];
			if (p < 0.0)
				p = 0.0;
			if (p > 100.0)
				p = 100.0;
			vals[k] = (gulong)(p + 0.5);
		}

		gkrellm_set_chartconfig_fixed_grids(cm->chart->config, 1);
		gkrellm_set_chartconfig_grid_resolution(cm->chart->config, 100);

		ensure_equal_splits(cm);
		store_core_layers(cm->chart, 100UL, vals, nstore);
		refresh_chart(cm);

		p = cm->chart->panel;
		if (!p)
			continue;
		if (cm->krell)
			gkrellm_update_krell(p, cm->krell, pct);
		gkrellm_draw_panel_layers(p);
	}
}

static void create_plugin(GtkWidget *vbox, gint first_create)
{
	int i, k;

	if (first_create)
		gk_cpu_map_load("/proc/cpuinfo", clusters);

	gkrellm_spacers_set_types(mon, GKRELLM_SPACER_CHART, GKRELLM_SPACER_CHART);

	for (i = 0; i < GK_CLUSTER_COUNT; ++i) {
		ClusterMon *cm = &mons[i];
		GkrellmStyle *panel_style;
		GkrellmChart *cp;
		GkrellmPanel *p;
		GkCpuList *cl = &clusters[i];

		cm->id = (GkClusterId)i;
		snprintf(cm->label, sizeof(cm->label), "CPU %s",
		         gk_cluster_name(cm->id));
		cm->ncores = cl->count;
		if (cm->ncores > CORE_MAX)
			cm->ncores = CORE_MAX;
		if (cm->ncores > CORE_STORE_MAX)
			cm->ncores = CORE_STORE_MAX;
		for (k = 0; k < cm->ncores; ++k)
			cm->cpu_ids[k] = cl->cpus[k];

		if (first_create) {
			cm->vbox = gtk_vbox_new(FALSE, 0);
			gtk_box_pack_start(GTK_BOX(vbox), cm->vbox, FALSE, FALSE, 0);
			gtk_widget_show(cm->vbox);
			cm->chart = gkrellm_chart_new0();
			cm->chart->panel = gkrellm_panel_new0();
			cm->krell = NULL;
			cm->last_pct = 0;
			memset(cm->cds, 0, sizeof(cm->cds));
			memset(cm->data_pix, 0, sizeof(cm->data_pix));
			memset(cm->grid_pix, 0, sizeof(cm->grid_pix));
		}
		cp = cm->chart;
		p = cp->panel;
		panel_style = gkrellm_panel_style(style_id);

		cm->krell = gkrellm_create_krell(p,
		                                 gkrellm_krell_panel_piximage(style_id),
		                                 panel_style);
		gkrellm_monotonic_krell_values(cm->krell, FALSE);
		gkrellm_set_krell_full_scale(cm->krell, 100, 1);
		gkrellm_panel_configure(p, cm->label, panel_style);
		gkrellm_panel_create(cm->vbox, mon, p);

		gkrellm_set_chart_height_default(cp, CHART_H);
		gkrellm_chart_create(cm->vbox, mon, cp, &cm->chart_config);
		gkrellm_set_draw_chart_function(cp, refresh_chart, cm);

		for (k = 0; k < cm->ncores; ++k)
			amber_shade(k, cm->ncores, &cm->color[k], &cm->grid_color[k]);
		render_core_pixmaps(cm, CHART_H);

		for (k = 0; k < cm->ncores; ++k) {
			char name[16];

			snprintf(name, sizeof(name), "cpu%d", cm->cpu_ids[k]);
			cm->cds[k] = gkrellm_add_chartdata(cp, &cm->data_pix[k],
			                                   cm->grid_pix[k], name);
			gkrellm_monotonic_chartdata(cm->cds[k], FALSE);
			gkrellm_set_chartdata_draw_style_default(cm->cds[k],
			                                        CHARTDATA_IMPULSE);
			gkrellm_set_chartdata_draw_style(cm->cds[k], CHARTDATA_IMPULSE);
			gkrellm_set_chartdata_flags(cm->cds[k], CHARTDATA_NO_CONFIG);
			cm->cds[k]->hide = FALSE;
			/*
			 * Equal mini-bands: cd[i].height uses cd[i+1].split_fraction.
			 * For N bands, fraction on layer k (k>=1) is 1/(N-k+1).
			 */
			if (k >= 1) {
				cm->cds[k]->split_chart = TRUE;
				cm->cds[k]->split_fraction =
				    1.0f / (gfloat)(cm->ncores - k + 1);
			}
		}

		gkrellm_set_chartconfig_auto_grid_resolution(cp->config, FALSE);
		gkrellm_set_chartconfig_fixed_grids(cp->config, 1);
		gkrellm_set_chartconfig_grid_resolution(cp->config, 100);
		gkrellm_alloc_chartdata(cp);
		gkrellm_set_chart_height(cp, CHART_H);
		for (k = 0; k < cm->ncores; ++k) {
			if (!cm->cds[k])
				continue;
			cm->cds[k]->hide = FALSE;
			if (k >= 1) {
				cm->cds[k]->split_chart = TRUE;
				cm->cds[k]->split_fraction =
				    1.0f / (gfloat)(cm->ncores - k + 1);
			} else {
				cm->cds[k]->split_chart = FALSE;
			}
			gkrellm_render_data_pixmap(NULL, &cm->data_pix[k], &cm->color[k],
			                           cp->h > 0 ? cp->h : CHART_H);
		}
		/* Force split height recompute (same-h set_chart_height is a no-op) */
		gkrellm_set_chart_height(cp, CHART_H + 1);
		gkrellm_set_chart_height(cp, CHART_H);

		if (first_create) {
			g_signal_connect(G_OBJECT(cp->drawing_area), "expose_event",
			                 G_CALLBACK(chart_expose), NULL);
			g_signal_connect(G_OBJECT(p->drawing_area), "expose_event",
			                 G_CALLBACK(panel_expose), NULL);
		} else {
			refresh_chart(cm);
		}
	}
}

static GkrellmMonitor plugin_mon = {
	N_("CPU Clusters"),
	0,
	create_plugin,
	update_plugin,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	MON_PROC,
	NULL,
	NULL
};

GkrellmMonitor *gkrellm_init_plugin(void)
{
	style_id = gkrellm_add_chart_style(&plugin_mon, STYLE_NAME);
	mon = &plugin_mon;
	return &plugin_mon;
}
