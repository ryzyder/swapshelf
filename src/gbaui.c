#include "gbaui.h"
#include "pixfont_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- lienzo ---------------- */
void uiSetWidth(struct Ui* u, int w) {
	if (w < 240) w = 240;
	if (w > UI_MAX_W) w = UI_MAX_W;
	if (u->px && u->w == w) return;
	free(u->px);
	u->px = malloc((size_t) w * UI_H * sizeof(uint32_t));
	u->w = u->px ? w : 0;
}

void uiFree(struct Ui* u) {
	free(u->px); free(u->bg);
	memset(u, 0, sizeof(*u));
}

void uiClear(struct Ui* u, uint32_t c) {
	for (int i = 0; i < u->w * UI_H; ++i) u->px[i] = c;
}

static inline void P(struct Ui* u, int x, int y, uint32_t c) {
	if ((unsigned) x < (unsigned) u->w && (unsigned) y < UI_H) u->px[y * u->w + x] = c;
}

void uiRect(struct Ui* u, int x, int y, int w, int h, uint32_t c) {
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > u->w ? u->w : x + w, y1 = y + h > UI_H ? UI_H : y + h;
	for (int j = y0; j < y1; ++j)
		for (int i = x0; i < x1; ++i) u->px[j * u->w + i] = c;
}

/* Mezcla un color encima (con alfa); si el píxel es transparente queda ese color con alfa */
void uiShade(struct Ui* u, int x, int y, int w, int h, uint32_t rgb, int alpha) {
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > u->w ? u->w : x + w, y1 = y + h > UI_H ? UI_H : y + h;
	uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
	for (int j = y0; j < y1; ++j)
		for (int i = x0; i < x1; ++i) {
			uint32_t p = u->px[j * u->w + i];
			uint32_t pa = p >> 24;
			if (!pa) { u->px[j * u->w + i] = ((uint32_t) alpha << 24) | (rgb & 0xFFFFFF); continue; }
			uint32_t pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
			pr = (pr * (255 - alpha) + r * alpha) / 255;
			pg = (pg * (255 - alpha) + g * alpha) / 255;
			pb = (pb * (255 - alpha) + b * alpha) / 255;
			uint32_t na = pa + (255 - pa) * alpha / 255;
			u->px[j * u->w + i] = (na << 24) | (pr << 16) | (pg << 8) | pb;
		}
}

/* ---------------- texto ---------------- */
static uint32_t nextCp(const char** p) {
	const unsigned char* s = (const unsigned char*) *p;
	uint32_t c = s[0];
	int n = 1;
	if (c >= 0xF0 && s[1] && s[2] && s[3]) { c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4; }
	else if (c >= 0xE0 && s[1] && s[2]) { c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
	else if (c >= 0xC0 && s[1]) { c = ((c & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
	*p += n;
	return c;
}

static int glyph(uint32_t cp) {
	int lo = 0, hi = PIXFONT_COUNT - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (PIXFONT_CP[m] == cp) return m;
		if (PIXFONT_CP[m] < cp) lo = m + 1; else hi = m - 1;
	}
	/* sin glifo: probar sin acento, si no '?' */
	switch (cp) {
	case 0xE0: case 0xE2: case 0xE4: return glyph('a');
	case 0xE8: case 0xEA: case 0xEB: return glyph('e');
	case 0xF2: case 0xF4: case 0xF6: return glyph('o');
	case 0xC7: return glyph('C');
	case 0xE7: return glyph('c');
	}
	return cp == '?' ? 0 : glyph('?');
}

int uiUtf8Len(const char* s) {
	int n = 0;
	while (*s) { nextCp(&s); ++n; }
	return n;
}

int uiTextW(const char* s, int scale) {
	int w = 0, any = 0;
	while (*s) { w += (PIXFONT_W[glyph(nextCp(&s))] + 1) * scale; any = 1; }
	return any ? w - scale : 0;
}

static void drawRun(struct Ui* u, const char* s, int x, int y, uint32_t col, int scale, int maxChars) {
	int n = 0;
	while (*s && (maxChars < 0 || n < maxChars)) {
		int g = glyph(nextCp(&s));
		int gw = PIXFONT_W[g];
		for (int r = 0; r < PIXFONT_ROWS; ++r) {
			uint8_t bits = PIXFONT_BITS[g][r];
			if (!bits) continue;
			for (int k = 0; k < gw; ++k)
				if (bits & (1u << k)) {
					if (scale == 1) P(u, x + k, y + r, col);
					else for (int a = 0; a < scale; ++a) for (int b = 0; b < scale; ++b) P(u, x + k * scale + a, y + r * scale + b, col);
				}
		}
		x += (gw + 1) * scale;
		++n;
	}
}

void uiText(struct Ui* u, const char* s, int x, int y, uint32_t col, uint32_t shadow, int scale, int maxChars) {
	if (shadow) drawRun(u, s, x + 1, y + 1, shadow, scale, maxChars);
	drawRun(u, s, x, y, col, scale, maxChars);
}

void uiTextOutline(struct Ui* u, const char* s, int x, int y, uint32_t col, uint32_t outline) {
	static const int d[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
	for (int i = 0; i < 8; ++i) drawRun(u, s, x + d[i][0], y + d[i][1], outline, 1, -1);
	drawRun(u, s, x, y, col, 1, -1);
}

int uiTextWrap(struct Ui* u, const char* s, int x, int y, int maxW, int lineH, uint32_t col, uint32_t shadow, int maxChars, int maxLines) {
	char line[256];
	int lines = 0, shown = 0;
	const char* p = s;
	while (*p && lines < maxLines) {
		/* tomar palabras mientras quepan */
		const char* best = NULL;
		const char* q = p;
		for (;;) {
			const char* e = q;
			while (*e && *e != ' ' && *e != '\n') ++e;
			size_t len = (size_t) (e - p);
			if (len >= sizeof(line)) len = sizeof(line) - 1;
			memcpy(line, p, len); line[len] = 0;
			if (uiTextW(line, 1) > maxW && best) break;
			best = e;
			if (!*e || *e == '\n') break;
			q = e + 1;
		}
		size_t len = (size_t) (best - p);
		if (len >= sizeof(line)) len = sizeof(line) - 1;
		memcpy(line, p, len); line[len] = 0;
		int n = uiUtf8Len(line);
		int lim = maxChars < 0 ? -1 : (maxChars - shown < 0 ? 0 : maxChars - shown);
		if (u) uiText(u, line, x, y + lines * lineH, col, shadow, 1, lim);
		shown += n;
		++lines;
		p = best;
		if (*p == ' ' || *p == '\n') ++p;
	}
	return lines;
}

/* ---------------- ventanas ---------------- */
void uiWindow(struct Ui* u, int x, int y, int w, int h) {
	const uint32_t edge = 0xFF283040u, frame = 0xFF4860A0u, inner = 0xFF90B0E0u, fill = UI_WHITE;
	uiRect(u, x + 2, y, w - 4, h, edge);
	uiRect(u, x, y + 2, w, h - 4, edge);
	uiRect(u, x + 1, y + 1, w - 2, h - 2, edge);
	uiRect(u, x + 2, y + 1, w - 4, h - 2, frame);
	uiRect(u, x + 1, y + 2, w - 2, h - 4, frame);
	uiRect(u, x + 3, y + 2, w - 6, h - 4, inner);
	uiRect(u, x + 2, y + 3, w - 4, h - 6, inner);
	uiRect(u, x + 4, y + 4, w - 8, h - 8, fill);
}

void uiCursor(struct Ui* u, int x, int y) { uiText(u, "\xE2\x96\xB6", x, y, UI_INK, UI_INK_SH, 1, -1); }

void uiMore(struct Ui* u, int x, int y, float t) {
	int b = ((int) (t * 3.0f)) % 2;
	uiText(u, "\xE2\x96\xBC", x, y + b, UI_RED, 0, 1, -1);
}

void uiBar(struct Ui* u, int x, int y, int w, double p) {
	if (p < 0) p = 0;
	if (p > 1) p = 1;
	uiRect(u, x, y, w, 6, 0xFF404858u);
	uiRect(u, x + 1, y + 1, w - 2, 4, 0xFF506070u);
	int fw = (int) ((w - 2) * p + 0.5);
	uint32_t c = 0xFF70F8A8u, d = 0xFF58D080u;
	uiRect(u, x + 1, y + 1, fw, 2, c);
	uiRect(u, x + 1, y + 3, fw, 2, d);
}






/* ---------------- paisaje ---------------- */
static void buildBackground(struct Ui* u) {
	free(u->bg);
	u->bg = malloc((size_t) u->w * UI_H * sizeof(uint32_t));
	u->bgW = u->bg ? u->w : 0;
	if (!u->bg) return;
	uint32_t* save = u->px;
	u->px = u->bg;
	const int W = u->w;
	static const uint32_t SKY[6] = { 0xFF2860B8u, 0xFF3878C8u, 0xFF5090D8u, 0xFF70A8E0u, 0xFF98C0E8u, 0xFFC0D8F0u };
	const int band = 14;
	for (int i = 0; i < 6; ++i) {
		uiRect(u, 0, i * band, W, i == 5 ? 100 : band + 1, SKY[i]);
		if (i > 0) for (int x = 0; x < W; x += 2) P(u, x + (i % 2), i * band, SKY[i - 1]);
	}
	/* centrar el dibujo en pantallas anchas: x del paisaje relativo a 240 */
	const int off = (W - 240) / 2;
	for (int x = 0; x < W; ++x) {
		float fx = (float) (x - off);
		int h1 = (int) (70 + 10 * sinf(fx * 0.045f) + 6 * sinf(fx * 0.11f + 1));
		for (int y = h1; y < 100; ++y) P(u, x, y, y > h1 + 1 ? 0xFF6080A8u : 0xFF88A0C8u);
	}
	for (int x = 0; x < W; ++x) {
		float fx = (float) (x - off);
		int h2 = (int) (92 + 6 * sinf(fx * 0.03f + 2) + 3 * sinf(fx * 0.09f));
		for (int y = h2; y < 126; ++y) P(u, x, y, y > h2 + 1 ? 0xFF388850u : 0xFF68B868u);
	}
	for (int k = -((off + 10) / 7); ; ++k) {
		int x = off - 4 + 7 * k;
		if (x > W + 8) break;
		uint32_t hsh = (uint32_t) (k + 1000) * 2654435761u;
		int top = 104 + (int) ((hsh >> 16) % 7) - 3;
		for (int j = 0; j < 20; ++j)
			for (int i = -5; i <= 5; ++i)
				if ((i * i) / 25.0f + ((j - 4) * (j - 4)) / 30.0f <= 1.0f || j > 4) {
					uint32_t c = i > 1 ? 0xFF206840u : 0xFF308048u;
					if (j < 2) c = 0xFF50A058u;
					P(u, x + i, top + j, c);
				}
	}
	uiRect(u, 0, 124, W, UI_H - 124, 0xFF409840u);
	u->px = save;
}

static void cloud(struct Ui* u, float cx, int cy) {
	static const int parts[4][3] = { { 0, 0, 6 }, { 7, -3, 7 }, { 15, 0, 6 }, { 7, 2, 7 } };
	for (int k = 0; k < 4; ++k) {
		int r = parts[k][2];
		for (int j = -r; j <= r; ++j)
			for (int i = -r; i <= r; ++i)
				if (i * i + j * j <= r * r && j < 4)
					P(u, (int) cx + parts[k][0] + i, cy + parts[k][1] + j, j < r * 0.3f ? 0xFFF8F8F8u : 0xFFD8E0F0u);
	}
}

void uiLandscape(struct Ui* u, float t, int brightness) {
	if (!u->px) return;
	if (!u->bg || u->bgW != u->w) buildBackground(u);
	if (u->bg) memcpy(u->px, u->bg, (size_t) u->w * UI_H * sizeof(uint32_t));
	const int W = u->w, span = W + 60;
	cloud(u, fmodf(20 + t * 6, (float) span) - 30, 22);
	cloud(u, fmodf(150 + t * 4, (float) span) - 30, 40);
	cloud(u, fmodf(300 + t * 5, (float) span) - 30, 14);
	/* pasto que se mece */
	for (int x = 0; x < W; x += 4) {
		int sway = (int) lroundf(sinf(t * 2 + x * 0.2f));
		static const int rows[3] = { 126, 140, 154 };
		for (int k = 0; k < 3; ++k) {
			int row = rows[k], o = ((x + row) % 8) % 4;
			P(u, x + o + sway, row - 2, 0xFF60B850u);
			P(u, x + o, row - 1, 0xFF60B850u);
			P(u, x + o + 1, row - 1, 0xFF287830u);
			P(u, x + o + 1 + sway, row - 3, 0xFF88D060u);
		}
	}
	if (brightness != 100) {
		for (int i = 0; i < W * UI_H; ++i) {
			uint32_t p = u->px[i];
			uint32_t r = ((p >> 16) & 0xFF) * brightness / 100, g = ((p >> 8) & 0xFF) * brightness / 100, b = (p & 0xFF) * brightness / 100;
			u->px[i] = 0xFF000000u | r << 16 | g << 8 | b;
		}
	}
}

/* ---------------- logo ---------------- */

