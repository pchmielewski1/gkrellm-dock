# Plugins and panels

Reference for every monitor in the DGX Spark GB10 dock: data sources, UI, defaults, and config keys.

Managed keys are written by `scripts/install_config.sh` (run by `scripts/install.sh`). **LLM NIM** prefs (`url`, `display_name`, `docs_release`, `airgap`, `features`, chart scales, timeout, slots) are **preserved** if already present — Connection + Display choices survive re-apply. Tokens live under `~/.config/gkrellm-dock/` (not `user-config`).

---

## Plugin load order

```text
nvidia.so → llm_nim.so → cpu_clusters.so → net_clusters.so → board_acpi.so   (uma_dram.so: built, not enabled by default — duplicates UMA % in the GPU block)
```

---

## `nvidia.so` — GPU (NVML)

**Source tree:** `plugins/nvidia/` (derived from [gkrellm-nvidia](https://github.com/carcass82/gkrellm-nvidia)).

### What it shows

| Element | Meaning | Scale / notes |
|---------|---------|----------------|
| Header | Brand header — hard-coded `"NVIDIA GB10"` (the NVML product-name row exists in the settings panel but is off in the managed mask `272`) | — |
| GPU Load | SM utilization | 0–100 % |
| GPU Clock | Graphics clock | **Chart window 2400–2550 MHz**; label shows raw MHz |
| GPU Power | Board power draw | 0–100 W chart scale |
| Text row | Temperature + UMA memory % | NVML mask includes temp + memory |

Clock values outside 2400–2550 are **clamped on the chart** only; the text readout still shows the real MHz. Hardware max clock on GB10 is higher (~3003 MHz via NVML); the narrow window is intentional for readable mid-band activity.

### Config

```text
nvidia NVML 272 abcdefghijkl /usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1
```

- `272` — property visibility mask (includes Temp + UMA%).  
- Library path is the Ubuntu aarch64 NVML soname.

### Build pitfall

Link **both** `nvidia.o` and `nvml-lib.o`. A partial link produces a plugin that loads with an empty GPU block. Prefer `make -C plugins/nvidia clean && make -C plugins/nvidia`.

---

## `llm_nim.so` — NIM / vLLM / SGLang / TensorFold (Nemotron)

**Source tree:** `plugins/llm_nim/`.

Compact panel for a local OpenAI-compatible NIM/vLLM server (reference container: `nemotron-nim` serving `nvidia/nemotron-3-super-120b-a12b` on port **8000**). Scrapes `GET /metrics` (Prometheus) and optionally `GET /v1/models` for the header name.

**Metrics backends:** primary vLLM/NIM (`vllm:*` counters + histograms). Secondary SGLang: auto-detected via `sglang:gen_throughput` / `token_usage` / `prompt_tokens_total`; full overlay maps histograms, token counters, spec/HTTP/CPU and approximate RSS from GPU memory gauges. Requires `--enable-metrics`. vLLM-only strips (Dc decode phase, Rn inference, Bt, Xp, engine sleep) stay empty on SGLang. Third backend **TensorFold** (engine ≥ 0.6.1): auto-detected via any `tensorfold:` metric family; live token counters, speculative-decode stats and prefill/decode time come from `GET /health`, TFT/E2E fall back to lifetime means. Detection order SGLang → TensorFold → vLLM; strips with no source on a backend stay empty. Details: [LLM_NIM_UI.md](LLM_NIM_UI.md).

### Settings tab

**Plugins → LLM NIM** (notebook):

- **Connection** — base URL; optional display name (header override); NGC / HF tokens (masked; **Save tokens** / **Test NGC login**); docs release pin; air-gap toggle.
- **Catalog** — curated/paste catalog list, add image, sync docs, pull.
- **Local** — local NIM Docker images + profiles discovery.
- **Recipes** — list/load/export recipes, Spark preset, start with optional profile.
- **Instances** — list/stop/restart/refresh; adopt/orphans; **Use on dock** (slot 0), **Add to dock** (slots 1–3), **Clear extra slots**.

_Note: the NIM lifecycle helper (`gkrellm-nim`) has moved to a separate NIM lifecycle project._
- **Options** — decode chart max (t/s); prefill chart max (t/s); scrape **timeout** (ms).
- **Display** — checkboxes for every feature bit below, in four groups: `Throughput`, `Cache / queue`, `Latency`, `Host / HTTP / sizes`. Full field-by-field reference: [LLM_NIM_UI.md](LLM_NIM_UI.md).

Apply updates live config and reformats status lines immediately.

### Dock layout

Each enabled **Display** bit owns its own strip (show/hide on checkbox, live rebuild):

```text
[Nemotron              ●]  header + engine lamp (green/yellow/red/gray)
[in           1.2M]        session prefill tokens since dock start
[out          8.4M]        session decode tokens since dock start
[Dec          27/s]        label left | value right
[~~~~ line chart ~~~~]     X=time (horizontal history), Y=value
[KV           16%]
[~~~~ line chart ~~~~]
…
```

- **Engine lamp** — header right; Display bit **Engine status lamp** shows/hides it (no Eng tile). Colors: awake=green, weights_offloaded=yellow, discard_all=red, down=gray
- **Session `in` / `out`** — always shown (not in the features bitmask). Cumulative tokens for **this GKrellM process**:
  - `in` = Σ Δ `vllm:prompt_tokens_total` (prefill / prompt)
  - `out` = Σ Δ `vllm:generation_tokens_total` (decode / generation)
  - Values use `k` / `M` / `B`. Reset when the GKrellM dock process restarts. Counter regress (NIM restart mid-session) skips that tick; later Δ keep adding.
- **Charts** (non-text bits) — LINE, history scrolls **left→right in time** (X), amplitude is value (Y)
- **Text only** — queue, wait-reason, preempt, RSS, mean sizes
- Dec/Prefill each have their own chart (no shared TX/RX combo)

**Multi-slot:** up to **4** scrape targets. Slot 0 is the full panel above (`url` / `display_name`). Slots 1–3 (config `slotN_url` / `slotN_name`) add a compact header + lamp + one-line summary (`12/s kv=40% q=0`), no charts. The multi-slot lifecycle tooling has moved to a separate NIM lifecycle project.

### Histogram means

Histogram-backed bits (TTFT, ITL, E2E, TPOT, queue/prefill/decode/inference times, mean prompt/gen/batch) use a **window mean**: `Δsum / Δcount` over the scrape interval when the counter advances; otherwise the lifetime `_sum / _count` from the metric.

Counter deltas (TPS, prefix/spec/prompt-cache %, req/s, HTTP/s, CPU%) use `Δvalue / Δt` or `Δnum / Δden`.

Prefill vs session `in`: `vllm:prompt_tokens_total` usually advances in **one jump** when prefill is accounted (even if the GPU worked for many seconds). `in` adds that full Δ once; **Pre** shows the same-interval t/s (no peak-hold). Chart full-scale still **adapts** up to a recent peak then decays toward the configured floor.

### Config keys

| Key | Default | Meaning |
|-----|---------|---------|
| `llm_nim url` | `http://127.0.0.1:8000` | Base URL |
| `llm_nim display_name` | _(empty)_ | Header title override (else auto-shorten model) |
| `llm_nim docs_release` | `2.0.10` | NIM docs / schema seed pin |
| `llm_nim airgap` | `0` | `1` = air-gap / offline |
| `llm_nim features` | `1908735` | Display bitmask (health showcase; bits 0–3 alone = `15`) |
| `llm_nim chart_max_tps` | `50` | Decode (TX) full scale |
| `llm_nim chart_max_prefill` | `1000` | Prefill (RX) base scale |
| `llm_nim timeout_ms` | `500` | HTTP scrape timeout (ms) |
| `llm_nim dock_slots` | _(optional)_ | Enabled slot count hint (cap 4) |
| `llm_nim slot1_url` … `slot3_url` | _(empty)_ | Extra dock scrape URLs |
| `llm_nim slot1_name` … `slot3_name` | _(empty)_ | Extra dock display names |

### Features bitmask (bits 0–26)

| Bit | Value | Label | Source | Dock text |
|-----|-------|-------|--------|-----------|
| 0 | 1 | TPS / decode | `generation_tokens_total` Δ/Δt | chart TX |
| 1 | 2 | KV | `kv_cache_usage_perc` | `KV…%` |
| 2 | 4 | Queue | running / waiting | `R…/W…` |
| 3 | 8 | Prefill | `prompt_tokens_total` Δ/Δt | chart RX |
| 4 | 16 | TTFT | `time_to_first_token_seconds` (hist) | `TFT…` |
| 5 | 32 | Spec | spec-decode accepted / draft Δ | `Sp…%` |
| 6 | 64 | Prefix | prefix hits / queries Δ | `Px…%` |
| 7 | 128 | ITL | `inter_token_latency_seconds` (hist) | `ITL…` |
| 8 | 256 | E2E | `e2e_request_latency_seconds` (hist) | `E2E…` |
| 9 | 512 | TPOT | `request_time_per_output_token_seconds` (hist) | `TP…` |
| 10 | 1024 | Preempt | `num_preemptions_total` | `Pr…` |
| 11 | 2048 | Req/s | `request_success_total` Δ/Δt | `…/s` |
| 12 | 4096 | RSS | `process_resident_memory_bytes` | `…G` |
| 13 | 8192 | Q-wait | `request_queue_time_seconds` (hist) | `Qw…` |
| 14 | 16384 | Prefill-t | `request_prefill_time_seconds` (hist) | `Pf…` |
| 15 | 32768 | Decode-t | `request_decode_time_seconds` (hist) | `Dc…` |
| 16 | 65536 | Prompt-cache | cached / prompt tokens Δ | `Pc…%` |
| 17 | 131072 | HTTP 5xx% | `http_requests_total` (5xx / all) | `5x…%` |
| 18 | 262144 | CPU% | `process_cpu_seconds_total` Δ | `CPU…%` |
| 19 | 524288 | Wait reason | `num_requests_waiting_by_reason` | `Wc/Wd` |
| 20 | 1048576 | Engine lamp | `engine_sleep_state` | header ● (no tile) |
| 21 | 2097152 | Mean prompt | `request_prompt_tokens` (hist) | `Pm…` |
| 22 | 4194304 | Mean gen | `request_generation_tokens` (hist) | `Gm…` |
| 23 | 8388608 | Batch toks | `iteration_tokens_total` (hist) | `Bt…` |
| 24 | 16777216 | Ext prefix | external prefix hits / queries | `Xp…%` |
| 25 | 33554432 | HTTP/s | all `http_requests_total` Δ/Δt | `H…/s` |
| 26 | 67108864 | Infer-t | `request_inference_time_seconds` (hist) | `Rn…` |

Default install uses `features=1908735` (throughput + TTFT/ITL/E2E/TPOT + prefix/spec/prompt-cache + preempt + req/s + RSS + CPU + wait-reason + engine). Compact core-only is `features=15` (bits 0–3). Toggle any bit in Settings or OR values into `features`.

The full metric catalog and NIM lifecycle helper have moved to a separate NIM lifecycle project.

### Offline behaviour

If the scrape fails, the panel shows dashes / down and charts stay at zero. Failures are throttled (no log spam). Depends on **libcurl**.

---

## `cpu_clusters.so` — X925 / A725

**Source tree:** `plugins/cpu_clusters/` + `lib/cpu_map.c`, `lib/cpu_stat.c`.

### Mapping

Cores are classified from `/proc/cpuinfo` **CPU part**:

| Cluster | Part ID | Typical cores (fallback) |
|---------|---------|---------------------------|
| X925 | `0xd85` | 5–9, 15–19 |
| A725 | `0xd87` | 0–4, 10–14 |

Fallback lists apply if part IDs are missing. ACPI `CLU0`/`CLU1` groupings are **not** used (they mix E+P differently).

### UI

Two tall charts (~120 px) with **ten equal mini-bands per cluster** (one impulse strip per core) and a label/krell showing the **cluster average** busy % from `/proc/stat`. Stock composite `cpu` is **disabled** in managed config so it does not duplicate these panels.

---

## `net_clusters.so` — Docker / veth (folded)

**Source tree:** `plugins/net_clusters/`.

The stock Net monitor draws **one ~60 px chart per interface**; with dozens of containers (one host-side `veth*` each) the dock grows without bound. `net_clusters` folds every interface matching a regex into **one** compact chart, the same idea as `cpu_clusters` does for cores.

| Element | Meaning |
|---------|---------|
| Panel title | `Docker` (configurable label) |
| Top band (cyan) | **in** — bytes/s entering the containers (sum of veth `tx_bytes` Δ) |
| Bottom band (amber) | **out** — bytes/s leaving the containers (sum of veth `rx_bytes` Δ) |
| Chart text | `N  ↓in ↑out` — `N` = matching **UP** interfaces, rates summed (`512`, `1.2K`, `34M`, `1.5G`) |

- Source: `/proc/net/dev`, rescanned **every second**, so containers that start, stop or are recreated (new `veth*` names) are picked up **without restarting the dock**.
- Only interfaces whose `/sys/class/net/<if>/operstate` is `up` (or `unknown`) are folded in and counted; down, `lowerlayerdown` or already-vanished interfaces (Docker creates and drops temporary veths) are skipped. Note `N` counts **interfaces, not containers**: a container attached to several Docker networks owns one veth per network.
- New interfaces get a baseline sample first (no spike from a delta against 0); counter resets are ignored for that tick.
- Auto-scaled chart (rates range from bytes/s to hundreds of MB/s).
- Container-to-container traffic on a Docker bridge is counted once as **out** (sender) and once as **in** (receiver), so on a busy internal network in ≈ out.
- Physical NICs and `docker0` stay on the stock Net monitor; stock Net ignores `^veth` (see below).

Config keys (written by `install_config.sh`, preserved on re-run):

```text
net_clusters pattern ^veth      # POSIX extended regex on interface name
net_clusters label Docker       # panel title
```

Change `pattern` to fold other interfaces (e.g. `^(veth|br-)`); an invalid regex falls back to `^veth`.

---

## `uma_dram.so` — DRAM (UMA)

**Source tree:** `plugins/uma_dram/`. **Optional — not in the default `plugin_enable`** (its chart + readout duplicated the UMA % already shown in the GPU block and cost ~120 px of dock height). Enable by adding `uma_dram.so` after `net_clusters.so` in `~/.gkrellm2/plugin_enable`.

GB10 uses unified memory. The ACPI `DRAM8901` device exposes MMIO only (no usable OS telemetry), so this panel reads:

- `MemTotal`, `MemAvailable` from `/proc/meminfo`  
- Used ≈ Total − Available  
- Chart: used %; text: `used/total GB` and `%`

Stock **Mem** meter stays **off** (theme krells crush its label); UMA % lives in the GPU block. Stock **Swap** stays **on**.

---

## `board_acpi.so` — Board thermals

**Source tree:** `plugins/board_acpi/` + `lib/thermal_map.c`.  
Panel layout: a **Board** title bar above the seven temperature rows (no Fan). Full sensor guide: [SENSORS.md](SENSORS.md).

### Zones (display order)

| Label | Role (summary) |
|-------|----------------|
| **TSOC** | SoC / package platform temperature |
| **TGPU** | GPU-domain ACPI temperature |
| **TS0E** | Sensor bank 0, edge |
| **TS0P** | Sensor bank 0, proximity / package point |
| **TS1E** | Sensor bank 1, edge |
| **TS1P** | Sensor bank 1, proximity / package point |
| **TUNC** | Uncore / auxiliary platform sensor |

Discovery: `/sys/class/thermal/thermal_zone*/temp` + ACPI short name from the zone’s device path. Missing zones show `—`.

---

## Stock panels (managed)

### Disk

- Composite device **Disk** only (per-disk charts disabled).  
- Fixed chart config: **5 × 40 MB/s = 200 MB/s** full scale (stock auto-scale is unreadable on NVMe).

```text
disk assign_method 2
disk chart_config Disk 40 40000000 5 0 0 1 : …
```

### Net

```text
net net_enabled_as_default 0
net ignore_patterns ^ppp[0-9]+$|^br-|^virbr|^tun|^tap|^veth
net enables docker0 1 1 0
net enables wlP9s9 1 1 0
net enables enP7s7 1 1 0
net_clusters pattern ^veth
net_clusters label Docker
```

- **Stock Net enabled:** physical NICs used on Spark (`wlP9s9`, `enP7s7`) and `docker0`.  
- **Container `veth*`:** not shown one by one — folded into a single chart by [`net_clusters.so`](#net_clustersso--docker--veth-folded).  
- **Ignored:** PPP, Docker compose bridges (`br-*`), virbr, tun/tap, `veth*` (stock monitor only).  
- All `veth*` cookies under `~/.gkrellm2/data/net/` are removed on install (no per-container state to keep).  
- Container recreate needs **no restart** — `net_clusters` rescans `/proc/net/dev` every second.

Interface names for Wi‑Fi/Ethernet may differ on some OEM images — edit `install_config.sh` if your NICs are named differently, then reinstall config.

### Other stock

| Panel | Managed setting |
|-------|-----------------|
| Hostname | On (full) |
| Mail | Off |
| Uptime | On |
| Swap | On (meter) |
| Composite CPU | Off |

---

## Shared libraries (`lib/`)

| File | Purpose |
|------|---------|
| `cpu_map.c` | Parse cpuinfo → X925/A725 lists |
| `cpu_stat.c` | Aggregate `/proc/stat` busy/idle |
| `thermal_map.c` | Map thermal zone types to board labels |

Unit tests: `make unit` → `tests/test_cpu_map.py`.
