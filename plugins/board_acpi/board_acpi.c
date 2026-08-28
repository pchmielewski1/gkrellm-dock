/*****************************************************************************
 * board_acpi — ACPI thermal zones (GB10 Spark Board panel)
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <string.h>

#include "../../lib/thermal_map.h"

#define STYLE_NAME "board_acpi"
#define MAX_ROWS 12
#define ROW_GAP 3

/* Display order matches the managed Board panel (see docs/PLUGINS.md). */
static const char *k_zone_order[] = {
	"TSOC", "TGPU", "TS0E", "TS0P", "TS1E", "TS1P", "TUNC", NULL
};

typedef struct {
	GkrellmDecal *label;
	GkrellmDecal *value;
	const char *name;
} DecalRow;

static GkrellmMonitor *mon;
static GkrellmPanel *header;
static GkrellmPanel *panel;
static gint style_id;
static GtkWidget *main_vbox;
static DecalRow rows[MAX_ROWS];
static int nrows;

static void place_value_right(GkrellmDecal *label, GkrellmDecal *value)
{
	GkrellmStyle *style = gkrellm_meter_style(style_id);
	GkrellmMargin *m = gkrellm_get_style_margins(style);
	int w = gkrellm_chart_width();
	int w_text;

	if (!label || !value)
		return;
	w_text = gkrellm_gdk_string_width(value->text_style.font, "0000C");
	value->x = w - m->left - m->right - w_text - 1;
	if (value->x < label->x + 4)
		value->x = label->x + 4;
}

static gint header_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	if (header && header->pixmap)
		gdk_draw_pixmap(widget->window,
		                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		                header->pixmap, ev->area.x, ev->area.y,
		                ev->area.x, ev->area.y, ev->area.width,
		                ev->area.height);
	return FALSE;
}

static gint panel_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	gdk_draw_pixmap(widget->window,
	                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
	                panel->pixmap, ev->area.x, ev->area.y,
	                ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	return FALSE;
}

static void update_plugin(void)
{
	GkThermalMap map;
	int i;
	GkrellmStyle *style;
	GkrellmMargin *m;
	int w;

	if (!GK.second_tick || !panel)
		return;

	if (header)
		gkrellm_draw_panel_layers(header);

	style = gkrellm_meter_style(style_id);
	m = gkrellm_get_style_margins(style);
	w = gkrellm_chart_width();

	gk_thermal_map_scan_sysfs(&map);
	for (i = 0; i < nrows; ++i) {
		int mC = gk_thermal_temp_by_name(&map, rows[i].name);
		char vbuf[32];
		int w_text;
		if (mC < 0)
			snprintf(vbuf, sizeof(vbuf), "—");
		else
			snprintf(vbuf, sizeof(vbuf), "%.0fC", mC / 1000.0);
		gkrellm_draw_decal_text(panel, rows[i].label,
		                        (gchar *)rows[i].name, 0);
		w_text = gkrellm_gdk_string_width(rows[i].value->text_style.font,
		                                  vbuf);
		rows[i].value->x = w - m->left - m->right - w_text - 1;
		gkrellm_draw_decal_text(panel, rows[i].value, vbuf, 0);
	}
	gkrellm_draw_panel_layers(panel);
}

static int create_row(const char *name, int y)
{
	GkrellmStyle *style = gkrellm_meter_style(style_id);
	GkrellmTextstyle *ts = gkrellm_meter_textstyle(style_id);
	DecalRow *r = &rows[nrows++];
	int yh;

	r->name = name;
	r->label = gkrellm_create_decal_text(panel, (gchar *)name, ts, style,
	                                     -1, y, -1);
	r->value = gkrellm_create_decal_text(panel, "000C", ts, style, -1, y, -1);
	place_value_right(r->label, r->value);
	yh = MAX(r->label->y, r->value->y) + MAX(r->label->h, r->value->h);
	return yh + ROW_GAP;
}

static void create_plugin(GtkWidget *vbox, gint first_create)
{
	GkrellmStyle *style;
	int y, i;

	if (first_create) {
		main_vbox = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), main_vbox, FALSE, FALSE, 0);
		gtk_widget_show(main_vbox);
		header = gkrellm_panel_new0();
		panel = gkrellm_panel_new0();
	}

	/*
	 * Title bar above the sensors — same mechanism as DRAM UMA:
	 * panel_configure(..., "Board", panel_style) draws the themed label bar.
	 */
	style = gkrellm_panel_style(style_id);
	gkrellm_panel_configure(header, "Board", style);
	gkrellm_panel_create(main_vbox, mon, header);

	/* Sensor list — no duplicate built-in label. */
	style = gkrellm_meter_style(style_id);
	nrows = 0;
	y = -1;
	for (i = 0; k_zone_order[i]; ++i)
		y = create_row(k_zone_order[i], y);

	gkrellm_panel_configure(panel, NULL, style);
	gkrellm_panel_create(main_vbox, mon, panel);

	if (first_create) {
		g_signal_connect(G_OBJECT(header->drawing_area), "expose_event",
		                 G_CALLBACK(header_expose), NULL);
		g_signal_connect(G_OBJECT(panel->drawing_area), "expose_event",
		                 G_CALLBACK(panel_expose), NULL);
	}
}

static GkrellmMonitor plugin_mon = {
	N_("Board ACPI"),
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
	MON_MEM | MON_INSERT_AFTER,
	NULL,
	NULL
};

GkrellmMonitor *gkrellm_init_plugin(void)
{
	style_id = gkrellm_add_meter_style(&plugin_mon, STYLE_NAME);
	mon = &plugin_mon;
	return &plugin_mon;
}
