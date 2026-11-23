# Release notes — clock-event dots + ViolationStatus blip catch

Live on DGX Spark GB10 · theme `gb10-blue`.

## Highlights

- **GPU clock-event dots** — six LEDs between Power and Temp/UMA (Cap → App): green / red / gray, ~2.5 s latch, hover tooltips.
- **Sub-poll accuracy (Cap / SWth)** — diffs `nvmlDeviceGetViolationStatus` (POWER / THERMAL); a ≥1 ms Δ between polls pulses the matching dot even if the event bit was already clear, so short throttle blips are not missed. Tooltip may show `last poll X.X ms capped`.
- **LLM power button** — beveled start/stop on the model header (green / yellow / red) via `gkrellm-nim`; engine lamp unchanged; **Q** sits under the name above `in`/`out`.
- **Headroom proxy panel** — optional second block under NIM (`Options → Headroom proxy`); JSON `/stats`+`/health`; strips **Q / Tok / Sav / Lat / Req** (text-only).
- **Disk / Net** — auto-scale Disk with 5 grids + live `$M`/`$r`/`$w`; stock Net RX/TX via `\ww`.
- **Theme** — dark beveled panel buttons; refreshed dock screenshots (incl. `gkrellm-headroom.png`).

## Upgrade

```bash
git pull
./scripts/install.sh
gkrellm -t ~/.gkrellm2/themes/gb10-blue
```

## Docs

[README.md](../README.md) · [PLUGINS.md](PLUGINS.md) · [LLM_NIM_UI.md](LLM_NIM_UI.md) · [SCREENSHOTS.md](SCREENSHOTS.md)
