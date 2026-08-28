# Plugins, panels and config reference

Exact reference for every GKrellM plugin/panel in the DGX Spark GB10 dock and every managed config key. Values below come from `scripts/install_config.sh` and the plugin sources (`plugins/*/`).

---

## Plugin load order

Five `.so` files, listed top-to-bottom in `~/.gkrellm2/plugin_enable` (written by `scripts/install_config.sh`):

```text
nvidia.so → llm_nim.so → cpu_clusters.so → uma_dram.so → board_acpi.so
```

Panel anchors (where each plugin lands in the monitor stack): `nvidia` — after clock/hostname (`MON_CLOCK | MON_INSERT_AFTER`); `uma_dram` — memory area (`MON_MEM`); `board_acpi` — memory area, inserted after (`MON_MEM | MON_INSERT_AFTER`); `cpu_clusters` — process/CPU area (`MON_PROC`).

---

## Plugins

### `nvidia.so` — GPU (NVML)

**Source tree:** `plugins/nvidia/` (derived from [gkrellm-nvidia](https://github.com/carcass82/gkrellm-nvidia)). All data from libNVML (`nvmlDeviceGet*`), except the UMA row (host `/proc/meminfo`). Panel retries NVML init every ~5 s if the driver was not ready at create time.

| Element | Source | Scale / notes |
|---------|--------|----------------|
| Header bar | hard-coded string `"NVIDIA GB10"` | brand header above the charts |
| GPU Load chart | `nvmlDeviceGetUtilizationRates` (SM) | fixed 0–100 %, 2 grids × 50, 48 px, impulse bars, text `%u%%` |
| GPU Clock chart | `nvmlDeviceGetClockInfo` (GFX) | **chart window 2400–2550 MHz** (out-of-window values clamped on the chart only), 2 grids × 75 = 150 MHz span, text shows raw `%uMHz` |
| GPU Power chart | `nvmlDeviceGetPowerUsage` | fixed 0–100 W, 2 grids × 50, text shows raw `%uW` |
| Temp row | `nvmlDeviceGetTemperature` | text `NN.N C`, one decimal (e.g. `45.0C`; source format `"%.01fC"`) |
| UMA row | `/proc/meminfo` → used % = (Total − Available)/Total × 100, clamped 0–100 | text `%u%%` |

NVML memory-usage values are unused on GB10 (UMA → N/A); the UMA row falls back to host `/proc/meminfo` when NVML reports nothing. Clocks above ~3003 MHz (NVML hardware max) are possible; the 2400–2550 window is intentional (default boost 2418 MHz, ~2500–2550 under load — wider windows peg or flatten the chart).

**Config key** (managed):

```text
nvidia NVML 272 abcdefghijkl /usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1
```

- `272` — property visibility mask (bit N = property N). `272 = 256 + 16` → **Temp** (bit 4 = 16) + **UMA** (bit 8 = 256).
- `abcdefghijkl` — display order of the 12 property slots (identity order here).
- Path — Ubuntu aarch64 NVML soname.

**Settings UI** (right-click panel or **Configure → Plugins → nvidia**, single **Options** page):

![nvidia plugin options](configure-nvidia-options.png)

| Field | What it does |
|-------|----------------|
| **libNVML path** | Text entry with valid/invalid icon (green ✓ when NVML loads). Changing it re-initializes NVML on **Apply** / **OK**. Default on Spark: `/usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1`. |
| **Counters** (checkboxes, drag to reorder) | Each checked row becomes a dock strip. Order + mask saved to `nvidia NVML …` in `user-config`. |

| Counter checkbox | Dock strip when enabled |
|------------------|-------------------------|
| GPU Load | Load % chart (0–100) |
| GPU Clock | Clock chart (managed window 2400–2550 MHz) |
| GPU Memory Clock | Text row (MHz) |
| GPU Temperature | Temp in the text row under charts |
| GPU Fan Speed | Text row (RPM) |
| GPU Fan Speed (percentage) | Text row (%) |
| GPU Power Draw | Power chart (W) |
| Unified Memory (host) | UMA % in the text row |
| GPU Used Memory | Text row |
| GPU Reserved Memory | Text row |
| GPU Total Memory | Text row |

Managed install enables Load, Clock, Power, Temp, UMA (mask `272`). Fan and memory breakdown rows stay off unless you check them here.

**Build pitfall:** link **both** `nvidia.o` and `nvml-lib.o`. A partial link produces a plugin that loads with an empty GPU block. Prefer `make -C plugins/nvidia clean && make -C plugins/nvidia`.

### `llm_nim.so` — NIM / vLLM / SGLang

Compact dock panel for a local OpenAI-compatible inference server (reference: NIM/vLLM container on port **8000**, or SGLang with `--enable-metrics`). Scrapes `GET /metrics` (Prometheus) and optionally `GET /v1/models` for the header name. **vLLM/NIM:** primary `vllm:*` counters and histograms. **SGLang:** auto-detected via `sglang:gen_throughput`, `sglang:token_usage`, or `sglang:prompt_tokens_total`; full overlay maps histograms, token counters, spec/HTTP/CPU and approximate RSS (see `docs/LLM_NIM_UI.md` → Metrics backends). Without `--enable-metrics`, `/metrics` 404s → `down`. vLLM-only strips (**Dc**, **Rn**, **Bt**, **Xp**, engine sleep states) stay empty on SGLang. Shows a header + engine status lamp, session `in`/`out` token totals (Σ Δ of prompt/generation token counters, this GKrellM process only), and per-feature strips/charts selected by the `features` bitmask (managed default `1908735`; core-only `15`). Up to 4 scrape slots (slot 0 full panel; slots 1–3 compact). Has its own settings notebook (Connection / Catalog / Local / Recipes / Instances / Options / Display); full reference: `docs/LLM_NIM_UI.md`. Managed defaults: `url http://127.0.0.1:8000`, `docs_release 2.0.10`, `airgap 0`, `chart_max_tps 50`, `chart_max_prefill 1000`, `timeout_ms 500` — preserved across config re-apply if already present in `user-config`. Offline → dashes / down, charts at zero; depends on libcurl.

### `cpu_clusters.so` — X925 / A725

**Source tree:** `plugins/cpu_clusters/` + `lib/cpu_map.c`, `lib/cpu_stat.c`.

Cores are classified from `/proc/cpuinfo` **CPU part**:

| Cluster | Part ID | Typical cores (fallback) |
|---------|---------|---------------------------|
| X925 | `0xd85` | 5–9, 15–19 |
| A725 | `0xd87` | 0–4, 10–14 |

Fallback lists apply if part IDs are missing. ACPI `CLU0`/`CLU1` groupings are **not** used (they mix E+P differently).

Two tall charts (**120 px**, label `CPU X925` / `CPU A725`) with one equal impulse mini-band per core (warm amber shade ramp, one per core index) on a fixed 0–100 % scale per band, from `/proc/stat` (`/proc/stat` busy/idle via `lib/cpu_stat.c`, one tick per GKrellM second). Panel label + krell show the **cluster average** busy % across all its cores. Stock composite `cpu` is disabled in managed config so it does not duplicate these panels. No `user-config` keys, no settings tab.

### `uma_dram.so` — DRAM (UMA)

**Source tree:** `plugins/uma_dram/`. GB10 uses unified memory; the ACPI `DRAM8901` device exposes MMIO only (no usable OS telemetry), so the panel reads `MemTotal` / `MemAvailable` from `/proc/meminfo`: used ≈ Total − Available (available clamped to total).

- Panel `"DRAM UMA"` with krell, full scale 100.
- Chart: **used %** on fixed 0–100 % scale (2 grids × 50), LINE style, 40 px; text overlay `%lu%%`.
- Text panel: `used/total GB` (GB = MB/1024, integer) and `%` (clamped 0–100).

Stock **Mem** meter stays **off** (theme krells crush its label). Stock **Swap** stays **on**. No `user-config` keys, no settings tab.

### `board_acpi.so` — Board thermals

**Source tree:** `plugins/board_acpi/` + `lib/thermal_map.c`. A **Board** title bar above seven temperature rows (no Fan). Full sensor guide: [SENSORS.md](SENSORS.md).

| Label | Role (summary) |
|-------|----------------|
| **TSOC** | SoC / package platform temperature |
| **TGPU** | GPU-domain ACPI temperature |
| **TS0E** | Sensor bank 0, edge |
| **TS0P** | Sensor bank 0, proximity / package point |
| **TS1E** | Sensor bank 1, edge |
| **TS1P** | Sensor bank 1, proximity / package point |
| **TUNC** | Uncore / auxiliary platform sensor |

Discovery: `/sys/class/thermal/thermal_zone*/temp` + ACPI short name from the zone's device path (`gk_thermal_map_scan_sysfs`). Values rendered `NN.NC` (millidegrees ÷ 1000, one decimal; the `%.0fC` format spec keeps that precision), right-aligned; missing zones show `—`. No `user-config` keys, no settings tab.

---

## Stock panels (managed by install_config.sh)

| Panel | Managed setting |
|-------|-----------------|
| Hostname | **On, full** (`enable_hostname 1`, `hostname_short 0`) |
| Sysname | Off (`enable_sysname 0`) |
| Composite CPU + all per-core | **Off** (`cpu enabled cpu 0`, `cpu enabled cpu0`–`cpu19 0`) — replaced by `cpu_clusters.so` |
| Mem meter | **Off** (`meminfo mem_meter 0 0 0`) — replaced by `uma_dram.so` (theme krells crush its label) |
| Swap | **On** as meter with name label (`meminfo swap_meter 1 0`, chart off: `swap_chart 0 0`) |
| Mail | **Off** (`mail enable 0 0 0 0`) — frees vertical space |
| Uptime | **On** (`uptime enable 1`) |

### Disk

- Composite device **Disk** only (per-disk charts disabled; the managed config still lists `nvme0n1` / `sda` device entries, flags off).
- Fixed chart config: **5 × 40 MB/s = 200 MB/s** full scale (stock auto-scale is unreadable on NVMe).

```text
disk assign_method 2
disk device Disk 0 0 0 1 1 0 0 Disk
disk chart_config Disk 40 40000000 5 0 0 1 : 0 0 0 0 500 : 0 0 0 0 500
disk device nvme0n1 0 0 28 0 1 -1 0 nvme0n1
disk device sda 0 0 45 0 1 -1 0 sda
```

### Net

```text
net timer_enabled 0
net timer_iface none
net net_enabled_as_default 0
net ignore_patterns ^ppp[0-9]+$|^br-|^virbr|^tun|^tap
net enables docker0 1 1 0
net enables wlP9s9 1 1 0
net enables enP7s7 1 1 0
net enables <live-veth> 1 1 0   # appended dynamically for every UP veth*
```

- **Enabled:** physical NICs used on Spark (`wlP9s9`, `enP7s7`), `docker0`, and every **UP** `veth*` (host side of running containers) — names read live from `/sys/class/net/veth*/operstate` at install time.
- **Ignored:** PPP, Docker compose bridges (`br-*`), virbr, tun/tap.
- Stale `veth*` cookies under `~/.gkrellm2/data/net/` are pruned; live ones kept. `ppp0` / `ppp0_disabled` cookies are dropped.
- After **recreating a container, restart the dock** so the new veth name gets enabled.
- Wi‑Fi/Ethernet NIC names may differ on some OEM images — edit `install_config.sh` if yours do, then reinstall config.

---

## Managed user-config keys

Every key `scripts/install_config.sh` writes to `~/.gkrellm2/user-config` (GKrellM 2.3.11 managed header). `llm_nim *` keys are **preserved** from the existing file if present; everything else is rewritten.

### Window / dock

| Key | Value | Meaning |
|-----|-------|---------|
| `enable_hostname` | `1` | Show hostname panel |
| `hostname_short` | `0` | Use full hostname (not short) |
| `enable_sysname` | `0` | No sysname panel |
| `mbmon_port` | `0` | mbmon hardware monitoring off |
| `sticky_state` | `1` | Panel sticky (visible on all workspaces) |
| `dock_type` | `0` | No WM dock integration (floating panel) |
| `decorated` | `0` | No window-manager decoration |
| `skip_taskbar` | `1` | Don't show in taskbar |
| `skip_pager` | `1` | Don't show in pager |
| `above` | `1` | Keep window above others |
| `below` | `0` | (not below) |
| `track_gtk_theme_name` | `0` | Don't track GTK theme |
| `default_track_theme` | `"gb10-blue"` | Theme to use |
| `save_position` | `1` | Persist/restore window position (`~/.gkrellm2/data/startup_position`) |
| `chart_width` | `120` | Chart width in px |
| `update_HZ` | `10` | Update rate (samples per second) |
| `allow_multiple_instances` | `0` | Single gkrellm instance |
| `float_factor` | `1000` | Float display factor (GKrellM internal; values scaled ×1/1000) |

### CPU block

| Key | Value | Meaning |
|-----|-------|---------|
| `cpu enabled cpu` | `0` | Stock composite CPU chart off (replaced by `cpu_clusters.so`) |
| `cpu enabled cpu0` … `cpu enabled cpu19` | `0` (×20) | All stock per-core charts off |
| `cpu show_panel_labels` | `1` | Show CPU panel labels |
| `cpu omit_nice_mode` | `0` | Nice-time accounting on |
| `cpu config_tracking` | `1` | Stock CPU per-core config tracking on |

### Mem / Swap / Uptime / Mail

| Key | Value | Meaning |
|-----|-------|---------|
| `meminfo mem_meter` | `0 0 0` | Stock Mem meter off (`uma_dram.so` is the readout) |
| `meminfo swap_meter` | `1 0` | Stock Swap meter **on** with name label |
| `meminfo swap_chart` | `0 0` | Swap chart off |
| `uptime enable` | `1` | Uptime panel on |
| `mail enable` | `0 0 0 0` | Mail off (frees vertical space) |

### Net block

| Key | Value | Meaning |
|-----|-------|---------|
| `net timer_enabled` | `0` | No net timer |
| `net timer_iface` | `none` | — |
| `net net_enabled_as_default` | `0` | Don't auto-enable discovered interfaces |
| `net ignore_patterns` | `^ppp[0-9]+$\|^br-\|^virbr\|^tun\|^tap` | Skip PPP / compose bridges / virbr / tun / tap |
| `net enables docker0` | `1 1 0` | Docker bridge enabled |
| `net enables wlP9s9` | `1 1 0` | Wi-Fi NIC enabled |
| `net enables enP7s7` | `1 1 0` | Ethernet NIC enabled |
| `net enables <veth>` | `1 1 0` | One line per **UP** container veth, appended at install time |

### Disk block

| Key | Value | Meaning |
|-----|-------|---------|
| `disk assign_method` | `2` | Composite-only device assignment |
| `disk device Disk` | `0 0 0 1 1 0 0 Disk` | Composite **Disk** device descriptor (fixed) |
| `disk chart_config Disk` | `40 40000000 5 0 0 1 : 0 0 0 0 500 : 0 0 0 0 500` | Fixed chart: height 40, grid 40 MB/s (40000000 B/s), 5 grids → **200 MB/s** full scale; two data entries |
| `disk device nvme0n1` | `0 0 28 0 1 -1 0 nvme0n1` | Per-disk entry, off |
| `disk device sda` | `0 0 45 0 1 -1 0 sda` | Per-disk entry, off |

### nvidia

| Key | Value | Meaning |
|-----|-------|---------|
| `nvidia NVML` | `272 abcdefghijkl /usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1` | Mask `272` = Temp (16) + UMA (256); identity row order; Ubuntu aarch64 NVML path |

### llm_nim

| Key | Default written | Meaning (preserved if already present) |
|-----|-----------------|------------------------------------------|
| `llm_nim url` | `http://127.0.0.1:8000` | Base URL of the NIM/vLLM server |
| `llm_nim display_name` | *(empty)* | Header title override |
| `llm_nim dock_slots` | `1` | Enabled slot count hint (cap 4) |
| `llm_nim slot1_url` … `slot3_url` | *(empty)* | Extra dock scrape URLs (slots 1–3) |
| `llm_nim slot1_name` … `slot3_name` | *(empty)* | Extra dock display names |
| `llm_nim docs_release` | `2.0.10` | NIM docs / schema seed pin |
| `llm_nim airgap` | `0` | `1` = air-gap / offline |
| `llm_nim features` | `1908735` | Display bitmask (throughput + TTFT/ITL/E2E/TPOT + prefix/spec/prompt-cache + preempt + req/s + RSS + CPU + wait-reason + engine lamp; core-only = `15`) |
| `llm_nim chart_max_tps` | `50` | Decode chart full scale (t/s) |
| `llm_nim chart_max_prefill` | `1000` | Prefill chart base scale (t/s) |
| `llm_nim timeout_ms` | `500` | HTTP scrape timeout (ms) |

Separate file `~/.gkrellm2/plugin_enable` holds the five load-order lines (see [Plugin load order](#plugin-load-order)).

---

## Where things live on disk

| Path | Contents |
|------|----------|
| `~/.gkrellm2/plugins/` | The five plugin `.so` files |
| `~/.gkrellm2/themes/gb10-blue/` | Dock theme (pass to gkrellm as `-t ~/.gkrellm2/themes/gb10-blue`) |
| `~/.gkrellm2/user-config` | Managed GKrellM user config (all keys above) |
| `~/.gkrellm2/plugin_enable` | Plugin load order (5 lines) |
| `~/.gkrellm2/data/` | Runtime data: `startup_position` (GKrellM window `x y`, kept when `save_position 1`), `net/` per-interface cookies (`veth*` pruned on install, `ppp0*` dropped) |
| `~/.config/gkrellm-dock/` | Optional: NIM token secret files (`ngc_api_key`, `hf_token`) written by the LLM NIM Connection tab when the helper is missing |

Starting the dock: `gkrellm -t ~/.gkrellm2/themes/gb10-blue`. Window placement / login autostart is handled by the host environment, not this repository.

---

## Notes

- The `nvidia` header string is hard-coded as `"NVIDIA GB10"` in the source; the NVML product-name row exists in the nvidia settings panel but is disabled by the managed mask `272` — enable it there to show the NVML name.
- `install_config.sh` writes `llm_nim dock_slots 1` as its managed default; the plugin recomputes it from the enabled slots on every save.
- The `disk device` / `net enables` flag fields beyond the documented on/off intent (composite on, per-disk off; `1 1 0` for enabled interfaces) are GKrellM-internal and not further documented here — treat those lines as managed-as-is.
- `mbmon_port 0` and the three `hostname_*`-adjacent keys (`enable_sysname`, `track_gtk_theme_name`) are stock GKrellM keys written for determinism; they have no dock-specific meaning.
- Managed config targets GKrellM **2.3.11** (per the header comment written to `user-config`).
