#!/usr/bin/env python3
"""Genera src/font_data.h: fuentes de mapa de bits para el menú (sin depender de SDL_ttf).

Uso: python3 tools/gen_font.py [ruta/a/fuente.ttf]
Por defecto usa DejaVu Sans Bold (licencia libre Bitstream Vera / DejaVu).
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
CHARS = [chr(c) for c in range(32, 127)] + list("áéíóúÁÉÍÓÚñÑüÜ¿¡é·•←→↑↓àâãçêôõÀÂÃÇÊÔÕ−")
CHARS = list(dict.fromkeys(CHARS))
SIZES = {"title": 64, "item": 40, "small": 26}

out = ["/* Generado por tools/gen_font.py - no editar a mano */",
       "#pragma once", "#include <stdint.h>", "",
       "struct BitmapFont {",
       "\tint height, ascent, count, atlasW;",
       "\tconst uint32_t* codepoints;",
       "\tconst uint16_t* x;",
       "\tconst uint8_t* w;",
       "\tconst uint8_t* advance;",
       "\tconst uint8_t* atlas; /* alfa 8 bits, atlasW x height */",
       "};", ""]

for name, size in SIZES.items():
    font = ImageFont.truetype(TTF, size)
    ascent, descent = font.getmetrics()
    h = ascent + descent
    glyphs = []
    for ch in CHARS:
        bbox = font.getbbox(ch)
        adv = int(round(font.getlength(ch)))
        w = max(adv, bbox[2]) + 1
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
        glyphs.append((ch, w, adv, img))
    PAD = 3  # columnas vacías entre letras: al escalar con filtro no se cuela la vecina
    atlas_w = sum(g[1] + PAD for g in glyphs)
    atlas = Image.new("L", (atlas_w, h), 0)
    xs, x = [], 0
    for ch, w, adv, img in glyphs:
        atlas.paste(img, (x, 0)); xs.append(x); x += w + PAD
    px = atlas.tobytes()
    cps = ", ".join(str(ord(g[0])) for g in glyphs)
    out.append(f"static const uint32_t font_{name}_cp[] = {{ {cps} }};")
    out.append(f"static const uint16_t font_{name}_x[] = {{ {', '.join(map(str, xs))} }};")
    out.append(f"static const uint8_t font_{name}_w[] = {{ {', '.join(str(g[1]) for g in glyphs)} }};")
    out.append(f"static const uint8_t font_{name}_adv[] = {{ {', '.join(str(g[2]) for g in glyphs)} }};")
    out.append(f"static const uint8_t font_{name}_atlas[] = {{")
    for i in range(0, len(px), 32):
        out.append("\t" + ",".join(str(b) for b in px[i:i + 32]) + ",")
    out.append("};")
    out.append(f"static const struct BitmapFont FONT_{name.upper()} = {{ {h}, {ascent}, {len(glyphs)}, {atlas_w}, "
               f"font_{name}_cp, font_{name}_x, font_{name}_w, font_{name}_adv, font_{name}_atlas }};")
    out.append("")

dst = Path(__file__).resolve().parent.parent / "src" / "font_data.h"
dst.write_text("\n".join(out) + "\n", encoding="utf-8")
print("escrito", dst)
