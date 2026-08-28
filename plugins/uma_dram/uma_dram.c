/*****************************************************************************
 * uma_dram — Unified memory (DRAM) chart for DGX Spark UMA
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <string.h>

#define STYLE_NAME "uma_dram"

static GkrellmMonitor *mon;
static gint style_id;
static GtkWidget *main_vbox;
static GkrellmChart *chart;
static GkrellmChartconfig *chart_config;
static GkrellmChartdata *used_cd;
static GkrellmPanel *text_panel;
static GkrellmDecal *used_decal;
static GkrellmDecal *pct_decal;
static GkrellmKrell *krell;
static gulong last_pct;

static int read_meminfo(unsigned long *total_kb, unsigned long *avail_kb)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	*total_kb = *avail_kb = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long v;
		if (sscanf(line, "MemTotal: %lu", &v) == 1)
			*total_kb = v;
		else if (sscanf(line, "MemAvailable: %lu", &v) == 1)
			*avail_kb = v;
	}
	fclose(f);
	return (*total_kb > 0) ? 0 : -1;
}

static void refresh_chart(gpointer data)
{
	char buf[32];
	(void)data;
	gkrellm_draw_chartdata(chart);
	snprintf(buf, sizeof(buf), "%lu%%", last_pct);
	gkrellm_draw_chart_text(chart, style_id, buf);
	gkrellm_draw_chart_to_screen(chart);
}

static gint chart_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	GdkPixmap *pixmap = NULL;
	if (chart && chart->drawing_area == widget)
		pixmap = chart->pixmap;
	else if (chart && chart->panel && chart->panel->drawing_area == widget)
		pixmap = chart->panel->pixmap;
	if (pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                pixmap, ev->area.x, ev->area.y,
		                ev->area.x, ev->area.y,
		                ev->area.width, ev->area.height);
	return FALSE;
}

static gint text_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	if (text_panel && text_panel->pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                text_panel->pixmap, ev->area.x, ev->area.y,
		                ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

static gint panel_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	GkrellmPanel *p = chart ? chart->panel : NULL;
	if (p && p->drawing_area == widget && p->pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                p->pixmap, ev->area.x, ev->area.y,
		                ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

static void update_plugin(void)
{
	unsigned long total = 0, avail = 0, used;
	char buf[64];
	gulong total_mb, used_mb, used_gb, total_gb;
	GkrellmPanel *p;
	GkrellmStyle *style;
	GkrellmMargin *m;
	int w, w_text;

	if (!GK.second_tick || !chart)
		return;
	if (read_meminfo(&total, &avail) != 0)
		return;
	if (avail > total)
		avail = total;
	used = total - avail;
	total_mb = total / 1024;
	used_mb = used / 1024;
	if (total_mb < 1)
		total_mb = 1;
	last_pct = (used_mb * 100UL) / total_mb;
	if (last_pct > 100)
		last_pct = 100;

	/* Scale chart 0–100% so the line is readable (not absolute MB). */
	gkrellm_store_chartdata(chart, 100UL, last_pct);
	refresh_chart(NULL);

	p = chart->panel;
	if (krell && p) {
		gkrellm_update_krell(p, krell, last_pct);
		gkrellm_draw_panel_layers(p);
	}

	if (text_panel && used_decal) {
		used_gb = used_mb / 1024;
		total_gb = total_mb / 1024;
		if (total_gb < 1)
			total_gb = 1;
		style = gkrellm_meter_style(style_id);
		m = gkrellm_get_style_margins(style);
		w = gkrellm_chart_width();

		snprintf(buf, sizeof(buf), "%lu/%lu GB", used_gb, total_gb);
		gkrellm_draw_decal_text(text_panel, used_decal, buf, 0);

		if (pct_decal) {
			snprintf(buf, sizeof(buf), "%lu%%", last_pct);
			w_text = gkrellm_gdk_string_width(pct_decal->text_style.font, buf);
			pct_decal->x = w - m->left - m->right - w_text - 1;
			gkrellm_draw_decal_text(text_panel, pct_decal, buf, 0);
		}
		gkrellm_draw_panel_layers(text_panel);
	}
}

static void create_plugin(GtkWidget *vbox, gint first_create)
{
	GkrellmPanel *p;
	GkrellmStyle *style;
	GkrellmTextstyle *ts;

	if (first_create) {
		main_vbox = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), main_vbox, FALSE, FALSE, 0);
		gtk_widget_show(main_vbox);
		chart = gkrellm_chart_new0();
		chart->panel = gkrellm_panel_new0();
		text_panel = gkrellm_panel_new0();
		krell = NULL;
		last_pct = 0;
	}
	p = chart->panel;
	style = gkrellm_panel_style(style_id);

	krell = gkrellm_create_krell(p, gkrellm_krell_panel_piximage(style_id),
	                             style);
	gkrellm_monotonic_krell_values(krell, FALSE);
	gkrellm_set_krell_full_scale(krell, 100, 1);
	gkrellm_panel_configure(p, "DRAM UMA", style);
	gkrellm_panel_create(main_vbox, mon, p);

	gkrellm_set_chart_height_default(chart, 40);
	gkrellm_chart_create(main_vbox, mon, chart, &chart_config);
	gkrellm_set_draw_chart_function(chart, refresh_chart, NULL);
	used_cd = gkrellm_add_default_chartdata(chart, _("used %"));
	gkrellm_monotonic_chartdata(used_cd, FALSE);
	gkrellm_set_chartdata_draw_style(used_cd, CHARTDATA_LINE);
	gkrellm_set_chartconfig_auto_grid_resolution(chart->config, FALSE);
	gkrellm_set_chartconfig_fixed_grids(chart->config, 2);
	gkrellm_set_chartconfig_grid_resolution(chart->config, 50);
	gkrellm_alloc_chartdata(chart);
	gkrellm_set_chart_height(chart, 40);

	style = gkrellm_meter_style(style_id);
	ts = gkrellm_meter_textstyle(style_id);
	used_decal = gkrellm_create_decal_text(text_panel, "000/000 GB",
	                                       ts, style, -1, -1, -1);
	pct_decal = gkrellm_create_decal_text(text_panel, "100%",
	                                      ts, style, -1, used_decal->y, -1);
	gkrellm_panel_configure(text_panel, NULL, style);
	gkrellm_panel_create(main_vbox, mon, text_panel);

	if (first_create) {
		g_signal_connect(G_OBJECT(chart->drawing_area), "expose_event",
		                 G_CALLBACK(chart_expose), NULL);
		g_signal_connect(G_OBJECT(p->drawing_area), "expose_event",
		                 G_CALLBACK(panel_expose), NULL);
		g_signal_connect(G_OBJECT(text_panel->drawing_area), "expose_event",
		                 G_CALLBACK(text_expose), NULL);
	} else {
		refresh_chart(NULL);
	}
}

static GkrellmMonitor plugin_mon = {
	N_("DRAM UMA"),
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
	MON_MEM,
	NULL,
	NULL
};

GkrellmMonitor *gkrellm_init_plugin(void)
{
	style_id = gkrellm_add_chart_style(&plugin_mon, STYLE_NAME);
	mon = &plugin_mon;
	return &plugin_mon;
}
