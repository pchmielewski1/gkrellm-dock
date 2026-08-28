/*****************************************************************************
 * GKrellM nVidia                                                            *
 * A plugin for GKrellM showing nVidia GPU info using libNVML                *
 * Copyright (C) 2025 Carlo Casta <carlo.casta@gmail.com>                    *
 *                                                                           *
 * This program is free software; you can redistribute it and/or modify      *  
 * it under the terms of the GNU General Public License as published by      *
 * the Free Software Foundation; either version 2 of the License, or         *
 * (at your option) any later version.                                       *
 *                                                                           *
 * This program is distributed in the hope that it will be useful,           *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
 * GNU General Public License for more details.                              *
 *                                                                           *
 * You should have received a copy of the GNU General Public License         *
 * along with this program; if not, write to the Free Software               *
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA *
 *                                                                           *
 *****************************************************************************/
#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <string.h>
#include "nvml-lib.h"

#define GK_PLUGIN_NAME "nvidia"
#define GK_CONFIG_KEYWORD "nvidia"
#define GK_MAX_TEXT 64
#define GK_MAX_PATH CFG_BUFSIZE

#ifndef GK_MAX_GPUS
 #define GK_MAX_GPUS 4
#endif
#define GK_MAX_GPU_FANS 1

static GKNVMLLib nvml;
static gboolean reset_lib = FALSE;

#ifndef GKFREQ_NVML_SONAME
 #define GKFREQ_NVML_SONAME "libnvidia-ml.so.1"
#endif

#ifndef GDK_BUTTON_SECONDARY
 #define GDK_BUTTON_SECONDARY 3
#endif

/* convert nvml return to boolean */
#define NVFN(fn) (nvml.fn == NVML_SUCCESS)

/* convert bytes to mbytes */
#define B2MB(b) (b / 0x100000)

/* mark unused variables to avoid compile warnings */
#define UNUSED(x) (void)(x)

/* helper for array length */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

/* helper to keep struct size consistent with enum */
#define ASSERT_SIZE(a, sz) typedef char a ## _sz[(ARRAY_SIZE(a) == sz) - 1]

#define GKSWAP(x, y) do {     \
	__typeof__(x) _tmp = x;   \
	x = y;                    \
	y = _tmp;                 \
} while(0)

typedef struct _GKNvidia {
	GtkWidget *main_vbox;
	GkrellmMonitor *monitor;
	GkrellmPanel *panel;
	GkrellmPanel *header_panel;
	int style_id;
	int chart_style_id;
	GkrellmChart *load_chart;
	GkrellmChart *clock_chart;
	GkrellmChart *power_chart;
	GkrellmChartconfig *load_cfg;
	GkrellmChartconfig *clock_cfg;
	GkrellmChartconfig *power_cfg;
	guint max_clock;
	guint max_power_mw;
	/* Cumulative counters: store_chartdata diffs → absolute gauge values */
	gulong cum_load, cum_load_tot;
	gulong cum_clock, cum_clock_tot;
	gulong cum_power, cum_power_tot;
} GKNvidia;

static GKNvidia plugin;

typedef enum _TextAlignment {
	RIGHT,
	CENTER,
	LEFT
} TextAlignment_t;

typedef enum _GPUProperty {
	GPU_NAME,
	GPU_USAGE,
	GPU_CLOCK,
	GPU_MEMCLOCK,
	GPU_TEMP,
	GPU_FAN,
	GPU_FANUSAGE,
	GPU_POWER,
	GPU_MEMUSAGE,
	GPU_USEDMEM,
	GPU_RESERVEDMEM,
	GPU_TOTALMEM,
	GPU_PROPS_NUM
} GPUProperty_t;

typedef struct _GkrellmDecalRowInfo {
	gboolean enable;
	guint order;
	TextAlignment_t alignment;
	char *label;
	char *optionlabel;
} GkrellmDecalRowInfo_t;

/* Name lives in the top header panel; Load/Clock/Power are charts */
static GkrellmDecalRowInfo_t decal_info[] = {
 { FALSE, 0, CENTER, "",                   ""                                },
 { FALSE, 1, RIGHT,  _("Load"),            _("GPU Load")                     },
 { FALSE, 2, RIGHT,  _("Clock"),           _("GPU Clock")                    },
 { FALSE, 3, RIGHT,  _("Memory Clock"),    _("GPU Memory Clock")             },
 { TRUE,  4, RIGHT,  _("Temp"),            _("GPU Temperature")              },
 { FALSE, 5, RIGHT,  _("Fan"),             _("GPU Fan Speed")                },
 { FALSE, 6, RIGHT,  _("Fan"),             _("GPU Fan Speed (percentage)")   },
 { FALSE, 7, RIGHT,  _("Power"),           _("GPU Power Draw")               },
 { TRUE,  8, RIGHT,  _("UMA"),             _("Unified Memory (host)")        },
 { FALSE, 9, RIGHT,  _("Used Memory"),     _("GPU Used Memory")              },
 { FALSE,10, RIGHT,  _("Reserved Memory"), _("GPU Reserved Memory")          },
 { FALSE,11, RIGHT,  _("Total Memory"),    _("GPU Total Memory")             }
};

/* make sure this stays consistent with gpu properties */
ASSERT_SIZE(decal_info, GPU_PROPS_NUM);

typedef struct _GkrellmDecalRow {
	GkrellmDecal *label;
	GkrellmDecal *data;
} GkrellmDecalRow_t;

static GkrellmDecalRow_t decal_text[GK_MAX_GPUS * GPU_PROPS_NUM];

#define INVALID_PROP -1u

typedef struct _NVGpuInfo {
	gboolean good;
	char name[GK_MAX_TEXT];
	nvmlDevice_t h;
	nvmlPciInfo_t pci;
	guint clock;
	guint memclock;
	guint temp;
	guint fan;
	guint pwr;
	nvmlUsage_t usage;
	nvmlMemory_t memory;
	guint fan_count;
	nvmlFan_t fan_data[GK_MAX_GPU_FANS];
} NVGpuInfo;

static NVGpuInfo gpu_info[GK_MAX_GPUS];

static gboolean is_decal_enabled(GPUProperty_t prop)
{
	int i;

	for (i = 0; i < GPU_PROPS_NUM; ++i)
		if (decal_info[i].order == prop)
			return decal_info[i].enable;

	return FALSE;
}

static void set_decal_enabled(GPUProperty_t prop, gboolean toggle)
{
	int i;

	for (i = 0; i < GPU_PROPS_NUM; ++i)
		if (decal_info[i].order == prop)
			decal_info[i].enable = toggle;
}

static void rebuild_nv_panel(void);

static void update_gpu_info(void)
{
	guint i, gpu_count, f;
	NVGpuInfo *g;

	memset(gpu_info, 0, sizeof(NVGpuInfo) * GK_MAX_GPUS);

	if (NVFN(nvmlDeviceGetCount(&gpu_count)))
		if (CLAMP(gpu_count, 0, GK_MAX_GPUS) > 0)
			for (i = 0; i < gpu_count; ++i) {
				g = &gpu_info[i];
				/* PCI info is nice-to-have; do not require it for charts */
				g->good = NVFN(nvmlDeviceGetHandleByIndex(i, &(g->h))) &&
				          NVFN(nvmlDeviceGetName(g->h, g->name, GK_MAX_TEXT));
				if (g->good && nvml.nvmlDeviceGetPciInfo)
					(void)NVFN(nvmlDeviceGetPciInfo(g->h, &(g->pci)));

				g->memory.version = nvmlMemory_ver;

				if (nvml.nvmlDeviceGetNumFans &&
				    NVFN(nvmlDeviceGetNumFans(g->h, &(g->fan_count))))
					g->fan_count = CLAMP(g->fan_count, 0, GK_MAX_GPU_FANS);
				else
					g->fan_count = 0;

				for (f = 0; f < g->fan_count; ++f) {
					g->fan_data[f].version = nvmlFan_ver;
					g->fan_data[f].fanidx = f;
				}
			}
}

static void update_gpu_data(void)
{
	int i;
	NVGpuInfo *g;

	for (i = 0; i < GK_MAX_GPUS; ++i) {
		
		g = &gpu_info[i];
		
		if (!g->good)
			continue;

		/* Always sample chart metrics */
		if (!NVFN(nvmlDeviceGetClockInfo(g->h, NVML_CLOCK_GFX, &(g->clock))))
			g->clock = INVALID_PROP;
		else if (g->clock != INVALID_PROP && g->clock > plugin.max_clock)
			plugin.max_clock = g->clock;

		if (!is_decal_enabled(GPU_MEMCLOCK) ||
		    !NVFN(nvmlDeviceGetClockInfo(g->h, NVML_CLOCK_MEM, &(g->memclock))))
			g->memclock = INVALID_PROP;

		if (!is_decal_enabled(GPU_TEMP) ||
		    !NVFN(nvmlDeviceGetTemperature(g->h, NVML_TEMP_GPU, &(g->temp))))
			g->temp = INVALID_PROP;

		if (!is_decal_enabled(GPU_FANUSAGE) || !nvml.nvmlDeviceGetFanSpeed_v2 ||
		    !NVFN(nvmlDeviceGetFanSpeed_v2(g->h, 0, &(g->fan))))
			g->fan = INVALID_PROP;

		if (!is_decal_enabled(GPU_FAN) || !nvml.nvmlDeviceGetFanSpeedRPM ||
		    !NVFN(nvmlDeviceGetFanSpeedRPM(g->h, &(g->fan_data[0]))))
			g->fan_data[0].speed = INVALID_PROP;

		if (!NVFN(nvmlDeviceGetPowerUsage(g->h, &(g->pwr))))
			g->pwr = INVALID_PROP;
		else if (g->pwr != INVALID_PROP && g->pwr > plugin.max_power_mw)
			plugin.max_power_mw = g->pwr;

		if (!NVFN(nvmlDeviceGetUtilizationRates(g->h, &(g->usage))))
			g->usage.gpu = g->usage.memory = INVALID_PROP;

		if ((!is_decal_enabled(GPU_USEDMEM) &&
		     !is_decal_enabled(GPU_RESERVEDMEM) &&
		     !is_decal_enabled(GPU_TOTALMEM) &&
		     !is_decal_enabled(GPU_MEMUSAGE)) ||
		    !nvml.nvmlDeviceGetMemoryInfo_v2 ||
		    !NVFN(nvmlDeviceGetMemoryInfo_v2(g->h, &(g->memory))))
			g->memory.free =
			g->memory.reserved =
			g->memory.total =
			g->memory.used = INVALID_PROP;
	}
}

static gboolean read_uma_pct(guint *pct_out)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	unsigned long total = 0, avail = 0, used;
	if (!f)
		return FALSE;
	while (fgets(line, sizeof(line), f)) {
		unsigned long v;
		if (sscanf(line, "MemTotal: %lu", &v) == 1)
			total = v;
		else if (sscanf(line, "MemAvailable: %lu", &v) == 1)
			avail = v;
	}
	fclose(f);
	if (total == 0)
		return FALSE;
	if (avail > total)
		avail = total;
	used = total - avail;
	*pct_out = (guint)((used * 100UL) / total);
	if (*pct_out > 100)
		*pct_out = 100;
	return TRUE;
}

static gboolean get_gpu_data(int gpu_id, int info, char *buf, int buf_size)
{
	gboolean res = FALSE;
	NVGpuInfo *g;

	if (gpu_info[gpu_id].good) {

		g = &gpu_info[gpu_id];

		switch (info) {
		case GPU_NAME:
			strcpy(buf, g->name);
			res = TRUE;
			break;

		case GPU_CLOCK:
			snprintf(buf, buf_size, "%uMHz", g->clock);
			res = g->clock != INVALID_PROP;
			break;

		case GPU_MEMCLOCK:
			snprintf(buf, buf_size, "%uMHz", g->memclock);
			res = g->memclock != INVALID_PROP;
			break;

		case GPU_TEMP:
			snprintf(buf, buf_size, "%.01fC", (float)(g->temp));
			res = g->temp != INVALID_PROP;
			break;

		case GPU_FANUSAGE:
			snprintf(buf, buf_size, "%u%%", CLAMP(g->fan, 0u, 100u));
			res = g->fan != INVALID_PROP;
			break;

		case GPU_FAN:
			snprintf(buf, buf_size, "%uRPM", g->fan_data[0].speed);
			res = g->fan_count > 0 && g->fan_data[0].speed != INVALID_PROP;
			break;

		case GPU_POWER:
			snprintf(buf, buf_size, "%uW", g->pwr / 1000);
			res = g->pwr != INVALID_PROP;
			break;

		case GPU_USAGE:
			snprintf(buf, buf_size, "%u%%", g->usage.gpu);
			res = g->usage.gpu != INVALID_PROP;
			break;

		case GPU_MEMUSAGE:
			/* GB10 UMA: NVML memory util is N/A — use host /proc/meminfo */
			{
				guint pct = 0;
				if (g->usage.memory != INVALID_PROP && g->usage.memory > 0) {
					snprintf(buf, buf_size, "%u%%", g->usage.memory);
					res = TRUE;
				} else if (read_uma_pct(&pct)) {
					snprintf(buf, buf_size, "%u%%", pct);
					res = TRUE;
				} else {
					res = FALSE;
				}
			}
			break;

		case GPU_USEDMEM:
			snprintf(buf, buf_size, "%lluMB", B2MB(g->memory.used));
			res = g->memory.used != INVALID_PROP;
			break;

		case GPU_RESERVEDMEM:
			snprintf(buf, buf_size, "%lluMB", B2MB(g->memory.reserved));
			res = g->memory.reserved != INVALID_PROP;
			break;

		case GPU_TOTALMEM:
			snprintf(buf, buf_size, "%lluMB", B2MB(g->memory.total));
			res = g->memory.total != INVALID_PROP;
			break;

		default:
			res = FALSE;
			break;
		}

	}

	if (!res)
		strcpy(buf, "N/A");

	return res;
}

static void refresh_gpu_chart(gpointer data)
{
	GkrellmChart *cp = (GkrellmChart *)data;
	char buf[48];
	NVGpuInfo *g = &gpu_info[0];

	gkrellm_draw_chartdata(cp);
	if (g->good) {
		if (cp == plugin.load_chart && g->usage.gpu != INVALID_PROP)
			snprintf(buf, sizeof(buf), "%u%%", g->usage.gpu);
		else if (cp == plugin.clock_chart && g->clock != INVALID_PROP)
			snprintf(buf, sizeof(buf), "%uMHz", g->clock);
		else if (cp == plugin.power_chart && g->pwr != INVALID_PROP)
			snprintf(buf, sizeof(buf), "%uW", g->pwr / 1000);
		else
			buf[0] = '\0';
		if (buf[0])
			gkrellm_draw_chart_text(cp, plugin.chart_style_id, buf);
	}
	gkrellm_draw_chart_to_screen(cp);
}

static gint gpu_chart_expose(GtkWidget *widget, GdkEventExpose *ev)
{
	GkrellmChart *charts[3];
	int i;
	charts[0] = plugin.load_chart;
	charts[1] = plugin.clock_chart;
	charts[2] = plugin.power_chart;
	for (i = 0; i < 3; ++i) {
		GkrellmChart *cp = charts[i];
		GdkPixmap *pixmap = NULL;
		if (!cp)
			continue;
		if (cp->drawing_area == widget)
			pixmap = cp->pixmap;
		else if (cp->panel && cp->panel->drawing_area == widget)
			pixmap = cp->panel->pixmap;
		if (pixmap) {
			gdk_draw_pixmap(widget->window,
			                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
			                pixmap, ev->area.x, ev->area.y,
			                ev->area.x, ev->area.y,
			                ev->area.width, ev->area.height);
			return FALSE;
		}
	}
	return FALSE;
}

/* Fixed chart ceilings requested for GB10 dock readability.
 * Clock: Applications/default boost is 2418 MHz (CUDA/docs); this host’s
 * NVML max is 3003 MHz and we routinely see ~2500–2550 under load — old
 * 2400–2500 window pegged the chart. Zoomed band keeps boost detail. */
#define GPU_LOAD_SCALE   100UL   /* 0–100 % */
#define GPU_POWER_SCALE  100UL   /* 0–100 W */
#define GPU_CLOCK_LO     2400UL  /* chart window 2400–2550 MHz */
#define GPU_CLOCK_HI     2550UL
#define GPU_CLOCK_SPAN   (GPU_CLOCK_HI - GPU_CLOCK_LO)  /* 150 */

static void store_gauge_chart(GkrellmChart *cp, gulong *cum_val,
                              gulong *cum_tot, gulong value, gulong scale)
{
	if (!cp || scale < 1)
		return;
	/* 2 grids × (scale/2) = full scale */
	gkrellm_set_chartconfig_fixed_grids(cp->config, 2);
	gkrellm_set_chartconfig_grid_resolution(cp->config, (gint)(scale / 2UL));
	/* Stock-CPU style: monotonic counters + total → bar height = value/scale */
	*cum_val += value;
	*cum_tot += scale;
	gkrellm_store_chartdata(cp, *cum_tot, *cum_val);
	refresh_gpu_chart(cp);
}

static void update_gpu_charts(void)
{
	NVGpuInfo *g = &gpu_info[0];
	gulong load, clock, pwr_w, clock_plot;

	if (!g->good || !plugin.load_chart)
		return;
	if (!GK.second_tick)
		return;

	load = (g->usage.gpu == INVALID_PROP) ? 0 : (gulong)g->usage.gpu;
	if (load > GPU_LOAD_SCALE)
		load = GPU_LOAD_SCALE;

	clock = (g->clock == INVALID_PROP) ? 0 : (gulong)g->clock;
	/* Map 2400–2550 MHz → 0–SPAN on the chart (text still shows raw MHz) */
	if (clock <= GPU_CLOCK_LO)
		clock_plot = 0;
	else if (clock >= GPU_CLOCK_HI)
		clock_plot = GPU_CLOCK_SPAN;
	else
		clock_plot = clock - GPU_CLOCK_LO;

	pwr_w = (g->pwr == INVALID_PROP) ? 0 : (gulong)(g->pwr / 1000);
	if (pwr_w > GPU_POWER_SCALE)
		pwr_w = GPU_POWER_SCALE;

	store_gauge_chart(plugin.load_chart, &plugin.cum_load, &plugin.cum_load_tot,
	                  load, GPU_LOAD_SCALE);
	store_gauge_chart(plugin.clock_chart, &plugin.cum_clock,
	                  &plugin.cum_clock_tot, clock_plot, GPU_CLOCK_SPAN);
	store_gauge_chart(plugin.power_chart, &plugin.cum_power,
	                  &plugin.cum_power_tot, pwr_w, GPU_POWER_SCALE);
}

static void create_one_gpu_chart(GkrellmChart **cp_out,
                                 GkrellmChartconfig **cfg_out,
                                 const char *label,
                                 gint grid_res,
                                 gint fixed_grids)
{
	GkrellmChart *cp;
	GkrellmPanel *p;
	GtkWidget *box;
	GkrellmChartdata *cd;

	box = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(plugin.main_vbox), box, FALSE, FALSE, 0);
	gtk_widget_show(box);

	*cp_out = gkrellm_chart_new0();
	(*cp_out)->panel = gkrellm_panel_new0();
	cp = *cp_out;
	p = cp->panel;

	/* Pack label ABOVE chart so name matches the graph under it */
	gkrellm_panel_configure(p, (gchar *)label,
	                        gkrellm_panel_style(plugin.chart_style_id));
	gkrellm_panel_create(box, plugin.monitor, p);

	gkrellm_set_chart_height_default(cp, 48);
	gkrellm_chart_create(box, plugin.monitor, cp, cfg_out);
	gkrellm_set_draw_chart_function(cp, refresh_gpu_chart, cp);
	cd = gkrellm_add_default_chartdata(cp, (gchar *)label);
	/* Filled bars; LINE is a 1px stroke that looks empty when values are steady */
	gkrellm_set_chartdata_draw_style_default(cd, CHARTDATA_IMPULSE);
	gkrellm_set_chartdata_draw_style(cd, CHARTDATA_IMPULSE);
	gkrellm_set_chartdata_flags(cd, CHARTDATA_NO_CONFIG);
	gkrellm_set_chartconfig_auto_grid_resolution(cp->config, FALSE);
	gkrellm_set_chartconfig_fixed_grids(cp->config, fixed_grids);
	gkrellm_set_chartconfig_grid_resolution(cp->config, grid_res);
	gkrellm_alloc_chartdata(cp);
	gkrellm_set_chart_height(cp, 48);

	g_signal_connect(G_OBJECT(cp->drawing_area), "expose_event",
	                 G_CALLBACK(gpu_chart_expose), NULL);
	g_signal_connect(G_OBJECT(p->drawing_area), "expose_event",
	                 G_CALLBACK(gpu_chart_expose), NULL);
}

static gint panel_expose_event(GtkWidget *widget, GdkEventExpose *ev)
{
	gdk_draw_pixmap(widget->window,
	                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
	                plugin.panel->pixmap,
	                ev->area.x,
	                ev->area.y,
	                ev->area.x,
	                ev->area.y,
	                ev->area.width,
	                ev->area.height);

	return 0;
}

static void panel_click_event(GtkWidget *w, GdkEventButton *event, gpointer p)
{
	UNUSED(w);
	UNUSED(p);

	if (event->button == GDK_BUTTON_SECONDARY)
		gkrellm_open_config_window(plugin.monitor);
}

static void update_plugin(void)
{
	GkrellmStyle *style = gkrellm_panel_style(plugin.style_id);
	GkrellmMargin *m = gkrellm_get_style_margins(style);
	GkrellmDecal *d;
	int w = gkrellm_chart_width();
	int w_text, i, p, idx, p_idx;
	static char prop[GK_MAX_TEXT] = "N/A";
	static int retry_countdown;

	/* Recover if NVML was unavailable at create time (empty path / late driver). */
	if (!gpu_info[0].good && GK.second_tick) {
		if (--retry_countdown <= 0) {
			retry_countdown = 5;
			if (!is_valid_gpulib(&nvml))
				initialize_gpulib(&nvml);
			if (is_valid_gpulib(&nvml)) {
				update_gpu_info();
				if (gpu_info[0].good)
					rebuild_nv_panel();
			}
		}
	}

	update_gpu_data();

	for (i = 0; i < GK_MAX_GPUS; ++i) {

		if (!gpu_info[i].good)
			continue;

		idx = i * GPU_PROPS_NUM;

		for (p = 0; p < GPU_PROPS_NUM; ++p) {

			p_idx = decal_info[p].order;

			d = decal_text[idx + p_idx].label;

			if (decal_info[p].enable && d != NULL) {

				gkrellm_draw_decal_text(plugin.panel,
				                        d,
				                        decal_info[p].label,
				                        0);

				get_gpu_data(i, p_idx, prop, GK_MAX_TEXT);
				
				w_text = gkrellm_gdk_string_width(d->text_style.font, prop);

				switch (decal_info[p].alignment) {
				case LEFT:
					decal_text[idx + p_idx].data->x = m->left;
					break;
				case CENTER:
					decal_text[idx + p_idx].data->x = (w - w_text) / 2 - 1;
					break;
				case RIGHT:
					decal_text[idx + p_idx].data->x = w -
					                                  m->left -
					                                  m->right -
					                                  w_text -
					                                  1;
					break;
				}

				gkrellm_draw_decal_text(plugin.panel,
				                        decal_text[idx + p_idx].data,
				                        prop,
				                        0);
			}

		}
	}

	gkrellm_draw_panel_layers(plugin.panel);
	update_gpu_charts();
}

static int create_decal_row(int i,
                            GPUProperty_t offset,
                            gchar *label,
                            gchar *text,
                            int y)
{
	GkrellmStyle *style = gkrellm_meter_style(plugin.style_id);
	GkrellmTextstyle *ts = gkrellm_meter_textstyle(plugin.style_id);
	int idx = i * GPU_PROPS_NUM + offset;

	decal_text[idx].label = gkrellm_create_decal_text(plugin.panel,
	                                                  label,
	                                                  ts,
	                                                  style,
	                                                  -1,
	                                                  y,
	                                                  -1);
	
	decal_text[idx].data = gkrellm_create_decal_text(plugin.panel,
	                                                 text,
	                                                 ts,
	                                                 style,
	                                                 -1,
	                                                 y,
	                                                 -1);

	return MAX(decal_text[idx].label->y, decal_text[idx].data->y) +
	       MAX(decal_text[idx].label->h, decal_text[idx].data->h);
}

static void populate_panel(void)
{
	int i, j, y, p;
	char* l;
	static char SIZE_STRING[] = "WWWWWWWW";

	for (y = -1, i = 0; i < GK_MAX_GPUS; ++i) {

		if (!gpu_info[i].good)
			continue;

		for (j = GPU_NAME; j < GPU_PROPS_NUM; ++j) {

			if (decal_info[j].enable) {
				p = decal_info[j].order;
				l = decal_info[j].label;
				y = create_decal_row(i, p, l, SIZE_STRING, y);
				/* Extra air so rows don't crush into chart labels above */
				y += ((j == GPU_NAME) ? 4 : 3);
			}

		}
	}
}

static void destroy_nv_panel(void)
{
	gkrellm_panel_destroy(plugin.panel);
	plugin.panel = NULL;
}

static void create_nv_panel(gint first_create)
{
	if (!plugin.panel)
		plugin.panel = gkrellm_panel_new0();

	populate_panel();

	gkrellm_panel_configure(plugin.panel,
	                        NULL,
	                        gkrellm_meter_style(plugin.style_id));

	gkrellm_panel_create(plugin.main_vbox, plugin.monitor, plugin.panel);

	if (first_create) {
		g_signal_connect(G_OBJECT(plugin.panel->drawing_area),
		                 "expose_event",
		                 G_CALLBACK(panel_expose_event),
		                 NULL);
		
		g_signal_connect(G_OBJECT(plugin.panel->drawing_area),
		                 "button_press_event",
		                 G_CALLBACK(panel_click_event),
		                 NULL);
	}
}

static void rebuild_nv_panel(void)
{
	destroy_nv_panel();
	create_nv_panel(TRUE);
}

static void shutdown_plugin(void)
{
	int i;

	for (i = 0; i < GK_MAX_GPUS; ++i)
		gpu_info[i].good = FALSE;

	shutdown_gpulib(&nvml);
}

static gint header_expose_event(GtkWidget *widget, GdkEventExpose *ev)
{
	if (!plugin.header_panel || !plugin.header_panel->pixmap)
		return FALSE;
	gdk_draw_pixmap(widget->window,
	                widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
	                plugin.header_panel->pixmap,
	                ev->area.x, ev->area.y, ev->area.x, ev->area.y,
	                ev->area.width, ev->area.height);
	return FALSE;
}

static void create_header_panel(gint first_create)
{
	GkrellmStyle *style = gkrellm_panel_style(plugin.style_id);

	if (!plugin.header_panel)
		plugin.header_panel = gkrellm_panel_new0();

	gkrellm_panel_configure(plugin.header_panel, "NVIDIA GB10", style);
	gkrellm_panel_create(plugin.main_vbox, plugin.monitor, plugin.header_panel);

	if (first_create)
		g_signal_connect(G_OBJECT(plugin.header_panel->drawing_area),
		                 "expose_event",
		                 G_CALLBACK(header_expose_event), NULL);
}

static void create_plugin(GtkWidget* vbox, gint first_create)
{
	if (first_create) {
		plugin.main_vbox = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), plugin.main_vbox, FALSE, FALSE, 0);
		gtk_widget_show(plugin.main_vbox);
		plugin.max_clock = 0;
		plugin.max_power_mw = 0;
		plugin.cum_load = plugin.cum_load_tot = 0;
		plugin.cum_clock = plugin.cum_clock_tot = 0;
		plugin.cum_power = plugin.cum_power_tot = 0;
	}

	if (initialize_gpulib(&nvml))
			update_gpu_info();

	gkrellm_disable_plugin_connect(plugin.monitor, shutdown_plugin);

	if (first_create) {
		/* Brand header first — GPU + CPU below are parts of GB10 */
		create_header_panel(TRUE);
		/* All three: grid_res 50 × 2 grids = full scale 100 */
		create_one_gpu_chart(&plugin.load_chart, &plugin.load_cfg,
		                     "GPU Load", 50, 2);   /* 0–100 % */
		create_one_gpu_chart(&plugin.clock_chart, &plugin.clock_cfg,
		                     "GPU Clock", 75, 2);  /* 2400–2550 MHz window */
		create_one_gpu_chart(&plugin.power_chart, &plugin.power_cfg,
		                     "GPU Power", 50, 2);  /* 0–100 W */
	} else {
		create_header_panel(FALSE);
	}

	create_nv_panel(first_create);
}

static void cb_toggle(GtkWidget *button, gpointer data)
{
	gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
	set_decal_enabled(GPOINTER_TO_INT(data), active);

	rebuild_nv_panel();
}

static void gkrellm_gtk_entry_set_icon(GtkWidget *widget, gboolean ok)
{
	static const char *ICON_OK = "gtk-yes";
	static const char *ICON_KO = "gtk-no";

	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(widget),
	                                  GTK_ENTRY_ICON_SECONDARY,
	                                  ok? ICON_OK : ICON_KO);
}

static void cb_pathchanged(GtkWidget *widget, gpointer data)
{
	UNUSED(data);
	gchar *text = NULL;
	gboolean valid_path = FALSE;

	gkrellm_dup_string(&text, gkrellm_gtk_entry_get_text(&widget));

	valid_path = is_valid_gpulib_path(text);
	gkrellm_gtk_entry_set_icon(widget, valid_path);

	reset_lib = valid_path;
	if (valid_path)
		strcpy(nvml.path, text);
}

/*
 * wrapper for gtk entry with label following the style of
 * gkrellm_gtk_check_button_connected or gkrellm_gtk_button_connected
 * found in gkrellm src/gui.c
 */
static void gkrellm_gtk_entry_connected(GtkWidget *box,
                                        GtkWidget **entry,
                                        gchar *text,
                                        gboolean expand,
                                        gboolean fill,
                                        gint pad,
                                        void (*cb_func)(),
                                        gpointer data,
                                        gchar *label)
{
	GtkWidget *l = gtk_label_new(label);
	GtkWidget *e = gtk_entry_new_with_max_length(GK_MAX_PATH);

	GtkWidget *h = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(h), l, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(h), e, TRUE, TRUE, 4);

	if (text)
		gtk_entry_set_text(GTK_ENTRY(e), text);
	
	if (box) {
		if (pad < 0)
			gtk_box_pack_end(GTK_BOX(box), h, expand, fill, -(pad + 1));
		else
			gtk_box_pack_start(GTK_BOX(box), h, expand, fill, pad);
	}
	
	if (cb_func)
		g_signal_connect(G_OBJECT(e), "changed", G_CALLBACK(cb_func), data);
	
	if (entry)
		*entry = e;
}

static void cb_drag_data_get(GtkWidget        *widget,
                             GdkDragContext   *context,
                             GtkSelectionData *selection_data,
                             guint             info,
                             guint             time,
                             gpointer          data)
{
	UNUSED(context);
	UNUSED(info);
	UNUSED(time);
	UNUSED(data);

	gtk_selection_data_set(selection_data,
	                       gtk_selection_data_get_target(selection_data),
	                       CHAR_BIT,
	                       (const guchar*)&widget,
	                       sizeof(gpointer));

}

static void cb_drag_data_received(GtkWidget        *widget,
                                  GdkDragContext   *context,
                                  gint              x,
                                  gint              y,
                                  GtkSelectionData *selection_data,
                                  guint             info,
                                  guint32           time,
                                  gpointer          data)
{
	UNUSED(context);
	UNUSED(x);
	UNUSED(y);
	UNUSED(info);
	UNUSED(time);
	UNUSED(data);
	GtkWidget *target, *source, *container;
	GList *children;
	int source_pos, target_pos;

	target = widget;
	source = *(gpointer*)gtk_selection_data_get_data(selection_data);
	container = gtk_widget_get_ancestor(source, GTK_TYPE_BOX);

	children = gtk_container_get_children(GTK_CONTAINER(container));
	source_pos = g_list_index(children, source);
	target_pos = g_list_index(children, target);

	if (source_pos > -1 && source_pos < GPU_PROPS_NUM - 1) {
		gtk_box_reorder_child(GTK_BOX(container),
		                      source,
		                      target_pos);

		gtk_box_reorder_child(GTK_BOX(container),
		                      target,
		                      source_pos);

		GKSWAP(decal_info[source_pos + 1], decal_info[target_pos + 1]);

		rebuild_nv_panel();
	}
}

static void create_plugin_tab(GtkWidget *tab_vbox)
{
	int i;
	GtkWidget *tabs, *vbox, *cntvbox, *nvml_entry, *button;
	
	static GtkTargetEntry dnd_entry[] = {
	 { "GkrellmNvidiaOption", GTK_TARGET_SAME_APP, 0 }
	};

	tabs = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tabs), GTK_POS_TOP);
	gtk_box_pack_start(GTK_BOX(tab_vbox), tabs, TRUE, TRUE, 0);

	vbox = gkrellm_gtk_framed_notebook_page(tabs, _(" Options "));

	gkrellm_gtk_entry_connected(vbox,
	                            &nvml_entry,
	                            nvml.path,
	                            FALSE,
	                            FALSE,
	                            0,
	                            cb_pathchanged,
	                            NULL,
	                            _("libNVML path"));

	gkrellm_gtk_entry_set_icon(nvml_entry, is_valid_gpulib_path(nvml.path));

	cntvbox = gkrellm_gtk_framed_vbox(vbox, _(" Counters "), 2, TRUE, 4, 4);

	for (i = GPU_NAME + 1; i < GPU_PROPS_NUM; ++i) {
		gkrellm_gtk_check_button_connected(cntvbox,
		                                   &button,
		                                   decal_info[i].enable,
		                                   FALSE,
		                                   FALSE,
		                                   0,
		                                   cb_toggle,
		                                   GINT_TO_POINTER(decal_info[i].order),
		                                   decal_info[i].optionlabel);

 		gtk_drag_source_set(button,
		                    GDK_BUTTON1_MASK,
		                    dnd_entry,
		                    1,
		                    GDK_ACTION_MOVE);

		gtk_drag_dest_set(button,
		                  GTK_DEST_DEFAULT_ALL,
		                  dnd_entry,
		                  1,
		                  GDK_ACTION_MOVE);

		g_signal_connect(button,
		                 "drag-data-get",
		                 G_CALLBACK(cb_drag_data_get),
		                 NULL);

		g_signal_connect(button,
		                 "drag-data-received",
		                 G_CALLBACK(cb_drag_data_received),
		                 NULL);
	}
}

static void apply_plugin_config(void)
{
	if (reset_lib) {
		if (reinitialize_gpulib(&nvml))
			update_gpu_info();
		rebuild_nv_panel();
		reset_lib = FALSE;
	}
}

static void save_plugin_config(FILE *f)
{
	guint i, config_mask = 0;
	static gchar config_order[GPU_PROPS_NUM + 1] = { '\0' };

	for (i = 0; i < GPU_PROPS_NUM; ++i) {
		config_mask |= (is_decal_enabled(i)? 1 : 0) << i;
		config_order[i] = 'a' + decal_info[i].order;
	}

	fprintf(f, "%s NVML %u %s %s\n", GK_CONFIG_KEYWORD,
	                                 config_mask,
	                                 config_order,
	                                 nvml.path);
}

static gboolean is_valid_ordering(gchar* order_string)
{
	char c;

	if (strlen(order_string) != GPU_PROPS_NUM)
		return FALSE;

	for (c = 'a'; c < 'a' + GPU_PROPS_NUM; ++c)
		if (!strchr(order_string, c))
			return FALSE;

	return TRUE;
}

static void load_plugin_config(gchar *arg)
{
	gchar config_key[16], config_order[16];
	gchar config_line[GK_MAX_PATH];
	gboolean read_config_ok = FALSE;
	guint i, prop_mask, config_mask, i_cfg, i_idx, j_idx;
	
	if (sscanf(arg, "%15s %511[^\n]", config_key, config_line) == 2) {
	
		if (!strcmp(config_key, "NVML"))
			if (sscanf(config_line, "%u %15s %511s", &config_mask,
			                                         config_order,
			                                         nvml.path) == 3)
				read_config_ok = is_valid_ordering(config_order) &&
				                 is_valid_gpulib_path(nvml.path);
	}

	if (read_config_ok) {

		for (i = 0; i < GPU_PROPS_NUM; ++i) {
			prop_mask = 1u << i;
			decal_info[i].enable = ((config_mask & prop_mask) == prop_mask);
		}

		for (i = 0; i < GPU_PROPS_NUM; ++i) {
			i_cfg = config_order[i] - 'a';
			i_idx = decal_info[i].order;
			if (i_idx != i_cfg) {
				j_idx = strchr(config_order, 'a' + i_idx) - config_order;
				GKSWAP(decal_info[i], decal_info[j_idx]);
			}
		}

	} else {
		strncpy(nvml.path, GKFREQ_NVML_SONAME, sizeof(nvml.path) - 1);
		/* Keep compile-time defaults in decal_info[] (charts cover load/clock/power) */
		for (i = 0; i < GK_MAX_GPUS; ++i)
			gpu_info[i].good = FALSE;
	}
}

static GkrellmMonitor plugin_mon =
{
	GK_PLUGIN_NAME,              /* Name, for config tab.                    */
	0,                           /* Id, 0 if a plugin                        */
	create_plugin,               /* The create_plugin() function             */
	update_plugin,               /* The update_plugin() function             */
	create_plugin_tab,           /* The create_plugin_tab() config function  */
	apply_plugin_config,         /* The apply_plugin_config() function       */

	save_plugin_config,          /* The save_plugin_config() function        */
	load_plugin_config,          /* The load_plugin_config() function        */
	GK_CONFIG_KEYWORD,           /* config keyword                           */

	NULL,                        /* Undefined 2                              */
	NULL,                        /* Undefined 1                              */
	NULL,                        /* Undefined 0                              */

	/* Right after clock/hostname — brand header + GPU before CPU plugins */
	MON_CLOCK | MON_INSERT_AFTER,
	NULL,                        /* Handle if a plugin, filled in by GKrellM */
	NULL                         /* path if a plugin, filled in by GKrellM   */
};

GkrellmMonitor* gkrellm_init_plugin(void)
{
	plugin.panel = NULL;
	plugin.header_panel = NULL;
	plugin.main_vbox = NULL;
	plugin.load_chart = plugin.clock_chart = plugin.power_chart = NULL;
	plugin.max_clock = 0;
	plugin.max_power_mw = 0;
	plugin.style_id = gkrellm_add_meter_style(&plugin_mon, GK_PLUGIN_NAME);
	plugin.chart_style_id = gkrellm_add_chart_style(&plugin_mon, GK_PLUGIN_NAME);
	plugin.monitor = &plugin_mon;
	strncpy(nvml.path, GKFREQ_NVML_SONAME, sizeof(nvml.path) - 1);

	return plugin.monitor;
}
