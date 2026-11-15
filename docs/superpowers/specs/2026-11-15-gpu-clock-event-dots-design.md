# GPU clock-event status dots (nvidia.so)

**Date:** 2026-11-15  
**Status:** Approved visually; written for implementation  
**Scope:** `plugins/nvidia/` + docs for that plugin only

## Intent

Add a single, always-visible status strip that surfaces NVIDIA **Clocks Event Reasons** as colored dots, so a glance at the dock shows whether the GPU is being held back (power, thermal, HW brake, etc.) without running `nvidia-smi -q -d PERFORMANCE`.

## Why

The dock already shows Load, Clock, Power, Temp, and UMA. Those do not say *why* the clock is held. Event reasons answer that. Screen height is nearly full (~1 cm free), so the UI must be one thin row only.

## Goals

- One new meter row in **`nvidia.so`**, placed **between GPU Power (charts) and the Temp / UMA text rows**.
- Six bare dots (no on-panel letter labels), left → right:
  1. **Cap** — SW Power Cap  
  2. **SWth** — SW Thermal Slowdown  
  3. **HWth** — HW Thermal Slowdown  
  4. **HWsl** — HW Slowdown  
  5. **Brk** — HW Power Brake Slowdown  
  6. **App** — Applications Clocks Setting  
- **Green** = reason not active (OK). **Red** = reason active (problem). **Gray** = unknown / unavailable (NVML call failed, symbol missing, or value not reported) — never pretend OK with green.
- **Latch:** if a reason is Active even briefly, keep that dot red for ~2–3 seconds so flashes are visible. Latch does not apply to gray.
- **Hover tooltip** per dot: short label + one-line description (same wording as docs). When gray, tooltip still names the bit and notes that status is unavailable.
- Document the mapping in `docs/PLUGINS.md` / `docs/PLUGINS_CLI.md` (and keep them consistent), including gray = unavailable.

## Non-goals

- New plugin / separate `.so`.
- Performance State (P0…) on the panel.
- Clocks Event Reasons **Counters** (µs) on the panel.
- Showing Idle, Sync Boost, or Display Clock Setting as dots (not treated as “problems” for this strip).
- Changing chart scales, Temp/UMA layout, header text, plugin mask semantics for existing counters, or any other plugin.

## Anti-goals

- **Do not change anything above or below the new strip.** Header, GPU Load / Clock / Power charts, and Temp / UMA rows must look and behave as they do today. Only insert the new row in between.
- Do not grow the dock by more than one thin meter row (~one line of height).
- Do not add permanent text labels next to the dots on the panel (tooltips + docs only).

## Constraints

- Data source: **NVML** already used by the plugin (`libnvidia-ml`), not shelling out to `nvidia-smi`.
  - Preferred: `nvmlDeviceGetCurrentClocksEventReasons` (bits). Fallback acceptable if only throttle-reasons API is available on older headers — builder picks the working bind for Spark driver 580.x.
- GTK2 / GKrellM2 decals and styles already used by `nvidia.c`; match existing meter row height/margins.
- Tooltips: use whatever GKrellM/GTK2 path already works in-tree for hover help on decals/widgets; if none exists, GtkTooltips / `gtk_widget_set_tooltip_text` equivalent for the GTK version this dock builds against — builder chooses the smallest working approach without new dependencies.
- Managed install / default enable: new strip on by default for this dock (Spark); if the plugin already has a visibility mask for text rows, add a dedicated enable bit or always-on strip — **builder call:** prefer a new optional counter/mask bit defaulting **on** in managed config, so power users can hide it; if that risks mask churn or breaking existing `nvidia NVML …` lines, always-on without a mask bit is acceptable.
- **NVML failure / unsupported:** keep the row; paint dots **gray** (user decision). Do not hide the strip and do not show green.

## UI behavior (detail)

| State | Appearance |
|-------|------------|
| All OK | Six green dots on one meter row; panel chrome unchanged |
| Any Active (or latched) | That dot red; optional subtle row border/tint is OK if it does not affect Temp/UMA or charts — **builder call:** prefer recoloring only the dots first; skip row chrome tint unless dots alone are hard to notice |
| Unavailable | All six dots **gray** (or individually gray if a bit is not in the supported mask); row still present so dock height stays stable |
| Hover | Tooltip: e.g. `Cap — SW Power Cap: power limit holding clocks` (EN in code strings OK if the rest of the plugin is English; docs may mirror in EN as existing PLUGINS.md). Gray: same label + “status unavailable”. |

Sampling: same update cadence as other NVML polls in the plugin.

**Visual reference:** approved mockup from brainstorm session (`.superpowers/brainstorm/…/gpu-throttle-dots-c6-bare.html`): six evenly spaced bare dots, no on-panel captions; match existing meter row height/margins.

## Docs (required)

In the nvidia plugin section, add a short subsection for the dots strip: order L→R, green/red meaning, latch note, tooltip = same text, and that Idle / Sync Boost / Display Clock are intentionally omitted. Update CLI twin doc if it duplicates the What-it-shows table.

## Out of scope for first ship

- History charts of event reasons.
- Click-to-copy `nvidia-smi` command.
- Multi-GPU layouts beyond what the plugin already supports (if multi-GPU exists, one strip per GPU panel as today — **builder call:** follow existing per-GPU panel structure).

## Implementation home

- `plugins/nvidia/nvml-lib.h` / `nvml-lib.c` — bind event-reasons API.  
- `plugins/nvidia/nvidia.c` — create/draw strip, sample bits, latch, tooltips.  
- `scripts/install_config.sh` / managed config only if a new mask bit or enable flag is needed.  
- `docs/PLUGINS.md`, `docs/PLUGINS_CLI.md`.

## Success criteria

- Visual: Power charts above and Temp/UMA below pixel-layout-equivalent to before; only new row inserted between.
- Functional: forcing / observing an Active reason (e.g. SW Power Cap under load) turns the matching dot red; after clear, returns to green after latch window.
- Docs list all six dots with the same names as tooltips.
