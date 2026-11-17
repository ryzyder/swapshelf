#!/usr/bin/env python3
"""Genera src/logo_font.h: la fuente del logo SwapShelf (Inter Black, peso 900).

Solo lleva mayúsculas, espacio y '?', que es lo que usa el logo.
Inter es de Rasmus Andersson, licencia SIL Open Font License 1.1 (ver LICENSE).

Uso: python3 tools/gen_logo_font.py [ruta/a/Inter-Black.otf]
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter/Inter-Black.otf"
CHARS = [" ", "?"] + [chr(c) for c in range(ord("A"), ord("Z") + 1)]
SIZE = 64
PAD = 3

font = ImageFont.truetype(TTF, SIZE)
ascent, descent = font.getmetrics()
h = ascent + descent
cap = font.getbbox("H")  # alto de mayúscula, para centrar el texto en la pieza
glyphs = []
for ch in CHARS:
    bbox = font.getbbox(ch)
    adv = int(round(font.getlength(ch)))
    w = max(adv, bbox[2]) + 1
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
    glyphs.append((ch, w, adv, img))
atlas_w = sum(g[1] + PAD for g in glyphs)
atlas = Image.new("L", (atlas_w, h), 0)
xs, x = [], 0
for ch, w, adv, img in glyphs:
    atlas.paste(img, (x, 0)); xs.append(x); x += w + PAD
px = atlas.tobytes()

out = ["/* Generado por tools/gen_logo_font.py - no editar a mano */",
       "/* Inter Black (c) Rasmus Andersson, SIL Open Font License 1.1 */",
       "#pragma once", "",
       f"#define LOGO_FONT_CAP_TOP {cap[1]}   /* fila donde empieza la mayúscula */",
       f"#define LOGO_FONT_CAP_H {cap[3] - cap[1]}",
       f"static const uint32_t font_logo_cp[] = {{ {', '.join(str(ord(g[0])) for g in glyphs)} }};",
       f"static const uint16_t font_logo_x[] = {{ {', '.join(map(str, xs))} }};",
       f"static const uint8_t font_logo_w[] = {{ {', '.join(str(g[1]) for g in glyphs)} }};",
       f"static const uint8_t font_logo_adv[] = {{ {', '.join(str(g[2]) for g in glyphs)} }};",
       "static const uint8_t font_logo_atlas[] = {"]
for i in range(0, len(px), 32):
    out.append("\t" + ",".join(str(b) for b in px[i:i + 32]) + ",")
out.append("};")
out.append(f"static const struct BitmapFont FONT_LOGO = {{ {h}, {ascent}, {len(glyphs)}, {atlas_w}, "
           "font_logo_cp, font_logo_x, font_logo_w, font_logo_adv, font_logo_atlas };")

dst = Path(__file__).resolve().parent.parent / "src" / "logo_font.h"
dst.write_text("\n".join(out) + "\n", encoding="utf-8")
print("escrito", dst)
