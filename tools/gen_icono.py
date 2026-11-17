#!/usr/bin/env python3
"""Genera los íconos de SwapShelf (assets/icon.png, .jpg, .bmp e .ico).

El ícono es el logo del menú en cuadrado: las dos piezas fusionadas (cian
y roja) con el corte diagonal, el relieve invertido y una S en cada pieza,
en Inter Black (licencia SIL OFL 1.1), sobre fondo oscuro.

Uso: python3 tools/gen_icono.py [ruta/a/Inter-Black.otf]
"""
import math
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter/Inter-Black.otf"
CY, RD, BG, WH = (0, 195, 227), (255, 69, 84), (14, 17, 24), (255, 255, 255)
N = 2048  # se dibuja grande y se achica para que los bordes queden suaves


def mix(c, t, k):
    return tuple(int(a + (b - a) * k) for a, b in zip(c, t))


def corrida(m, dx, dy):
    return ImageChops.offset(m, int(dx), int(dy))


def relieve(img, pieza, base, lx, ly, e):
    """Borde claro del lado de la luz (lx, ly) y oscuro del otro, como en la app."""
    img.paste(mix(base, WH, 0.22), mask=pieza)
    img.paste(mix(base, (0, 0, 0), 0.16), mask=ImageChops.multiply(pieza, corrida(pieza, -lx * e, -ly * e)))
    cuerpo = ImageChops.multiply(ImageChops.multiply(pieza, corrida(pieza, -lx * e, -ly * e)), corrida(pieza, lx * e, ly * e))
    img.paste(base, mask=cuerpo.filter(ImageFilter.GaussianBlur(e * 0.3)))


def contorno(x0, y0, w, h, r):
    pts = []
    esquinas = [(r[0], r[0], 180), (w - r[1], r[1], 270), (w - r[2], h - r[2], 0), (r[3], h - r[3], 90)]
    for (cx, cy, a0), rr in zip(esquinas, r):
        for i in range(25):
            t = math.radians(a0 + 90 * i / 24)
            pts.append((x0 + cx + math.cos(t) * rr, y0 + cy + math.sin(t) * rr))
    return pts


def icono():
    img = Image.new("RGB", (N, N), BG)
    s = N * 0.84
    w, h = s, s * 0.84
    x0, y0 = (N - w) / 2, (N - h) / 2
    forma = Image.new("L", img.size, 0)
    ImageDraw.Draw(forma).polygon(contorno(x0, y0, w, h, [h * 0.42, h * 0.16, h * 0.42, h * 0.16]), fill=255)
    cx, sl, g = x0 + w / 2, s * 0.06, s * 0.012
    k = -2 * sl / h
    X = lambda y: cx - sl - (y - y0) * k  # corte "\"
    izq = Image.new("L", img.size, 0)
    ImageDraw.Draw(izq).polygon([(0, 0), (X(0) - g, 0), (X(N) - g, N), (0, N)], fill=255)
    der = Image.new("L", img.size, 0)
    ImageDraw.Draw(der).polygon([(X(0) + g, 0), (N, 0), (N, N), (X(N) + g, N)], fill=255)
    e = h * 0.035
    relieve(img, ImageChops.multiply(forma, izq), CY, -1, -1, e)
    relieve(img, ImageChops.multiply(forma, der), RD, 1, 1, e)
    f = ImageFont.truetype(TTF, int(s * 0.56))
    d = ImageDraw.Draw(img)
    so = h * 0.02
    for lx, sg in ((x0 + w * 0.27, 1), (x0 + w * 0.73, -1)):
        bb = f.getbbox("S")
        tx, ty = lx - (bb[0] + bb[2]) / 2, y0 + h / 2 - (bb[1] + bb[3]) / 2
        sombra = Image.new("L", img.size, 0)
        ImageDraw.Draw(sombra).text((tx + sg * so * 0.7, ty + sg * so), "S", font=f, fill=60)
        img.paste((0, 0, 0), mask=ImageChops.multiply(sombra, forma))
        d.text((tx, ty), "S", font=f, fill=WH)
    return img


dst = Path(__file__).resolve().parent.parent / "assets"
big = icono()
big.resize((512, 512), Image.LANCZOS).save(dst / "icon.png")
big.resize((256, 256), Image.LANCZOS).save(dst / "icon.jpg", quality=95)
big.resize((64, 64), Image.LANCZOS).save(dst / "icon.bmp")
big.resize((256, 256), Image.LANCZOS).save(dst / "icon.ico", sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("íconos escritos en", dst)
