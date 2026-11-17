/* Zadark HD: filtro de Zadark para juegos de GBA (color remaster + xBR + CAS + resplandor + textura).
 *  - Color "remaster" por tabla (vibrance, curva en S, toque cálido)
 *  - CAS (AMD FidelityFX Contrast Adaptive Sharpening, MIT) sobre la imagen x4
 *  - Filtro diagonal de TV de PCSX2: máscara RGB (x + y % 3) % 3 en píxeles de pantalla
 */
#include "postfx.h"
#include "xbr.h"

#include <math.h>
#include <string.h>

static inline uint8_t clamp8(float v) {
	return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t) (v + 0.5f);
}

/* ---------------- Zadark HD: color "remaster" con tabla de 32 768 colores ----------------
 * La GBA tiene colores de 15 bits: cualquier ajuste se calcula una vez por color y se
 * aplica con una búsqueda en tabla (gratis incluso en Switch). */
static uint32_t g_lut[32768];
static int g_lutLevel = -1;

static const struct { float vib, curve, warm, gamma; } VIVO[4] = {
	{ 0.15f, 0.10f, 0.008f, 1.00f },
	{ 0.30f, 0.20f, 0.015f, 1.02f },
	{ 0.45f, 0.30f, 0.025f, 1.04f },
	{ 0.65f, 0.42f, 0.035f, 1.06f },
};

static void buildLut(int nivel) {
	const float vib = VIVO[nivel - 1].vib, curve = VIVO[nivel - 1].curve, warm = VIVO[nivel - 1].warm, ig = 1.0f / VIVO[nivel - 1].gamma;
	for (int i = 0; i < 32768; ++i) {
		int r5 = i & 31, g5 = (i >> 5) & 31, b5 = (i >> 10) & 31;
		float c[3] = { ((r5 << 3) | (r5 >> 2)) / 255.0f, ((g5 << 3) | (g5 >> 2)) / 255.0f, ((b5 << 3) | (b5 >> 2)) / 255.0f };
		float y = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
		float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
		float amt = vib * (1.0f - (mx - mn));  /* vibrance: más a lo apagado */
		/* blancos, grises y negros quedan neutros: el toque cálido solo va a los colores con tono */
		float t = ((mx - mn) - 0.05f) / 0.20f;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		const float warmK = warm * t * t * (3.0f - 2.0f * t);
		for (int k = 0; k < 3; ++k) {
			float v = y + (c[k] - y) * (1.0f + amt);
			v = v < 0 ? 0 : v > 1 ? 1 : v;
			float sc = v * v * (3.0f - 2.0f * v);  /* curva en S */
			v = v + (sc - v) * curve;
			v = powf(v, ig);
			if (k == 0) v *= 1.0f + warmK;
			if (k == 2) v *= 1.0f - warmK;
			c[k] = v;
		}
		g_lut[i] = 0xFF000000u | clamp8(c[0] * 255.0f) | ((uint32_t) clamp8(c[1] * 255.0f) << 8) | ((uint32_t) clamp8(c[2] * 255.0f) << 16);
	}
	g_lutLevel = nivel;
}

void zhdColor(const uint32_t* src, uint32_t* dst, int n, int vivo) {
	if (vivo < 1) vivo = 1;
	if (vivo > 4) vivo = 4;
	if (vivo != g_lutLevel) buildLut(vivo);
	for (int i = 0; i < n; ++i) {
		uint32_t p = src[i];
		uint32_t idx = ((p & 0xFF) >> 3) | (((p >> 8) & 0xFF) >> 3) << 5 | (((p >> 16) & 0xFF) >> 3) << 10;
		dst[i] = g_lut[idx];
	}
}

int zhdTextura(int nivel) {
	static const int T[4] = { 0, 20, 35, 60 };
	return T[(nivel < 1 ? 1 : nivel > 4 ? 4 : nivel) - 1];
}

int zhdSuavizado(int nivel) {
	static const int S[4] = { 35, 40, 45, 50 };
	return S[(nivel < 1 ? 1 : nivel > 4 ? 4 : nivel) - 1];
}

/* Resplandor: solo lo más claro, reducido a 1/4 y difuminado; la GPU lo amplía y lo suma */
void zhdBloom(const uint32_t* src, int w, int h, uint32_t* out /* (w/4) x (h/4), ARGB8888 */) {
	const int bw = w / 4, bh = h / 4;
	/* El mismo filtro se usa en NES/SNES/Genesis; 320x240 es el máximo. */
	static float acc[3][(320 / 4) * (240 / 4)], tmp[3][(320 / 4) * (240 / 4)];
	if (bw * bh > (int) (sizeof(acc[0]) / sizeof(float))) return;
	const float thr = 0.86f * 255.0f, inv = 1.0f / ((255.0f - thr) * 16.0f);
	for (int by = 0; by < bh; ++by)
		for (int bx = 0; bx < bw; ++bx) {
			float r = 0, g = 0, b = 0;
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 4; ++i) {
					uint32_t p = src[(by * 4 + j) * w + bx * 4 + i];
					float pr = (float) (p & 0xFF), pg = (float) ((p >> 8) & 0xFF), pb = (float) ((p >> 16) & 0xFF);
					float y = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;
					if (y <= thr) continue;
					float k = (y - thr) * inv;
					r += pr * k; g += pg * k; b += pb * k;
				}
			acc[0][by * bw + bx] = r; acc[1][by * bw + bx] = g; acc[2][by * bw + bx] = b;
		}
	/* difuminado 1-2-1 horizontal y vertical, dos veces */
	for (int pass = 0; pass < 2; ++pass)
		for (int c = 0; c < 3; ++c) {
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x) {
					int xl = x > 0 ? x - 1 : 0, xr = x < bw - 1 ? x + 1 : x;
					tmp[c][y * bw + x] = (acc[c][y * bw + xl] + 2 * acc[c][y * bw + x] + acc[c][y * bw + xr]) * 0.25f;
				}
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x) {
					int yu = y > 0 ? y - 1 : 0, yd = y < bh - 1 ? y + 1 : y;
					acc[c][y * bw + x] = (tmp[c][yu * bw + x] + 2 * tmp[c][y * bw + x] + tmp[c][yd * bw + x]) * 0.25f;
				}
		}
	for (int i = 0; i < bw * bh; ++i)
		out[i] = 0xFF000000u | (uint32_t) clamp8(acc[0][i]) << 16 | (uint32_t) clamp8(acc[1][i]) << 8 | clamp8(acc[2][i]);
}

/* ---------------- CAS (por franjas, en varios núcleos) ----------------
 * Versión en enteros: amp se cuantiza a 256 pasos y el peso y el divisor salen de tablas. */
#define CAS_STEPS 256
static int32_t g_casW[CAS_STEPS + 1];   /* peso (negativo) en Q12 */
static int32_t g_casD[CAS_STEPS + 1];   /* 1 / (1 + 4 peso) en Q12 */
static uint32_t g_casRcp[511];          /* CAS_STEPS * 65536 / mx */
static int g_casSharp = -1;

static void setupCas(int nitidez) {
	if (nitidez == g_casSharp) return;
	g_casSharp = nitidez;
	float s = nitidez / 100.0f;
	float peak = -1.0f / (8.0f + (5.0f - 8.0f) * s);
	for (int i = 0; i <= CAS_STEPS; ++i) {
		float w = sqrtf((float) i / CAS_STEPS) * peak;
		g_casW[i] = (int32_t) lroundf(w * 4096.0f);
		g_casD[i] = (int32_t) lroundf(4096.0f / (1.0f + 4.0f * w));
	}
	g_casRcp[0] = 0;
	for (int m = 1; m < 511; ++m) g_casRcp[m] = (uint32_t) ((CAS_STEPS * 65536u) / (unsigned) m);
}

struct CasCtx { const uint32_t* src; uint32_t* dst; int w, h; };

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static void casRows(void* p, int y0, int y1) {
	const struct CasCtx* c = p;
	const int w = c->w, h = c->h;
	for (int y = y0; y < y1; ++y) {
		const uint32_t* up = c->src + (y > 0 ? y - 1 : 0) * w;
		const uint32_t* row = c->src + y * w;
		const uint32_t* dn = c->src + (y < h - 1 ? y + 1 : y) * w;
		uint32_t* out = c->dst + y * w;
		for (int x = 0; x < w; ++x) {
			int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : x;
			uint32_t e = row[x], b = up[x], d = row[xl], f = row[xr], hh = dn[x];
			if (b == e && d == e && f == e && hh == e) { out[x] = e; continue; } /* zona plana */
			uint32_t a = up[xl], cc = up[xr], g = dn[xl], ii = dn[xr];
			uint32_t res = e & 0xFF000000u;
			for (int s = 0; s < 24; s += 8) {
				int vb = (b >> s) & 0xFF, vd = (d >> s) & 0xFF, ve = (e >> s) & 0xFF, vf = (f >> s) & 0xFF, vh = (hh >> s) & 0xFF;
				int mn = imin(imin(imin(vb, vd), imin(vf, vh)), ve);
				int mx = imax(imax(imax(vb, vd), imax(vf, vh)), ve);
				if (mn == mx) { res |= (uint32_t) ve << s; continue; }  /* canal plano: sin cambio */
				int va = (a >> s) & 0xFF, vc = (cc >> s) & 0xFF, vg = (g >> s) & 0xFF, vi = (ii >> s) & 0xFF;
				int mn2 = imin(mn, imin(imin(va, vc), imin(vg, vi)));
				int mx2 = imax(mx, imax(imax(va, vc), imax(vg, vi)));
				int mnS = mn + mn2, mxS = mx + mx2;           /* 0..510, como mn+mn2 de AMD */
				if (mxS == 0) { res |= (uint32_t) ve << s; continue; }
				int num = imin(mnS, 510 - mxS);
				int idx = (int) (((uint32_t) num * g_casRcp[mxS]) >> 16);
				if (idx > CAS_STEPS) idx = CAS_STEPS;
				int64_t v = ((int64_t) (vb + vd + vf + vh) * g_casW[idx] + ((int64_t) ve << 12)) * g_casD[idx];
				int vv = (int) ((v + (1 << 23)) >> 24);
				res |= (uint32_t) (vv < 0 ? 0 : vv > 255 ? 255 : vv) << s;
			}
			out[x] = res;
		}
	}
}

/* ---------------- Menos redondeo: mezcla xBR con el píxel original ---------------- */
struct BlendCtx { uint32_t* big; const uint32_t* src; int srcW; uint32_t k; };

static void blendRows(void* p, int y0, int y1) {
	const struct BlendCtx* c = p;
	const int W = c->srcW * 4;
	const uint32_t k = c->k, ik = 256 - k;
	for (int y = y0; y < y1; ++y) {
		uint32_t* row = c->big + y * W;
		const uint32_t* srow = c->src + (y >> 2) * c->srcW;
		for (int x = 0; x < W; ++x) {
			uint32_t a = row[x], n = srow[x >> 2];
			if (a == n) continue;
			uint32_t rb = (((a & 0x00FF00FFu) * k + (n & 0x00FF00FFu) * ik) >> 8) & 0x00FF00FFu;
			uint32_t g = (((a & 0x0000FF00u) * k + (n & 0x0000FF00u) * ik) >> 8) & 0x0000FF00u;
			row[x] = (a & 0xFF000000u) | rb | g;
		}
	}
}

void postfxSoften(uint32_t* big, const uint32_t* src, int srcW, int srcH, int suavizado) {
	if (suavizado >= 100) return;
	if (suavizado < 0) suavizado = 0;
	struct BlendCtx ctx = { big, src, srcW, (uint32_t) (suavizado * 256 / 100) };
	xbrParallel(blendRows, &ctx, srcH * 4);
}

/* ---------------- API ---------------- */
void postfxSharpen(const uint32_t* src, uint32_t* dst, int w, int h, int nitidez) {
	if (nitidez < 0) nitidez = 0;
	if (nitidez > 100) nitidez = 100;
	setupCas(nitidez);
	struct CasCtx ctx = { src, dst, w, h };
	xbrParallel(casRows, &ctx, h);
}

/* Máscara del filtro diagonal de PCSX2: cada píxel de pantalla deja pasar un canal
 * entero y los otros dos a (1 - fuerza/2). Formato ARGB8888. */
void postfxBuildMask(uint32_t* px, int w, int h, int pitchPx, int fuerza) {
	if (fuerza < 0) fuerza = 0;
	if (fuerza > 100) fuerza = 100;
	uint32_t lo = (uint32_t) (255 - fuerza * 255 / 200);
	const uint32_t pat[3] = {
		0xFF000000u | 0xFF0000u | lo << 8 | lo,       /* R */
		0xFF000000u | lo << 16 | 0xFF00u | lo,        /* G */
		0xFF000000u | lo << 16 | lo << 8 | 0xFFu,     /* B */
	};
	for (int y = 0; y < h; ++y) {
		uint32_t* row = px + y * pitchPx;
		int o = y % 3;
		for (int x = 0; x < w; ++x) row[x] = pat[(x + o) % 3];
	}
}
