# Theme: `gb10-blue`

Dark navy dock skin with cyan/ice accents, sized for a ~120 px chart width on DGX Spark.

## Install location

- Source: `themes/gb10-blue/`  
- Installed: `~/.gkrellm2/themes/gb10-blue/`  
- Selected by managed config: `default_track_theme "gb10-blue"` and by `gkrellm -t …` at start.

## Design intent

- Dense vertical stack readable at 3840×2160 with a thin right-edge dock  
- High-contrast text on navy panels  
- Chart grids that stay visible under orange/cyan data fills  
- Meter krells that do not obliterate short labels (stock Mem still struggles — leave Mem off)

## Critical `gkrellmrc` rules

### Valid style lines

GKrellM expects:

```text
StyleMeter name.key = value
```

**Invalid** (breaks style parsing and produces `StyleXXX` warnings):

```text
StyleMeter name
StyleMeter name something
```

### Transparency

Use:

```text
StylePanel *.transparency = 0
StyleMeter *.transparency = 0
StyleChart *.transparency = 0
```

`transparency=1` enables shaped windows and has **clipped** Mem/Swap/Board/Uptime off the bottom of the dock on this theme. Do not re-enable it without re-testing full-height screenshots.

## Asset overview

| Asset | Role |
|-------|------|
| `bg_panel.png`, `bg_meter.png`, `bg_chart.png` | Panel / meter / chart backgrounds |
| `bg_grid.png` | Chart grid |
| `frame_*.png`, `spacer_*.png` | Chrome and spacing |
| `krell_*.png` | Meter indicators |
| `data_in.png` / `data_out.png` (+ `_grid`) | Bidirectional chart fills (net, LLM) |
| `net/decal_net_leds.png` | Net activity LEDs |
| `mem/`, `swap/`, `timer/` | Subdirectory overrides |

Regenerate procedural pieces (if needed) with `scripts/gen_theme.py`, then `make install`.

## After editing the theme

```bash
make install        # re-copies the theme and re-applies managed config
gkrellm -t ~/.gkrellm2/themes/gb10-blue   # (re)start to pick it up
```

If you change the theme only inside the GKrellM GUI, still re-run `scripts/install_config.sh` (or `make install`) so managed monitors (CPU off, Mem off, disk scale, net, LLM keys) remain correct.
