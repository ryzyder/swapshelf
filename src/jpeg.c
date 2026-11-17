/*
 * jpeg: decodificador JPEG (baseline y progresivo, Huffman, 8 bits, 1 o 3 componentes).
 * Todos los escaneos se guardan como coeficientes y al final se hace la IDCT,
 * así el modo baseline y el progresivo comparten el mismo camino.
 */
#include "jpeg.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const uint8_t ZZ[64 + 16] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	/* relleno por si un archivo dañado pide k > 63 */
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

struct Huff {
	uint16_t fast[512];      /* (largo << 8) | símbolo, 0 = no está en 9 bits */
	int maxcode[18], valptr[17], mincode[17];
	uint8_t vals[256];
	int present;
};

struct Comp {
	int id, h, v, tq, td, ta;
	int bw, bh;              /* bloques por fila / columna (rellenado al MCU) */
	int bwReal, bhReal;      /* bloques que cubre la imagen de este componente */
	short* coef;
	int dcPred;
	uint8_t* pix;            /* plano final, bw*8 x bh*8 */
};

struct Jpeg {
	const uint8_t* d;
	size_t len, pos;
	uint32_t buf;
	int cnt, marker;
	uint16_t qt[4][64];      /* en orden natural */
	struct Huff hdc[4], hac[4];
	struct Comp comp[3];
	int nc, w, h, hmax, vmax, mcux, mcuy, progressive, restart;
	int eobrun;
};

static int rd16(struct Jpeg* j) {
	if (j->pos + 2 > j->len) return -1;
	int v = j->d[j->pos] << 8 | j->d[j->pos + 1];
	j->pos += 2;
	return v;
}

static int buildHuff(struct Huff* h, const uint8_t* bits, const uint8_t* vals, int nvals) {
	memset(h, 0, sizeof(*h));
	memcpy(h->vals, vals, (size_t) nvals);
	int code = 0, k = 0;
	for (int l = 1; l <= 16; ++l) {
		h->valptr[l] = k;
		h->mincode[l] = code;
		for (int i = 0; i < bits[l - 1]; ++i, ++code, ++k) {
			if (l <= 9) {
				int first = code << (9 - l), n = 1 << (9 - l);
				for (int f = 0; f < n; ++f) h->fast[first + f] = (uint16_t) (l << 8 | h->vals[k]);
			}
		}
		h->maxcode[l] = bits[l - 1] ? code - 1 : -1;
		if (code > (1 << l)) return 0;
		code <<= 1;
	}
	h->maxcode[17] = 0x7FFFFFFF;
	h->present = 1;
	return 1;
}

static void fill(struct Jpeg* j) {
	while (j->cnt <= 24) {
		uint32_t b = 0;
		if (!j->marker && j->pos < j->len) {
			b = j->d[j->pos];
			if (b == 0xFF) {
				uint32_t nb = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0xD9;
				if (nb == 0) j->pos += 2;
				else { j->marker = 1; b = 0; }
			} else ++j->pos;
		}
		j->buf |= b << (24 - j->cnt);
		j->cnt += 8;
	}
}

static int getbits(struct Jpeg* j, int n) {
	if (n <= 0) return 0;
	if (n > 16) n = 16;  /* archivo dañado */
	fill(j);
	int v = (int) (j->buf >> (32 - n));
	j->buf <<= n;
	j->cnt -= n;
	return v;
}

static int getbit(struct Jpeg* j) { return getbits(j, 1); }

static int extend(int v, int n) { if (n > 16) n = 16; return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int decodeHuff(struct Jpeg* j, const struct Huff* h) {
	fill(j);
	uint16_t f = h->fast[j->buf >> 23];
	if (f) {
		int l = f >> 8;
		j->buf <<= l;
		j->cnt -= l;
		return f & 0xFF;
	}
	for (int l = 10; l <= 16; ++l) {
		int code = (int) (j->buf >> (32 - l));
		if (code <= h->maxcode[l]) {
			j->buf <<= l;
			j->cnt -= l;
			int idx = h->valptr[l] + code - h->mincode[l];
			return idx >= 0 && idx < 256 ? h->vals[idx] : 0;
		}
	}
	j->buf <<= 16;  /* código inválido: se descarta y se sigue */
	j->cnt -= 16;
	return 0;
}

/* ---- bloques ---- */
static void blockBaseline(struct Jpeg* j, struct Comp* c, short* b) {
	int t = decodeHuff(j, &j->hdc[c->td]);
	int diff = t ? extend(getbits(j, t), t) : 0;
	c->dcPred += diff;
	b[0] = (short) c->dcPred;
	for (int k = 1; k < 64;) {
		int rs = decodeHuff(j, &j->hac[c->ta]);
		int r = rs >> 4, s = rs & 15;
		if (!s) { if (r != 15) break; k += 16; continue; }
		k += r;
		if (k > 63) break;
		b[ZZ[k]] = (short) extend(getbits(j, s), s);
		++k;
	}
}

static void blockDcFirst(struct Jpeg* j, struct Comp* c, short* b, int al) {
	int t = decodeHuff(j, &j->hdc[c->td]);
	int diff = t ? extend(getbits(j, t), t) : 0;
	c->dcPred += diff;
	b[0] = (short) (c->dcPred * (1 << al));
}

static void blockDcRefine(struct Jpeg* j, short* b, int al) {
	if (getbit(j)) b[0] |= (short) (1 << al);
}

static void blockAcFirst(struct Jpeg* j, struct Comp* c, short* b, int ss, int se, int al) {
	if (j->eobrun > 0) { --j->eobrun; return; }
	for (int k = ss; k <= se;) {
		int rs = decodeHuff(j, &j->hac[c->ta]);
		int r = rs >> 4, s = rs & 15;
		if (!s) {
			if (r < 15) {
				j->eobrun = (1 << r) - 1;
				if (r) j->eobrun += getbits(j, r);
				break;
			}
			k += 16;
			continue;
		}
		k += r;
		if (k > 63) break;
		b[ZZ[k]] = (short) (extend(getbits(j, s), s) * (1 << al));
		++k;
	}
}

static void blockAcRefine(struct Jpeg* j, struct Comp* c, short* b, int ss, int se, int al) {
	int p1 = 1 << al, m1 = -1 * (1 << al);
	int k = ss;
	if (j->eobrun == 0) {
		for (; k <= se; ++k) {
			int rs = decodeHuff(j, &j->hac[c->ta]);
			int r = rs >> 4, s = rs & 15;
			if (s) s = getbit(j) ? p1 : m1;
			else if (r != 15) {
				j->eobrun = 1 << r;
				if (r) j->eobrun += getbits(j, r);
				break;
			}
			do {
				short* t = &b[ZZ[k]];
				if (*t != 0) {
					if (getbit(j) && (*t & p1) == 0) *t = (short) (*t >= 0 ? *t + p1 : *t + m1);
				} else {
					if (--r < 0) break;
				}
				++k;
			} while (k <= se);
			if (s && k <= 63) b[ZZ[k]] = (short) s;
		}
	}
	if (j->eobrun > 0) {
		for (; k <= se; ++k) {
			short* t = &b[ZZ[k]];
			if (*t != 0 && getbit(j) && (*t & p1) == 0) *t = (short) (*t >= 0 ? *t + p1 : *t + m1);
		}
		--j->eobrun;
	}
}

static void decodeBlock(struct Jpeg* j, struct Comp* c, int bx, int by, int ss, int se, int ah, int al) {
	short* b = c->coef + ((size_t) by * c->bw + bx) * 64;
	if (!j->progressive) blockBaseline(j, c, b);
	else if (ss == 0) { if (!ah) blockDcFirst(j, c, b, al); else blockDcRefine(j, b, al); }
	else if (!ah) blockAcFirst(j, c, b, ss, se, al);
	else blockAcRefine(j, c, b, ss, se, al);
}

static int handleRestart(struct Jpeg* j, int ncs, struct Comp** cs) {
	j->buf = 0;
	j->cnt = 0;
	j->marker = 0;
	if (j->pos + 1 < j->len && j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) j->pos += 2;
	for (int i = 0; i < ncs; ++i) cs[i]->dcPred = 0;
	j->eobrun = 0;
	return 1;
}

static int decodeScan(struct Jpeg* j) {
	int ls = rd16(j);
	if (ls < 6 || j->pos + (size_t) ls - 2 > j->len) return 0;
	int ns = j->d[j->pos++];
	if (ns < 1 || ns > 3) return 0;
	struct Comp* cs[3];
	for (int i = 0; i < ns; ++i) {
		int id = j->d[j->pos++], t = j->d[j->pos++];
		cs[i] = NULL;
		for (int k = 0; k < j->nc; ++k) if (j->comp[k].id == id) cs[i] = &j->comp[k];
		if (!cs[i]) return 0;
		cs[i]->td = (t >> 4) & 3;
		cs[i]->ta = t & 3;
	}
	int ss = j->d[j->pos++], se = j->d[j->pos++], a = j->d[j->pos++];
	int ah = a >> 4, al = a & 15;
	if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
	if (ss > 63 || se > 63 || ss > se) return 0;
	for (int i = 0; i < ns; ++i) {
		cs[i]->dcPred = 0;
		if (ss == 0 && !ah && !j->hdc[cs[i]->td].present) return 0;
		if ((se > 0 || !j->progressive) && !(ss == 0 && j->progressive) && !j->hac[cs[i]->ta].present) return 0;
	}
	j->buf = 0; j->cnt = 0; j->marker = 0; j->eobrun = 0;
	int todo = j->restart ? j->restart : 0x7FFFFFFF;
	if (ns == 1) {
		struct Comp* c = cs[0];
		for (int by = 0; by < c->bhReal; ++by)
			for (int bx = 0; bx < c->bwReal; ++bx) {
				decodeBlock(j, c, bx, by, ss, se, ah, al);
				if (--todo <= 0) { handleRestart(j, ns, cs); todo = j->restart; }
			}
	} else {
		for (int my = 0; my < j->mcuy; ++my)
			for (int mx = 0; mx < j->mcux; ++mx) {
				for (int i = 0; i < ns; ++i) {
					struct Comp* c = cs[i];
					for (int v = 0; v < c->v; ++v)
						for (int h = 0; h < c->h; ++h) decodeBlock(j, c, mx * c->h + h, my * c->v + v, ss, se, ah, al);
				}
				if (--todo <= 0) { handleRestart(j, ns, cs); todo = j->restart; }
			}
	}
	/* volver al siguiente marcador */
	while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7))) ++j->pos;
	return 1;
}

/* ---- IDCT y color ---- */
static float g_idct[8][8];

static void idctInit(void) {
	static int done = 0;
	if (done) return;
	for (int x = 0; x < 8; ++x)
		for (int u = 0; u < 8; ++u) g_idct[x][u] = (float) ((u ? 1.0 : sqrt(0.5)) * cos((2 * x + 1) * u * M_PI / 16.0) / 2.0);
	done = 1;
}

static void idctBlock(const short* in, const uint16_t* q, uint8_t* out, int stride) {
	float t[64], f[64];
	for (int i = 0; i < 64; ++i) f[i] = (float) in[i] * q[i];
	for (int y = 0; y < 8; ++y)          /* filas: u -> x */
		for (int x = 0; x < 8; ++x) {
			float s = 0;
			for (int u = 0; u < 8; ++u) s += g_idct[x][u] * f[y * 8 + u];
			t[y * 8 + x] = s;
		}
	for (int x = 0; x < 8; ++x)          /* columnas: v -> y */
		for (int y = 0; y < 8; ++y) {
			float s = 0;
			for (int v = 0; v < 8; ++v) s += g_idct[y][v] * t[v * 8 + x];
			int p = (int) lrintf(s + 128.0f);
			out[y * stride + x] = (uint8_t) (p < 0 ? 0 : p > 255 ? 255 : p);
		}
}

static uint8_t clamp8(float v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : (int) (v + 0.5f)); }

unsigned char* jpegDecode(const unsigned char* data, size_t len, int* outW, int* outH) {
	if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return NULL;
	struct Jpeg* j = calloc(1, sizeof(*j));
	if (!j) return NULL;
	j->d = data; j->len = len; j->pos = 2;
	unsigned char* rgb = NULL;
	int frame = 0, scans = 0;
	idctInit();
	while (j->pos + 4 <= j->len) {
		if (j->d[j->pos] != 0xFF) { ++j->pos; continue; }
		int m = j->d[j->pos + 1];
		j->pos += 2;
		if (m == 0xFF || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { if (m == 0xFF) --j->pos; continue; }
		if (m == 0xD9) break;  /* EOI */
		if (m == 0xDA) {       /* SOS */
			if (!frame || !decodeScan(j)) break;
			++scans;
			continue;
		}
		size_t segStart = j->pos;
		int l = rd16(j);
		if (l < 2 || segStart + (size_t) l > j->len) break;
		size_t end = segStart + (size_t) l;
		if (m == 0xDB) {  /* DQT */
			while (j->pos < end) {
				int pq = j->d[j->pos] >> 4, tq = j->d[j->pos] & 3;
				++j->pos;
				for (int i = 0; i < 64; ++i) {
					int v = pq ? (j->d[j->pos] << 8 | j->d[j->pos + 1]) : j->d[j->pos];
					j->pos += pq ? 2 : 1;
					j->qt[tq][ZZ[i]] = (uint16_t) v;
				}
			}
		} else if (m == 0xC4) {  /* DHT */
			while (j->pos + 17 <= end) {
				int tc = j->d[j->pos] >> 4, th = j->d[j->pos] & 3;
				const uint8_t* bits = j->d + j->pos + 1;
				int n = 0;
				for (int i = 0; i < 16; ++i) n += bits[i];
				if (n > 256 || j->pos + 17 + (size_t) n > end) goto fail;
				if (!buildHuff(tc ? &j->hac[th] : &j->hdc[th], bits, j->d + j->pos + 17, n)) goto fail;
				j->pos += 17 + (size_t) n;
			}
		} else if (m == 0xDD) {  /* DRI */
			j->restart = rd16(j);
		} else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {  /* SOF */
			if (frame) goto fail;
			j->progressive = m == 0xC2;
			if (j->d[j->pos] != 8) goto fail;
			j->h = j->d[j->pos + 1] << 8 | j->d[j->pos + 2];
			j->w = j->d[j->pos + 3] << 8 | j->d[j->pos + 4];
			j->nc = j->d[j->pos + 5];
			j->pos += 6;
			if (j->w <= 0 || j->h <= 0 || j->w > 8192 || j->h > 8192 || (j->nc != 1 && j->nc != 3)) goto fail;
			j->hmax = j->vmax = 1;
			for (int i = 0; i < j->nc; ++i) {
				struct Comp* c = &j->comp[i];
				c->id = j->d[j->pos];
				c->h = j->d[j->pos + 1] >> 4;
				c->v = j->d[j->pos + 1] & 15;
				c->tq = j->d[j->pos + 2] & 3;
				j->pos += 3;
				if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) goto fail;
				if (c->h > j->hmax) j->hmax = c->h;
				if (c->v > j->vmax) j->vmax = c->v;
			}
			if (j->nc == 1) j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;
			j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
			j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
			for (int i = 0; i < j->nc; ++i) {
				struct Comp* c = &j->comp[i];
				c->bw = j->mcux * c->h;
				c->bh = j->mcuy * c->v;
				int cw = (j->w * c->h + j->hmax - 1) / j->hmax, ch = (j->h * c->v + j->vmax - 1) / j->vmax;
				c->bwReal = (cw + 7) / 8;
				c->bhReal = (ch + 7) / 8;
				c->coef = calloc((size_t) c->bw * c->bh * 64, sizeof(short));
				if (!c->coef) goto fail;
			}
			frame = 1;
		}
		/* APPn, COM y demás: se saltan */
		j->pos = end;
	}
	if (!frame || !scans) goto fail;
	/* IDCT por componente */
	for (int i = 0; i < j->nc; ++i) {
		struct Comp* c = &j->comp[i];
		int stride = c->bw * 8;
		c->pix = malloc((size_t) stride * c->bh * 8);
		if (!c->pix) goto fail;
		for (int by = 0; by < c->bh; ++by)
			for (int bx = 0; bx < c->bw; ++bx)
				idctBlock(c->coef + ((size_t) by * c->bw + bx) * 64, j->qt[c->tq], c->pix + (size_t) by * 8 * stride + bx * 8, stride);
	}
	rgb = malloc((size_t) j->w * j->h * 3);
	if (!rgb) goto fail;
	for (int y = 0; y < j->h; ++y) {
		uint8_t* o = rgb + (size_t) y * j->w * 3;
		if (j->nc == 1) {
			const uint8_t* p = j->comp[0].pix + (size_t) y * j->comp[0].bw * 8;
			for (int x = 0; x < j->w; ++x) o[x * 3] = o[x * 3 + 1] = o[x * 3 + 2] = p[x];
			continue;
		}
		/* croma con interpolación bilineal centrada (como el "fancy upsampling" de libjpeg) */
		float Yv, cbv = 0, crv = 0;
		for (int x = 0; x < j->w; ++x) {
			float ch[3];
			for (int k = 0; k < 3; ++k) {
				struct Comp* c = &j->comp[k];
				int stride = c->bw * 8, cw = stride, chh = c->bh * 8;
				if (c->h == j->hmax && c->v == j->vmax) { ch[k] = c->pix[(size_t) y * stride + x]; continue; }
				float fx = (x + 0.5f) * c->h / j->hmax - 0.5f, fy = (y + 0.5f) * c->v / j->vmax - 0.5f;
				int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
				float ax = fx - x0, ay = fy - y0;
				int x1 = x0 + 1, y1 = y0 + 1;
				if (x0 < 0) x0 = 0;
				if (y0 < 0) y0 = 0;
				if (x1 > cw - 1) x1 = cw - 1;
				if (y1 > chh - 1) y1 = chh - 1;
				const uint8_t* r0 = c->pix + (size_t) y0 * stride;
				const uint8_t* r1 = c->pix + (size_t) y1 * stride;
				ch[k] = (r0[x0] * (1 - ax) + r0[x1] * ax) * (1 - ay) + (r1[x0] * (1 - ax) + r1[x1] * ax) * ay;
			}
			Yv = ch[0]; cbv = ch[1] - 128.0f; crv = ch[2] - 128.0f;
			o[x * 3 + 0] = clamp8(Yv + 1.402f * crv);
			o[x * 3 + 1] = clamp8(Yv - 0.344136f * cbv - 0.714136f * crv);
			o[x * 3 + 2] = clamp8(Yv + 1.772f * cbv);
		}
	}
	*outW = j->w;
	*outH = j->h;
	goto done;
fail:
	free(rgb);
	rgb = NULL;
done:
	for (int i = 0; i < 3; ++i) { free(j->comp[i].coef); free(j->comp[i].pix); }
	free(j);
	return rgb;
}
