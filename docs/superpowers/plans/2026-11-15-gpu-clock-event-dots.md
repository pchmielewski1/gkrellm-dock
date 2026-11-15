# GPU clock-event status dots Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add one thin row of six bare green/red/gray dots between GPU Power and Temp/UMA in `nvidia.so`, driven by NVML clocks event reasons, with hover tooltips and docs — without changing anything above or below that row.

**Architecture:** Keep header + Load/Clock/Power charts and the existing Temp/UMA text panel untouched. Insert a **new** thin widget strip into `plugin.main_vbox` after the Power chart and before `create_nv_panel()`. Pure bit→state/latch logic lives in a small testable C module; UI draws six dots and attaches per-dot tooltips.

**Tech Stack:** C, GTK2, GKrellM2 plugin API, NVML via existing `dlopen` bindings in `nvml-lib`, Python unit tests for pure logic (same style as `tests/test_cpu_map.py`).

**Spec:** `docs/superpowers/specs/2026-11-15-gpu-clock-event-dots-design.md`

## Global Constraints

- Insert **only** between GPU Power and Temp/UMA; do not change header, charts, Temp/UMA layout, scales, or other plugins.
- Six dots L→R: Cap, SWth, HWth, HWsl, Brk, App — bare (no on-panel captions).
- Colors: green = OK, red = Active (or latched ~2–3 s), gray = unavailable — never green when unknown.
- Tooltips + docs share the same label/description text.
- Data via NVML `nvmlDeviceGetCurrentClocksEventReasons` (optional `dlsym`, like fans); no `nvidia-smi` shell-out.
- NVML bit constants (throttle/event reasons, identical values):

| Dot | Bit name | Mask |
|-----|----------|------|
| Cap | SwPowerCap | `0x4` |
| SWth | SwThermalSlowdown | `0x20` |
| HWth | HwThermalSlowdown | `0x40` |
| HWsl | HwSlowdown | `0x8` |
| Brk | HwPowerBrakeSlowdown | `0x80` |
| App | ApplicationsClocksSetting | `0x2` |

## Review Focus

- NVML symbol missing or call fails → all six dots gray, tooltips say unavailable; strip still present.
- Momentary Active bit → red for ≥2 s after clear (latch); other dots unaffected.
- Supported-mask bit not present for a reason → that dot gray (others may still be green/red).
- Rebuild/recreate of text panel must not destroy or reorder charts; strip pack order stays Power → dots → Temp/UMA.
- Tooltip English strings must match the docs table verbatim for the six names.

---

### Task 1: Pure clock-event dot state + latch

**Files:**
- Create: `plugins/nvidia/clock_event_dots.h`
- Create: `plugins/nvidia/clock_event_dots.c`
- Create: `tests/test_clock_event_dots.py`
- Modify: `plugins/nvidia/Makefile` (compile/link new `.c`; expose symbols for test if needed via existing test `.so` pattern or compile a tiny `build/clock_event_dots_test` object)

**Interfaces:**
- Produces:
  - `#define GK_CLOCK_EVENT_DOT_COUNT 6`
  - `enum { GK_DOT_OK=0, GK_DOT_ACTIVE=1, GK_DOT_UNKNOWN=2 };`
  - `typedef struct { int state[6]; double latch_until[6]; } GkClockEventDots;`
  - `void gk_clock_event_dots_init(GkClockEventDots *d);`
  - `void gk_clock_event_dots_update(GkClockEventDots *d, int available, unsigned long long reasons, unsigned long long supported, double now_sec);`
  - Latch duration constant: `2.5` seconds.
  - Dot index order fixed as Cap=0 … App=5 with the masks in Global Constraints.
  - Tooltip string getters: `const char *gk_clock_event_dot_label(int i);` and `const char *gk_clock_event_dot_desc(int i);` (EN, used by UI and copied into docs).

- [ ] **Step 1: Write failing tests** in `tests/test_clock_event_dots.py` covering: unavailable → all UNKNOWN; reasons `0` + full supported → all OK; bit `0x4` → Cap ACTIVE; clear Cap then `now+1` still ACTIVE, `now+3` OK; unsupported Cap bit → Cap UNKNOWN while others OK.

- [ ] **Step 2: Run tests — expect FAIL** (module/symbols missing).

- [ ] **Step 3: Implement `clock_event_dots.c/.h`** with the signatures above; wire Makefile so tests can load the logic (prefer compiling `clock_event_dots.c` into a small shared object under `build/` for Python `ctypes`, mirroring `build/libgkdock_test.so`).

- [ ] **Step 4: Run tests — expect PASS.**

- [ ] **Step 5: Commit** `feat(nvidia): pure clock-event dot state and latch`

---

### Task 2: NVML bind for current clocks event reasons

**Files:**
- Modify: `plugins/nvidia/nvml-lib.h`
- Modify: `plugins/nvidia/nvml-lib.c`
- Test: extend or add a tiny probe in `tests/test_clock_event_dots.py` **or** manual `nm -D` check documented in the verification step (Spark has the symbol).

**Interfaces:**
- Consumes: existing optional `dlsym` pattern for fans.
- Produces: optional function pointer `nvmlDeviceGetCurrentClocksEventReasons` and optionally `nvmlDeviceGetSupportedClocksEventReasons` on `GKNVMLLib` (NULL if absent). Signature: `(nvmlDevice_t, unsigned long long *reasons) → nvmlReturn_t`.

- [ ] **Step 1: Add optional `dlsym` binds** (do not fail `initialize_gpulib` if missing).

- [ ] **Step 2: Verify** `nm -D /usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1 | grep CurrentClocksEventReasons` still matches the bound name; rebuild `make -C plugins/nvidia clean && make -C plugins/nvidia`.

- [ ] **Step 3: Commit** `feat(nvidia): bind NVML clocks event reasons`

---

### Task 3: Insert dots strip UI (no chart / Temp-UMA changes)

**Files:**
- Modify: `plugins/nvidia/nvidia.c` (create strip after Power chart, before `create_nv_panel`; sample in `update_plugin`; draw/update colors; tooltips via `gtk_widget_set_tooltip_text` on per-dot event boxes — same API as `llm_nim.c`)
- Modify: `plugins/nvidia/Makefile` if needed for link

**Interfaces:**
- Consumes: Task 1 helpers + Task 2 NVML pointers.
- Produces: always-visible strip of 6 dots when GPU is `good`; gray when NVML reasons unavailable.

**Implementation notes (decisions locked):**
- Pack a dedicated `GtkHBox` (or thin `GkrellmPanel` only for the strip) into `plugin.main_vbox` **after** `create_one_gpu_chart(... power ...)` and **before** `create_nv_panel()`.
- Do **not** add Temp/UMA rows into that strip; do **not** alter `populate_panel()` row order beyond leaving it as-is.
- Drawing: six small circular indicators (~8–9px), evenly spaced; only recolor dots (no row chrome tint unless needed for visibility — prefer dots only).
- Each hit-target widget gets tooltip: `"{label} — {desc}"` or when UNKNOWN append `" (status unavailable)"`.
- Always-on for Spark (no new mask bit unless mask churn is trivial; prefer always-on to avoid breaking `nvidia NVML 272 …` lines).

- [ ] **Step 1: Create strip widgets** in `create_plugin` / helpers without changing chart or text-panel code paths beyond pack order.

- [ ] **Step 2: Sample reasons** each `update_plugin` tick into `gk_clock_event_dots_update` using `g_get_monotonic_time()/1e6` (or equivalent) for latch.

- [ ] **Step 3: Manual check** — reload plugin; confirm visually: charts and Temp/UMA unchanged; new row between them; hover shows tooltips; all green or gray as appropriate on this host.

- [ ] **Step 4: Commit** `feat(nvidia): clock-event status dots between Power and Temp`

---

### Task 4: Documentation

**Files:**
- Modify: `docs/PLUGINS.md` (nvidia section)
- Modify: `docs/PLUGINS_CLI.md` (matching What-it-shows table)

**Interfaces:**
- Consumes: exact `gk_clock_event_dot_label` / `_desc` strings from Task 1.

- [ ] **Step 1: Add subsection** for the dots strip: position (above Temp/UMA), L→R table, green/red/gray, latch ~2.5s, tooltips, omitted Idle/Sync/Display.

- [ ] **Step 2: Commit** `docs(nvidia): document clock-event status dots`

---

## Spec coverage (self-review)

| Spec requirement | Task |
|------------------|------|
| One row between Power and Temp/UMA | 3 |
| Six bare dots Cap…App | 1+3 |
| Green/red/gray | 1+3 |
| Latch 2–3s | 1 |
| Tooltips | 3 |
| Docs | 4 |
| No change above/below | 3 (separate strip) |
| NVML not smi | 2 |
| Unavailable → gray | 1+3 |
