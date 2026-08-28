#!/usr/bin/env python3
"""Generate minimal gb10-blue GKrellM theme assets."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1] / "themes" / "gb10-blue"
ROOT.mkdir(parents=True, exist_ok=True)

NAVY = (8, 18, 36, 255)
NAVY2 = (12, 28, 52, 255)
LINE = (30, 60, 100, 255)
CYAN = (64, 196, 220, 255)


def solid(name: str, w: int, h: int, color=NAVY):
    img = Image.new("RGBA", (w, h), color)
    if name.startswith("bg_"):
        d = ImageDraw.Draw(img)
        d.line([(0, h - 1), (w, h - 1)], fill=LINE)
    img.save(ROOT / name)


def main():
    solid("bg_chart.png", 120, 50, NAVY)
    solid("bg_panel.png", 120, 20, NAVY2)
    solid("bg_spacer.png", 120, 3, NAVY)
    solid("bg_grid.png", 120, 50, (0, 0, 0, 0))
    # data fill for charts
    img = Image.new("RGBA", (120, 50), (20, 140, 180, 180))
    img.save(ROOT / "bg_data.png")
    img = Image.new("RGBA", (120, 1), CYAN)
    img.save(ROOT / "bg_data_grid.png")
    (ROOT / "gkrellmrc").write_text(
        """# gb10-blue — dark navy modern theme for DGX Spark dock
StyleMeter cpu_clusters
StyleMeter board_acpi
StyleMeter uma_dram
StyleMeter nvidia

chart_height 42
panel_label_position 1

# Global-ish
set_chart_height 42

# Text
large_font -*-helvetica-medium-r-*-*-10-*-*-*-*-*-*-*
normal_font -*-helvetica-medium-r-*-*-9-*-*-*-*-*-*-*
small_font -*-helvetica-medium-r-*-*-8-*-*-*-*-*-*-*

# Colors (fallback when pixmaps insufficient)
chart_in_color #40c4dc
chart_in_color_grid #1a6a7a
chart_out_color #7ec8e3
chart_out_color_grid #2a5060

bg_meter_color #0c1c34
bg_chart_color #081224
""",
        encoding="utf-8",
    )
    print("theme written to", ROOT)


if __name__ == "__main__":
    main()
