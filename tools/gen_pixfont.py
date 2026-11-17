# Genera src/pixfont_data.h a partir de tools/pixfont.py
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from pixfont import G

chars = sorted(G.keys(), key=ord)
out = ['/* Generado por tools/gen_pixfont.py: fuente pixel propia del lanzador (11 filas) */',
       '#ifndef SWS_PIXFONT_DATA_H', '#define SWS_PIXFONT_DATA_H', '#include <stdint.h>',
       f'#define PIXFONT_COUNT {len(chars)}', '#define PIXFONT_ROWS 11',
       'static const uint32_t PIXFONT_CP[PIXFONT_COUNT] = {']
out.append('\t' + ', '.join(str(ord(c)) for c in chars) + ',')
out.append('};')
out.append('static const uint8_t PIXFONT_W[PIXFONT_COUNT] = {')
out.append('\t' + ', '.join(str(len(G[c][0])) for c in chars) + ',')
out.append('};')
out.append('/* bit 0 = columna izquierda */')
out.append('static const uint8_t PIXFONT_BITS[PIXFONT_COUNT][PIXFONT_ROWS] = {')
for c in chars:
    rows = []
    for r in G[c]:
        v = 0
        for j, b in enumerate(r):
            if b: v |= 1 << j
        rows.append(str(v))
    out.append('\t{ ' + ', '.join(rows) + ' }, /* U+%04X */' % ord(c))
out.append('};')
out.append('#endif')
open(os.path.join(os.path.dirname(__file__), '..', 'src', 'pixfont_data.h'), 'w').write('\n'.join(out) + '\n')
print(len(chars), 'glifos')
