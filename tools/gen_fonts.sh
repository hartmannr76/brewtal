#!/usr/bin/env bash
# Regenerates the LVGL fonts for a given panel diameter:
#   tools/gen_fonts.sh 480        -> lib/brew_fonts_480/src/*.c
#   tools/gen_fonts.sh 360        -> lib/brew_fonts_360/src/*.c  (new sizes: also add a library.json)
# Needs: node/npm, python3 + fontTools + brotli  (pip install fonttools brotli)
# Sizes are the mockup's, scaled from a 360 px reference.
set -euo pipefail
DIA="${1:-480}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/lib/brew_fonts_$DIA/src"
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
npm init -y >/dev/null
npm i --silent lv_font_conv @tabler/icons-webfont inter-ui
python3 - <<'PY'
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
src = "node_modules/inter-ui/variable/InterVariable.woff2"
for w, opsz in ((360, 32), (450, 14), (560, 14)):
    f = TTFont(src); f.flavor = None
    instantiateVariableFont(f, {"wght": w, "opsz": opsz}).save(f"Inter-{w}.ttf")
PY
C=node_modules/.bin/lv_font_conv
T=node_modules/@tabler/icons-webfont/dist/fonts/tabler-icons.ttf
TXT="0x20-0x7E,0xB0,0xB7"
ICO="0xefa1,0xec0b,0xed45,0xed46,0xea5c,0xeb38"   # beer snowflake pause play chart-line temperature
DIG="0x2D-0x2E,0x30-0x39"
px() { python3 -c "print(round($1 * $DIA / 360))"; }
gen() { $C --no-compress --bpp 4 --format lvgl --lv-include lvgl.h "$@"; }
gen --size "$(px 94)" --font Inter-360.ttf -r $DIG --lv-font-name brew_font_num_big  -o "$OUT/brew_font_num_big.c"
gen --size "$(px 68)" --font Inter-360.ttf -r $DIG --lv-font-name brew_font_num_hist -o "$OUT/brew_font_num_hist.c"
gen --size "$(px 34)" --font Inter-450.ttf -r 0xB0 --lv-font-name brew_font_deg_big  -o "$OUT/brew_font_deg_big.c"
gen --size "$(px 25)" --font Inter-450.ttf -r 0xB0 --lv-font-name brew_font_deg_hist -o "$OUT/brew_font_deg_hist.c"
gen --size "$(px 18)" --font Inter-450.ttf -r $TXT --lv-font-name brew_font_status -o "$OUT/brew_font_status.c"
gen --size "$(px 16)" --font Inter-450.ttf -r $TXT --lv-font-name brew_font_small  -o "$OUT/brew_font_small.c"
gen --size "$(px 25)" --font Inter-450.ttf -r $TXT --font $T -r $ICO --lv-font-name brew_font_menu      -o "$OUT/brew_font_menu.c"
gen --size "$(px 25)" --font Inter-560.ttf -r $TXT --font $T -r $ICO --lv-font-name brew_font_menu_bold -o "$OUT/brew_font_menu_bold.c"
echo "fonts for ${DIA}px written to $OUT"
