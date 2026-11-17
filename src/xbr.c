/*
 * xbr.c - port en CPU del shader "xBR-lv3" de mGBA (Hyllian), escala x4
 *
 * Traducción directa de shaders/xbr-lv3.shader/xbr.fs de mGBA 0.10.5:
 *   - mismos parámetros por defecto del manifest.ini:
 *       XBR_Y_WEIGHT = 48, XBR_EQ_THRESHOLD = 10, XBR_EQ_THRESHOLD2 = 2,
 *       XBR_LV2_COEFFICIENT = 2, delta = 0.4
 *   - misma regla de esquina (CORNER_C, la predeterminada del shader)
 *   - misma mezcla final: pix1 en orden x,y,z,w y pix2 en orden w,z,y,x, y se
 *     queda con el resultado que más se aleja del color original (c_df).
 * El shader evalúa cada pixel de salida con fp = fract(texCoord * texSize);
 * a escala entera x4 hay 16 posiciones fijas, así que las funciones fx se
 * precalculan. Las decisiones de borde dependen solo del pixel de origen.
 *
 * Copyright (C) 2011-2015 Hyllian (shader original, licencia MIT).
 */
#include "xbr.h"

#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define S 4
#define Y_WEIGHT 48.0f
#define EQ_THRESHOLD 10.0f
#define EQ_THRESHOLD2 2.0f
#define LV2_COEF 2.0f
#define DELTA 0.4f

static const float Ao[4] = { 1, -1, -1, 1 }, Bo[4] = { 1, 1, -1, -1 }, Co[4] = { 1.5f, 0.5f, -0.5f, 0.5f };
static const float Ax[4] = { 1, -1, -1, 1 }, Bx[4] = { 0.5f, 2, -0.5f, -2 }, Cx[4] = { 1, 1, -0.5f, 0 };
static const float Ay[4] = { 1, -1, -1, 1 }, By[4] = { 2, 0.5f, -2, -0.5f }, Cy[4] = { 2, 0, -1, 0.5f };
static const float Az[4] = { 6, -2, -6, 2 }, Bz[4] = { 2, 6, -2, -6 }, Cz[4] = { 5, 3, -3, -1 };
static const float Aw[4] = { 2, -6, -2, 6 }, Bw[4] = { 6, 2, -6, -2 }, Cw[4] = { 5, -1, -3, 3 };

/* FX[subpíxel][regla][esquina]: fx45, fx30, fx60, fx15, fx75 */
static float FX[S * S][5][4];
/* Precalculado por esquina k y combinación de reglas (edr, left, up, left3, up3 = 5 bits):
 * MX = blend (maximo) por subpíxel y NCM = máscara de subpíxeles con nc verdadero. */
static float MX[4][32][S * S];
static uint16_t NCM[4][32];
static bool g_ready;

static float smoothstepf(float e0, float e1, float x) {
	float t = (x - e0) / (e1 - e0);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

void xbrInit(void) {
	if (g_ready) return;
	for (int sy = 0; sy < S; ++sy) {
		for (int sx = 0; sx < S; ++sx) {
			float fx = (sx + 0.5f) / S, fy = (sy + 0.5f) / S;
			for (int k = 0; k < 4; ++k) {
				FX[sy * S + sx][0][k] = smoothstepf(Co[k] - DELTA, Co[k] + DELTA, Ao[k] * fy + Bo[k] * fx);
				FX[sy * S + sx][1][k] = smoothstepf(Cx[k] - DELTA, Cx[k] + DELTA, Ax[k] * fy + Bx[k] * fx);
				FX[sy * S + sx][2][k] = smoothstepf(Cy[k] - DELTA, Cy[k] + DELTA, Ay[k] * fy + By[k] * fx);
				FX[sy * S + sx][3][k] = smoothstepf(Cz[k] - DELTA, Cz[k] + DELTA, Az[k] * fy + Bz[k] * fx);
				FX[sy * S + sx][4][k] = smoothstepf(Cw[k] - DELTA, Cw[k] + DELTA, Aw[k] * fy + Bw[k] * fx);
			}
		}
	}
	for (int k = 0; k < 4; ++k) {
		for (int combo = 0; combo < 32; ++combo) {
			bool edr = combo & 1, L = combo & 2, U = combo & 4, L3 = combo & 8, U3 = combo & 16;
			uint16_t mask = 0;
			for (int sp = 0; sp < S * S; ++sp) {
				const float f45 = FX[sp][0][k], f30 = FX[sp][1][k], f60 = FX[sp][2][k], f15 = FX[sp][3][k], f75 = FX[sp][4][k];
				bool n45 = edr && f45 != 0.0f;
				bool n30 = edr && L && f30 != 0.0f;
				bool n60 = edr && U && f60 != 0.0f;
				bool n15 = edr && L && L3 && f15 != 0.0f;
				bool n75 = edr && U && U3 && f75 != 0.0f;
				float m = n45 ? f45 : 0;
				if (n30 && f30 > m) m = f30;
				if (n60 && f60 > m) m = f60;
				if (n15 && f15 > m) m = f15;
				if (n75 && f75 > m) m = f75;
				MX[k][combo][sp] = m;
				if (n75 || n15 || n30 || n60 || n45) mask |= (uint16_t) (1u << sp);
			}
			NCM[k][combo] = mask;
		}
	}
	g_ready = true;
}

/* Luma del shader: dot(rgb, XBR_Y_WEIGHT * (0.299, 0.587, 0.114)), rgb en 0..1 */
static float g_luma[XBR_MAX_W * XBR_MAX_H];

static inline float df(float a, float b) { return fabsf(a - b); }
static inline bool eq(float a, float b) { return df(a, b) < EQ_THRESHOLD; }
static inline bool eq2(float a, float b) { return df(a, b) < EQ_THRESHOLD2; }

/* mix(E, P, t) por canal sin redondear (como en la GPU); formato ABGR8888 */
static inline void mixf(uint32_t e, uint32_t p, float t, float out[3]) {
	for (int ch = 0; ch < 3; ++ch) {
		float a = (float) ((e >> (ch * 8)) & 0xFF), b = (float) ((p >> (ch * 8)) & 0xFF);
		out[ch] = a + (b - a) * t;
	}
}

/* c_df del shader: suma de diferencias absolutas por canal */
static inline float cdff(uint32_t e, const float c[3]) {
	float s = 0;
	for (int ch = 0; ch < 3; ++ch) s += fabsf((float) ((e >> (ch * 8)) & 0xFF) - c[ch]);
	return s;
}

static inline uint32_t packf(const float c[3]) {
	uint32_t out = 0xFF000000u;
	for (int ch = 0; ch < 3; ++ch) {
		int v = (int) (c[ch] + 0.5f);
		out |= (uint32_t) (v < 0 ? 0 : v > 255 ? 255 : v) << (ch * 8);
	}
	return out;
}

static void xbrRows(const uint32_t* src, int w, int h, int srcStride, uint32_t* dst, int dstStride, int y0, int y1);
struct XbrCtx { const uint32_t* src; uint32_t* dst; int w, h, srcStride, dstStride; };
static void xbrRowsCtx(void* p, int y0, int y1) {
	struct XbrCtx* c = p;
	xbrRows(c->src, c->w, c->h, c->srcStride, c->dst, c->dstStride, y0, y1);
}

/* ------------------------------------------------------------------ */
/* Reparto en varios núcleos: franjas de filas en hilos ayudantes      */
/* ------------------------------------------------------------------ */
#define XBR_MAX_WORKERS 3
typedef void (*RowFn)(void* ctx, int y0, int y1);
static struct PoolJob { RowFn fn; void* ctx; int y0, y1; } g_job[XBR_MAX_WORKERS];
static SDL_Thread* g_thr[XBR_MAX_WORKERS];
static SDL_sem* g_go[XBR_MAX_WORKERS];
static SDL_sem* g_done;
static int g_workers;
static bool g_started;
static SDL_atomic_t g_quit;

static int xbrWorker(void* p) {
	int id = (int) (intptr_t) p;
#ifdef __SWITCH__
	/* Los hilos nuevos nacen en el núcleo del emulador: mover a 1 y 2 */
	int core = 1 + (id % 2);
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
#endif
	for (;;) {
		SDL_SemWait(g_go[id]);
		if (SDL_AtomicGet(&g_quit)) break;
		g_job[id].fn(g_job[id].ctx, g_job[id].y0, g_job[id].y1);
		SDL_SemPost(g_done);
	}
	return 0;
}

static void startWorkers(void) {
	int n = SDL_GetCPUCount() - 1;
#ifdef __SWITCH__
	n = 2;  /* la aplicación tiene los núcleos 0, 1 y 2 */
#endif
	if (n > XBR_MAX_WORKERS) n = XBR_MAX_WORKERS;
	if (n < 0) n = 0;
	g_done = SDL_CreateSemaphore(0);
	if (!g_done) return;
	for (int i = 0; i < n; ++i) {
		g_go[i] = SDL_CreateSemaphore(0);
		g_thr[i] = g_go[i] ? SDL_CreateThread(xbrWorker, "xbr", (void*) (intptr_t) i) : NULL;
		if (!g_thr[i]) break;
		g_workers = i + 1;
	}
	SDL_Log("Filtros: %d n\xC3\xBA" "cleo(s) de ayuda", g_workers);
}

/* Reparte fn(ctx, y0, y1) en franjas iguales entre el hilo principal y los ayudantes */
void xbrParallel(void (*fn)(void* ctx, int y0, int y1), void* ctx, int h) {
	if (!g_started) { g_started = true; startWorkers(); }
	int parts = g_workers + 1;
	for (int i = 0; i < g_workers; ++i) {
		g_job[i] = (struct PoolJob) { fn, ctx, h * (i + 1) / parts, h * (i + 2) / parts };
		SDL_SemPost(g_go[i]);
	}
	fn(ctx, 0, h / parts);
	for (int i = 0; i < g_workers; ++i) SDL_SemWait(g_done);
}

void xbrShutdown(void) {
	SDL_AtomicSet(&g_quit, 1);
	for (int i = 0; i < g_workers; ++i) { SDL_SemPost(g_go[i]); SDL_WaitThread(g_thr[i], NULL); SDL_DestroySemaphore(g_go[i]); }
	if (g_done) SDL_DestroySemaphore(g_done);
	g_workers = 0;
	g_done = NULL;
	g_started = false;
	SDL_AtomicSet(&g_quit, 0);
}

void xbrScale4(const uint32_t* src, int w, int h, int srcStride, uint32_t* dst, int dstStride) {
	if (!g_ready) xbrInit();
	if (w > XBR_MAX_W || h > XBR_MAX_H) return;

	const float wr = Y_WEIGHT * 0.299f / 255.0f, wg = Y_WEIGHT * 0.587f / 255.0f, wb = Y_WEIGHT * 0.114f / 255.0f;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			uint32_t c = src[y * srcStride + x];
			g_luma[y * w + x] = (c & 0xFF) * wr + ((c >> 8) & 0xFF) * wg + ((c >> 16) & 0xFF) * wb;
		}
	}

	struct XbrCtx ctx = { src, dst, w, h, srcStride, dstStride };
	xbrParallel(xbrRowsCtx, &ctx, h);
}

static void xbrRows(const uint32_t* src, int w, int h, int srcStride, uint32_t* dst, int dstStride, int y0, int y1) {
#define CX(v) ((v) < 0 ? 0 : (v) >= w ? w - 1 : (v))
#define CY(v) ((v) < 0 ? 0 : (v) >= h ? h - 1 : (v))
#define L(dx, dy) g_luma[CY(y + (dy)) * w + CX(x + (dx))]
#define P(dx, dy) (src[CY(y + (dy)) * srcStride + CX(x + (dx))] | 0xFF000000u)

	for (int y = y0; y < y1; ++y) {
		for (int x = 0; x < w; ++x) {
			uint32_t E = P(0, 0);
			uint32_t* out = dst + (y * S) * dstStride + x * S;

			/* Atajo: pixel igual a sus 4 vecinos => e == f y e == h en todas las
			 * rotaciones, así que interp_restriction_lv1 es falso: queda E. */
			if (P(1, 0) == E && P(0, 1) == E && P(-1, 0) == E && P(0, -1) == E) {
				for (int sy = 0; sy < S; ++sy) {
					uint32_t* row = out + sy * dstStride;
					row[0] = row[1] = row[2] = row[3] = E;
				}
				continue;
			}

			float A = L(-1, -1), B = L(0, -1), C = L(1, -1);
			float D = L(-1, 0), e = L(0, 0), F = L(1, 0);
			float G = L(-1, 1), H = L(0, 1), I = L(1, 1);
			float A1 = L(-1, -2), B1 = L(0, -2), C1 = L(1, -2);
			float A0 = L(-2, -1), D0 = L(-2, 0), G0 = L(-2, 1);
			float C4 = L(2, -1), F4 = L(2, 0), I4 = L(2, 1);
			float G5 = L(-1, 2), H5 = L(0, 2), I5 = L(1, 2);

			/* Vectores del shader (componentes x,y,z,w) */
			const float b[4] = { B, D, H, F }, c[4] = { C, A, G, I };
			const float d[4] = { D, H, F, B }, f[4] = { F, B, D, H };
			const float g[4] = { G, I, C, A }, hh[4] = { H, F, B, D }, i[4] = { I, C, A, G };
			const float i4[4] = { I4, C1, A0, G5 }, i5[4] = { I5, C4, A1, G0 }, h5[4] = { H5, F4, B1, D0 };
			const float f4[4] = { F4, B1, D0, H5 }, c1[4] = { C1, A0, G5, I4 }, g0[4] = { G0, I5, C4, A1 };
			const float b1[4] = { B1, D0, H5, F4 }, d0[4] = { D0, H5, F4, B1 };

			bool edr[4], edrL[4], edrU[4], edr3L[4], edr3U[4], px[4];
			bool any = false;
			for (int k = 0; k < 4; ++k) {
				/* interp_restriction_lv1, variante CORNER_C (predeterminada) */
				bool lv1 = e != f[k] && e != hh[k] &&
				           ((!eq(f[k], b[k]) && !eq(f[k], c[k])) ||
				            (!eq(hh[k], d[k]) && !eq(hh[k], g[k])) ||
				            (eq(e, i[k]) && ((!eq(f[k], f4[k]) && !eq(f[k], i4[k])) ||
				                             (!eq(hh[k], h5[k]) && !eq(hh[k], i5[k])))) ||
				            eq(e, g[k]) || eq(e, c[k]));
				float wd1 = df(e, c[k]) + df(e, g[k]) + df(i[k], h5[k]) + df(i[k], f4[k]) + 4.0f * df(hh[k], f[k]);
				float wd2 = df(hh[k], d[k]) + df(hh[k], i5[k]) + df(f[k], i4[k]) + df(f[k], b[k]) + 4.0f * df(e, i[k]);
				edr[k] = wd1 < wd2 && lv1;
				edrL[k] = LV2_COEF * df(f[k], g[k]) <= df(hh[k], c[k]) && e != g[k] && d[k] != g[k];
				edrU[k] = df(f[k], g[k]) >= LV2_COEF * df(hh[k], c[k]) && e != c[k] && b[k] != c[k];
				edr3L[k] = eq2(g[k], g0[k]) && !eq2(d0[k], g0[k]);
				edr3U[k] = eq2(c[k], c1[k]) && !eq2(b1[k], c1[k]);
				px[k] = df(e, f[k]) <= df(e, hh[k]);
				any |= edr[k];
			}
			if (!any) {
				for (int sy = 0; sy < S; ++sy) {
					uint32_t* row = out + sy * dstStride;
					row[0] = row[1] = row[2] = row[3] = E;
				}
				continue;
			}

			/* pix por esquina: x: px?F:H  y: px?B:F  z: px?D:B  w: px?H:D */
			uint32_t Fc = P(1, 0), Bc = P(0, -1), Dc = P(-1, 0), Hc = P(0, 1);
			uint32_t pix[4] = { px[0] ? Fc : Hc, px[1] ? Bc : Fc, px[2] ? Dc : Bc, px[3] ? Hc : Dc };

			int combo[4];
			for (int k = 0; k < 4; ++k)
				combo[k] = edr[k] | (edrL[k] << 1) | (edrU[k] << 2) | (edr3L[k] << 3) | (edr3U[k] << 4);
			const uint16_t m0 = NCM[0][combo[0]], m1 = NCM[1][combo[1]], m2 = NCM[2][combo[2]], m3 = NCM[3][combo[3]];
			for (int s = 0; s < S * S; ++s) {
				const uint16_t bit = (uint16_t) (1u << s);
				int k1 = (m0 & bit) ? 0 : (m1 & bit) ? 1 : (m2 & bit) ? 2 : (m3 & bit) ? 3 : -1;
				uint32_t res;
				if (k1 < 0) {
					res = E;
				} else {
					int k2 = (m3 & bit) ? 3 : (m2 & bit) ? 2 : (m1 & bit) ? 1 : 0;
					float r1[3], r2[3];
					mixf(E, pix[k1], MX[k1][combo[k1]][s], r1);
					if (k2 == k1) {
						res = packf(r1);
					} else {
						mixf(E, pix[k2], MX[k2][combo[k2]][s], r2);
						/* mix(res1, res2, step(c_df(E,res1), c_df(E,res2))) */
						res = packf(cdff(E, r2) >= cdff(E, r1) ? r2 : r1);
					}
				}
				out[(s / S) * dstStride + (s % S)] = res;
			}
		}
	}
#undef CX
#undef CY
#undef L
#undef P
}
