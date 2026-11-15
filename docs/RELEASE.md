# Release notes — dock UX refresh (clock-event dots, LLM power, Disk/Net)

Live on DGX Spark GB10 · theme `gb10-blue` · screenshots regenerated (`docs/gkrellm-*.png`).

## Highlights

- **GPU clock-event dots** — six bare status LEDs between Power and Temp/UMA (Cap → App). Green / red / gray; hover tooltips; documented in [PLUGINS.md](PLUGINS.md).
- **LLM power button** — beveled start/stop control on the model header (green = stopped, yellow = starting, red = running). Uses `gkrellm-nim`; instance name + helper path in Connection. Engine lamp stays as the sleep-state indicator.
- **Q under the name** — when Queue is enabled, it sits directly under the header, above session `in` / `out`.
- **Disk auto-scale** — no stuck `200M` ceiling. Five grid lines, live `$M` + side-by-side `$r` / `$w` (`\ww` format).
- **Net RX/TX** — stock `\ww` labels (`$M` + `$r` / `$t`); folded Docker `veth*` chart unchanged (`net_clusters`).
- **Theme** — dark beveled panel/meter buttons (no cyan pill flares); readable chart text fill for stock Disk/Net; nvidia charts still use glyph-only labels.

## Dock order (top → bottom)

1. Hostname / clock  
2. GPU — Load, Clock, Power, **clock-event dots**, Temp, UMA %  
3. LLM — header (**power** + lamp) → **Q** → `in` / `out` → Display strips  
4. CPU X925 / CPU A725  
5. Proc / Disk (auto-scale)  
6. Net (RX/TX) + Docker (`net_clusters`)  
7. Fan / Swap / Board / Uptime  

## Upgrade

```bash
git pull
./scripts/install.sh
# or: make plugins && make install && ./scripts/install_config.sh
gkrellm -t ~/.gkrellm2/themes/gb10-blue
```

LLM Connection prefs and Display bitmask are preserved across `install_config.sh`. Set **Instance name** (and optional helper path) before using the power button.

## Docs

- [README.md](../README.md) — features + gallery  
- [PLUGINS.md](PLUGINS.md) / [PLUGINS_CLI.md](PLUGINS_CLI.md) — panel + config keys  
- [LLM_NIM_UI.md](LLM_NIM_UI.md) — power button + Q placement  
- [SCREENSHOTS.md](SCREENSHOTS.md) — how to re-capture  
