/*****************************************************************************
 * Pure clock-event → status-dot mapping for the nvidia GKrellM plugin.
 *****************************************************************************/
#ifndef GK_CLOCK_EVENT_DOTS_H
#define GK_CLOCK_EVENT_DOTS_H

#define GK_CLOCK_EVENT_DOT_COUNT 6
#define GK_CLOCK_EVENT_LATCH_SEC 2.5

enum {
	GK_DOT_OK = 0,
	GK_DOT_ACTIVE = 1,
	GK_DOT_UNKNOWN = 2
};

/* NVML clocks event / throttle reason bits (same values either API). */
#define GK_NVML_REASON_SW_POWER_CAP              0x0000000000000004ULL
#define GK_NVML_REASON_SW_THERMAL                0x0000000000000020ULL
#define GK_NVML_REASON_HW_THERMAL                0x0000000000000040ULL
#define GK_NVML_REASON_HW_SLOWDOWN               0x0000000000000008ULL
#define GK_NVML_REASON_HW_POWER_BRAKE            0x0000000000000080ULL
#define GK_NVML_REASON_APP_CLOCKS                0x0000000000000002ULL

typedef struct {
	int state[GK_CLOCK_EVENT_DOT_COUNT];
	double latch_until[GK_CLOCK_EVENT_DOT_COUNT];
} GkClockEventDots;

void gk_clock_event_dots_init(GkClockEventDots *d);

/*
 * available=0 → all UNKNOWN.
 * supported bit clear for a dot → that dot UNKNOWN.
 * reason bit set → ACTIVE and latch; after clear, stay ACTIVE until now_sec
 * passes latch_until.
 */
void gk_clock_event_dots_update(GkClockEventDots *d,
                                int available,
                                unsigned long long reasons,
                                unsigned long long supported,
                                double now_sec);

/*
 * Force ACTIVE + latch on one dot (e.g. Cap/SWth after a ViolationStatus
 * blip between polls). No-op for out-of-range index or UNKNOWN dots.
 */
void gk_clock_event_dots_pulse(GkClockEventDots *d, int index, double now_sec);

const char *gk_clock_event_dot_label(int i);
const char *gk_clock_event_dot_desc(int i);
unsigned long long gk_clock_event_dot_mask(int i);

#endif
