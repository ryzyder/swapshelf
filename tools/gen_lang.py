#!/usr/bin/env python3
"""Genera src/lang.inc (textos en español, inglés y portugués) a partir de tools/traducciones.py.

Uso: python3 tools/gen_lang.py
Para agregar un texto: escríbelo en el código como T("texto en español") y agrega su
línea en tools/traducciones.py con la traducción al inglés y al portugués.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from traducciones import TR

def cstr(s):
    out, prev_hex = '', False
    for byte in s.encode('utf-8'):
        ch = chr(byte)
        if byte >= 0x80:
            out += '\\x%02X' % byte
            prev_hex = True
            continue
        if prev_hex and ch in '0123456789abcdefABCDEF':
            out += '" "'  # que el escape \\x no se coma la letra siguiente
        prev_hex = False
        out += {'\\': '\\\\', '"': '\\"', '\n': '\\n'}.get(ch, ch)
    return '"' + out + '"'

lines = [
    '/* Traducciones de SwapShelf. Generado por tools/gen_lang.py a partir de tools/traducciones.py:',
    ' * no editar a mano. El texto en español es la clave; si falta una traducción se muestra en español. */',
    'enum Lang { LANG_ES, LANG_EN, LANG_PT, LANG_COUNT };',
    'static const char* const LANG_NAME[LANG_COUNT] = { "Espa\\xC3\\xB1ol", "English", "Portugu\\xC3\\xAAs" };',
    'static const char* const LANG_CODE[LANG_COUNT] = { "es", "en", "pt" };',
    'static int g_lang = LANG_ES;',
    '',
    'static const char* const TR_TABLE[][3] = {',
]
for k, (en, pt) in TR.items():
    lines.append('\t{ %s, %s, %s },' % (cstr(k), cstr(en), cstr(pt)))
lines.append('};')
lines.append('''#define TR_COUNT ((int) (sizeof(TR_TABLE) / sizeof(TR_TABLE[0])))

/* Tabla hash (texto en español -> fila) armada la primera vez */
#define TR_HASH 1024
static short g_trHash[TR_HASH];
static bool g_trReady;

static unsigned trHashOf(const char* s) {
	unsigned h = 2166136261u;
	while (*s) h = (h ^ (unsigned char) *s++) * 16777619u;
	return h;
}

static void trBuild(void) {
	for (int i = 0; i < TR_HASH; ++i) g_trHash[i] = -1;
	for (int i = 0; i < TR_COUNT; ++i) {
		unsigned h = trHashOf(TR_TABLE[i][0]) & (TR_HASH - 1);
		while (g_trHash[h] >= 0) h = (h + 1) & (TR_HASH - 1);
		g_trHash[h] = (short) i;
	}
	g_trReady = true;
}

/* Texto traducido al idioma elegido (o el mismo texto en español) */
static const char* T(const char* es) {
	if (g_lang == LANG_ES || !es) return es;
	if (!g_trReady) trBuild();
	unsigned h = trHashOf(es) & (TR_HASH - 1);
	while (g_trHash[h] >= 0) {
		const char* const* row = TR_TABLE[g_trHash[h]];
		if (!strcmp(row[0], es)) return row[g_lang];
		h = (h + 1) & (TR_HASH - 1);
	}
	return es;
}''')
out = Path(__file__).parent.parent / 'src' / 'lang.inc'
out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
print(f'escrito {out} ({len(TR)} textos)')
