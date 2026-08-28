#!/usr/bin/env bash
# Capture the running gkrellm dock into docs/gkrellm-*.png
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="$ROOT/docs"
DISPLAY_ID="${DISPLAY:-:1}"
export DISPLAY="$DISPLAY_ID"

WID="$(xwininfo -root -tree 2>/dev/null | awk '/"gkrellm".*[0-9]+x[0-9]+/{
  if ($0 ~ /128x/ || $0 ~ /120x/) { print $1; exit }
}')"
if [[ -z "${WID:-}" ]]; then
  echo "gkrellm window not found on DISPLAY=$DISPLAY_ID" >&2
  exit 1
fi

eval "$(xwininfo -id "$WID" | awk '
  /Absolute upper-left X:/ {print "X="$NF}
  /Absolute upper-left Y:/ {print "Y="$NF}
  /Width:/ {print "W="$NF}
  /Height:/ {print "H="$NF}
')"

python3 - "$DOCS" "$X" "$Y" "$W" "$H" <<'PY'
import sys
from pathlib import Path
from PIL import Image
import gi
gi.require_version('Gdk', '3.0')
from gi.repository import Gdk

docs = Path(sys.argv[1])
x, y, w, h = map(int, sys.argv[2:6])
root = Gdk.get_default_root_window()
pb = Gdk.pixbuf_get_from_window(root, x, y, w, h)
assert pb is not None
full = docs / 'gkrellm-dock.png'
pb.savev(str(full), 'png', [], [])
im = Image.open(full)
W, H = im.size

# Region crops tuned for current llm_nim-heavy layout (~128×2k).
# Re-tune Y bounds after large UI changes; see docs/SCREENSHOTS.md.
regions = {
    'gkrellm-top.png': (0, min(68, H)),
    'gkrellm-gpu.png': (min(68, H), min(295, H)),
    'gkrellm-gpu-zoom.png': (min(100, H), min(270, H)),
    'gkrellm-llm.png': (min(295, H), min(460, H)),
    'gkrellm-llm-rows.png': (min(295, H), min(1320, H)),
    'gkrellm-cpu.png': (min(1320, H), min(1585, H)),
    'gkrellm-cpu-x925.png': (min(1320, H), min(1465, H)),
    'gkrellm-cpu-a725.png': (min(1465, H), min(1585, H)),
    'gkrellm-cpu-zone.png': (min(1320, H), min(1585, H)),
    'gkrellm-proc-area.png': (min(1585, H), min(1720, H)),
    'gkrellm-gap-proc.png': (min(1585, H), min(1665, H)),
    'gkrellm-net.png': (min(1665, H), min(1935, H)),
    'gkrellm-mem-zoom.png': (min(1910, H), min(2010, H)),
    'gkrellm-bottom.png': (min(1910, H), H),
    'gkrellm-slice-top.png': (0, H // 3),
    'gkrellm-slice-mid.png': (H // 3, 2 * H // 3),
    'gkrellm-slice-mid2.png': (2 * H // 3, H),
}
for name, (y0, y1) in regions.items():
    if y1 <= y0:
        continue
    im.crop((0, y0, W, y1)).save(docs / name, optimize=True)

# GitHub hero: 2× nearest-neighbor + pad
hero = im.resize((W * 2, H * 2), Image.NEAREST)
pad = 24
canvas = Image.new('RGB', (hero.width + pad * 2, hero.height + pad * 2), (12, 18, 32))
canvas.paste(hero, (pad, pad))
canvas.save(docs / 'gkrellm-dock-hero.png', optimize=True)
print(f'captured {full} ({W}x{H}) + crops + hero')
PY
