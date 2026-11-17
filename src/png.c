/*
 * png: lector PNG (inflate propio, todos los tipos de color, 1-16 bits, Adam7).
 * Pensado para portadas: prioriza ser pequeño y seguro ante archivos dañados.
 */
#include "png.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- inflate (RFC 1950/1951) ---------------- */
struct Inf {
	const uint8_t* in;
	size_t inLen, inPos;
	uint32_t bitBuf;
	int bitCnt;
	uint8_t* out;
	size_t outLen, outPos;
	int err;
};

struct Huf { short count[16]; short sym[320]; };

static int bits(struct Inf* s, int need) {
	uint32_t v = s->bitBuf;
	while (s->bitCnt < need) {
		if (s->inPos >= s->inLen) { s->err = 1; return 0; }
		v |= (uint32_t) s->in[s->inPos++] << s->bitCnt;
		s->bitCnt += 8;
	}
	s->bitBuf = v >> need;
	s->bitCnt -= need;
	return (int) (v & ((1u << need) - 1));
}

static int decodeSym(struct Inf* s, const struct Huf* h) {
	int code = 0, first = 0, index = 0;
	for (int len = 1; len < 16; ++len) {
		code |= bits(s, 1);
		if (s->err) return -1;
		int count = h->count[len];
		if (code - count < first) return h->sym[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}

static int buildHuf(struct Huf* h, const short* lens, int n) {
	short offs[16];
	memset(h->count, 0, sizeof(h->count));
	for (int i = 0; i < n; ++i) h->count[lens[i]]++;
	if (h->count[0] == n) return 0;
	int left = 1;
	for (int len = 1; len < 16; ++len) { left <<= 1; left -= h->count[len]; if (left < 0) return -1; }
	offs[1] = 0;
	for (int len = 1; len < 15; ++len) offs[len + 1] = (short) (offs[len] + h->count[len]);
	for (int i = 0; i < n; ++i) if (lens[i]) h->sym[offs[lens[i]]++] = (short) i;
	return left;
}

static void put(struct Inf* s, uint8_t b) {
	if (s->outPos >= s->outLen) { s->err = 1; return; }
	s->out[s->outPos++] = b;
}

static const short LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(struct Inf* s, const struct Huf* lc, const struct Huf* dc) {
	for (;;) {
		int sym = decodeSym(s, lc);
		if (s->err || sym < 0) { s->err = 1; return; }
		if (sym < 256) { put(s, (uint8_t) sym); if (s->err) return; continue; }
		if (sym == 256) return;
		sym -= 257;
		if (sym >= 29) { s->err = 1; return; }
		int len = LBASE[sym] + bits(s, LEXT[sym]);
		int ds = decodeSym(s, dc);
		if (s->err || ds < 0 || ds >= 30) { s->err = 1; return; }
		size_t dist = (size_t) (DBASE[ds] + bits(s, DEXT[ds]));
		if (s->err || dist > s->outPos) { s->err = 1; return; }
		for (int i = 0; i < len; ++i) { put(s, s->out[s->outPos - dist]); if (s->err) return; }
	}
}

static void fixedBlock(struct Inf* s) {
	static struct Huf lc, dc;
	static int done = 0;
	if (!done) {
		short l[288];
		int i = 0;
		for (; i < 144; ++i) l[i] = 8;
		for (; i < 256; ++i) l[i] = 9;
		for (; i < 280; ++i) l[i] = 7;
		for (; i < 288; ++i) l[i] = 8;
		buildHuf(&lc, l, 288);
		for (i = 0; i < 30; ++i) l[i] = 5;
		buildHuf(&dc, l, 30);
		done = 1;
	}
	codes(s, &lc, &dc);
}

static void dynamicBlock(struct Inf* s) {
	static const short ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	short lens[320];
	struct Huf lc, dc;
	int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
	if (s->err || nlen > 286 || ndist > 30) { s->err = 1; return; }
	memset(lens, 0, sizeof(lens));
	for (int i = 0; i < ncode; ++i) lens[ORDER[i]] = (short) bits(s, 3);
	if (s->err || buildHuf(&lc, lens, 19) != 0) { s->err = 1; return; }
	int idx = 0;
	while (idx < nlen + ndist) {
		int sym = decodeSym(s, &lc);
		if (s->err || sym < 0) { s->err = 1; return; }
		if (sym < 16) { lens[idx++] = (short) sym; continue; }
		short v = 0;
		int rep;
		if (sym == 16) { if (!idx) { s->err = 1; return; } v = lens[idx - 1]; rep = 3 + bits(s, 2); }
		else if (sym == 17) rep = 3 + bits(s, 3);
		else rep = 11 + bits(s, 7);
		if (s->err || idx + rep > nlen + ndist) { s->err = 1; return; }
		while (rep--) lens[idx++] = v;
	}
	if (lens[256] == 0) { s->err = 1; return; }
	int e = buildHuf(&lc, lens, nlen);
	if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) { s->err = 1; return; }
	e = buildHuf(&dc, lens + nlen, ndist);
	if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) { s->err = 1; return; }
	codes(s, &lc, &dc);
}

/* Descomprime un flujo zlib en out (tamaño exacto esperado). 0 = bien. */
static int zinflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
	if (inLen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return -1;
	struct Inf s = { in, inLen, 2, 0, 0, out, outLen, 0, 0 };
	int last;
	do {
		last = bits(&s, 1);
		int type = bits(&s, 2);
		if (s.err) return -1;
		if (type == 0) {
			s.bitBuf = 0; s.bitCnt = 0;
			if (s.inPos + 4 > s.inLen) return -1;
			unsigned len = s.in[s.inPos] | s.in[s.inPos + 1] << 8;
			unsigned nlen = s.in[s.inPos + 2] | s.in[s.inPos + 3] << 8;
			s.inPos += 4;
			if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen || s.outPos + len > s.outLen) return -1;
			memcpy(s.out + s.outPos, s.in + s.inPos, len);
			s.inPos += len; s.outPos += len;
		} else if (type == 1) fixedBlock(&s);
		else if (type == 2) dynamicBlock(&s);
		else return -1;
		if (s.err) return -1;
	} while (!last);
	return s.outPos == s.outLen ? 0 : -1;
}

/* ---------------- PNG ---------------- */
static uint32_t be32(const uint8_t* p) { return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

/* Deshace los filtros de una imagen (o pasada) de h filas de rb bytes */
static int unfilter(uint8_t* d, int h, size_t rb, int bpp) {
	uint8_t* prev = NULL;
	for (int y = 0; y < h; ++y) {
		uint8_t* row = d + (size_t) y * (rb + 1);
		int f = row[0];
		uint8_t* r = row + 1;
		for (size_t i = 0; i < rb; ++i) {
			int a = i >= (size_t) bpp ? r[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= (size_t) bpp) ? prev[i - bpp] : 0;
			switch (f) {
			case 0: break;
			case 1: r[i] = (uint8_t) (r[i] + a); break;
			case 2: r[i] = (uint8_t) (r[i] + b); break;
			case 3: r[i] = (uint8_t) (r[i] + ((a + b) >> 1)); break;
			case 4: r[i] = (uint8_t) (r[i] + paeth(a, b, c)); break;
			default: return -1;
			}
		}
		prev = r;
	}
	return 0;
}

unsigned char* pngDecode(const unsigned char* data, size_t len, int* outW, int* outH) {
	static const uint8_t SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (!data || len < 33 || memcmp(data, SIG, 8)) return NULL;
	size_t pos = 8;
	uint32_t w = 0, h = 0;
	int depth = 0, ctype = -1, interlace = 0;
	uint8_t pal[256][4];
	int npal = 0;
	int trnsGray = -1, trnsR = -1, trnsG = -1, trnsB = -1;
	memset(pal, 255, sizeof(pal));
	uint8_t* idat = NULL;
	size_t idatLen = 0, idatCap = 0;
	unsigned char* rgb = NULL;
	uint8_t* raw = NULL;
	while (pos + 12 <= len) {
		uint32_t clen = be32(data + pos);
		const uint8_t* type = data + pos + 4;
		const uint8_t* c = data + pos + 8;
		if (clen > len - pos - 12) break;
		if (!memcmp(type, "IHDR", 4) && clen >= 13) {
			w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12];
			if (!w || !h || w > 8192 || h > 8192 || c[10] || c[11] || interlace > 1) goto fail;
		} else if (!memcmp(type, "PLTE", 4)) {
			npal = (int) (clen / 3);
			if (npal > 256) npal = 256;
			for (int i = 0; i < npal; ++i) { pal[i][0] = c[i * 3]; pal[i][1] = c[i * 3 + 1]; pal[i][2] = c[i * 3 + 2]; }
		} else if (!memcmp(type, "tRNS", 4)) {
			if (ctype == 3) for (uint32_t i = 0; i < clen && i < 256; ++i) pal[i][3] = c[i];
			else if (ctype == 0 && clen >= 2) trnsGray = c[0] << 8 | c[1];
			else if (ctype == 2 && clen >= 6) { trnsR = c[0] << 8 | c[1]; trnsG = c[2] << 8 | c[3]; trnsB = c[4] << 8 | c[5]; }
		} else if (!memcmp(type, "IDAT", 4)) {
			if (idatLen + clen > idatCap) {
				size_t nc = (idatLen + clen) * 2 + 4096;
				uint8_t* n = realloc(idat, nc);
				if (!n) goto fail;
				idat = n; idatCap = nc;
			}
			memcpy(idat + idatLen, c, clen);
			idatLen += clen;
		} else if (!memcmp(type, "IEND", 4)) break;
		pos += 12 + clen;
	}
	if (!w || !idat) goto fail;
	int chans;
	switch (ctype) {
	case 0: chans = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) goto fail; break;
	case 2: chans = 3; if (depth != 8 && depth != 16) goto fail; break;
	case 3: chans = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) goto fail; if (!npal) goto fail; break;
	case 4: chans = 2; if (depth != 8 && depth != 16) goto fail; break;
	case 6: chans = 4; if (depth != 8 && depth != 16) goto fail; break;
	default: goto fail;
	}
	int bitsPx = chans * depth, bpp = bitsPx >= 8 ? bitsPx / 8 : 1;
	static const int PX[7] = { 0, 4, 0, 2, 0, 1, 0 }, PY[7] = { 0, 0, 4, 0, 2, 0, 1 }, SX[7] = { 8, 8, 4, 4, 2, 2, 1 }, SY[7] = { 8, 8, 8, 4, 4, 2, 2 };
	int passes = interlace ? 7 : 1;
	uint32_t pw[7], ph[7];
	size_t prb[7], total = 0;
	for (int p = 0; p < passes; ++p) {
		pw[p] = interlace ? (w + SX[p] - 1 - PX[p]) / SX[p] : w;
		ph[p] = interlace ? (h + SY[p] - 1 - PY[p]) / SY[p] : h;
		if (w <= (uint32_t) PX[p]) pw[p] = 0;
		if (h <= (uint32_t) PY[p]) ph[p] = 0;
		prb[p] = ((size_t) pw[p] * bitsPx + 7) / 8;
		if (pw[p] && ph[p]) total += (size_t) ph[p] * (prb[p] + 1);
	}
	raw = malloc(total ? total : 1);
	rgb = malloc((size_t) w * h * 3);
	if (!raw || !rgb || zinflate(idat, idatLen, raw, total)) goto fail;
	uint8_t* src = raw;
	for (int p = 0; p < passes; ++p) {
		if (!pw[p] || !ph[p]) continue;
		if (unfilter(src, (int) ph[p], prb[p], bpp)) goto fail;
		for (uint32_t y = 0; y < ph[p]; ++y) {
			const uint8_t* r = src + (size_t) y * (prb[p] + 1) + 1;
			uint32_t oy = interlace ? y * SY[p] + PY[p] : y;
			for (uint32_t x = 0; x < pw[p]; ++x) {
				uint32_t ox = interlace ? x * SX[p] + PX[p] : x;
				int R, G, B, A = 255;
				if (depth < 8) {
					size_t bit = (size_t) x * depth;
					int v = (r[bit >> 3] >> (8 - depth - (int) (bit & 7))) & ((1 << depth) - 1);
					if (ctype == 3) { R = pal[v][0]; G = pal[v][1]; B = pal[v][2]; A = pal[v][3]; }
					else { int g8 = v * 255 / ((1 << depth) - 1); R = G = B = g8; if (v == trnsGray) A = 0; }
				} else if (depth == 8) {
					const uint8_t* q = r + (size_t) x * chans;
					if (ctype == 3) { R = pal[q[0]][0]; G = pal[q[0]][1]; B = pal[q[0]][2]; A = pal[q[0]][3]; }
					else if (ctype == 0) { R = G = B = q[0]; if (q[0] == trnsGray) A = 0; }
					else if (ctype == 4) { R = G = B = q[0]; A = q[1]; }
					else { R = q[0]; G = q[1]; B = q[2]; if (ctype == 6) A = q[3]; else if (R == trnsR && G == trnsG && B == trnsB) A = 0; }
				} else {
					const uint8_t* q = r + (size_t) x * chans * 2;
					int v0 = q[0] << 8 | q[1];
					if (ctype == 0) { R = G = B = q[0]; if (v0 == trnsGray) A = 0; }
					else if (ctype == 4) { R = G = B = q[0]; A = q[2]; }
					else {
						R = q[0]; G = q[2]; B = q[4];
						if (ctype == 6) A = q[6];
						else if (v0 == trnsR && (q[2] << 8 | q[3]) == trnsG && (q[4] << 8 | q[5]) == trnsB) A = 0;
					}
				}
				/* transparencia sobre el fondo oscuro de las etiquetas */
				uint8_t* o = rgb + ((size_t) oy * w + ox) * 3;
				o[0] = (uint8_t) ((R * A + 0x12 * (255 - A)) / 255);
				o[1] = (uint8_t) ((G * A + 0x14 * (255 - A)) / 255);
				o[2] = (uint8_t) ((B * A + 0x1A * (255 - A)) / 255);
			}
		}
		src += (size_t) ph[p] * (prb[p] + 1);
	}
	free(raw);
	free(idat);
	*outW = (int) w;
	*outH = (int) h;
	return rgb;
fail:
	free(raw);
	free(idat);
	free(rgb);
	return NULL;
}
