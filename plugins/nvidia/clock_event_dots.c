/*****************************************************************************
 * Pure clock-event → status-dot mapping for the nvidia GKrellM plugin.
 *****************************************************************************/
#include "clock_event_dots.h"

#include <string.h>

static const unsigned long long dot_masks[GK_CLOCK_EVENT_DOT_COUNT] = {
	GK_NVML_REASON_SW_POWER_CAP,
	GK_NVML_REASON_SW_THERMAL,
	GK_NVML_REASON_HW_THERMAL,
	GK_NVML_REASON_HW_SLOWDOWN,
	GK_NVML_REASON_HW_POWER_BRAKE,
	GK_NVML_REASON_APP_CLOCKS,
};

static const char *const dot_labels[GK_CLOCK_EVENT_DOT_COUNT] = {
	"Cap", "SWth", "HWth", "HWsl", "Brk", "App",
};

static const char *const dot_descs[GK_CLOCK_EVENT_DOT_COUNT] = {
	"SW Power Cap: power limit holding clocks",
	"SW Thermal Slowdown: software thermal throttling",
	"HW Thermal Slowdown: hardware thermal throttling",
	"HW Slowdown: general hardware slowdown",
	"HW Power Brake: hardware power brake",
	"Applications Clocks: application or locked clocks setting",
};

void gk_clock_event_dots_init(GkClockEventDots *d)
{
	int i;

	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	for (i = 0; i < GK_CLOCK_EVENT_DOT_COUNT; ++i)
		d->state[i] = GK_DOT_UNKNOWN;
}

void gk_clock_event_dots_update(GkClockEventDots *d,
                                int available,
                                unsigned long long reasons,
                                unsigned long long supported,
                                double now_sec)
{
	int i;

	if (!d)
		return;

	if (!available) {
		for (i = 0; i < GK_CLOCK_EVENT_DOT_COUNT; ++i) {
			d->state[i] = GK_DOT_UNKNOWN;
			d->latch_until[i] = 0.0;
		}
		return;
	}

	for (i = 0; i < GK_CLOCK_EVENT_DOT_COUNT; ++i) {
		unsigned long long bit = dot_masks[i];

		if ((supported & bit) == 0ULL) {
			d->state[i] = GK_DOT_UNKNOWN;
			d->latch_until[i] = 0.0;
			continue;
		}

		if (reasons & bit) {
			d->state[i] = GK_DOT_ACTIVE;
			d->latch_until[i] = now_sec + GK_CLOCK_EVENT_LATCH_SEC;
			continue;
		}

		if (d->latch_until[i] > now_sec)
			d->state[i] = GK_DOT_ACTIVE;
		else {
			d->state[i] = GK_DOT_OK;
			d->latch_until[i] = 0.0;
		}
	}
}

const char *gk_clock_event_dot_label(int i)
{
	if (i < 0 || i >= GK_CLOCK_EVENT_DOT_COUNT)
		return "";
	return dot_labels[i];
}

const char *gk_clock_event_dot_desc(int i)
{
	if (i < 0 || i >= GK_CLOCK_EVENT_DOT_COUNT)
		return "";
	return dot_descs[i];
}

unsigned long long gk_clock_event_dot_mask(int i)
{
	if (i < 0 || i >= GK_CLOCK_EVENT_DOT_COUNT)
		return 0ULL;
	return dot_masks[i];
}
