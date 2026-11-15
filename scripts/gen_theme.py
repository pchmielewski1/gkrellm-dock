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


def bevel_button(w: int, h: int, face, pressed: bool = False):
    """Dark navy beveled button (not cyan pill flares)."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=(6, 12, 22, 255))
    d.rectangle([1, 1, w - 2, h - 2], fill=face)
    hi = (180, 210, 230, 255) if not pressed else (20, 35, 55, 255)
    lo = (20, 35, 55, 255) if not pressed else (180, 210, 230, 255)
    d.line([(1, 1), (w - 2, 1)], fill=hi)
    d.line([(1, 1), (1, h - 2)], fill=hi)
    d.line([(1, h - 2), (w - 2, h - 2)], fill=lo)
    d.line([(w - 2, 1), (w - 2, h - 2)], fill=lo)
    return img


def main():
    solid("bg_chart.png", 120, 50, NAVY)
    solid("bg_panel.png", 120, 20, NAVY2)
    solid("bg_spacer.png", 120, 3, NAVY)
    # 1px-tall tile drawn on each chart grid line (visible on navy, not stripe-loud).
    Image.new("RGBA", (120, 1), (70, 120, 160, 170)).save(ROOT / "bg_grid.png")
    face_out = (28, 48, 72, 255)
    face_in = (18, 32, 52, 255)
    bevel_button(40, 16, face_out, False).save(ROOT / "button_panel_out.png")
    bevel_button(40, 16, face_in, True).save(ROOT / "button_panel_in.png")
    bevel_button(40, 16, face_out, False).save(ROOT / "button_meter_out.png")
    bevel_button(40, 16, face_in, True).save(ROOT / "button_meter_in.png")
    db = Image.new("RGBA", (40, 32), (0, 0, 0, 0))
    db.paste(bevel_button(40, 16, face_out, False), (0, 0))
    db.paste(bevel_button(40, 16, face_in, True), (0, 16))
    db.save(ROOT / "decal_button.png")
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
