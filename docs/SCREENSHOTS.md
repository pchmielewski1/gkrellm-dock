# Screenshots

Live captures of the GKrellM dock on DGX Spark (theme `gb10-blue`, chart width ~120–128 px).

Regenerate from a running dock:

```bash
./scripts/capture_screenshots.sh
```

Requires `DISPLAY` (usually `:1`), `xwininfo`, PyGObject (`Gdk`), and Pillow.

## Files

| File | What it shows |
|------|----------------|
| [`gkrellm-dock-hero.png`](gkrellm-dock-hero.png) | Full dock, 2× nearest-neighbor + pad — **README hero** for GitHub |
| [`gkrellm-dock.png`](gkrellm-dock.png) | Full dock at native 1× resolution (source for crops) |
| [`gkrellm-top.png`](gkrellm-top.png) | Hostname, calendar date, clock |
| [`gkrellm-gpu.png`](gkrellm-gpu.png) | NVIDIA GB10 block — Load, Clock, Power, Temp, UMA % |
| [`gkrellm-gpu-zoom.png`](gkrellm-gpu-zoom.png) | Closer crop of GPU charts |
| [`gkrellm-llm.png`](gkrellm-llm.png) | NIM header: model + engine lamp; **`in`/`out` session token totals** (tokens since this GKrellM process); Dec + KV start |
| [`gkrellm-llm-rows.png`](gkrellm-llm-rows.png) | Full LLM NIM panel (all enabled Display strips + LINE charts) |
| [`gkrellm-cpu.png`](gkrellm-cpu.png) | Both Grace clusters — **CPU X925** and **CPU A725** |
| [`gkrellm-cpu-x925.png`](gkrellm-cpu-x925.png) | X925 cluster only |
| [`gkrellm-cpu-a725.png`](gkrellm-cpu-a725.png) | A725 cluster only |
| [`gkrellm-cpu-zone.png`](gkrellm-cpu-zone.png) | Same as `gkrellm-cpu.png` (alias crop for older docs) |
| [`gkrellm-proc-area.png`](gkrellm-proc-area.png) | Process/user counts and Proc meter |
| [`gkrellm-gap-proc.png`](gkrellm-gap-proc.png) | Short crop around the Proc gap |
| [`gkrellm-net.png`](gkrellm-net.png) | Disk + net (`docker0`, container `veth*`, Wi‑Fi/Ethernet) |
| [`gkrellm-mem-zoom.png`](gkrellm-mem-zoom.png) | DRAM UMA used/total GB + % (and start of Swap) |
| [`gkrellm-bottom.png`](gkrellm-bottom.png) | DRAM UMA, Swap, Board thermals, Uptime |
| [`gkrellm-slice-top.png`](gkrellm-slice-top.png) | Top third of the full dock |
| [`gkrellm-slice-mid.png`](gkrellm-slice-mid.png) | Middle third |
| [`gkrellm-slice-mid2.png`](gkrellm-slice-mid2.png) | Lower third |

## Notes

- Crop Y bounds assume a tall layout with many LLM Display bits enabled. After changing which metrics are shown, re-run the capture script and adjust bounds in `scripts/capture_screenshots.sh` if sections drift.
- Engine status is the green/yellow/red/gray lamp on the **Nemotron** header row (right), not a separate Eng tile.
- **`in` / `out`** under the header are session Prefill / Decode token totals for the current GKrellM dock process (see [PLUGINS.md](PLUGINS.md) · LLM NIM).
