/* ================================================================== */
/* Biblioteca multi-ROM (se incluye desde main.c)                       */
/*  - escanea la carpeta de ROMs (GB, GBC, GBA)                         */
/*  - identifica por CRC32 y por el header de cada consola              */
/*  - carátula automática: la pantalla de título del propio juego       */
/*  - skin de tarjetas con el color de la consola y animación de        */
/*    inserción con sonido                                              */
/* ================================================================== */

#define LIB_W 1280
#define LIB_H 720

enum LibSys { SYS_SNES, SYS_GEN, SYS_GBA, SYS_GBC, SYS_GB, SYS_NES, SYS_COUNT };
static const char* const SYS_NAME[SYS_COUNT] = { "SNES", "Genesis", "GBA", "GBC", "GB", "NES" };
static const Uint32 SYS_COLOR[SYS_COUNT] = { 0x8E5CE6, 0x2F78E8, 0xF08A24, 0xE8C021, 0x6FAF4A, 0xC75D55 };
/* Consolas que ya se pueden jugar. NES usa el núcleo FCEUmm integrado. */
/* Carpeta de cada consola dentro de Roms (la carpeta decide la consola) */
static const char* const SYS_DIR[SYS_COUNT] = { "snes", "genesis", "gba", "gbc", "gb", "nes" };
static int sysFromDir(const char* name) {
	static const struct { const char* n; int s; } A[] = {
		{ "snes", SYS_SNES }, { "sfc", SYS_SNES }, { "supernintendo", SYS_SNES }, { "super nintendo", SYS_SNES },
		{ "genesis", SYS_GEN }, { "megadrive", SYS_GEN }, { "mega drive", SYS_GEN }, { "md", SYS_GEN },
		{ "gba", SYS_GBA }, { "gameboyadvance", SYS_GBA }, { "game boy advance", SYS_GBA },
		{ "gbc", SYS_GBC }, { "gameboycolor", SYS_GBC }, { "game boy color", SYS_GBC },
		{ "gb", SYS_GB }, { "gameboy", SYS_GB }, { "game boy", SYS_GB },
		{ "nes", SYS_NES }, { "nintendoentertainmentsystem", SYS_NES }, { "nintendo entertainment system", SYS_NES }, { "famicom", SYS_NES },
	};
	for (size_t i = 0; i < sizeof(A) / sizeof(A[0]); ++i) if (!strcasecmp(name, A[i].n)) return A[i].s;
	return -1;
}
static const char* const SYS_EXTS[SYS_COUNT] = { ".sfc .smc", ".md .gen .smd .bin", ".gba", ".gbc", ".gb", ".nes" };

struct LibGame {
	char path[1024];
	char name[128];      /* nombre para mostrar */
	char official[160];  /* nombre de la base de datos, si se conoce */
	char note[96];       /* "Hack o versión modificada de ...", etc. */
	char code[8];        /* código del header (GBA) */
	char htitle[20];     /* título del header */
	uint32_t crc;
	int sys;
	long long size, mtime;
	SDL_Texture* card;   /* tarjeta compuesta (etiqueta + banda) */
	SDL_Surface* label;  /* carátula cargada (hilo principal) */
	bool labelChecked;
	bool labelPending;   /* pedida al hilo de carga */
	Uint32 labelAskedAt; /* para reintentar si todavía no había portada */
	Uint32 usedAt;       /* último cuadro en que se dibujó (para liberar memoria) */
	int metaRev;         /* última versión de metadatos aplicada */
};

static void runControls(struct App* a, int sys, uint32_t crc, const char* gameName, SDL_Texture* backdrop);
static bool confirmDialog(struct App* a, const char* title, const char* body, const char* yes, SDL_Texture* backdrop);
static void drawDialogBox(struct App* a, const char* title, const char* body, const char* yes, int focus);
static struct LibGame* g_lib;
static int g_libN, g_libCap;

/* ---------------- tema de los menús: oscuro o claro, con color de acento ---------------- */
struct Theme {
	Uint32 bg, panel, panel2, panel3;  /* fondo y paneles */
	Uint32 text, text2, dim, dim2, faint;  /* textos, de más a menos visibles */
	Uint32 acc, acct;  /* acento (relleno) y acento para textos sobre fondo */
};
static struct Theme g_th;
#define TH g_th

enum { ACC_CYAN, ACC_RED, ACC_GREEN, ACC_VIOLET, ACC_ORANGE, ACC_COUNT };
static const Uint32 ACC_RGB[ACC_COUNT] = { 0x00C3E3, 0xFF4554, 0x40D28C, 0xA77BF0, 0xF08A24 };
static const char* const ACC_NAME[ACC_COUNT] = { "Cian", "Rojo", "Verde", "Violeta", "Naranja" };
static const char* const ACC_KEY[ACC_COUNT] = { "cian", "rojo", "verde", "violeta", "naranja" };

static Uint32 mixRGB(Uint32 a, Uint32 b, float t) {
	int r = (int) (((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
	int g = (int) (((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
	int bl = (int) ((a & 255) * (1 - t) + (b & 255) * t);
	return (Uint32) (r << 16 | g << 8 | bl);
}

static void themeApply(bool light, int accent) {
	Uint32 acc = ACC_RGB[accent >= 0 && accent < ACC_COUNT ? accent : ACC_CYAN];
	if (!light) {
		g_th = (struct Theme) { 0x0E1118, 0x181D28, 0x141821, 0x252B38, 0xE8ECF4, 0xC8CEDA, 0x8C96AA, 0x6E7890, 0x4A5264, acc, mixRGB(acc, 0xFFFFFF, 0.35f) };
	} else {
		/* claro: textos oscuros; el acento como texto se oscurece para que se lea sobre blanco */
		g_th = (struct Theme) { 0xE9EDF3, 0xFFFFFF, 0xF4F6FA, 0xDCE2EB, 0x161B26, 0x343D4F, 0x5A6377, 0x737C8F, 0xA9B1BF, acc, mixRGB(acc, 0x000000, 0.42f) };
	}
}

static void themeClearColor(struct App* a) {
	SDL_SetRenderDrawColor(a->renderer, (TH.bg >> 16) & 255, (TH.bg >> 8) & 255, TH.bg & 255, 255);
}

/* ---------------- fuentes (texto en alta resolución) ---------------- */
#include "font_data.h"
#include "logo_font.h"
static const struct BitmapFont* FONTS[3] = { &FONT_TITLE, &FONT_ITEM, &FONT_SMALL };
enum { F_TITLE = 0, F_ITEM, F_SMALL };
static SDL_Texture* g_fontTex[3];

static SDL_Texture* makeFontTexture(SDL_Renderer* r, const struct BitmapFont* f) {
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, f->atlasW, f->height, 32, SDL_PIXELFORMAT_RGBA32);
	if (!s) return NULL;
	for (int y = 0; y < f->height; ++y) {
		uint8_t* row = (uint8_t*) s->pixels + y * s->pitch;
		for (int x = 0; x < f->atlasW; ++x) {
			row[x * 4 + 0] = 255; row[x * 4 + 1] = 255; row[x * 4 + 2] = 255;
			row[x * 4 + 3] = f->atlas[y * f->atlasW + x];
		}
	}
	SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
	return t;
}

static uint32_t fUtf8(const char** p) {
	const unsigned char* s = (const unsigned char*) *p;
	uint32_t c = s[0];
	int n = 1;
	if (c >= 0xF0 && s[1] && s[2] && s[3]) { c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4; }
	else if (c >= 0xE0 && s[1] && s[2]) { c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
	else if (c >= 0xC0 && s[1]) { c = ((c & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
	*p += n;
	return c;
}

static int fGlyph(const struct BitmapFont* f, uint32_t cp) {
	for (int i = 0; i < f->count; ++i) if (f->codepoints[i] == cp) return i;
	for (int i = 0; i < f->count; ++i) if (f->codepoints[i] == '?') return i;
	return 0;
}

/* scale: tamaño relativo (1.0 = tamaño de la fuente) */
static int textW(int font, const char* s, float scale) {
	const struct BitmapFont* f = FONTS[font];
	int w = 0;
	while (*s) w += f->advance[fGlyph(f, fUtf8(&s))];
	return (int) (w * scale);
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };
static void text(struct App* a, int font, float scale, const char* s, int x, int y, Uint32 rgb, Uint8 alpha, int align) {
	const struct BitmapFont* f = FONTS[font];
	SDL_Texture* t = g_fontTex[font];
	if (!t) return;
	if (align == AL_CENTER) x -= textW(font, s, scale) / 2;
	else if (align == AL_RIGHT) x -= textW(font, s, scale);
	SDL_SetTextureColorMod(t, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
	SDL_SetTextureAlphaMod(t, alpha);
	float fx = (float) x;
	while (*s) {
		int g = fGlyph(f, fUtf8(&s));
		SDL_Rect src = { f->x[g], 0, f->w[g], f->height };
		SDL_FRect dst = { fx, (float) y, f->w[g] * scale, f->height * scale };
		SDL_RenderCopyF(a->renderer, t, &src, &dst);
		fx += f->advance[g] * scale;
	}
}

/* Texto recortado con "…" si no cabe */
static void textFit(struct App* a, int font, float scale, const char* s, int x, int y, int maxW, Uint32 rgb) {
	if (textW(font, s, scale) <= maxW) { text(a, font, scale, s, x, y, rgb, 255, AL_LEFT); return; }
	char buf[256];
	size_t n = strlen(s);
	if (n >= sizeof(buf)) n = sizeof(buf) - 4;
	memcpy(buf, s, n); buf[n] = 0;
	while (n > 0) {
		do { --n; } while (n > 0 && ((unsigned char) buf[n] & 0xC0) == 0x80);
		memcpy(buf + n, "...", 4);
		if (textW(font, buf, scale) <= maxW) break;
	}
	text(a, font, scale, buf, x, y, rgb, 255, AL_LEFT);
}

/* ---------------- dibujo ---------------- */
/* Rectángulo redondeado con triángulos (SDL_RenderGeometry): una sola pieza, así
 * no aparecen líneas en las uniones al escalar la pantalla ni con transparencia. */
static void fillRR(struct App* a, float x, float y, float w, float h, float r, Uint32 rgb, Uint8 alpha) {
	if (w <= 0 || h <= 0) return;
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if (r < 0.5f) {
		SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->renderer, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
		SDL_FRect q = { x, y, w, h };
		SDL_RenderFillRectF(a->renderer, &q);
		return;
	}
	int seg = (int) (r / 2.5f);
	if (seg < 3) seg = 3;
	if (seg > 14) seg = 14;
	SDL_Vertex v[1 + 4 * 15];
	int idx[3 * 4 * 15];
	SDL_Color c = { (Uint8) ((rgb >> 16) & 0xFF), (Uint8) ((rgb >> 8) & 0xFF), (Uint8) (rgb & 0xFF), alpha };
	int n = 0;
	v[n++] = (SDL_Vertex) { { x + w / 2, y + h / 2 }, c, { 0, 0 } };
	const float cxs[4] = { x + w - r, x + w - r, x + r, x + r }, cys[4] = { y + r, y + h - r, y + h - r, y + r };
	for (int k = 0; k < 4; ++k) {
		float a0 = (-90.0f + 90.0f * k) * 0.0174533f;
		for (int i = 0; i <= seg; ++i) {
			float t = a0 + (float) i / seg * 1.5707963f;
			v[n++] = (SDL_Vertex) { { cxs[k] + cosf(t) * r, cys[k] + sinf(t) * r }, c, { 0, 0 } };
		}
	}
	int m = 0;
	for (int i = 1; i < n; ++i) {
		idx[m++] = 0;
		idx[m++] = i;
		idx[m++] = i + 1 < n ? i + 1 : 1;
	}
	SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(a->renderer, NULL, v, n, idx, m);
}

static void fillRect2(struct App* a, float x, float y, float w, float h, Uint32 rgb, Uint8 alpha) {
	SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->renderer, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
	SDL_FRect r = { x, y, w, h };
	SDL_RenderFillRectF(a->renderer, &r);
}

/* ---------------- identificación ---------------- */
/* El nombre oficial y la detección de hacks salen de la base No-Intro (scraper.inc) */

static bool hasExt(const char* name, const char* ext) {
	size_t n = strlen(name), e = strlen(ext);
	if (n < e) return false;
	for (size_t i = 0; i < e; ++i) if (tolower((unsigned char) name[n - e + i]) != ext[i]) return false;
	return true;
}

/* Nombre legible a partir del archivo: sin extensión ni etiquetas entre paréntesis */
static void prettyName(char* out, size_t n, const char* path) {
	const char* b = strrchr(path, '/');
	const char* b2 = strrchr(path, '\\');
	if (b2 > b) b = b2;
	b = b ? b + 1 : path;
	snprintf(out, n, "%s", b);
	char* dot = strrchr(out, '.');
	if (dot) *dot = 0;
	char* p = strchr(out, '(');
	char* q = strchr(out, '[');
	if (q && (!p || q < p)) p = q;
	if (p && p > out) *p = 0;
	size_t l = strlen(out);
	while (l && (out[l - 1] == ' ' || out[l - 1] == '_')) out[--l] = 0;
	for (char* c = out; *c; ++c) if (*c == '_') *c = ' ';
}

static void copyHeaderText(char* out, const uint8_t* p, int n) {
	int j = 0;
	for (int i = 0; i < n && p[i]; ++i) out[j++] = (p[i] >= 32 && p[i] < 127) ? (char) p[i] : ' ';
	out[j] = 0;
	while (j && out[j - 1] == ' ') out[--j] = 0;
}

/* Lee la ROM, calcula CRC32 y reconoce la consola por el contenido (no solo la extensión) */
static bool identifyRom(struct LibGame* g, int forced) {
	FILE* f = fopen(g->path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0x200 || n > 64 * 1024 * 1024) { fclose(f); return false; }
	uint8_t* d = malloc((size_t) n);
	if (!d) { fclose(f); return false; }
	size_t rd = fread(d, 1, (size_t) n, f);
	fclose(f);
	if (rd != (size_t) n) { free(d); return false; }
	g->code[0] = g->htitle[0] = 0;
	bool isSnes = forced == SYS_SNES || (forced < 0 && (hasExt(g->path, ".sfc") || hasExt(g->path, ".smc")));
	bool isGen = forced == SYS_GEN || (forced < 0 && (hasExt(g->path, ".md") || hasExt(g->path, ".gen") || hasExt(g->path, ".smd") || hasExt(g->path, ".bin")));
	bool isNes = forced == SYS_NES || (forced < 0 && hasExt(g->path, ".nes"));
	if (isGen && hasExt(g->path, ".smd") && n > 512 && (n - 512) % 16384 == 0) {
		/* .smd: 512 bytes de cabecera y bloques entrelazados de 16 KB -> formato normal */
		long m = n - 512;
		uint8_t* o = malloc((size_t) m);
		if (o) {
			for (long blk = 0; blk < m; blk += 16384)
				for (int i = 0; i < 8192; ++i) { o[blk + 2 * i + 1] = d[512 + blk + i]; o[blk + 2 * i] = d[512 + blk + 8192 + i]; }
			free(d); d = o; n = m;
		}
	}
	if (isSnes) {
		/* cabecera de copiadora de 512 bytes: se quita antes del CRC (como en No-Intro) */
		long off = (n % 1024 == 512) ? 512 : 0;
		g->crc = swsCrc32(d + off, (size_t) (n - off));
		/* header interno: LoROM 0x7FC0 o HiROM 0xFFC0, el que tenga checksum y complemento válidos */
		long hdr = -1;
		for (int k = 0; k < 2 && hdr < 0; ++k) {
			long h = off + (k == 0 ? 0x7FC0 : 0xFFC0);
			if (h + 0x20 > n) continue;
			unsigned comp = d[h + 0x1C] | d[h + 0x1D] << 8, sum = d[h + 0x1E] | d[h + 0x1F] << 8;
			if ((comp ^ sum) == 0xFFFF) hdr = h;
		}
		if (hdr < 0 && off + 0x7FC0 + 0x20 <= n) hdr = off + 0x7FC0;
		if (hdr >= 0) copyHeaderText(g->htitle, d + hdr, 19);
		g->sys = SYS_SNES;
		free(d);
		prettyName(g->name, sizeof(g->name), g->path);
		g->official[0] = g->note[0] = 0;
		return true;
	}
	if (isNes) {
		/* iNES/NES 2.0: el DAT oficial contempla copias con y sin cabecera; el
		 * CRC se calcula sobre el archivo tal como lo abrió el núcleo. */
		if (n < 16 || memcmp(d, "NES\x1A", 4)) { free(d); return false; }
		g->crc = swsCrc32(d, (size_t) n);
		g->sys = SYS_NES;
		free(d);
		prettyName(g->name, sizeof(g->name), g->path);
		g->official[0] = g->note[0] = 0;
		return true;
	}
	g->crc = swsCrc32(d, (size_t) n);
	if (isGen || (forced < 0 && n > 0x200 && (!memcmp(d + 0x100, "SEGA", 4) || !memcmp(d + 0x101, "SEGA", 4)))) {
		if (n > 0x190 && (!memcmp(d + 0x100, "SEGA", 4) || !memcmp(d + 0x101, "SEGA", 4))) {
			copyHeaderText(g->htitle, d + 0x150, 19);  /* nombre internacional (recortado) */
			copyHeaderText(g->code, d + 0x183, 7);       /* número de serie, ej. 00001009 */
		} else if (!isGen) { free(d); return false; }
		g->sys = SYS_GEN;
		free(d);
		prettyName(g->name, sizeof(g->name), g->path);
		g->official[0] = g->note[0] = 0;
		return true;
	}
	static const uint8_t GB_LOGO[4] = { 0xCE, 0xED, 0x66, 0x66 };
	if (n >= 0xC0 && d[0xB2] == 0x96 && !hasExt(g->path, ".gb") && !hasExt(g->path, ".gbc")) {
		g->sys = SYS_GBA;                           /* byte fijo 0x96 del header de GBA */
		copyHeaderText(g->htitle, d + 0xA0, 12);
		copyHeaderText(g->code, d + 0xAC, 4);
	} else if (!memcmp(d + 0x104, GB_LOGO, 4)) {
		g->sys = (d[0x143] & 0x80) ? SYS_GBC : SYS_GB;  /* bandera de color */
		copyHeaderText(g->htitle, d + 0x134, (d[0x143] & 0x80) ? 11 : 16);
	} else if (hasExt(g->path, ".gba")) {
		g->sys = SYS_GBA;
	} else {
		free(d);
		return false;
	}
	/* la carpeta manda (ej. un juego de GB puesto en Roms/gbc) */
	if (forced == SYS_GBA || forced == SYS_GBC || forced == SYS_GB) g->sys = forced;
	free(d);
	prettyName(g->name, sizeof(g->name), g->path);
	g->official[0] = g->note[0] = 0;
	return true;
}

/* ---------------- caché de la biblioteca ---------------- */
static void libCachePath(char* out, size_t n) { pathJoin(out, n, "biblioteca.txt"); }

struct CacheEntry { char path[1024]; long long size, mtime; uint32_t crc; int sys; char code[8], htitle[20], name[128], official[160], note[96]; };
static struct CacheEntry* g_cache;
static int g_cacheN;

static void libCacheLoad(void) {
	char p[1024];
	libCachePath(p, sizeof(p));
	FILE* f = fopen(p, "r");
	if (!f) return;
	char line[2048];
	if (!fgets(line, sizeof(line), f) || strncmp(line, "#biblioteca v2", 14) != 0) { fclose(f); return; }  /* caché de otra versión */
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* fld[10] = { 0 };
		int k = 0;
		char* s = line;
		fld[k++] = s;
		while (*s && k < 10) { if (*s == '\t') { *s = 0; fld[k++] = s + 1; } ++s; }
		if (k < 10) continue;
		struct CacheEntry* e = realloc(g_cache, sizeof(*g_cache) * (g_cacheN + 1));
		if (!e) break;
		g_cache = e;
		e = &g_cache[g_cacheN++];
		snprintf(e->path, sizeof(e->path), "%s", fld[0]);
		e->size = atoll(fld[1]); e->mtime = atoll(fld[2]); e->crc = (uint32_t) strtoul(fld[3], NULL, 16); e->sys = atoi(fld[4]);
		snprintf(e->code, sizeof(e->code), "%s", fld[5]); snprintf(e->htitle, sizeof(e->htitle), "%s", fld[6]);
		snprintf(e->name, sizeof(e->name), "%s", fld[7]); snprintf(e->official, sizeof(e->official), "%s", fld[8]);
		snprintf(e->note, sizeof(e->note), "%s", fld[9]);
	}
	fclose(f);
}

static void libCacheSave(void) {
	char p[1024];
	libCachePath(p, sizeof(p));
	FILE* f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "#biblioteca v2\n");
	for (int i = 0; i < g_libN; ++i) {
		struct LibGame* g = &g_lib[i];
		fprintf(f, "%s\t%lld\t%lld\t%08X\t%d\t%s\t%s\t%s\t%s\t%s\n", g->path, g->size, g->mtime, (unsigned) g->crc, g->sys,
		        g->code, g->htitle, g->name, g->official, g->note);
	}
	fclose(f);
}

/* Pantalla de progreso mientras se revisan juegos nuevos (la primera vez con
 * muchos juegos se lee cada archivo entero para sacar su CRC32). */
static int drawBrand(struct App* a, int font, float sc, int x, int y, Uint32 ink, Uint8 alpha, int align);
static struct App* g_scanApp;
static SDL_threadID g_mainThreadId;
static int g_scanNew, g_scanSeen;
static Uint32 g_scanLast;

static char g_scanFile[256];  /* archivo que se está leyendo (solo para mostrar) */

static void scanDraw(struct App* a, const char* path);

static void scanProgress(const char* path) {
	const char* file = strrchr(path, '/');
	snprintf(g_scanFile, sizeof(g_scanFile), "%s", file ? file + 1 : path);
	struct App* a = g_scanApp;  /* NULL si el escaneo corre en segundo plano */
	if (!a || !a->renderer) return;
	Uint32 now = SDL_GetTicks();
	if (now - g_scanLast < 120) return;
	g_scanLast = now;
#ifdef __SWITCH__
	appletMainLoop();
#endif
	SDL_PumpEvents();
	scanDraw(a, g_scanFile);
}

static void scanDraw(struct App* a, const char* path) {
	Uint32 now = SDL_GetTicks();
	SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
	themeClearColor(a);
	SDL_RenderClear(a->renderer);
	drawBrand(a, F_TITLE, 0.8f, LIB_W / 2, 230, TH.text, 255, AL_CENTER);
	text(a, F_ITEM, 0.6f, g_scanNew ? T("Revisando juegos nuevos") : T("Cargando tu biblioteca"), LIB_W / 2, 340, TH.text, 255, AL_CENTER);
	char msg[160];
	snprintf(msg, sizeof(msg), T("%d juego%s nuevo%s identificado%s  \xC2\xB7  %d en total"), g_scanNew, g_scanNew == 1 ? "" : "s",
	         g_scanNew == 1 ? "" : "s", g_scanNew == 1 ? "" : "s", g_scanSeen);
	text(a, F_SMALL, 0.6f, msg, LIB_W / 2, 392, TH.dim, 255, AL_CENTER);
	const char* file = strrchr(path, '/');
	file = file ? file + 1 : path;
	int fw = textW(F_SMALL, file, 0.5f);
	if (fw > LIB_W - 200) fw = LIB_W - 200;
	textFit(a, F_SMALL, 0.5f, file, LIB_W / 2 - fw / 2, 426, LIB_W - 200, TH.dim2);
	/* barra sin final conocido: un tramo que va y viene */
	const float bx = 390, bw = LIB_W - 780;
	fillRR(a, bx, 480, bw, 8, 4, TH.panel, 255);
	float ph = (now % 1600) / 1600.0f, k = ph < 0.5f ? ph * 2 : 2 - ph * 2;
	fillRR(a, bx + k * (bw - 160), 480, 160, 8, 4, TH.acc, 255);
	if (g_scanNew) text(a, F_SMALL, 0.5f, T("Solo pasa la primera vez: despu\xC3\xA9s se usa lo guardado en biblioteca.txt"), LIB_W / 2, 520, TH.dim2, 255, AL_CENTER);
#ifdef SWS_TEST
	{ static bool shot = false; if (!shot && g_scanNew >= 10) { shot = true; testCapture(a, "escaneo"); } }
#endif
	SDL_RenderPresent(a->renderer);
}

static void libAdd(const char* path, long long size, long long mtime, int forced) {
	if (g_libN == g_libCap) {
		int nc = g_libCap ? g_libCap * 2 : 64;
		struct LibGame* ng = realloc(g_lib, sizeof(*g_lib) * nc);
		if (!ng) return;
		g_lib = ng;
		g_libCap = nc;
	}
	struct LibGame* g = &g_lib[g_libN];
	memset(g, 0, sizeof(*g));
	snprintf(g->path, sizeof(g->path), "%s", path);
	g->size = size; g->mtime = mtime;
	for (int i = 0; i < g_cacheN; ++i) {
		struct CacheEntry* e = &g_cache[i];
		if (e->size == size && e->mtime == mtime && !strcmp(e->path, path)) {
			g->crc = e->crc; g->sys = e->sys;
			snprintf(g->code, sizeof(g->code), "%s", e->code); snprintf(g->htitle, sizeof(g->htitle), "%s", e->htitle);
			snprintf(g->name, sizeof(g->name), "%s", e->name); snprintf(g->official, sizeof(g->official), "%s", e->official);
			snprintf(g->note, sizeof(g->note), "%s", e->note);
			++g_libN;
			++g_scanSeen;
			return;
		}
	}
	scanProgress(path);
	if (identifyRom(g, forced)) {
		++g_scanNew;
		++g_scanSeen;
		SDL_Log("Biblioteca: %s -> %s %08X %s %s", path, SYS_NAME[g->sys], (unsigned) g->crc, g->code, g->name);
		++g_libN;
	}
}

static void libScanDir(const char* dir, int depth, int forced) {
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[1024];
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 3) {
				int fs = forced >= 0 ? forced : sysFromDir(e->d_name);  /* carpeta de consola */
				libScanDir(p, depth + 1, fs);
			}
			continue;
		}
		if (hasExt(e->d_name, ".gba") || hasExt(e->d_name, ".gbc") || hasExt(e->d_name, ".gb") || hasExt(e->d_name, ".sfc") ||
		    hasExt(e->d_name, ".smc") || hasExt(e->d_name, ".md") || hasExt(e->d_name, ".gen") || hasExt(e->d_name, ".smd") ||
		    hasExt(e->d_name, ".bin") || hasExt(e->d_name, ".nes"))
			libAdd(p, (long long) st.st_size, (long long) st.st_mtime, forced);
	}
	closedir(d);
}

static int libCompare(const void* x, const void* y) {
	const struct LibGame* a = x;
	const struct LibGame* b = y;
	return strcasecmp(a->name, b->name);
}

static void libScan(struct App* a) {
	for (int i = 0; i < g_libN; ++i) {
		if (g_lib[i].card) SDL_DestroyTexture(g_lib[i].card);
		if (g_lib[i].label) SDL_FreeSurface(g_lib[i].label);
	}
	g_libN = 0;
	free(g_cache); g_cache = NULL; g_cacheN = 0;
	libCacheLoad();
	char dir[1024];
	resolvePath(dir, sizeof(dir), a->cfg.romDir);
	MKDIR(dir);
	/* carpetas por consola: se crean solas si no existen */
	for (int k = 0; k < SYS_COUNT; ++k) {
		char sub[1100];
		snprintf(sub, sizeof(sub), "%s/%s", dir, SYS_DIR[k]);
		DIR* chk = opendir(sub);
		if (chk) closedir(chk);
		else {
			/* no crear "gb" si ya existe otra variante, p. ej. "GameBoy" */
			MKDIR(sub);
		}
	}
	g_scanApp = SDL_ThreadID() == g_mainThreadId ? a : NULL;
	g_scanNew = g_scanSeen = 0;
	g_scanLast = SDL_GetTicks();
	libScanDir(dir, 0, -1);
	g_scanApp = NULL;
	qsort(g_lib, (size_t) g_libN, sizeof(*g_lib), libCompare);
	libCacheSave();
	SDL_Log("Biblioteca: %d juego(s) en %s", g_libN, dir);
}

/* ---------------- carátulas automáticas ---------------- */
/* Se ejecuta el juego unos segundos sin mostrarlo y se guarda el cuadro más "vivo"
 * (normalmente la pantalla de título). Los archivos van a Caratulas/<CRC>.bmp;
 * una imagen propia con ese nombre tiene prioridad. */
static void coverPath(char* out, size_t n, uint32_t crc) {
	char rel[64];
	snprintf(rel, sizeof(rel), "Caratulas/%08X.bmp", (unsigned) crc);
	pathJoin(out, n, rel);
}

static double frameScore(const color_t* fb, int w, int h, int stride) {
	double sum = 0, sum2 = 0, sat = 0;
	int lit = 0, cnt = 0;
	for (int y = 0; y < h; y += 2)
		for (int x = 0; x < w; x += 2) {
			uint32_t c = (uint32_t) fb[y * stride + x];
			int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
			int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
			int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
			double l = 0.3 * r + 0.59 * g + 0.11 * b;
			sum += l; sum2 += l * l; sat += mx - mn;
			if (l > 24) ++lit;
			++cnt;
		}
	double mean = sum / cnt, var = sum2 / cnt - mean * mean;
	return sqrt(var > 0 ? var : 0) + sat / cnt * 0.8 + (double) lit / cnt * 30.0;
}

static SDL_atomic_t g_coverBusy;

struct CoverJob { char path[1024]; char out[1024]; };

static bool makeCover(const char* romPath, const char* outPath) {
	struct mCore* core = mCoreFind(romPath);
	if (!core || !core->init(core)) return false;
	mCoreInitConfig(core, NULL);
	mCoreConfigSetDefaultIntValue(&core->config, "useBios", 0);
	mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 1);
	mCoreConfigSetDefaultIntValue(&core->config, "sgb.borders", 0);
	mCoreLoadForeignConfig(core, &core->config);
	color_t* fb = calloc(256 * 224, sizeof(color_t));
	color_t* best = calloc(256 * 224, sizeof(color_t));
	core->setVideoBuffer(core, fb, 256);
	struct VFile* vf = VFileOpen(romPath, O_RDONLY);
	if (!fb || !best || !vf || !core->loadROM(core, vf)) {
		if (vf) vf->close(vf);
		free(fb); free(best);
		mCoreConfigDeinit(&core->config);
		core->deinit(core);
		return false;
	}
	core->reset(core);
	unsigned w = 240, h = 160;
	core->desiredVideoDimensions(core, &w, &h);
	if (w > 256) w = 256;
	if (h > 224) h = 224;
	double bestScore = -1;
	for (int f = 1; f <= 4800; ++f) {
		core->setKeys(core, 0);
		core->runFrame(core);
		if (f % 300 == 0 && f >= 600) {
			double s = frameScore(fb, (int) w, (int) h, 256);
			if (s > bestScore) { bestScore = s; memcpy(best, fb, 256 * 224 * sizeof(color_t)); }
		}
	}
	bool ok = false;
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, (int) w, (int) h, 24, SDL_PIXELFORMAT_RGB24);
	if (s) {
		for (unsigned y = 0; y < h; ++y) {
			uint8_t* row = (uint8_t*) s->pixels + y * s->pitch;
			for (unsigned x = 0; x < w; ++x) {
				uint32_t c = (uint32_t) best[y * 256 + x];
				row[x * 3 + 0] = c & 0xFF; row[x * 3 + 1] = (c >> 8) & 0xFF; row[x * 3 + 2] = (c >> 16) & 0xFF;
			}
		}
		ok = SDL_SaveBMP(s, outPath) == 0;
		SDL_FreeSurface(s);
	}
	free(fb); free(best);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return ok;
}

static int coverThread(void* p) {
	struct CoverJob* jobs = p;
	for (int i = 0; jobs[i].path[0]; ++i) {
		if (SDL_AtomicGet(&g_coverBusy) < 0) break;  /* cancelado al salir */
		bool ok = makeCover(jobs[i].path, jobs[i].out);
		SDL_Log("Car\xC3\xA1tula %s: %s", ok ? "creada" : "fall\xC3\xB3", jobs[i].out);
	}
	free(jobs);
	SDL_AtomicSet(&g_coverBusy, 0);
	return 0;
}

static SDL_Thread* g_coverThr;
static bool g_preferBox = true;

static void startCovers(void) {
	char d[1024];
	pathJoin(d, sizeof(d), "Caratulas");
	MKDIR(d);
	struct CoverJob* jobs = calloc((size_t) g_libN + 1, sizeof(*jobs));
	if (!jobs) return;
	int n = 0;
	for (int i = 0; i < g_libN; ++i) {
		char out[1024];
		coverPath(out, sizeof(out), g_lib[i].crc);
		/* La carátula automática es una captura de mGBA; NES obtiene sus
		 * carátulas desde los metadatos, no se debe enviar al núcleo GBA. */
		if (fileExists(out) || (g_lib[i].sys != SYS_GBA && g_lib[i].sys != SYS_GBC && g_lib[i].sys != SYS_GB)) continue;
		char box[1024];
		snprintf(box, sizeof(box), "%s", out);
		{ char* dot = strrchr(box, '.'); if (dot) snprintf(dot, sizeof(box) - (size_t) (dot - box), "_caja.bmp"); }
		if (g_preferBox && fileExists(box)) continue;  /* ya tiene portada de caja */
		snprintf(jobs[n].path, sizeof(jobs[n].path), "%s", g_lib[i].path);
		snprintf(jobs[n].out, sizeof(jobs[n].out), "%s", out);
		++n;
	}
	if (!n) { free(jobs); return; }
	SDL_AtomicSet(&g_coverBusy, 1);
	g_coverThr = SDL_CreateThread(coverThread, "caratulas", jobs);
	if (!g_coverThr) { free(jobs); SDL_AtomicSet(&g_coverBusy, 0); }
	else SDL_Log("Car\xC3\xA1tulas: generando %d en segundo plano", n);
}

static void stopCovers(void) {
	if (!g_coverThr) return;
	SDL_AtomicSet(&g_coverBusy, -1);
	SDL_WaitThread(g_coverThr, NULL);
	g_coverThr = NULL;
}

#include "scraper.inc"

/* ---------------- tarjeta de juego ---------------- */
#define CARD_W 150
#define CARD_H 225  /* proporción de un cartucho de Switch (2:3) */

/* ---- etiqueta según la consola (opción A) ----
 * Todo en píxeles de la etiqueta a 2x (276 x 326). La portada entra entera en su hueco
 * (con fondo desenfocado de la propia imagen si la forma no coincide); los textos
 * (título, "16-BIT") se dibujan encima en drawCard. */
#define LAB_W (CARD_W * 2 - 24)
#define LAB_H (CARD_H * 2 - 44 - 24)


static void putPx(uint8_t* buf, int x, int y, uint32_t c) {
	if (x < 0 || y < 0 || x >= LAB_W || y >= LAB_H) return;
	uint8_t* p = buf + ((size_t) y * LAB_W + x) * 3;
	p[0] = (uint8_t) (c >> 16); p[1] = (uint8_t) (c >> 8); p[2] = (uint8_t) c;
}

/* Portada dentro de (ax,ay,aw,ah) del buffer */
static void drawArt(uint8_t* buf, SDL_Surface* lab, int ax, int ay, int aw, int ah) {
	const uint8_t* src = lab->pixels;
	int sw = lab->w, sh = lab->h;
	uint8_t* tmp = malloc((size_t) sw * sh * 3);
	if (!tmp) return;
	for (int y = 0; y < sh; ++y) memcpy(tmp + (size_t) y * sw * 3, src + (size_t) y * lab->pitch, (size_t) sw * 3);
	float ar = (float) sw / sh, lr = (float) aw / ah, diff = ar > lr ? ar / lr : lr / ar;
	uint8_t* dst = buf + ((size_t) ay * LAB_W + ax) * 3;
	float cw = ar > lr ? sh * lr : (float) sw, ch = ar > lr ? (float) sh : sw / lr;
	if (diff < 1.08f) {
		areaResize(tmp, sw, sh, (sw - cw) / 2, (sh - ch) / 2, cw, ch, dst, aw, ah, LAB_W * 3);
	} else {
		enum { BW = 12, BH = 12 };
		uint8_t small[BW * BH * 3];
		areaResize(tmp, sw, sh, (sw - cw) / 2, (sh - ch) / 2, cw, ch, small, BW, BH, BW * 3);
		for (int y = 0; y < ah; ++y) {
			float fy = (y + 0.5f) * BH / ah - 0.5f;
			int y0 = (int) floorf(fy), y1 = y0 + 1;
			float ty = fy - y0;
			if (y0 < 0) y0 = 0;
			if (y1 > BH - 1) y1 = BH - 1;
			for (int x = 0; x < aw; ++x) {
				float fx = (x + 0.5f) * BW / aw - 0.5f;
				int x0 = (int) floorf(fx), x1 = x0 + 1;
				float tx = fx - x0;
				if (x0 < 0) x0 = 0;
				if (x1 > BW - 1) x1 = BW - 1;
				for (int c = 0; c < 3; ++c) {
					float v = (small[(y0 * BW + x0) * 3 + c] * (1 - tx) + small[(y0 * BW + x1) * 3 + c] * tx) * (1 - ty)
					        + (small[(y1 * BW + x0) * 3 + c] * (1 - tx) + small[(y1 * BW + x1) * 3 + c] * tx) * ty;
					dst[((size_t) y * LAB_W + x) * 3 + c] = (uint8_t) (v * 0.4f);
				}
			}
		}
		float k = ar > lr ? (float) aw / sw : (float) ah / sh;
		int fw = (int) (sw * k + 0.5f), fh = (int) (sh * k + 0.5f);
		if (fw > aw) fw = aw;
		if (fh > ah) fh = ah;
		areaResize(tmp, sw, sh, 0, 0, (float) sw, (float) sh, dst + ((size_t) ((ah - fh) / 2) * LAB_W + (aw - fw) / 2) * 3, fw, fh, LAB_W * 3);
	}
	free(tmp);
}


/* Compone el cartucho (a 2x para bordes suaves), con la forma de un cartucho
 * de Switch: cuerpo oscuro redondeado, muesca arriba, ventana con la banda del
 * color de la consola (su nombre se escribe encima en drawCard), la portada,
 * una franja oscura para el nombre del juego y un triángulo abajo. */
enum { CK_WIN_X = 22, CK_WIN_Y = 33, CK_BAND_Y1 = 104, CK_ART_Y1 = 338, CK_WIN_Y1 = 400 };  /* medidas a 2x */

static float rrCov(float px, float py, float x0, float y0, float x1, float y1, float r) {
	/* cobertura (0..1) de un rectángulo redondeado, con antialias de 1 px */
	float cx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
	float cy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
	float dx = px - cx, dy = py - cy;
	float d = (dx || dy) ? sqrtf(dx * dx + dy * dy) - r : -fminf(fminf(px - x0, x1 - px), fminf(py - y0, y1 - py));
	return d > 0.5f ? 0 : (d < -0.5f ? 1 : 0.5f - d);
}

static uint32_t blendRGB(uint32_t a, uint32_t b, float t) {
	return (uint32_t) ((int) (((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t) << 16 | (int) (((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t) << 8 | (int) ((a & 255) * (1 - t) + (b & 255) * t));
}

/* label: portada (o NULL); col / colR: banda (mitad izquierda / derecha);
 * artFill: color liso de la ventana si no hay portada (0 = rayas del color de la banda) */
static SDL_Texture* buildCardTex(struct App* a, SDL_Surface* label, Uint32 col, Uint32 colR, Uint32 artFill) {
	const int W = CARD_W * 2, H = CARD_H * 2;
	const int AW = W - CK_WIN_X * 2, AH = CK_ART_Y1 - CK_BAND_Y1;
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s) return NULL;
	/* portada (o rayas del color de la consola si todavía no hay) */
	uint8_t* art = calloc((size_t) LAB_W * LAB_H, 3);
	if (!art) { SDL_FreeSurface(s); return NULL; }
	if (label) drawArt(art, label, 0, 0, AW, AH);
	else
		for (int y = 0; y < AH; ++y)
			for (int x = 0; x < AW; ++x) {
				float k = (((x + y) / 24) % 2) ? 0.30f : 0.38f;
				putPx(art, x, y, artFill ? artFill : (uint32_t) (((col >> 16) & 0xFF) * k) << 16 | (uint32_t) (((col >> 8) & 0xFF) * k) << 8 | (uint32_t) ((col & 0xFF) * k));
			}
	uint32_t* px = s->pixels;
	const int pitch = s->pitch / 4;
	const uint32_t BODY = 0x1E1F23, RIM = 0x3A3C43, DARK = 0x111215, STRIP = 0x26272B;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float fx = x + 0.5f, fy = y + 0.5f;
			float cov = rrCov(fx, fy, 0, 0, (float) W, (float) H, 30);
			if (cov <= 0) { px[y * pitch + x] = 0; continue; }
			/* cuerpo con un borde apenas más claro (relieve) */
			float inner = rrCov(fx, fy, 3, 3, W - 3.0f, H - 3.0f, 27);
			uint32_t c = blendRGB(RIM, BODY, inner);
			/* muesca de arriba */
			float notch = rrCov(fx, fy, W * 0.35f, 10, W * 0.65f, 26, 6);
			if (notch > 0) c = blendRGB(c, DARK, notch);
			/* triángulo de abajo */
			float ty = fy - (CK_WIN_Y1 + 14), half = 28 - ty * 1.25f;
			if (ty >= 0 && half > 0 && fabsf(fx - W / 2.0f) < half) c = DARK;
			/* ventana: banda, portada y franja del nombre */
			float win = rrCov(fx, fy, CK_WIN_X, CK_WIN_Y, (float) (W - CK_WIN_X), CK_WIN_Y1, 10);
			if (win > 0) {
				uint32_t wc;
				if (y < CK_BAND_Y1) wc = blendRGB(x < W / 2 ? col : colR, 0xFFFFFF, 0.08f * (1.0f - (float) (y - CK_WIN_Y) / (CK_BAND_Y1 - CK_WIN_Y)));
				else if (y < CK_ART_Y1) {
					int ax = x - CK_WIN_X, ay = y - CK_BAND_Y1;
					const uint8_t* p = art + ((size_t) ay * LAB_W + (ax < 0 ? 0 : ax >= AW ? AW - 1 : ax)) * 3;
					wc = (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2];
				} else wc = STRIP;
				c = blendRGB(c, wc, win);
			}
			px[y * pitch + x] = ((uint32_t) (cov * 255) << 24) | c;
		}
	free(art);
	SDL_Texture* t = SDL_CreateTextureFromSurface(a->renderer, s);
	SDL_FreeSurface(s);
	if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
	return t;
}

static SDL_Texture* buildCard(struct App* a, struct LibGame* g) {
	return buildCardTex(a, g->label, SYS_COLOR[g->sys], SYS_COLOR[g->sys], 0);
}

/* Resplandor suave (textura de 9 partes, sin costuras) para el cartucho elegido */
#define GLOW_SZ 128
#define GLOW_B 48
static SDL_Texture* g_glowTex;

static SDL_Texture* glowTexture(struct App* a) {
	if (g_glowTex) return g_glowTex;
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, GLOW_SZ, GLOW_SZ, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s) return NULL;
	uint32_t* px = s->pixels;
	const float r = 16, x0 = GLOW_B, y0 = GLOW_B, x1 = GLOW_SZ - GLOW_B, y1 = GLOW_SZ - GLOW_B;
	for (int y = 0; y < GLOW_SZ; ++y)
		for (int x = 0; x < GLOW_SZ; ++x) {
			float fx = x + 0.5f, fy = y + 0.5f;
			float cx = fx < x0 + r ? x0 + r : (fx > x1 - r ? x1 - r : fx);
			float cy = fy < y0 + r ? y0 + r : (fy > y1 - r ? y1 - r : fy);
			float d = sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) - r;
			float k = d <= 0 ? 1 : (d >= GLOW_B ? 0 : powf(1 - d / GLOW_B, 2.2f));
			px[y * (s->pitch / 4) + x] = ((uint32_t) (k * 255) << 24) | 0xFFFFFF;
		}
	g_glowTex = SDL_CreateTextureFromSurface(a->renderer, s);
	SDL_FreeSurface(s);
	if (g_glowTex) { SDL_SetTextureBlendMode(g_glowTex, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(g_glowTex, SDL_ScaleModeLinear); }
	return g_glowTex;
}

/* Dibuja el resplandor alrededor de (x,y,w,h); e = cuánto se extiende */
static void drawGlow(struct App* a, float x, float y, float w, float h, float e, Uint32 rgb, Uint8 alpha) {
	SDL_Texture* t = glowTexture(a);
	if (!t) return;
	SDL_SetTextureColorMod(t, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	SDL_SetTextureAlphaMod(t, alpha);
	const int B = GLOW_B, M = GLOW_SZ - 2 * GLOW_B;
	/* el borde de la textura (B) cubre el resplandor; el centro se estira */
	float X[4] = { x - e, x, x + w, x + w + e }, Y[4] = { y - e, y, y + h, y + h + e };
	int SX[4] = { 0, B, B + M, GLOW_SZ }, SY[4] = { 0, B, B + M, GLOW_SZ };
	for (int j = 0; j < 3; ++j)
		for (int i = 0; i < 3; ++i) {
			SDL_Rect src = { SX[i], SY[j], SX[i + 1] - SX[i], SY[j + 1] - SY[j] };
			SDL_FRect dst = { X[i], Y[j], X[i + 1] - X[i], Y[j + 1] - Y[j] };
			SDL_RenderCopyF(a->renderer, t, &src, &dst);
		}
}

static void loadLabel(struct App* a, struct LibGame* g) {
	char p[1024];
	boxPath(p, sizeof(p), g->crc);  /* portada de la caja (TheGamesDB o propia) */
	if (!a->cfg.boxArt || !fileExists(p)) coverPath(p, sizeof(p), g->crc);
	if (!fileExists(p)) return;
	SDL_Surface* s = SDL_LoadBMP(p);
	if (!s) return;
	SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
	SDL_FreeSurface(s);
	if (!c) return;
	if (g->label) SDL_FreeSurface(g->label);
	g->label = c;
	if (g->card) { SDL_DestroyTexture(g->card); g->card = NULL; }
}


/* ---------- carga de portadas en segundo plano ----------
 * Leer cada BMP de la microSD en el hilo principal trababa el desplazamiento
 * con bibliotecas grandes. Un hilo lee los archivos; el hilo principal solo
 * arma las tarjetas (pocas por cuadro) y libera las que no se ven hace rato. */
struct LabelReq { uint32_t crc; char box[1024]; char cov[1024]; };
struct LabelRes { uint32_t crc; SDL_Surface* s; };
#define LABEL_Q 512
static struct LabelReq g_lreq[LABEL_Q];
static int g_lreqN;
static struct LabelRes g_lres[LABEL_Q];
static int g_lresN;
static SDL_mutex* g_lLock;
static SDL_sem* g_lSem;
static SDL_Thread* g_lThr;
static int g_cardBudget = 3;   /* tarjetas nuevas que se arman por cuadro */
static int g_cardCount;        /* tarjetas armadas en memoria */

static SDL_Surface* readLabelFile(const char* p) {
	if (!p[0] || !fileExists(p)) return NULL;
	SDL_Surface* s = SDL_LoadBMP(p);
	if (!s) return NULL;
	SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
	SDL_FreeSurface(s);
	return c;
}

static int labelThread(void* unused) {
	(void) unused;
	for (;;) {
		SDL_SemWait(g_lSem);
		SDL_LockMutex(g_lLock);
		if (!g_lreqN) { SDL_UnlockMutex(g_lLock); continue; }
		struct LabelReq r = g_lreq[--g_lreqN];  /* lo último pedido primero: lo que está en pantalla */
		SDL_UnlockMutex(g_lLock);
		SDL_Surface* s = readLabelFile(r.box);
		if (!s) s = readLabelFile(r.cov);
		SDL_LockMutex(g_lLock);
		if (g_lresN < LABEL_Q) g_lres[g_lresN++] = (struct LabelRes) { r.crc, s };
		else if (s) SDL_FreeSurface(s);
		SDL_UnlockMutex(g_lLock);
	}
	return 0;
}

static void requestLabel(struct App* a, struct LibGame* g) {
	if (!g_lThr) {
		g_lLock = SDL_CreateMutex();
		g_lSem = SDL_CreateSemaphore(0);
		g_lThr = SDL_CreateThread(labelThread, "portadas", NULL);
		if (!g_lThr) { loadLabel(a, g); g->labelChecked = true; return; }
	}
	struct LabelReq r;
	r.crc = g->crc;
	r.box[0] = r.cov[0] = 0;
	if (a->cfg.boxArt) boxPath(r.box, sizeof(r.box), g->crc);
	coverPath(r.cov, sizeof(r.cov), g->crc);
	SDL_LockMutex(g_lLock);
	if (g_lreqN == LABEL_Q) {  /* cola llena: se descarta lo más viejo */
		memmove(g_lreq, g_lreq + 1, sizeof(g_lreq[0]) * (LABEL_Q - 1));
		--g_lreqN;
	}
	g_lreq[g_lreqN++] = r;
	SDL_UnlockMutex(g_lLock);
	SDL_SemPost(g_lSem);
	g->labelPending = true;
	g->labelAskedAt = SDL_GetTicks();
}

/* Pide la portada si hace falta (no bloquea) */
static void wantLabel(struct App* a, struct LibGame* g) {
	if (!g->labelChecked) {
		g->labelChecked = true;
		if (!g->label) requestLabel(a, g);
		return;
	}
	/* sin portada todavía: se vuelve a mirar cada tanto (la carátula automática puede llegar) */
	if (!g->label && !g->labelPending && SDL_GetTicks() - g->labelAskedAt > 6000) requestLabel(a, g);
}

/* Recibe las portadas leídas y libera tarjetas que no se ven hace rato */
static void labelPump(void) {
	g_cardBudget = 3;
	if (g_lLock) {
		struct LabelRes got[64];
		int n = 0;
		SDL_LockMutex(g_lLock);
		while (g_lresN && n < 64) got[n++] = g_lres[--g_lresN];
		SDL_UnlockMutex(g_lLock);
		for (int k = 0; k < n; ++k) {
			bool used = false;
			for (int i = 0; i < g_libN; ++i) {
				struct LibGame* g = &g_lib[i];
				if (g->crc != got[k].crc || !g->labelPending) continue;
				g->labelPending = false;
				if (got[k].s && !used) {
					if (g->label) SDL_FreeSurface(g->label);
					g->label = got[k].s;
					used = true;
					if (g->card) { SDL_DestroyTexture(g->card); g->card = NULL; --g_cardCount; }
				}
			}
			if (got[k].s && !used) SDL_FreeSurface(got[k].s);
		}
	}
	static Uint32 lastSweep;
	Uint32 now = SDL_GetTicks();
	if (now - lastSweep > 2000) {
		lastSweep = now;
		g_cardCount = 0;
		for (int i = 0; i < g_libN; ++i) if (g_lib[i].card) ++g_cardCount;
	}
	if (g_cardCount > 260 && now - lastSweep < 20) {
		for (int i = 0; i < g_libN; ++i) {
			struct LibGame* g = &g_lib[i];
			if (!g->card || now - g->usedAt < 4000) continue;
			SDL_DestroyTexture(g->card);
			g->card = NULL;
			--g_cardCount;
			if (g->label) { SDL_FreeSurface(g->label); g->label = NULL; }
			g->labelChecked = false;
		}
	}
}

static void drawCard(struct App* a, struct LibGame* g, float x, float y, float scale, bool selected, int clipBottom) {
	float w = CARD_W * scale, h = CARD_H * scale;
	g->usedAt = SDL_GetTicks();
	if (!g->card && (selected || g_cardBudget > 0)) {
		if (!selected) --g_cardBudget;
		g->card = buildCard(a, g);
		if (g->card) ++g_cardCount;
	}
	if (!g->card) {
		/* todavía sin armar: un cartucho simple, sin trabar el desplazamiento */
		float vh = clipBottom > 0 && y + h > clipBottom ? clipBottom - y : h;
		if (vh <= 0) return;
		fillRR(a, x, y, w, vh, 15 * scale, 0x1E1F23, 255);
		float by0 = CK_WIN_Y / 2.0f * scale, by1 = CK_BAND_Y1 / 2.0f * scale;
		if (vh > by1) fillRR(a, x + 11 * scale, y + by0, w - 22 * scale, by1 - by0, 5 * scale, SYS_COLOR[g->sys], 255);
		if (vh > CK_ART_Y1 / 2.0f * scale) fillRR(a, x + 11 * scale, y + by1, w - 22 * scale, (CK_ART_Y1 - CK_BAND_Y1) / 2.0f * scale, 2 * scale, 0x2A2F3C, 255);
		if (vh > by1) text(a, F_ITEM, 0.5f * scale, SYS_NAME[g->sys], (int) (x + w / 2), (int) ((by0 + by1) / 2 + y - FONT_ITEM.height * 0.25f * scale), 0xFFFFFF, 255, AL_CENTER);
		if (vh > h - 30 * scale) textFit(a, F_SMALL, 0.42f * scale, g->name, (int) (x + 15 * scale), (int) (y + (CK_ART_Y1 + 12) / 2.0f * scale), (int) (w - 30 * scale), 0xF0F0F5);
		return;
	}
	if (selected) {
		/* resplandor difuminado del color de acento (una textura suave, sin líneas),
		 * sombra debajo y un borde redondeado fino: el elegido se separa del fondo */
		float pulse = 0.82f + 0.18f * sinf(SDL_GetTicks() / 420.0f);
		drawGlow(a, x + 4 * scale, y + 10 * scale, w - 8 * scale, h - 8 * scale, 18 * scale, 0x000000, 150);
		drawGlow(a, x, y, w, h, 26 * scale, TH.acc, (Uint8) (200 * pulse));
		fillRR(a, x - 3 * scale, y - 3 * scale, w + 6 * scale, h + 6 * scale, 18 * scale, TH.acc, 255);
	}
	SDL_Rect src = { 0, 0, CARD_W * 2, CARD_H * 2 };
	SDL_FRect dst = { x, y, w, h };
	if (clipBottom > 0 && y + h > clipBottom) {
		float vis = clipBottom - y;
		if (vis <= 0) return;
		src.h = (int) (CARD_H * 2 * vis / h);
		dst.h = vis;
	}
	SDL_RenderCopyF(a->renderer, g->card, &src, &dst);
	/* nombre de la consola en la banda de color */
	float bandY0 = y + CK_WIN_Y / 2.0f * scale, bandY1 = y + CK_BAND_Y1 / 2.0f * scale;
	if (clipBottom <= 0 || bandY1 < clipBottom) {
		float ns = 0.5f * scale;
		const char* nm = SYS_NAME[g->sys];
		int nw = textW(F_ITEM, nm, ns);
		if (nw > w - 30 * scale) { ns *= (w - 30 * scale) / nw; nw = textW(F_ITEM, nm, ns); }
		text(a, F_ITEM, ns, nm, (int) (x + w / 2 - nw / 2), (int) ((bandY0 + bandY1) / 2 - FONT_ITEM.height * ns / 2), 0xFFFFFF, 255, AL_LEFT);
	}
	/* nombre del juego en la franja oscura de abajo (hasta 2 líneas) */
	float sy0 = y + CK_ART_Y1 / 2.0f * scale, sy1 = y + CK_WIN_Y1 / 2.0f * scale;
	if (clipBottom <= 0 || sy1 < clipBottom) {
		float ts = 0.42f * scale, th = FONT_SMALL.height * ts;
		int maxW = (int) (w - 30 * scale);
		float cx = x + w / 2;
		const char* nm = g->name;
		if (textW(F_SMALL, nm, ts) <= maxW) text(a, F_SMALL, ts, nm, (int) cx, (int) ((sy0 + sy1 - th) / 2), 0xF0F0F5, 255, AL_CENTER);
		else {
			char l1[128];
			snprintf(l1, sizeof(l1), "%s", nm);
			char* sp = NULL;
			for (char* p = l1; *p; ++p)
				if (*p == ' ') { *p = 0; if (textW(F_SMALL, l1, ts) <= maxW) sp = p; *p = ' '; }
			if (sp) {
				*sp = 0;
				float gap = th * 0.9f, top = (sy0 + sy1) / 2 - gap;
				text(a, F_SMALL, ts, l1, (int) cx, (int) top, 0xF0F0F5, 255, AL_CENTER);
				char l2[128];
				snprintf(l2, sizeof(l2), "%s", sp + 1);
				while (textW(F_SMALL, l2, ts) > maxW && strlen(l2) > 4) { size_t n = strlen(l2); l2[n - 4] = '.'; l2[n - 3] = '.'; l2[n - 2] = '.'; l2[n - 1] = 0; }
				text(a, F_SMALL, ts, l2, (int) cx, (int) (top + gap), 0xF0F0F5, 255, AL_CENTER);
			} else textFit(a, F_SMALL, ts, nm, (int) (x + 15 * scale), (int) ((sy0 + sy1 - th) / 2), maxW, 0xF0F0F5);
		}
	}
}

/* ---------------- sonido de inserción (sintetizado) ---------------- */
static void playInsertSound(struct App* a, float delaySec) {
	if (!a->audio) return;
	const int rate = SAMPLE_RATE;
	int n = (int) (rate * (delaySec + 0.6f));
	int16_t* buf = calloc((size_t) n * 2, sizeof(int16_t));
	if (!buf) return;
	int start = (int) (rate * delaySec);
	for (int i = start; i < n; ++i) {
		float t = (float) (i - start) / rate;
		float v = 0;
		/* "clac": golpe corto con ruido + tono grave */
		if (t < 0.05f) v += ((rand() / (float) RAND_MAX) * 2 - 1) * (1 - t / 0.05f) * 0.55f;
		v += sinf(2 * 3.14159265f * 140 * t) * expf(-t * 40) * 0.6f;
		/* zumbido de encendido */
		if (t > 0.12f) {
			float u = t - 0.12f;
			v += sinf(2 * 3.14159265f * (220 + 400 * u) * u) * expf(-u * 6) * 0.18f * (u < 0.05f ? u / 0.05f : 1);
		}
		int s = (int) (v * 12000);
		if (s > 32767) s = 32767;
		if (s < -32768) s = -32768;
		buf[i * 2] = buf[i * 2 + 1] = (int16_t) s;
	}
	SDL_ClearQueuedAudio(a->audio);
	SDL_QueueAudio(a->audio, buf, (Uint32) n * 2 * sizeof(int16_t));
	SDL_PauseAudioDevice(a->audio, 0);
	free(buf);
}

/* ---------------- animación de inserción ---------------- */
static float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static void drawSlot(struct App* a, float glow, bool led) {
	fillRR(a, 440, 560, 400, 130, 32, 0x1E2330, 255);
	fillRR(a, 440, 560, 400, 24, 12, 0x262C3C, 255);
	float sx0 = LIB_W / 2 - CARD_W / 2 - 10, sx1 = LIB_W / 2 + CARD_W / 2 + 10;
	for (int r = 7; r >= 1 && glow > 0; --r) {
		Uint8 al = (Uint8) (glow * 140 * (1 - r / 8.0f));
		fillRR(a, sx0 - r * 2, 564 - r, sx1 - sx0 + r * 4, 16 + r * 2, 8 + r, TH.acc, al / 3);
	}
	fillRR(a, sx0, 564, sx1 - sx0, 16, 6, 0x05060A, 255);
	fillRR(a, 790, 640, 18, 18, 9, led ? 0x40D28C : 0x3C4048, 255);
	text(a, F_SMALL, 0.55f, "POWER", 476, 638, 0x6E768A, 255, AL_LEFT);
}

/* Devuelve false si se canceló (no se cancela: se puede saltar con A) */
/* posición y escala de la tarjeta elegida (para animar el cartucho) */
static float g_selX = 40, g_selY = 128, g_selScale = 1.22f;

static void runInsertAnim(struct App* a, struct LibGame* g, float fromX, float fromY) {
	const Uint32 start = SDL_GetTicks();
	bool sounded = false;
	const float slotY = 572;
	for (;;) {
		float t = (SDL_GetTicks() - start) / 1000.0f;
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_OK || in == IN_BACK || t > 2.45f) break;  /* se puede saltar */
		if (!sounded && t > 0.15f) { playInsertSound(a, 1.05f); sounded = true; }
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		if (t < 1.95f) drawSlot(a, t < 0.6f ? 0 : (t < 1.0f ? (t - 0.6f) / 0.4f : 1), t > 1.25f);
		const float cx = LIB_W / 2.0f - CARD_W / 2.0f;
		if (t < 0.6f) {
			float k = easeOut(t / 0.6f);
			float sc = lerpf(g_selScale, 1.0f, k);  /* parte del tamaño de la tarjeta elegida */
			float x = lerpf(fromX, cx, k), y = lerpf(fromY, 120, k);
			drawCard(a, g, x, y, sc, false, 0);
			text(a, F_ITEM, 0.6f, g->name, 40, 650, TH.text, 255, AL_LEFT);
		} else if (t < 1.25f) {
			float k = (t - 0.6f) / 0.65f;
			k = k * k;
			float y = lerpf(120, slotY - CARD_H * 0.62f, k);
			drawCard(a, g, cx, y, 1.0f, false, (int) slotY);
		} else if (t < 1.95f) {
			float k = (t - 1.25f) / 0.7f;
			float bounce = 4 * sinf(k * 3.14159265f * 3) * (1 - k);
			float shake = t < 1.4f ? 3 * sinf(k * 50) * (1 - k) : 0;
			drawCard(a, g, cx + shake, slotY - CARD_H * 0.62f + bounce, 1.0f, false, (int) slotY);
			char msg[200];
			snprintf(msg, sizeof(msg), T("Iniciando %s..."), g->name);
			text(a, F_SMALL, 0.65f, msg, 40, 36, TH.dim, 255, AL_LEFT);
		} else {
			/* luz que sale de la ranura y llena la pantalla */
			drawSlot(a, 1, true);
			drawCard(a, g, cx, slotY - CARD_H * 0.62f, 1.0f, false, (int) slotY);
			float k = easeOut((t - 1.95f) / 0.5f);
			float rx = lerpf(30, 1400, k), ry = lerpf(10, 900, k);
			for (int i = 0; i < 6; ++i) {
				float f = 1 - i / 6.0f;
				fillRR(a, LIB_W / 2 - rx * f, slotY - ry * f, rx * 2 * f, ry * 2 * f, ry * f, 0xECFFF6, (Uint8) (70 + 30 * k));
			}
			fillRect2(a, 0, 0, LIB_W, LIB_H, 0xECFFF6, (Uint8) (255 * k));
		}
#ifdef SWS_TEST
		{
			static int shots = 0;
			if (shots == 0 && t > 0.9f) { shots = 1; testCapture(a, "anim_baja"); }
			if (shots == 1 && t > 1.35f) { shots = 2; testCapture(a, "anim_clac"); }
			if (shots == 2 && t > 2.1f) { shots = 3; testCapture(a, "anim_luz"); }
		}
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(8);
	}
	/* el juego aparece desde blanco (lo dibuja present()) */
	a->fadeFromWhite = SDL_GetTicks();
}

/* ---------------- animación de extracción (al volver del juego) ---------------- */
static void playEjectSound(struct App* a) {
	if (!a->audio) return;
	const int rate = SAMPLE_RATE;
	int n = (int) (rate * 0.9f);
	int16_t* buf = calloc((size_t) n * 2, sizeof(int16_t));
	if (!buf) return;
	for (int i = 0; i < n; ++i) {
		float t = (float) i / rate, v = 0;
		/* apagado: tono que baja y se apaga */
		if (t < 0.3f) { float f = 420 - 300 * t / 0.3f; v += sinf(2 * 3.14159265f * f * t) * (1 - t / 0.3f) * 0.16f; }
		/* "clic" del resorte al soltar el cartucho */
		if (t > 0.38f) {
			float u = t - 0.38f;
			if (u < 0.03f) v += ((rand() / (float) RAND_MAX) * 2 - 1) * (1 - u / 0.03f) * 0.45f;
			v += sinf(2 * 3.14159265f * 300 * u) * expf(-u * 55) * 0.45f;
		}
		/* roce al sacarlo */
		if (t > 0.55f && t < 0.85f) { float u = (t - 0.55f) / 0.3f; v += ((rand() / (float) RAND_MAX) * 2 - 1) * sinf(u * 3.14159265f) * 0.06f; }
		int sm = (int) (v * 12000);
		if (sm > 32767) sm = 32767;
		if (sm < -32768) sm = -32768;
		buf[i * 2] = buf[i * 2 + 1] = (int16_t) sm;
	}
	SDL_ClearQueuedAudio(a->audio);
	SDL_QueueAudio(a->audio, buf, (Uint32) n * 2 * sizeof(int16_t));
	SDL_PauseAudioDevice(a->audio, 0);
	free(buf);
}

/* El cartucho se apaga, salta con el resorte, sale de la ranura y vuelve a su lugar
 * en la estantería (toX, toY). Se puede saltar con A o B. */
static void runEjectAnim(struct App* a, struct LibGame* g, float toX, float toY) {
	const Uint32 start = SDL_GetTicks();
	const float slotY = 572, cx = LIB_W / 2.0f - CARD_W / 2.0f, seated = slotY - CARD_H * 0.62f;
	playEjectSound(a);
	for (;;) {
		float t = (SDL_GetTicks() - start) / 1000.0f;
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_OK || in == IN_BACK || t > 1.75f) break;
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		float slotFade = t < 1.15f ? 1 : 1 - (t - 1.15f) / 0.35f;
		if (slotFade > 0) drawSlot(a, t < 0.3f ? 1 - t / 0.3f : 0, t < 0.25f);
		if (t < 0.38f) {
			drawCard(a, g, cx, seated, 1.0f, false, (int) slotY);
			text(a, F_SMALL, 0.65f, T("Apagando..."), 40, 36, TH.dim, 255, AL_LEFT);
		} else if (t < 0.6f) {
			/* el resorte lo empuja hacia arriba con un pequeño rebote */
			float k = (t - 0.38f) / 0.22f;
			float up = 26 * sinf(k * 3.14159265f * 0.5f) - 6 * sinf(k * 3.14159265f * 2) * (1 - k);
			drawCard(a, g, cx, seated - up, 1.0f, false, (int) slotY);
		} else if (t < 1.1f) {
			float k = easeOut((t - 0.6f) / 0.5f);
			drawCard(a, g, cx, lerpf(seated - 26, 120, k), 1.0f, false, (int) slotY);
		}
		/* la ranura se desvanece al final */
		if (slotFade < 1) fillRect2(a, 0, 520, LIB_W, LIB_H - 520, TH.bg, (Uint8) (255 * (1 - (slotFade < 0 ? 0 : slotFade))));
		if (t >= 1.1f) {
			/* vuelve a su lugar en la estantería (encima del desvanecido) */
			float u = (t - 1.1f) / 0.65f;
			float k = easeOut(u > 1 ? 1 : u);
			drawCard(a, g, lerpf(cx, toX, k), lerpf(120, toY, k), lerpf(1.0f, g_selScale, k), false, 0);
		}
		/* el juego se va a negro: entrada desde negro */
		if (t < 0.2f) fillRect2(a, 0, 0, LIB_W, LIB_H, 0x000000, (Uint8) (255 * (1 - t / 0.2f)));
#ifdef SWS_TEST
		{
			static int shots = 0;
			if (shots == 0 && t > 0.45f) { shots = 1; testCapture(a, "extraer_resorte"); }
			if (shots == 1 && t > 0.9f) { shots = 2; testCapture(a, "extraer_sale"); }
			if (shots == 2 && t > 1.4f) { shots = 3; testCapture(a, "extraer_vuelve"); }
		}
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(8);
	}
}

/* ---------------- animación de apertura ----------------
 * Sobre negro aparece "SWS" (S cian, W blanca, S coral). Las dos S giran 90°, se
 * acuestan como ondas y atraviesan la W; con un "clac" aparece el cartucho del logo
 * y el nombre. Coordenadas del logo: las del SVG (200 x 200), escaladas a pantalla. */
#define LOGO_CYAN  0x00C3E3
#define LOGO_CORAL 0xFF6B57
#define LOGO_INK   0x12151D
#define LOGO_RED   0xFF4554  /* rojo neón, para la segunda S del nombre */


/* Corta un polígono convexo y se queda con el lado donde f >= 0 (f lineal) */
static int clipConvex(const SDL_FPoint* in, int n, SDL_FPoint* out, float ax, float by, float c) {
	int m = 0;
	for (int i = 0; i < n; ++i) {
		SDL_FPoint p = in[i], q = in[(i + 1) % n];
		float fp = ax * p.x + by * p.y + c, fq = ax * q.x + by * q.y + c;
		if (fp >= 0) out[m++] = p;
		if ((fp >= 0) != (fq >= 0)) {
			float t = fp / (fp - fq);
			out[m++] = (SDL_FPoint) { p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t };
		}
	}
	return m;
}

static void fillConvex(struct App* a, const SDL_FPoint* p, int n, Uint32 rgb, Uint8 alpha) {
	if (n < 3) return;
	SDL_Vertex v[257];
	int idx[3 * 256];
	if (n > 256) n = 256;
	SDL_Color c = { (Uint8) ((rgb >> 16) & 0xFF), (Uint8) ((rgb >> 8) & 0xFF), (Uint8) (rgb & 0xFF), alpha };
	float cx = 0, cy = 0;
	for (int i = 0; i < n; ++i) { cx += p[i].x; cy += p[i].y; }
	v[0] = (SDL_Vertex) { { cx / n, cy / n }, c, { 0, 0 } };
	for (int i = 0; i < n; ++i) v[i + 1] = (SDL_Vertex) { p[i], c, { 0, 0 } };
	int m = 0;
	for (int i = 1; i <= n; ++i) { idx[m++] = 0; idx[m++] = i; idx[m++] = i < n ? i + 1 : 1; }
	SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(a->renderer, NULL, v, n + 1, idx, m);
}

/* Intersección de dos polígonos convexos: recorta p con cada lado de q */
static int intersectConvex(const SDL_FPoint* p, int np, const SDL_FPoint* q, int nq, SDL_FPoint* out) {
	SDL_FPoint bufA[256], bufB[256];
	float area = 0;
	for (int i = 0; i < nq; ++i) { SDL_FPoint u = q[i], v = q[(i + 1) % nq]; area += u.x * v.y - v.x * u.y; }
	float sg = area >= 0 ? 1.0f : -1.0f;
	int n = np < 256 ? np : 256;
	memcpy(bufA, p, sizeof(SDL_FPoint) * n);
	SDL_FPoint *cur = bufA, *nxt = bufB;
	for (int i = 0; i < nq && n >= 3; ++i) {
		SDL_FPoint u = q[i], v = q[(i + 1) % nq];
		float dx = v.x - u.x, dy = v.y - u.y;
		if (dx * dx + dy * dy < 1e-6f) continue;
		/* adentro: sg * cruz(v - u, x - u) >= 0 */
		float ax = -sg * dy, by = sg * dx, c = sg * (dy * u.x - dx * u.y);
		if (n * 2 > 250) break;
		n = clipConvex(cur, n, nxt, ax, by, c);
		SDL_FPoint* t = cur; cur = nxt; nxt = t;
	}
	memcpy(out, cur, sizeof(SDL_FPoint) * n);
	return n;
}

/* Pieza del logo en relieve: borde claro del lado de la luz (lx, ly) y
 * oscuro del lado contrario, como un botón de plástico. */
static void bevelConvex(struct App* a, const SDL_FPoint* p, int n, float lx, float ly, float e, Uint32 base, Uint8 alpha) {
	SDL_FPoint s1[256], s2[256], t[256], u[256];
	for (int i = 0; i < n; ++i) {
		s1[i] = (SDL_FPoint) { p[i].x - lx * e, p[i].y - ly * e };  /* corrida hacia la sombra */
		s2[i] = (SDL_FPoint) { p[i].x + lx * e, p[i].y + ly * e };  /* corrida hacia la luz */
	}
	fillConvex(a, p, n, mixRGB(base, 0xFFFFFF, 0.22f), alpha);
	int nt = intersectConvex(p, n, s1, n, t);
	fillConvex(a, t, nt, mixRGB(base, 0x000000, 0.16f), alpha);
	/* cuerpo en dos pasos para que el borde no quede tan duro */
	for (int k = 0; k < 2; ++k) {
		float f = k ? 1.0f : 0.6f;
		for (int i = 0; i < n; ++i) {
			s1[i] = (SDL_FPoint) { p[i].x - lx * e * f, p[i].y - ly * e * f };
			s2[i] = (SDL_FPoint) { p[i].x + lx * e * f, p[i].y + ly * e * f };
		}
		nt = intersectConvex(p, n, s1, n, t);
		int nu = intersectConvex(t, nt, s2, n, u);
		fillConvex(a, u, nu, base, k ? alpha : (Uint8) (alpha / 2));
	}
}

static SDL_Texture* g_logoTex;

static int logoTextW(const char* s, float sc) {
	int w = 0;
	while (*s) w += FONT_LOGO.advance[fGlyph(&FONT_LOGO, fUtf8(&s))];
	return (int) (w * sc);
}

static void logoText(struct App* a, const char* s, float x, float y, float sc, Uint32 rgb, Uint8 alpha) {
	if (!g_logoTex) g_logoTex = makeFontTexture(a->renderer, &FONT_LOGO);
	if (!g_logoTex) return;
	SDL_SetTextureColorMod(g_logoTex, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
	SDL_SetTextureAlphaMod(g_logoTex, alpha);
	while (*s) {
		int g = fGlyph(&FONT_LOGO, fUtf8(&s));
		SDL_Rect src = { FONT_LOGO.x[g], 0, FONT_LOGO.w[g], FONT_LOGO.height };
		SDL_FRect dst = { x, y, FONT_LOGO.w[g] * sc, FONT_LOGO.height * sc };
		SDL_RenderCopyF(a->renderer, g_logoTex, &src, &dst);
		x += FONT_LOGO.advance[g] * sc;
	}
}

/* Logo SwapShelf: los dos Joy-Con fusionados en una sola pieza, cian a la
 * izquierda (curva grande arriba) y roja a la derecha (curva grande abajo),
 * separados por un corte diagonal \\ con una ranura fina, y las dos palabras
 * juntas en Inter Black para que se lea un solo nombre. Cada pieza tiene
 * relieve, y el de la roja va invertido como la pieza: luz arriba-izquierda
 * en la cian, abajo-derecha en la roja. El alto sale de la fuente y la
 * escala; align: AL_LEFT o AL_CENTER. Devuelve el ancho dibujado.
 * (ink se mantiene por compatibilidad.) */
static int drawBrand(struct App* a, int font, float sc, int x, int y, Uint32 ink, Uint8 alpha, int align) {
	(void) ink;
	const struct BitmapFont* f = FONTS[font];
	float h = f->height * sc * 0.92f;
	float ts = h * 0.587f / 64.0f;  /* Inter generada a 64 px: letra al ~59 % del alto */
	int wa = logoTextW("SWAP", ts), wb = logoTextW("SHELF", ts);
	float pad = h * 0.40f, midGap = h * 0.30f, slant = h * 0.11f, gap = h * 0.04f;
	float W = pad + wa + midGap + wb + pad;
	float x0 = align == AL_CENTER ? x - W / 2 : (float) x, y0 = (float) y;
	/* contorno completo: curva grande arriba-izq y abajo-der, mediana en las otras */
	float R[4] = { h * 0.62f, h * 0.26f, h * 0.62f, h * 0.26f };
	const float cxs[4] = { x0 + R[0], x0 + W - R[1], x0 + W - R[2], x0 + R[3] };
	const float cys[4] = { y0 + R[0], y0 + R[1], y0 + h - R[2], y0 + h - R[3] };
	const float a0[4] = { 180, 270, 0, 90 };
	SDL_FPoint shape[4 * 13];
	int n = 0;
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i <= 12; ++i) {
			float t = (a0[k] + 90.0f * i / 12) * 0.0174533f;
			shape[n++] = (SDL_FPoint) { cxs[k] + cosf(t) * R[k], cys[k] + sinf(t) * R[k] };
		}
	/* corte "\": x = mx - slant + (y - y0) * kk  (de arriba a la izquierda hacia abajo a la derecha) */
	float mx = x0 + pad + wa + midGap / 2, kk = 2 * slant / h;
	SDL_FPoint half[4 * 13 + 4];
	float e = h * 0.035f;  /* relieve fino: se ve moderno, no brillante */
	if (e < 1.0f) e = 1.0f;
	int nl = clipConvex(shape, n, half, -1, kk, mx - slant - y0 * kk - gap / 2);
	bevelConvex(a, half, nl, -1, -1, e, LOGO_CYAN, alpha);
	int nr = clipConvex(shape, n, half, 1, -kk, -(mx - slant - y0 * kk + gap / 2));
	bevelConvex(a, half, nr, 1, 1, e, LOGO_RED, alpha);
	/* letras centradas por la altura de la mayúscula, con una sombra corta
	 * que sigue la luz de cada pieza: SWAP hacia abajo-derecha, SHELF al revés */
	float ty = y0 + h / 2 - (LOGO_FONT_CAP_TOP + LOGO_FONT_CAP_H / 2.0f) * ts;
	float so = h * 0.02f;
	if (so < 1.0f) so = 1.0f;
	logoText(a, "SWAP", x0 + pad + so * 0.7f, ty + so, ts, 0x000000, (Uint8) (alpha * 0.22f));
	logoText(a, "SHELF", x0 + pad + wa + midGap - so * 0.7f, ty - so, ts, 0x000000, (Uint8) (alpha * 0.22f));
	logoText(a, "SWAP", x0 + pad, ty, ts, 0xFFFFFF, alpha);
	logoText(a, "SHELF", x0 + pad + wa + midGap, ty, ts, 0xFFFFFF, alpha);
	return (int) W;
}

/* Logo en cuadrado (ícono, etiqueta del cartucho de la intro): las mismas
 * dos piezas que drawBrand con una S en cada una, centrado en (cx, cy) y
 * de ancho s. dxL / dxR corren cada pieza en x (la intro las junta). */
static void drawMono(struct App* a, float cx, float cy, float s, float dxL, float dxR, Uint8 alpha) {
	float w = s, h = s * 0.84f, x0 = cx - w / 2, y0 = cy - h / 2;
	float R[4] = { h * 0.42f, h * 0.16f, h * 0.42f, h * 0.16f };
	const float cxs[4] = { x0 + R[0], x0 + w - R[1], x0 + w - R[2], x0 + R[3] };
	const float cys[4] = { y0 + R[0], y0 + R[1], y0 + h - R[2], y0 + h - R[3] };
	const float a0[4] = { 180, 270, 0, 90 };
	SDL_FPoint shape[4 * 13], half[4 * 13 + 4];
	int n = 0;
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i <= 12; ++i) {
			float t = (a0[k] + 90.0f * i / 12) * 0.0174533f;
			shape[n++] = (SDL_FPoint) { cxs[k] + cosf(t) * R[k], cys[k] + sinf(t) * R[k] };
		}
	float slant = s * 0.06f, gap = s * 0.024f, kk = 2 * slant / h, mx = cx;
	float e = h * 0.035f;
	if (e < 1.0f) e = 1.0f;
	float ts = s * 0.56f / 64.0f, so = h * 0.02f;
	if (so < 1.0f) so = 1.0f;
	float ty = cy - (LOGO_FONT_CAP_TOP + LOGO_FONT_CAP_H / 2.0f) * ts, sw = logoTextW("S", ts);
	for (int side = 0; side < 2; ++side) {
		int m = side == 0 ? clipConvex(shape, n, half, -1, kk, mx - slant - y0 * kk - gap / 2)
		                  : clipConvex(shape, n, half, 1, -kk, -(mx - slant - y0 * kk + gap / 2));
		float dx = side == 0 ? dxL : dxR, sg = side == 0 ? 1.0f : -1.0f;
		for (int i = 0; i < m; ++i) half[i].x += dx;
		bevelConvex(a, half, m, -sg, -sg, e, side == 0 ? LOGO_CYAN : LOGO_RED, alpha);
		float lx = x0 + w * (side == 0 ? 0.27f : 0.73f) + dx - sw / 2;
		logoText(a, "S", lx + sg * so * 0.7f, ty + sg * so, ts, 0x000000, (Uint8) (alpha * 0.22f));
		logoText(a, "S", lx, ty, ts, 0xFFFFFF, alpha);
	}
}

static void playLogoSound(struct App* a) {
	if (!a->audio) return;
	const int rate = SAMPLE_RATE;
	int n = (int) (rate * 0.45f);
	int16_t* buf = calloc((size_t) n * 2, sizeof(int16_t));
	if (!buf) return;
	for (int i = 0; i < n; ++i) {
		float t = (float) i / rate, v = 0;
		/* dos golpes secos muy juntos: "cla-c" */
		for (int h = 0; h < 2; ++h) {
			float u = t - h * 0.055f;
			if (u < 0) continue;
			float f = h ? 1150.0f : 1700.0f;
			if (u < 0.012f) v += ((rand() / (float) RAND_MAX) * 2 - 1) * (1 - u / 0.012f) * 0.5f;
			v += sinf(2 * 3.14159265f * f * u) * expf(-u * 90) * 0.55f;
			v += sinf(2 * 3.14159265f * f * 0.5f * u) * expf(-u * 60) * 0.25f;
		}
		int sm = (int) (v * 11000);
		if (sm > 32767) sm = 32767;
		if (sm < -32768) sm = -32768;
		buf[i * 2] = buf[i * 2 + 1] = (int16_t) sm;
	}
	SDL_ClearQueuedAudio(a->audio);
	SDL_QueueAudio(a->audio, buf, (Uint32) n * 2 * sizeof(int16_t));
	SDL_PauseAudioDevice(a->audio, 0);
	free(buf);
}

static float easeInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }

/* Cartucho de la intro: el mismo diseño que los de la biblioteca, con la banda
 * cian y roja, el logo en cuadrado como etiqueta y la versión abajo. */
static SDL_Texture* g_introCart;
static float introMonoSize(float scale) { return 96 * scale; }  /* ancho del logo dentro de la ventana */
static float introMonoY(float y, float scale) { return y + (CK_BAND_Y1 + CK_ART_Y1) / 4.0f * scale; }
static void drawIntroCart(struct App* a, float x, float y, float scale, int clipBottom, bool mark) {
	if (!g_introCart) g_introCart = buildCardTex(a, NULL, LOGO_CYAN, LOGO_RED, LOGO_INK);
	float w = CARD_W * scale, h = CARD_H * scale;
	if (clipBottom > 0) { SDL_Rect c = { 0, 0, LIB_W, clipBottom }; SDL_RenderSetClipRect(a->renderer, &c); }
	if (g_introCart) { SDL_FRect d = { x, y, w, h }; SDL_RenderCopyF(a->renderer, g_introCart, NULL, &d); }
	float by0 = y + CK_WIN_Y / 2.0f * scale, by1 = y + CK_BAND_Y1 / 2.0f * scale;
	float ns = (by1 - by0) * 0.42f / 64.0f;
	logoText(a, "SWAPSHELF", x + w / 2 - logoTextW("SWAPSHELF", ns) / 2.0f,
	         (by0 + by1) / 2 - (LOGO_FONT_CAP_TOP + LOGO_FONT_CAP_H / 2.0f) * ns, ns, 0xFFFFFF, 255);
	if (mark) drawMono(a, x + w / 2, introMonoY(y, scale), introMonoSize(scale), 0, 0, 255);
	float sy0 = y + CK_ART_Y1 / 2.0f * scale, sy1 = y + CK_WIN_Y1 / 2.0f * scale;
	float ts = 0.42f * scale;
	text(a, F_SMALL, ts, "SWS \xC2\xB7 v" SWS_VERSION, (int) (x + w / 2), (int) ((sy0 + sy1 - FONT_SMALL.height * ts) / 2), 0xC8CEDA, 255, AL_CENTER);
	if (clipBottom > 0) SDL_RenderSetClipRect(a->renderer, NULL);
}

/* Intro: las dos piezas del logo se juntan con un clac, aparece el cartucho alrededor,
 * baja hasta la ranura, entra con un clac y la luz da paso a la biblioteca.
 * Se puede saltar con A o B. */
static Uint32 g_libFadeFromWhite;
static void runIntro(struct App* a) {
	const Uint32 start = SDL_GetTicks();
	const float ox = LIB_W / 2.0f, oy = 300;  /* centro del logo en pantalla */
	const float S0 = 2.1f;                /* escala del cartucho grande (el logo cabe en la ventana) */
	const float slotY = 572, seatY = slotY - CARD_H * 0.62f;
	bool clac = false, insertSnd = false;
	for (;;) {
		float t = (SDL_GetTicks() - start) / 1000.0f;
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_OK || in == IN_BACK || in == IN_START || t > 4.35f) break;
		if (!clac && t >= 1.55f) { playLogoSound(a); clac = true; }
		if (!insertSnd && t >= 2.85f) { playInsertSound(a, 0.5f); insertSnd = true; }
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 255);
		SDL_RenderClear(a->renderer);
		if (t < 1.55f) {
			/* las dos piezas del logo llegan desde los costados y se juntan
			 * en el corte diagonal justo cuando suena el clac */
			float q = t < 0.15f ? 0 : (t - 0.15f) / 1.4f;
			if (q > 1) q = 1;
			q = 1 - (1 - q) * (1 - q) * (1 - q);  /* frena al llegar */
			float sz = introMonoSize(S0 * 0.92f), off = (1 - q) * 360;
			Uint8 al = (Uint8) (t < 0.45f ? 255 * t / 0.45f : 255);
			drawMono(a, ox, oy, sz, -off, off, al);
		} else {
			/* posición y tamaño del cartucho en cada momento */
			float sc, cxm, top;  /* escala, centro x, borde de arriba */
			int clip = 0;
			float slotFade = 0;
			bool led = false;
			if (t < 2.3f) {
				/* aparece alrededor del logo con un pequeño rebote */
				float pop = t > 1.85f ? 1 : (t - 1.55f) / 0.3f;
				float bounce = sinf(pop * 3.14159265f) * 0.06f * (1 - pop);
				sc = S0 * (0.92f + 0.08f * (1 - (1 - pop) * (1 - pop))) * (1 + bounce);
				cxm = ox;
				top = oy - (CK_BAND_Y1 + CK_ART_Y1) / 4.0f * sc;  /* la ventana queda sobre el logo */
			} else if (t < 2.85f) {
				/* se achica y va arriba de la ranura */
				float q = easeInOut((t - 2.3f) / 0.55f);
				float topBig = oy - (CK_BAND_Y1 + CK_ART_Y1) / 4.0f * S0;
				sc = lerpf(S0, 1.0f, q);
				cxm = ox;
				top = lerpf(topBig, 110, q);
				slotFade = q;
			} else if (t < 3.4f) {
				/* baja y entra en la ranura */
				float q = (t - 2.85f) / 0.55f;
				q = q * q;
				sc = 1.0f; cxm = ox;
				top = lerpf(110, seatY, q);
				clip = (int) slotY;
				slotFade = 1;
			} else {
				/* clac, rebote y luz */
				float q = (t - 3.4f) / 0.5f;
				if (q > 1) q = 1;
				float bounce = 4 * sinf(q * 3.14159265f * 3) * (1 - q);
				sc = 1.0f; cxm = ox;
				top = seatY + bounce;
				clip = (int) slotY;
				slotFade = 1;
				led = true;
			}
			if (slotFade > 0) {
				/* la consola entra desde abajo */
				SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 255);
				float off = (1 - slotFade) * 160;
				SDL_Rect vp = { 0, 0, LIB_W, LIB_H };
				(void) vp;
				if (off > 0.5f) {
					/* desplazada hacia abajo mientras aparece */
					SDL_Rect r = { 0, (int) off, LIB_W, LIB_H };
					SDL_RenderSetViewport(a->renderer, &r);
					drawSlot(a, slotFade, led);
					SDL_RenderSetViewport(a->renderer, NULL);
				} else drawSlot(a, t > 2.85f ? 1 : slotFade, led);
			}
			drawIntroCart(a, cxm - CARD_W * sc / 2, top, sc, clip, true);
			/* nombre debajo, mientras el cartucho está grande */
			if (t < 2.6f) {
				float ta = t < 2.0f ? (t - 1.55f) / 0.45f : (t < 2.3f ? 1 : 1 - (t - 2.3f) / 0.3f);
				if (ta < 0) ta = 0;
				if (ta > 1) ta = 1;
				drawBrand(a, F_TITLE, 0.62f, LIB_W / 2, (int) (top + CARD_H * sc + 16), 0xF2F4F8, (Uint8) (255 * ta), AL_CENTER);
			}
			/* luz que sale de la ranura y llena la pantalla */
			if (t > 3.85f) {
				float q = easeOut((t - 3.85f) / 0.5f);
				float rx = lerpf(30, 1400, q), ry = lerpf(10, 900, q);
				for (int i = 0; i < 6; ++i) {
					float f = 1 - i / 6.0f;
					fillRR(a, LIB_W / 2 - rx * f, slotY - ry * f, rx * 2 * f, ry * 2 * f, ry * f, 0xECFFF6, (Uint8) (70 + 30 * q));
				}
				fillRect2(a, 0, 0, LIB_W, LIB_H, 0xECFFF6, (Uint8) (255 * (q > 1 ? 1 : q)));
			}
		}
#ifdef SWS_TEST
		{
			static int shots = 0;
			if (shots == 0 && t > 0.6f) { shots = 1; testCapture(a, "intro_sws"); }
			if (shots == 1 && t > 1.15f) { shots = 2; testCapture(a, "intro_giro"); }
			if (shots == 2 && t > 2.15f) { shots = 3; testCapture(a, "intro_logo"); }
			if (shots == 3 && t > 2.7f) { shots = 4; testCapture(a, "intro_baja"); }
			if (shots == 4 && t > 3.5f) { shots = 5; testCapture(a, "intro_ranura"); }
		}
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(8);
	}
	g_libFadeFromWhite = SDL_GetTicks();  /* la biblioteca aparece desde la luz */
}

/* ---------------- pantalla de la biblioteca ---------------- */
static void runControls(struct App* a, int sys, uint32_t crc, const char* gameName, SDL_Texture* backdrop);
static bool confirmDialog(struct App* a, const char* title, const char* body, const char* yes, SDL_Texture* backdrop);
static bool g_listsDirty;

/* ================================================================== */
/* Biblioteca                                                          */
/*  Pestaña "Todo": inicio con filas (seguir jugando, favoritos, todos  */
/*  los juegos y accesos a cada consola).                               */
/*  Pestaña de consola: cuadrícula de 8 por fila, orden y salto por     */
/*  letra. Búsqueda con teclado en pantalla y menú de cada juego.       */
/* ================================================================== */
static int g_libTab;   /* 0 = todo, 1.. = consola */

/* ---------- historial: veces jugado, última vez y favoritos ---------- */
struct Played { uint32_t crc; int count; long long last; bool fav; long long secs; /* tiempo jugado */ };
static struct Played* g_played;
static int g_playedN;

static void playedPath(char* out, size_t n) { pathJoin(out, n, "Datos/jugados.txt"); }

static void playedLoad(void) {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	char p[1024];
	playedPath(p, sizeof(p));
	FILE* f = fopen(p, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') continue;
		unsigned crc = 0;
		int count = 0, fav = 0;
		long long last = 0, secs = 0;
		if (sscanf(line, "%x %d %lld %d %lld", &crc, &count, &last, &fav, &secs) < 3) continue;
		struct Played* n = realloc(g_played, sizeof(*g_played) * (size_t) (g_playedN + 1));
		if (!n) break;
		g_played = n;
		g_played[g_playedN++] = (struct Played) { crc, count, last, fav != 0, secs };
	}
	fclose(f);
}

static void playedSave(void) {
	char p[1024], d[1024];
	pathJoin(d, sizeof(d), "Datos");
	MKDIR(d);
	playedPath(p, sizeof(p));
	FILE* f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "# crc veces ultima_vez(unix) favorito segundos_jugados\n");
	for (int i = 0; i < g_playedN; ++i)
		fprintf(f, "%08X %d %lld %d %lld\n", (unsigned) g_played[i].crc, g_played[i].count, g_played[i].last, g_played[i].fav ? 1 : 0, g_played[i].secs);
	fclose(f);
}

static struct Played* playedFind(uint32_t crc, bool create) {
	for (int i = 0; i < g_playedN; ++i)
		if (g_played[i].crc == crc) return &g_played[i];
	if (!create) return NULL;
	struct Played* n = realloc(g_played, sizeof(*g_played) * (size_t) (g_playedN + 1));
	if (!n) return NULL;
	g_played = n;
	g_played[g_playedN] = (struct Played) { crc, 0, 0, false, 0 };
	return &g_played[g_playedN++];
}

static void playedOnLaunch(uint32_t crc) {
	struct Played* p = playedFind(crc, true);
	if (!p) return;
	p->count++;
	p->last = (long long) time(NULL);
	playedSave();
}

/* Suma el tiempo de una partida (sin contar la pausa) */
static void playedAddTime(uint32_t crc, long long secs) {
	if (secs <= 0) return;
	struct Played* p = playedFind(crc, true);
	if (!p) return;
	p->secs += secs;
	playedSave();
}

/* "menos de 1 min", "25 min", "3 h 05 min" */
static void playTimeText(long long secs, char* out, size_t n) {
	if (secs < 60) snprintf(out, n, "%s", T("menos de 1 min"));
	else if (secs < 3600) snprintf(out, n, T("%lld min"), secs / 60);
	else snprintf(out, n, T("%lld h %02lld min"), secs / 3600, (secs % 3600) / 60);
}

/* Orden de búsqueda de portadas: favoritos, luego los más jugados */
static int gamePriority(uint32_t crc) {
	struct Played* p = playedFind(crc, false);
	if (!p) return 0;
	return (p->fav ? 1000000 : 0) + (p->count > 999 ? 999 : p->count) * 1000 + (p->count ? 1 : 0);
}

static bool isFav(uint32_t crc) { struct Played* p = playedFind(crc, false); return p && p->fav; }

static void toggleFav(struct App* a, struct LibGame* g) {
	struct Played* p = playedFind(g->crc, true);
	if (!p) return;
	p->fav = !p->fav;
	playedSave();
	char msg[200];
	snprintf(msg, sizeof(msg), p->fav ? T("%s: agregado a favoritos") : T("%s: quitado de favoritos"), g->name);
	setMessage(a, msg);
}

/* "hace 10 min", "ayer", "hace 3 días"... */
static void agoText(long long when, char* out, size_t n) {
	long long d = (long long) time(NULL) - when;
	if (when <= 0) { snprintf(out, n, "%s", T("nunca")); return; }
	if (d < 3600) snprintf(out, n, T("hace %lld min"), d / 60 > 0 ? d / 60 : 1);
	else if (d < 86400) snprintf(out, n, T("hace %lld h"), d / 3600);
	else if (d < 2 * 86400) snprintf(out, n, "%s", T("ayer"));
	else if (d < 30 * 86400) snprintf(out, n, T("hace %lld d\xC3\xAD" "as"), d / 86400);
	else snprintf(out, n, T("hace %lld meses"), d / (30 * 86400));
}

/* Texto normalizado para buscar y ordenar: minúsculas y sin acentos */
static void normText(const char* in, char* out, size_t n) {
	size_t o = 0;
	for (const unsigned char* p = (const unsigned char*) in; *p && o + 1 < n; ++p) {
		int c = *p;
		if (c == 0xC3 && p[1]) {
			int d = *++p;
			c = (d >= 0x80 && d <= 0x86) || (d >= 0xA0 && d <= 0xA6) ? 'a' : (d == 0x87 || d == 0xA7) ? 'c'
			  : (d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB) ? 'e' : (d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF) ? 'i'
			  : (d == 0x91 || d == 0xB1) ? 'n' : (d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6) ? 'o'
			  : (d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC) ? 'u' : '?';
		} else if (c >= 0x80) continue;
		out[o++] = (char) tolower(c);
	}
	out[o] = 0;
}

/* Letra para la barra A-Z ("#" para números y símbolos; se ignora "The ") */
static char gameLetter(const struct LibGame* g) {
	char t[160];
	normText(g->name, t, sizeof(t));
	const char* s = t;
	if (!strncmp(s, "the ", 4)) s += 4;
	return (*s >= 'a' && *s <= 'z') ? (char) toupper(*s) : '#';
}

/* ---------- listas de cada vista ---------- */
enum SortMode { SORT_AZ, SORT_RECENT, SORT_PLAYED, SORT_FAV, SORT_COUNT };
static const char* const SORT_NAME[SORT_COUNT] = { "A-Z", "Recientes", "M\xC3\xA1s jugados", "Favoritos" };
static int g_sort;

static int cmpAZ(const void* x, const void* y) {
	char a[160], b[160];
	normText(g_lib[*(const int*) x].name, a, sizeof(a));
	normText(g_lib[*(const int*) y].name, b, sizeof(b));
	const char* pa = strncmp(a, "the ", 4) ? a : a + 4;
	const char* pb = strncmp(b, "the ", 4) ? b : b + 4;
	return strcmp(pa, pb);
}
static int cmpSorted(const void* x, const void* y) {
	const struct LibGame* ga = &g_lib[*(const int*) x];
	const struct LibGame* gb = &g_lib[*(const int*) y];
	struct Played* pa = playedFind(ga->crc, false);
	struct Played* pb = playedFind(gb->crc, false);
	long long va = 0, vb = 0;
	if (g_sort == SORT_RECENT) { va = pa ? pa->last : 0; vb = pb ? pb->last : 0; }
	else if (g_sort == SORT_PLAYED) {  /* más tiempo jugado; a igual tiempo, más veces */
		va = pa ? pa->secs * 1000 + pa->count : 0;
		vb = pb ? pb->secs * 1000 + pb->count : 0;
	}
	else if (g_sort == SORT_FAV) { va = pa && pa->fav; vb = pb && pb->fav; }
	if (va != vb) return va > vb ? -1 : 1;
	return cmpAZ(x, y);
}

/* Juegos de una consola en el orden elegido */
static int consoleList(int sys, int* out, int cap) {
	int n = 0;
	for (int i = 0; i < g_libN && n < cap; ++i)
		if (g_lib[i].sys == sys) out[n++] = i;
	qsort(out, (size_t) n, sizeof(int), g_sort == SORT_AZ ? cmpAZ : cmpSorted);
	return n;
}

/* Filas del inicio */
enum { ROW_RECENT, ROW_FAV, ROW_ALL, ROW_CONSOLES };
struct HomeRow { int kind; int n; int idx[4096]; };
static struct HomeRow g_rows[4];
static int g_rowN;
static int g_homeRow, g_homeCol[4];

static int cmpRecent(const void* x, const void* y) {
	struct Played* pa = playedFind(g_lib[*(const int*) x].crc, false);
	struct Played* pb = playedFind(g_lib[*(const int*) y].crc, false);
	long long va = pa ? pa->last : 0, vb = pb ? pb->last : 0;
	return va == vb ? 0 : (va > vb ? -1 : 1);
}

/* Consolas visibles: se ocultan solas si no tienen juegos, y el usuario puede
 * ocultar las que quiera desde Opciones (ini: ocultar=gb,gbc). */
static int g_sysCount[SYS_COUNT];
static unsigned g_sysHidden;  /* bit por consola */

static void countSystems(void) {
	memset(g_sysCount, 0, sizeof(g_sysCount));
	for (int i = 0; i < g_libN; ++i) g_sysCount[g_lib[i].sys]++;
}

static bool sysVisible(int sys) { return g_sysCount[sys] > 0 && !(g_sysHidden & (1u << sys)); }

static void buildHome(void) {
	countSystems();
	g_rowN = 0;
	struct HomeRow* r = &g_rows[g_rowN];
	r->kind = ROW_RECENT; r->n = 0;
	for (int i = 0; i < g_libN && r->n < 4096; ++i) {
		struct Played* p = playedFind(g_lib[i].crc, false);
		if (p && p->count > 0 && sysVisible(g_lib[i].sys)) {
			bool dup = false;
			for (int k = 0; k < r->n; ++k) if (g_lib[r->idx[k]].crc == g_lib[i].crc) dup = true;
			if (!dup) r->idx[r->n++] = i;
		}
	}
	qsort(r->idx, (size_t) r->n, sizeof(int), cmpRecent);
	if (r->n > 10) r->n = 10;
	++g_rowN;  /* la fila de recientes siempre está, aunque esté vacía */
	r = &g_rows[g_rowN];
	r->kind = ROW_CONSOLES; r->n = 0;  /* consolas visibles y, al final, Opciones (-1) */
	for (int k = 0; k < SYS_COUNT; ++k) if (sysVisible(k)) r->idx[r->n++] = k;
	r->idx[r->n++] = -1;
	++g_rowN;
	if (g_homeRow >= g_rowN) g_homeRow = g_rowN - 1;
	for (int k = 0; k < g_rowN; ++k) if (g_homeCol[k] >= g_rows[k].n) g_homeCol[k] = g_rows[k].n ? g_rows[k].n - 1 : 0;
}

/* Estado de la cuadrícula */
/* cartuchos por fila y aumento del elegido: swapshelf.ini (por_fila, aumento) */
static int g_gridCols = 7;
static float g_gridZoom = 1.3f;
#define GRID_COLS g_gridCols
static int g_gridSel;
static float g_gridScroll;
static int g_gridList[4096], g_gridN;

/* Juego elegido en la vista actual (-1 si no hay) */
static int currentGame(void) {
	if (g_libTab == 0) {
		if (g_homeRow >= g_rowN) return -1;
		struct HomeRow* r = &g_rows[g_homeRow];
		if (r->kind == ROW_CONSOLES || !r->n) return -1;
		return r->idx[g_homeCol[g_homeRow]];
	}
	return g_gridN ? g_gridList[g_gridSel] : -1;
}

static bool g_gridHas[27];  /* letras con juegos, para la barra A-Z */

static void refreshLists(void) {
	countSystems();
	if (g_libTab == 0) buildHome();
	else {
		g_gridN = consoleList(g_libTab - 1, g_gridList, 4096);
		if (g_gridSel >= g_gridN) g_gridSel = g_gridN ? g_gridN - 1 : 0;
		memset(g_gridHas, 0, sizeof(g_gridHas));
		for (int i = 0; i < g_gridN; ++i) { char L = gameLetter(&g_lib[g_gridList[i]]); g_gridHas[L == '#' ? 0 : L - 'A' + 1] = true; }
	}
}

static void ensureLabel(struct App* a, struct LibGame* g, float t) {
	(void) t;
	wantLabel(a, g);
}

/* ---------- dibujo ---------- */
static void drawHeaderTabs(struct App* a, const char* sub) {
	int bw0 = drawBrand(a, F_ITEM, 0.62f, 40, 22, TH.text, 255, AL_LEFT);
	text(a, F_SMALL, 0.6f, sub, 40 + bw0 + 14, 32, TH.dim, 255, AL_LEFT);
	char st[96] = "";
	if (g_metaLock && SDL_AtomicGet(&g_metaBusy) != 0) { SDL_LockMutex(g_metaLock); snprintf(st, sizeof(st), "%s", g_metaStatus); SDL_UnlockMutex(g_metaLock); }
	if (!st[0] && SDL_AtomicGet(&g_coverBusy) > 0) snprintf(st, sizeof(st), "%s", T("Preparando car\xC3\xA1tulas..."));
	if (!st[0] && g_appletMode) snprintf(st, sizeof(st), "%s", T("Modo applet: rendimiento limitado (abre con R sobre un juego)"));
	if (st[0]) text(a, F_SMALL, 0.55f, st, LIB_W - 40, 32, TH.dim, 255, AL_RIGHT);
	float x = 40;
	for (int k = 0; k <= SYS_COUNT; ++k) {
		if (k && !sysVisible(k - 1) && g_libTab != k) continue;  /* consola oculta o sin juegos */
		const char* name = k == 0 ? T("Inicio") : SYS_NAME[k - 1];
		int tw = textW(F_SMALL, name, 0.6f);
		bool on = g_libTab == k;
		fillRR(a, x - 14, 70, tw + 28 + (k ? 14 : 0), 34, 17, on ? (k ? SYS_COLOR[k - 1] : TH.acc) : TH.panel, 255);
		if (k && !on) fillRR(a, x - 4, 83, 8, 8, 4, SYS_COLOR[k - 1], 255);
		text(a, F_SMALL, 0.6f, name, (int) (x + (k ? 12 : 0)), 76, on ? 0x0A0C10 : TH.text2, 255, AL_LEFT);
		x += tw + 50 + (k ? 14 : 0);
	}
	/* acceso a buscar */
#ifdef __SWITCH__
	const char* key = "-";  /* botón − (la fuente no tiene el signo menos) */
#else
	const char* key = "Ctrl+F";
#endif
	int kw = textW(F_SMALL, key, 0.5f) + 14, bw = textW(F_SMALL, T("Buscar"), 0.58f) + kw + 46;
	fillRR(a, LIB_W - 40 - bw, 70, bw, 34, 17, TH.panel, 255);
	text(a, F_SMALL, 0.58f, T("Buscar"), LIB_W - 40 - bw + 18, 76, TH.text2, 255, AL_LEFT);
	fillRR(a, LIB_W - 40 - kw - 10, 76, kw, 22, 6, TH.panel3, 255);
	text(a, F_SMALL, 0.5f, key, LIB_W - 40 - kw / 2 - 10, 78, TH.text2, 255, AL_CENTER);
}

static void drawEmpty(struct App* a, int sy) {
	char dir[1024], msg[200], ttl[120];
	resolvePath(dir, sizeof(dir), a->cfg.romDir);
	if (sy < 0) {
		snprintf(ttl, sizeof(ttl), "%s", T("Tu biblioteca est\xC3\xA1 vac\xC3\xAD" "a"));
		snprintf(msg, sizeof(msg), "%s", T("Copia tus juegos en las carpetas de cada consola (snes, genesis, gba, gbc, gb, nes) dentro de:"));
	} else {
		snprintf(ttl, sizeof(ttl), T("Todav\xC3\xAD" "a no hay juegos de %s"), SYS_NAME[sy]);
		snprintf(msg, sizeof(msg), T("Copia tus juegos de %s (%s) en esta carpeta:"), SYS_NAME[sy], SYS_EXTS[sy]);
		size_t dl = strlen(dir);
		snprintf(dir + dl, sizeof(dir) - dl, "/%s", SYS_DIR[sy]);
	}
	fillRR(a, 40, 150, LIB_W - 80, 200, 18, TH.panel, 255);
	text(a, F_ITEM, 0.7f, ttl, 70, 180, TH.text, 255, AL_LEFT);
	textFit(a, F_SMALL, 0.6f, msg, 70, 232, LIB_W - 140, TH.dim);
	textFit(a, F_SMALL, 0.62f, dir, 70, 262, LIB_W - 140, TH.acct);
	text(a, F_SMALL, 0.55f, T("Solo tus propias copias de seguridad: SwapShelf no incluye juegos."), 70, 302, TH.dim, 255, AL_LEFT);
}

/* Franja con el juego elegido */
static void drawInfoStrip(struct App* a, const struct LibGame* g, int y) {
	fillRR(a, 40, y, LIB_W - 80, 54, 16, TH.panel, 255);
	int nw = textW(F_SMALL, g->name, 0.7f);
	if (nw > 520) nw = 520;
	textFit(a, F_SMALL, 0.7f, g->name, 60, y + 13, 520, TH.text);
	int bx = 60 + nw + 16, bw = textW(F_SMALL, SYS_NAME[g->sys], 0.5f) + 22;
	fillRR(a, bx, y + 14, bw, 26, 13, SYS_COLOR[g->sys], 255);
	text(a, F_SMALL, 0.5f, SYS_NAME[g->sys], bx + 11, y + 17, 0x141008, 255, AL_LEFT);
	char info[260], ago[48] = "";
	struct Played* p = playedFind(g->crc, false);
	if (p && p->count) {
		agoText(p->last, ago, sizeof(ago));
		char pt[48];
		playTimeText(p->secs, pt, sizeof(pt));
		snprintf(info, sizeof(info), T("%s  \xC2\xB7  jugado %d %s  \xC2\xB7  %s  \xC2\xB7  \xC3\xBAltimo: %s"), g->official[0] ? g->official : (g->note[0] ? g->note : T("Sin nombre oficial")), p->count, p->count == 1 ? T("vez") : T("veces"), pt, ago);
	} else snprintf(info, sizeof(info), T("%s  \xC2\xB7  sin jugar"), g->official[0] ? g->official : (g->note[0] ? g->note : T("Sin nombre oficial")));
	int ix = bx + bw + 16;
	bool fav = p && p->fav;
	textFit(a, F_SMALL, 0.5f, info, ix, y + 18, LIB_W - 60 - ix - (fav ? 110 : 0), TH.dim);
	if (fav) text(a, F_SMALL, 0.52f, T("Favorito"), LIB_W - 60, y + 17, TH.acct, 255, AL_RIGHT);
}

static void drawHints(struct App* a) {
#ifdef __SWITCH__
	const char* hint = g_libTab == 0
		? T("A: Jugar    Y: Men\xC3\xBA del juego    X: Favorito    -: Buscar    L / R: Consola")
		: T("A: Jugar    Y: Men\xC3\xBA    X: Favorito    -: Buscar    +: Orden    L / R: Consola    ZL / ZR: Letra");
#else
	const char* hint = g_libTab == 0
		? T("Enter: Jugar   M: Men\xC3\xBA del juego   F: Favorito   Ctrl+F: Buscar   Q / E: Consola   Esc: Salir")
		: T("Enter: Jugar   M: Men\xC3\xBA   F: Favorito   Ctrl+F: Buscar   O: Orden   Q / E: Consola   RePag / AvPag: Letra");
#endif
	text(a, F_SMALL, 0.52f, hint, 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
	if (a->message[0] && SDL_GetTicks() < a->messageUntil) {
		int mw = textW(F_SMALL, a->message, 0.58f) + 40;
		fillRR(a, (LIB_W - mw) / 2.0f, 664, mw, 40, 20, 0xE0A030, 255);
		text(a, F_SMALL, 0.58f, a->message, LIB_W / 2, 672, 0x14100A, 255, AL_CENTER);
	}
}

/* Una fila horizontal del inicio */
static void drawHomeRow(struct App* a, int ri, float y, float sc, float t) {
	struct HomeRow* r = &g_rows[ri];
	static float scroll[4];
	bool rowOn = ri == g_homeRow;
	const char* ttl = r->kind == ROW_RECENT ? T("Seguir jugando") : r->kind == ROW_FAV ? T("Favoritos") : r->kind == ROW_ALL ? T("Todos los juegos") : T("Consolas");
	const char* sub = r->kind == ROW_RECENT ? T("los \xC3\xBAltimos 10, el m\xC3\xA1s reciente primero") : r->kind == ROW_FAV ? T("marcados con X") : r->kind == ROW_ALL ? T("de la A a la Z") : "";
	text(a, F_SMALL, 0.66f, ttl, 40, (int) y, rowOn ? TH.text : TH.text2, 255, AL_LEFT);
	if (sub[0]) text(a, F_SMALL, 0.5f, sub, 40 + textW(F_SMALL, ttl, 0.66f) + 12, (int) y + 4, TH.dim2, 255, AL_LEFT);
	/* las filas de juegos dejan más aire bajo el título (el elegido brilla y crece) */
	float top = y + (r->kind == ROW_CONSOLES ? 30 : 44);
	if (r->kind == ROW_CONSOLES) {
		const int tiles = SYS_COUNT + 1;  /* mismo ancho siempre, aunque haya consolas ocultas */
		float w = (LIB_W - 80 - (tiles - 1) * 12) / (float) tiles;
		for (int t = 0; t < r->n - 1; ++t) {
			int k = r->idx[t];
			float x = 40 + t * (w + 12);
			bool on = rowOn && g_homeCol[ri] == t;
			if (on) fillRR(a, x - 3, top - 3, w + 6, 66, 16, TH.acc, 255);
			fillRR(a, x, top, w, 60, 14, TH.panel, 255);
			fillRR(a, x + 14, top + 15, 10, 10, 5, SYS_COLOR[k], 255);
			text(a, F_SMALL, 0.6f, SYS_NAME[k], (int) x + 32, (int) top + 9, TH.text, 255, AL_LEFT);
			char c[32];
			snprintf(c, sizeof(c), T("%d juego%s"), g_sysCount[k], g_sysCount[k] == 1 ? "" : "s");
			text(a, F_SMALL, 0.5f, c, (int) x + 14, (int) top + 34, TH.dim, 255, AL_LEFT);
		}
		{
			float x = 40 + (r->n - 1) * (w + 12);
			bool on = rowOn && g_homeCol[ri] == r->n - 1;
			if (on) fillRR(a, x - 3, top - 3, w + 6, 66, 16, TH.acc, 255);
			fillRR(a, x, top, w, 60, 14, TH.panel3, 255);
			text(a, F_SMALL, 0.6f, T("Opciones"), (int) x + 14, (int) top + 9, TH.text, 255, AL_LEFT);
#ifdef __SWITCH__
			text(a, F_SMALL, 0.5f, T("ajustes y acerca de"), (int) x + 14, (int) top + 34, TH.dim, 255, AL_LEFT);
#else
			text(a, F_SMALL, 0.5f, T("tambi\xC3\xA9n con F2"), (int) x + 14, (int) top + 34, TH.dim, 255, AL_LEFT);
#endif
		}
		return;
	}
	if (!r->n) {
		fillRR(a, 40, top, LIB_W - 80, CARD_H * sc, 18, TH.panel, 255);
		text(a, F_ITEM, 0.6f, T("Todav\xC3\xAD" "a no jugaste nada"), 72, (int) (top + CARD_H * sc / 2 - 44), TH.text, 255, AL_LEFT);
		text(a, F_SMALL, 0.56f, T("Elige una consola abajo o busca un juego: los \xC3\xBAltimos 10 que juegues aparecen aqu\xC3\xAD."), 72, (int) (top + CARD_H * sc / 2 + 4), TH.dim, 255, AL_LEFT);
		return;
	}
	const float step = CARD_W * sc + 14;
	int sel = g_homeCol[ri];
	float target = sel * step - (LIB_W - 80 - CARD_W * sc) / 2;
	float maxScroll = r->n * step - (LIB_W - 80);
	if (target > maxScroll) target = maxScroll;
	if (target < 0) target = 0;
	scroll[ri] += (target - scroll[ri]) * 0.25f;
	for (int i = 0; i < r->n; ++i) {
		float cx = 40 + i * step - scroll[ri];
		if (cx < -CARD_W * sc || cx > LIB_W) continue;
		struct LibGame* g = &g_lib[r->idx[i]];
		ensureLabel(a, g, t);
		bool on = rowOn && i == sel;
		if (r->kind == ROW_RECENT) {
			char ago[48];
			struct Played* p = playedFind(g->crc, false);
			agoText(p ? p->last : 0, ago, sizeof(ago));
			text(a, F_SMALL, 0.46f, ago, (int) cx, (int) (top + CARD_H * sc + 12), TH.dim, 255, AL_LEFT);
		}
		if (on) continue;  /* el elegido va al final, más grande */
		drawCard(a, g, cx, top, sc, false, 0);
	}
	if (rowOn && sel < r->n) {
		static int zoomSel = -1, zoomRow = -1;
		static float zoom = 1.0f;
		if (zoomSel != sel || zoomRow != ri) { zoomSel = sel; zoomRow = ri; zoom = 1.0f; }
		zoom += (1.14f - zoom) * 0.3f;
		float s2 = sc * zoom, w2 = CARD_W * s2, h2 = CARD_H * s2;
		float cx = 40 + sel * step - scroll[ri] - (w2 - CARD_W * sc) / 2;
		float cy = top - (h2 - CARD_H * sc) / 2;
		if (cx < 12) cx = 12;
		if (cx + w2 > LIB_W - 12) cx = LIB_W - 12 - w2;
		if (cy < top - 4) cy = top - 4;  /* sin tapar el título de la fila */
		g_selX = cx; g_selY = cy; g_selScale = s2;
		drawCard(a, &g_lib[r->idx[sel]], cx, cy, s2, true, 0);
	}
}

static void drawHome(struct App* a, float t) {
	char sub[64];
	snprintf(sub, sizeof(sub), T("%d juego%s"), g_libN, g_libN == 1 ? "" : "s");
	drawHeaderTabs(a, sub);
	if (!g_libN) { drawEmpty(a, -1); drawHints(a); return; }
	/* filas que entran sobre la franja de información; la elegida siempre visible */
	float hgt[4];
	for (int ri = 0; ri < g_rowN; ++ri) {
		int k = g_rows[ri].kind;
		float sc = k == ROW_RECENT ? 1.0f : 0.7f;
		hgt[ri] = k == ROW_CONSOLES ? 104 : 44 + CARD_H * sc + (k == ROW_RECENT ? 46 : 22);
	}
	const float top = 126, bottom = 590;
	int first = g_homeRow;
	float used = hgt[g_homeRow];
	while (first > 0 && used + hgt[first - 1] <= bottom - top && first > g_homeRow - 1) { --first; used += hgt[first]; }
	float y = top;
	for (int ri = first; ri < g_rowN && y + hgt[ri] <= bottom + 1; ++ri) {
		float sc = g_rows[ri].kind == ROW_RECENT ? 1.0f : 0.7f;
		drawHomeRow(a, ri, y, sc, t);
		y += hgt[ri];
	}
	int gi = currentGame();
	if (gi >= 0) drawInfoStrip(a, &g_lib[gi], 598);
	drawHints(a);
}

static void drawGrid(struct App* a, float t) {
	int sys = g_libTab - 1;
	char sub[96];
	snprintf(sub, sizeof(sub), T("%s  \xC2\xB7  %d juego%s"), SYS_NAME[sys], g_gridN, g_gridN == 1 ? "" : "s");
	drawHeaderTabs(a, sub);
	if (!g_gridN) { drawEmpty(a, sys); drawHints(a); return; }
	/* orden */
#ifdef __SWITCH__
	const char* olab = T("Orden (+):");
#else
	const char* olab = T("Orden (O):");
#endif
	text(a, F_SMALL, 0.5f, olab, 40, 124, TH.dim, 255, AL_LEFT);
	float ox = 40 + textW(F_SMALL, olab, 0.5f) + 12;
	for (int k = 0; k < SORT_COUNT; ++k) {
		int w = textW(F_SMALL, T(SORT_NAME[k]), 0.5f) + 24;
		bool on = g_sort == k;
		fillRR(a, ox, 120, w, 26, 13, on ? TH.text : TH.panel, 255);
		text(a, F_SMALL, 0.5f, T(SORT_NAME[k]), (int) ox + 12, 123, on ? TH.bg : TH.text2, 255, AL_LEFT);
		ox += w + 8;
	}
	int rows = (g_gridN + GRID_COLS - 1) / GRID_COLS, selRow = g_gridSel / GRID_COLS;
	char rl[48];
	snprintf(rl, sizeof(rl), T("Fila %d de %d"), selRow + 1, rows);
	text(a, F_SMALL, 0.5f, rl, LIB_W - 80, 124, TH.dim, 255, AL_RIGHT);
	/* cuadrícula */
	/* el tamaño sale de cuántos entran por fila (con 7: 0,95) */
	float scFit = (LIB_W - 80 - 40 - (GRID_COLS - 1) * 24.0f) / (GRID_COLS * (float) CARD_W);
	const float sc = scFit > 1.3f ? 1.3f : scFit, cw = CARD_W * sc, ch = CARD_H * sc;
	const float gx = 40, gy = 176, areaW = LIB_W - 80 - 40, colStep = (areaW - cw) / (GRID_COLS - 1), rowStep = ch + 22;
	const float viewH = 590 - gy;
	float target = selRow * rowStep - (viewH - rowStep) / 2;
	float maxScroll = rows * rowStep - viewH;
	if (target > maxScroll) target = maxScroll;
	if (target < 0) target = 0;
	g_gridScroll += (target - g_gridScroll) * 0.25f;
	/* precarga: filas de arriba y de abajo (se piden antes, así lo visible va primero) */
	int firstRow = (int) (g_gridScroll / rowStep), lastRow = (int) ((g_gridScroll + viewH) / rowStep);
	for (int r = lastRow + 3; r >= firstRow - 2; --r) {
		if (r < 0 || (r >= firstRow && r <= lastRow)) continue;
		for (int c = 0; c < GRID_COLS; ++c) {
			int i = r * GRID_COLS + c;
			if (i < g_gridN) wantLabel(a, &g_lib[g_gridList[i]]);
		}
	}
	/* aumento suave del cartucho elegido para leerlo mejor */
	static int zoomSel = -1;
	static float zoom = 1.0f;
	if (zoomSel != g_gridSel) { zoomSel = g_gridSel; zoom = 1.0f; }
	zoom += (g_gridZoom - zoom) * 0.3f;
	SDL_Rect clip = { 0, (int) gy - 16, LIB_W, (int) viewH + 16 };
	SDL_RenderSetClipRect(a->renderer, &clip);
	for (int i = 0; i < g_gridN; ++i) {
		int r = i / GRID_COLS, c = i % GRID_COLS;
		float x = gx + c * colStep, y = gy + r * rowStep - g_gridScroll;
		if (y + ch < gy - 10 || y > gy + viewH) continue;
		struct LibGame* g = &g_lib[g_gridList[i]];
		ensureLabel(a, g, t);
		if (i == g_gridSel) continue;  /* se dibuja al final, por encima */
		drawCard(a, g, x, y, sc, false, 0);
		if (isFav(g->crc)) fillRR(a, x + cw - 16, y + 4, 10, 10, 5, TH.acc, 255);
	}
	SDL_RenderSetClipRect(a->renderer, NULL);
	{
		int r = g_gridSel / GRID_COLS, c = g_gridSel % GRID_COLS;
		float s2 = sc * zoom, w2 = CARD_W * s2, h2 = CARD_H * s2;
		float x = gx + c * colStep - (w2 - cw) / 2, y = gy + r * rowStep - g_gridScroll - (h2 - ch) / 2;
		if (x < 12) x = 12;
		if (x + w2 > LIB_W - 70) x = LIB_W - 70 - w2;
		if (y < 178) y = 178;  /* el brillo no llega a la barra de orden */
		if (y + h2 > 590) y = 590 - h2;  /* ni la franja de abajo */
		struct LibGame* g = &g_lib[g_gridList[g_gridSel]];
		g_selX = x; g_selY = y; g_selScale = s2;
		drawCard(a, g, x, y, s2, true, 0);
		if (isFav(g->crc)) fillRR(a, x + w2 - 20, y + 6, 12, 12, 6, TH.acc, 255);
	}
	/* con cuadros libres se arman tarjetas de la fila siguiente */
	for (int i = (lastRow + 1) * GRID_COLS; i < g_gridN && i < (lastRow + 2) * GRID_COLS && g_cardBudget > 0; ++i) {
		struct LibGame* g = &g_lib[g_gridList[i]];
		if (!g->card && g->label) { g->card = buildCard(a, g); --g_cardBudget; g->usedAt = SDL_GetTicks(); }
	}
	/* barra de letras */
	bool* has = g_gridHas;
	char cur = gameLetter(&g_lib[g_gridList[g_gridSel]]);
	fillRR(a, LIB_W - 64, 156, 24, 430, 12, TH.panel, 255);
	for (int k = 0; k < 27; ++k) {
		char s2[2] = { (char) (k ? 'A' + k - 1 : '#'), 0 };
		bool on = s2[0] == cur;
		text(a, F_SMALL, 0.36f, s2, LIB_W - 52, 162 + k * 15, on ? TH.acct : (has[k] ? TH.text2 : TH.faint), 255, AL_CENTER);
	}
	drawInfoStrip(a, &g_lib[g_gridList[g_gridSel]], 598);
	drawHints(a);
}

static void drawLibrary(struct App* a, float t) {
	labelPump();
	SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
	themeClearColor(a);
	SDL_RenderClear(a->renderer);
	if (g_libTab == 0) drawHome(a, t);
	else drawGrid(a, t);
	if (g_libFadeFromWhite) {
		Uint32 e = SDL_GetTicks() - g_libFadeFromWhite;
		if (e < 400) fillRect2(a, 0, 0, LIB_W, LIB_H, 0xECFFF6, (Uint8) (255 - e * 255 / 400));
		else g_libFadeFromWhite = 0;
	}
}

/* Miniaturas del selector: se bajan en segundo plano mientras se elige.
 * Las imágenes vienen del CDN de TheGamesDB y no gastan consultas del cupo. */
struct PickThumbs {
	int n;
	char url[TGDB_CHOICES][700];
	uint8_t* rgb[TGDB_CHOICES];
	int w[TGDB_CHOICES], h[TGDB_CHOICES];
	int state[TGDB_CHOICES];   /* 0 bajando, 1 lista, -1 error */
	SDL_mutex* lock;
	SDL_atomic_t cancel;
};

static int pickThumbThread(void* p) {
	struct PickThumbs* t = p;
	for (int i = 0; i < t->n && !SDL_AtomicGet(&t->cancel); ++i) {
		size_t len = 0;
		const char* err = NULL;
		unsigned char* raw = netGet(t->url[i], &len, &err);
		int w = 0, h = 0;
		uint8_t* rgb = raw ? decodeImage(raw, len, &w, &h) : NULL;
		free(raw);
		SDL_LockMutex(t->lock);
		t->rgb[i] = rgb; t->w[i] = w; t->h[i] = h; t->state[i] = rgb ? 1 : -1;
		SDL_UnlockMutex(t->lock);
	}
	return 0;
}

/* Selector manual: Y sobre un juego permite elegir entre las coincidencias
 * de TheGamesDB antes de descargar la portada. */
static void runCoverPicker(struct App* a, struct LibGame* g) {
	if (!a->cfg.tgdbKey[0]) { setMessage(a, T("Agrega tu clave de TheGamesDB en swapshelf.ini")); return; }
	struct Meta* m = metaFind(g->crc);
	char query[160];
	snprintf(query, sizeof(query), "%s", m && m->dat == 'S' && m->nointro[0] ? m->nointro : g->name);
	struct TgdbChoice choices[TGDB_CHOICES];
	memset(choices, 0, sizeof(choices));
	const char* err = NULL;
	int r = tgdbChoices(a->cfg.tgdbKey, g->sys, query, choices, TGDB_CHOICES, &err);
	if (r == TG_KEY) { setMessage(a, T("TheGamesDB rechaz\xC3\xB3 la clave o se acab\xC3\xB3 el cupo")); return; }
	if (r == TG_NET) { char msg[180]; snprintf(msg, sizeof(msg), T("Sin conexi\xC3\xB3n con TheGamesDB (%s)"), err ? T(err) : T("error")); setMessage(a, msg); return; }
	if (r != TG_FOUND) { setMessage(a, T("No se encontraron portadas para este juego")); return; }
	int n = 0; while (n < TGDB_CHOICES && choices[n].id) ++n;
	struct PickThumbs* th = calloc(1, sizeof(*th));
	SDL_Thread* thr = NULL;
	if (th) {
		th->n = n;
		for (int i = 0; i < n; ++i) snprintf(th->url[i], sizeof(th->url[i]), "%s", choices[i].image);
		th->lock = SDL_CreateMutex();
		thr = th->lock ? SDL_CreateThread(pickThumbThread, "miniaturas", th) : NULL;
	}
	SDL_Texture* tex[TGDB_CHOICES] = { 0 };
	int sel = 0;
	bool chosen = false;
	Uint32 start = SDL_GetTicks();
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; break; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; break; }
		if (in == IN_BACK) break;
		if (in == IN_UP) sel = (sel + n - 1) % n;
		if (in == IN_DOWN) sel = (sel + 1) % n;
		if (in == IN_OK) { chosen = true; break; }
		/* miniaturas listas -> texturas */
		if (thr) {
			SDL_LockMutex(th->lock);
			for (int i = 0; i < n; ++i)
				if (th->state[i] == 1 && !tex[i]) {
					SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(th->rgb[i], th->w[i], th->h[i], 24, th->w[i] * 3, SDL_PIXELFORMAT_RGB24);
					if (sf) { tex[i] = SDL_CreateTextureFromSurface(a->renderer, sf); SDL_FreeSurface(sf); }
					if (tex[i]) SDL_SetTextureScaleMode(tex[i], SDL_ScaleModeLinear);
				}
			SDL_UnlockMutex(th->lock);
		}
		drawLibrary(a, 0.0f);
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		fillRect2(a, 0, 0, LIB_W, LIB_H, 0x080A10, 190);
		fillRR(a, 100, 80, 1080, 560, 24, TH.panel, 255);
		char ttl[220]; snprintf(ttl, sizeof(ttl), T("Elegir portada: %s"), g->name);
		textFit(a, F_ITEM, 0.72f, ttl, 140, 112, 1000, TH.text);
		text(a, F_SMALL, 0.52f, T("Solo resultados de esta consola, los lanzamientos oficiales primero"), 140, 158, TH.dim, 255, AL_LEFT);
		for (int i = 0; i < n; ++i) {
			int y = 204 + i * 44;
			bool on = i == sel;
			if (on) fillRR(a, 130, y - 6, 640, 38, 19, TH.acc, 255);
			char line[180];
			if (choices[i].year) snprintf(line, sizeof(line), "%s (%d)", choices[i].title, choices[i].year);
			else snprintf(line, sizeof(line), "%s", choices[i].title);
			const char* tag = choices[i].kind == 0 ? T("Oficial") : choices[i].kind == 1 ? T("Variante") : T("Hack u homebrew");
			Uint32 tagCol = on ? 0x08100C : (choices[i].kind == 0 ? TH.acc : choices[i].kind == 1 ? 0xE0A030 : TH.dim);
			textFit(a, F_SMALL, 0.6f, line, 154, y, 450, on ? 0x08100C : TH.text);
			text(a, F_SMALL, 0.48f, tag, 752, y + 3, tagCol, 255, AL_RIGHT);
		}
		/* vista previa de la elegida */
		const float px = 800, py = 196, pw = 340, ph = 400;
		fillRR(a, px, py, pw, ph, 16, TH.bg, 255);
		if (tex[sel]) {
			int tw = 1, tht = 1;
			SDL_QueryTexture(tex[sel], NULL, NULL, &tw, &tht);
			float s2 = (pw - 20) / tw < (ph - 20) / tht ? (pw - 20) / tw : (ph - 20) / tht;
			SDL_FRect d = { px + (pw - tw * s2) / 2, py + (ph - tht * s2) / 2, tw * s2, tht * s2 };
			SDL_RenderCopyF(a->renderer, tex[sel], NULL, &d);
		} else {
			int st = 0;
			if (thr) { SDL_LockMutex(th->lock); st = th->state[sel]; SDL_UnlockMutex(th->lock); }
			const char* msg = st < 0 ? T("No se pudo ver la imagen") : T("Cargando imagen...");
			if (!thr) msg = T("Sin vista previa");
			text(a, F_SMALL, 0.52f, msg, (int) (px + pw / 2), (int) (py + ph / 2 - 10), TH.dim2, 255, AL_CENTER);
			if (st == 0 && thr) {
				float k = ((SDL_GetTicks() - start) % 1200) / 1200.0f;
				fillRR(a, px + 60 + k * (pw - 180), py + ph / 2 + 24, 60, 6, 3, TH.acc, 255);
			}
		}
#ifdef __SWITCH__
		text(a, F_SMALL, 0.55f, T("A: Usar esta portada     B: Cancelar"), 140, 600, TH.dim, 255, AL_LEFT);
#else
		text(a, F_SMALL, 0.55f, T("Enter: Usar esta portada     Esc: Cancelar"), 140, 600, TH.dim, 255, AL_LEFT);
#endif
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot && SDL_GetTicks() - start > 2000) { shot = 1; testCapture(a, "selector_portada"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
	/* la imagen elegida ya bajada se reutiliza */
	uint8_t* rgb = NULL;
	int w = 0, h = 0;
	if (thr) {
		SDL_AtomicSet(&th->cancel, 1);
		SDL_WaitThread(thr, NULL);
		if (chosen && th->state[sel] == 1) { rgb = th->rgb[sel]; w = th->w[sel]; h = th->h[sel]; th->rgb[sel] = NULL; }
		for (int i = 0; i < n; ++i) free(th->rgb[i]);
		SDL_DestroyMutex(th->lock);
	}
	free(th);
	for (int i = 0; i < n; ++i) if (tex[i]) SDL_DestroyTexture(tex[i]);
	if (!chosen || a->quit) return;
	setMessage(a, T("Descargando la portada elegida..."));
	size_t len = 0;
	unsigned char* jpg = NULL;
	if (!rgb) {
		jpg = netGet(choices[sel].image, &len, &err);
		rgb = jpg ? decodeImage(jpg, len, &w, &h) : NULL;
	}
	char out[1024]; boxPath(out, sizeof(out), g->crc);
	bool ok = rgb && saveBoxArt(rgb, w, h, out);
	free(rgb); free(jpg);
	if (!ok) { setMessage(a, T("No se pudo descargar o leer esa portada")); return; }
	manualPickAdd(g->crc);  /* elegida a mano: no se reemplaza sola */
	SDL_LockMutex(g_metaLock);
	m = metaFind(g->crc);
	if (m) {
		m->box = 'S'; m->tgdbId = choices[sel].id;
		snprintf(m->title, sizeof(m->title), "%s", choices[sel].title);
		++m->rev; metaCommit();
	}
	SDL_UnlockMutex(g_metaLock);
	metaApply();
	setMessage(a, T("Portada elegida y guardada"));
}

/* ---------- búsqueda con teclado en pantalla ---------- */
static const char* const OSK_ROWS[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-.&" };
enum { OSK_COLS = 10, OSK_SPECIAL = 4 };  /* fila 4: Espacio, Borrar, Resultados */

static bool searchMatch(const struct LibGame* g, const char* q, int sysFilter, int* rank) {
	if (sysFilter >= 0 && g->sys != sysFilter) return false;
	if (!sysVisible(g->sys)) return false;  /* consola oculta en Opciones */
	if (!q[0]) return false;
	char n[200], o[200];
	normText(g->name, n, sizeof(n));
	normText(g->official, o, sizeof(o));
	const char* h = strstr(n, q);
	if (h) { *rank = h == n ? 0 : 1; return true; }
	if (strstr(o, q)) { *rank = 2; return true; }
	char sn[32];
	normText(SYS_NAME[g->sys], sn, sizeof(sn));
	if (!strcmp(sn, q)) { *rank = 3; return true; }
	return false;
}

static int g_searchRank[4096];
static int cmpSearch(const void* x, const void* y) {
	int ia = *(const int*) x, ib = *(const int*) y;
	if (g_searchRank[ia] != g_searchRank[ib]) return g_searchRank[ia] - g_searchRank[ib];
	return cmpAZ(x, y);
}

static int doSearch(const char* query, int sysFilter, int* out, int cap) {
	char q[128];
	normText(query, q, sizeof(q));
	int n = 0;
	for (int i = 0; i < g_libN && i < 4096 && n < cap; ++i) {
		int r = 9;
		if (searchMatch(&g_lib[i], q, sysFilter, &r)) { g_searchRank[i] = r; out[n++] = i; }
	}
	qsort(out, (size_t) n, sizeof(int), cmpSearch);
	return n;
}

/* Devuelve el índice del juego elegido o -1 */
static int runSearch(struct App* a) {
	char query[96] = "";
	int filter = -1;            /* -1 = todas las consolas */
	int kr = 1, kc = 0;         /* tecla enfocada */
	bool onResults = false;
	int sel = 0;
	float scroll = 0;
	static int res[4096];
	int nres = 0;
	bool dirty = true;
	Uint32 start = SDL_GetTicks();
	g_textSink = query;
	g_textSinkCap = sizeof(query);
	g_textSinkChanged = false;
	SDL_StartTextInput();
	int chosen = -1;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; break; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (g_textSinkChanged) { g_textSinkChanged = false; dirty = true; onResults = false; }
		if (in == IN_QUIT) { a->quit = true; break; }
		if (in == IN_BACK) {
			if (onResults) onResults = false;
			else break;
		}
		if (in == IN_PREV || in == IN_NEXT) {
			for (int tries = 0; tries <= SYS_COUNT; ++tries) {
				filter += in == IN_NEXT ? 1 : -1;
				if (filter >= SYS_COUNT) filter = -1;
				if (filter < -1) filter = SYS_COUNT - 1;
				if (filter < 0 || sysVisible(filter)) break;
			}
			dirty = true;
		}
		if (in == IN_ERASE || in == IN_MENU) {  /* Y en Switch o Retroceso en PC */
			size_t l = strlen(query);
			while (l > 0 && ((unsigned char) query[l - 1] & 0xC0) == 0x80) --l;  /* UTF-8 */
			if (l > 0) query[l - 1] = 0;
			dirty = true;
			onResults = false;
		}
		if (dirty) {
			nres = doSearch(query, filter, res, 4096);
			if (sel >= nres) sel = nres ? nres - 1 : 0;
			dirty = false;
		}
		if (in == IN_START && nres) {  /* + en Switch, Enter en PC */
			if (onResults) { chosen = res[sel]; break; }
			onResults = true;
		}
		if (onResults) {
			if (in == IN_LEFT && sel > 0) --sel;
			if (in == IN_RIGHT && sel < nres - 1) ++sel;
			if (in == IN_UP) onResults = false;
			if (in == IN_OK && nres) { chosen = res[sel]; break; }
		} else {
			if (in == IN_UP && kr > 0) { --kr; if (kr == 3 && kc > 2) kc = 2; }
			if (in == IN_DOWN) {
				if (kr < OSK_SPECIAL) { ++kr; if (kr == OSK_SPECIAL && kc > 2) kc = 2; }
				else if (nres) onResults = true;
			}
			int cols = kr == OSK_SPECIAL ? 3 : OSK_COLS;
			if (in == IN_LEFT) kc = (kc + cols - 1) % cols;
			if (in == IN_RIGHT) kc = (kc + 1) % cols;
			if (in == IN_OK) {
				size_t l = strlen(query);
				if (kr < OSK_SPECIAL) {
					if (l + 1 < sizeof(query)) { query[l] = (char) tolower(OSK_ROWS[kr][kc]); query[l + 1] = 0; }
					dirty = true;
				} else if (kc == 0) {
					if (l + 1 < sizeof(query) && l) { query[l] = ' '; query[l + 1] = 0; }
				} else if (kc == 1) {
					while (l > 0 && ((unsigned char) query[l - 1] & 0xC0) == 0x80) --l;
					if (l > 0) query[l - 1] = 0;
					dirty = true;
				} else if (nres) onResults = true;
			}
		}
		if (dirty) { nres = doSearch(query, filter, res, 4096); if (sel >= nres) sel = nres ? nres - 1 : 0; dirty = false; }
		musicPump(a);
		metaPump(a);
		/* dibujo */
		float t = (SDL_GetTicks() - start) / 1000.0f;
		labelPump();
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		int bw0 = drawBrand(a, F_ITEM, 0.62f, 40, 22, TH.text, 255, AL_LEFT);
		text(a, F_SMALL, 0.6f, T("Buscar"), 40 + bw0 + 14, 32, TH.dim, 255, AL_LEFT);
		/* caja de texto */
		fillRR(a, 38, 72, LIB_W - 76, 60, 18, onResults ? TH.panel3 : TH.acc, 255);
		fillRR(a, 40, 74, LIB_W - 80, 56, 16, TH.panel, 255);
		char shown[128];
		bool caret = ((int) (t * 2)) % 2 == 0 && !onResults;
		snprintf(shown, sizeof(shown), "%s%s", query, caret ? "|" : "");
		if (query[0]) text(a, F_ITEM, 0.62f, shown, 64, 86, TH.text, 255, AL_LEFT);
		else text(a, F_ITEM, 0.62f, caret ? T("|  Escribe parte del nombre") : T("   Escribe parte del nombre"), 64, 86, TH.dim2, 255, AL_LEFT);
		char cnt[96];
		if (query[0]) snprintf(cnt, sizeof(cnt), T("%d resultado%s"), nres, nres == 1 ? "" : "s");
		else snprintf(cnt, sizeof(cnt), "%s", T("busca en nombre, nombre oficial y consola"));
		text(a, F_SMALL, 0.5f, cnt, LIB_W - 64, 94, TH.dim, 255, AL_RIGHT);
		/* filtro por consola */
		float fx = 40;
		for (int k = -1; k < SYS_COUNT; ++k) {
			if (k >= 0 && !sysVisible(k)) continue;
			const char* nm = k < 0 ? T("Todas") : SYS_NAME[k];
			int w = textW(F_SMALL, nm, 0.52f) + 26;
			bool on = filter == k;
			fillRR(a, fx, 146, w, 28, 14, on ? (k < 0 ? TH.acc : SYS_COLOR[k]) : TH.panel, 255);
			text(a, F_SMALL, 0.52f, nm, (int) fx + 13, 149, on ? 0x0A0C10 : TH.text2, 255, AL_LEFT);
			fx += w + 8;
		}
		text(a, F_SMALL, 0.48f, T("L / R: filtrar por consola"), (int) fx + 10, 151, TH.dim2, 255, AL_LEFT);
		/* resultados */
		const float sc = 0.7f, step = CARD_W * sc + 14;
		if (nres) {
			float target = sel * step - (LIB_W - 80 - CARD_W * sc) / 2;
			float maxS = nres * step - (LIB_W - 80);
			if (target > maxS) target = maxS;
			if (target < 0) target = 0;
			scroll += (target - scroll) * 0.25f;
			for (int i = 0; i < nres; ++i) {
				float cx = 40 + i * step - scroll;
				if (cx < -CARD_W * sc || cx > LIB_W) continue;
				struct LibGame* g = &g_lib[res[i]];
				ensureLabel(a, g, t);
				bool on = onResults && i == sel;
				float s2 = on ? sc * 1.06f : sc, dx = on ? -(CARD_W * (s2 - sc)) / 2 : 0;
				if (on) { g_selX = cx + dx; g_selY = 186; g_selScale = s2; }
				drawCard(a, g, cx + dx, on ? 186 : 190, s2, on, 0);
			}
		} else if (query[0]) {
			text(a, F_SMALL, 0.6f, T("No hay juegos con ese nombre"), 40, 250, TH.dim, 255, AL_LEFT);
		}
		/* teclado */
		const float ky = 430, kw = (LIB_W - 80 - 9 * 8) / 10.0f, kh = 40;
		fillRR(a, 32, ky - 10, LIB_W - 64, 5 * (kh + 6) + 14, 16, TH.panel2, 255);
		for (int r = 0; r < OSK_SPECIAL; ++r)
			for (int c = 0; c < OSK_COLS; ++c) {
				bool on = !onResults && kr == r && kc == c;
				float x = 40 + c * (kw + 8), y = ky + r * (kh + 6);
				fillRR(a, x, y, kw, kh, 10, on ? TH.acc : TH.panel3, 255);
				char k[2] = { OSK_ROWS[r][c], 0 };
				text(a, F_SMALL, 0.62f, k, (int) (x + kw / 2), (int) y + 7, on ? 0x08100C : TH.text, 255, AL_CENTER);
			}
		{
			const char* sp[3] = { T("Espacio"), T("Borrar"), T("Ver resultados") };
			float w3 = (LIB_W - 80 - 16) / 3.0f;
			for (int c = 0; c < 3; ++c) {
				bool on = !onResults && kr == OSK_SPECIAL && kc == c;
				float x = 40 + c * (w3 + 8), y = ky + OSK_SPECIAL * (kh + 6);
				fillRR(a, x, y, w3, kh, 10, on ? TH.acc : TH.panel3, 255);
				text(a, F_SMALL, 0.55f, sp[c], (int) (x + w3 / 2), (int) y + 9, on ? 0x08100C : TH.text, 255, AL_CENTER);
			}
		}
#ifdef __SWITCH__
		const char* hint = onResults ? T("A: Jugar    Arriba: Teclado    B: Volver al teclado")
		                             : T("A: Escribir    Y: Borrar    +: Ver resultados    L / R: Consola    B: Salir");
#else
		const char* hint = onResults ? T("Enter: Jugar    Flechas: Elegir    Esc: Volver al teclado")
		                             : T("Escribe con el teclado    Retroceso: Borrar    Enter: Ver resultados    Q / E: Consola    Esc: Salir");
#endif
		text(a, F_SMALL, 0.52f, hint, 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot && strlen(query) >= 3) shot = 1; if (shot >= 1 && shot < 30) { if (++shot == 29) testCapture(a, onResults ? "buscar_resultados" : "buscar"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(12);
	}
	SDL_StopTextInput();
	g_textSink = NULL;
	return chosen;
}

/* Ficha con los datos del juego; se cierra con A o B */
static void runInfoBox(struct App* a, const struct LibGame* g) {
	char l[5][300], ago[48];
	snprintf(ago, sizeof(ago), "%s", T("nunca"));
	struct Played* p = playedFind(g->crc, false);
	if (p && p->count) agoText(p->last, ago, sizeof(ago));
	const char* file = strrchr(g->path, '/');
	file = file ? file + 1 : g->path;
	snprintf(l[0], sizeof(l[0]), T("Consola: %s"), SYS_NAME[g->sys]);
	snprintf(l[1], sizeof(l[1]), T("Nombre oficial: %s"), g->official[0] ? g->official : T("no identificado"));
	snprintf(l[2], sizeof(l[2]), T("Archivo: %s"), file);
	snprintf(l[3], sizeof(l[3]), "CRC32: %08X", (unsigned) g->crc);
	char pt[48];
	playTimeText(p ? p->secs : 0, pt, sizeof(pt));
	snprintf(l[4], sizeof(l[4]), T("Jugado %d %s (%s)  \xC2\xB7  \xC3\xBAltima vez: %s%s"), p ? p->count : 0, p && p->count == 1 ? T("vez") : T("veces"), pt, ago, p && p->fav ? T("  \xC2\xB7  favorito") : "");
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_OK || in == IN_BACK || in == IN_MENU) return;
		drawLibrary(a, 0.0f);
		fillRect2(a, 0, 0, LIB_W, LIB_H, 0x000000, 160);
		const float pw = 760, ph = 330, px = (LIB_W - pw) / 2, py = (LIB_H - ph) / 2;
		fillRR(a, px, py, pw, ph, 20, TH.panel, 255);
		fillRR(a, px, py, pw, 6, 3, SYS_COLOR[g->sys], 255);
		textFit(a, F_ITEM, 0.62f, g->name, (int) px + 32, (int) py + 28, (int) pw - 64, TH.text);
		for (int i = 0; i < 5; ++i) textFit(a, F_SMALL, 0.56f, l[i], (int) px + 32, (int) py + 90 + i * 36, (int) pw - 64, i == 3 ? TH.dim : TH.text2);
		text(a, F_SMALL, 0.5f, T("A / B: Cerrar"), (int) (px + pw - 32), (int) (py + ph - 40), TH.dim2, 255, AL_RIGHT);
		SDL_RenderPresent(a->renderer);
		SDL_Delay(12);
	}
}

/* ---------- menú del juego (Y) ---------- */
enum { GM_PLAY, GM_FAV, GM_COVER, GM_CONTROLS, GM_INFO, GM_RESCAN, GM_DELETE, GM_COUNT, GM_DELETED };

/* Devuelve GM_PLAY si hay que jugar, GM_RESCAN si hay que actualizar, -1 si nada */
static int runGameMenu(struct App* a, struct LibGame* g) {
	int sel = 0;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return -1; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return -1; }
		if (in == IN_BACK || in == IN_MENU) return -1;
		if (in == IN_UP) sel = (sel + GM_COUNT - 1) % GM_COUNT;
		if (in == IN_DOWN) sel = (sel + 1) % GM_COUNT;
		if (in == IN_FAV) { toggleFav(a, g); g_listsDirty = true; }
		if (in == IN_OK) {
			if (sel == GM_PLAY) return GM_PLAY;
			if (sel == GM_RESCAN) return GM_RESCAN;
			if (sel == GM_DELETE) {
				char body[300];
				snprintf(body, sizeof(body), "%s", T("Se borrar\xC3\xA1 el archivo de la ROM de la tarjeta. Tus partidas guardadas se conservan."));
				if (confirmDialog(a, T("\xC2\xBF" "Borrar este juego?"), body, T("Borrar"), NULL)) return GM_DELETED;
				if (a->quit) return -1;
			}
			if (sel == GM_FAV) { toggleFav(a, g); g_listsDirty = true; }
			if (sel == GM_COVER) { runCoverPicker(a, g); if (a->quit) return -1; }
			if (sel == GM_CONTROLS) { runControls(a, g->sys, g->crc, g->name, NULL); if (a->quit) return -1; }
			if (sel == GM_INFO) { runInfoBox(a, g); if (a->quit) return -1; }
		}
		musicPump(a);
		metaPump(a);
		drawLibrary(a, 0.0f);
		fillRect2(a, 0, 0, LIB_W, LIB_H, 0x000000, 150);
		const float pw = 520, ph = 96 + GM_COUNT * 52, px = (LIB_W - pw) / 2, py = (LIB_H - ph) / 2;
		fillRR(a, px, py, pw, ph, 20, TH.panel, 255);
		textFit(a, F_SMALL, 0.7f, g->name, (int) px + 28, (int) py + 22, (int) pw - 56, TH.text);
		text(a, F_SMALL, 0.5f, SYS_NAME[g->sys], (int) px + 28, (int) py + 54, SYS_COLOR[g->sys], 255, AL_LEFT);
		bool fav = isFav(g->crc);
		const char* items[GM_COUNT] = { T("Jugar"), fav ? T("Quitar de favoritos") : T("Agregar a favoritos"), T("Elegir portada"),
		                                T("Controles de este juego"), T("Informaci\xC3\xB3n"), T("Actualizar biblioteca y portadas"),
		                                T("Borrar juego") };
		{
			char cupo[120];
			tgdbAllowanceText(cupo, sizeof(cupo));
			if (cupo[0]) {
				char line[160];
				snprintf(line, sizeof(line), T("TheGamesDB: %s"), cupo);
				textFit(a, F_SMALL, 0.42f, line, (int) px + 28, (int) (py + ph + 10), (int) pw - 56, TH.dim);
			}
		}
		for (int i = 0; i < GM_COUNT; ++i) {
			float y = py + 88 + i * 52;
			bool on = i == sel;
			bool danger = i == GM_DELETE;
			if (on) fillRR(a, px + 16, y - 4, pw - 32, 46, 14, danger ? 0xE05050 : TH.acc, 255);
			text(a, F_SMALL, 0.62f, items[i], (int) px + 36, (int) y + 6, on ? 0x08100C : (danger ? 0xF07070 : TH.text), 255, AL_LEFT);
		}
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot) { shot = 1; testCapture(a, "menu_juego"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(12);
	}
}

/* ---------- Opciones (lo mismo que swapshelf.ini, desde la app) ---------- */
enum OptId { OPT_THEME, OPT_ACCENT, OPT_CONSOLES, OPT_COLS, OPT_ZOOM, OPT_COVERS, OPT_RESUME, OPT_SCALE, OPT_BORDER, OPT_VOLUME, OPT_MUSIC, OPT_SWAPAB, OPT_TURBO,
#ifndef __SWITCH__
             OPT_FULLSCREEN,
#endif
             OPT_LANG, OPT_UPDATE, OPT_ROMS, OPT_RESCAN, OPT_ABOUT, OPT_COUNT };

static void optValue(struct App* a, int id, char* out, size_t n) {
	const struct Settings* c = &a->cfg;
	switch (id) {
	case OPT_THEME: snprintf(out, n, "%s", c->lightTheme ? T("Claro") : T("Oscuro")); break;
	case OPT_CONSOLES: {
		int vis = 0;
		countSystems();
		for (int k = 0; k < SYS_COUNT; ++k) vis += sysVisible(k);
		snprintf(out, n, T("%d de %d"), vis, SYS_COUNT);
		break;
	}
	case OPT_ACCENT: snprintf(out, n, "%s", T(ACC_NAME[c->accent])); break;
	case OPT_COLS: snprintf(out, n, "%d", c->gridCols); break;
	case OPT_ZOOM: snprintf(out, n, "%d %%", c->gridZoom); break;
	case OPT_RESUME: snprintf(out, n, "%s", c->resume ? T("S\xC3\xAD") : T("No")); break;
	case OPT_UPDATE: snprintf(out, n, "%s", c->autoUpdate ? T("S\xC3\xAD") : T("No")); break;
	case OPT_COVERS: snprintf(out, n, "%s", c->boxArt ? T("Caja") : T("Pantalla de t\xC3\xADtulo")); break;
	case OPT_SCALE: snprintf(out, n, "%s", c->scale == SCALE_INTEGER ? T("Entero") : c->scale == SCALE_STRETCH ? T("Estirar") : T("Ajustar")); break;
	case OPT_BORDER: snprintf(out, n, "%s", c->border == BORDER_BLUR ? T("Difuminado") : c->border == BORDER_BLACK ? T("Negro") : T("Tema")); break;
	case OPT_VOLUME: snprintf(out, n, "%d %%", c->volume); break;
	case OPT_SWAPAB: snprintf(out, n, "%s", c->swapAB ? T("S\xC3\xAD") : T("No")); break;
	case OPT_TURBO: snprintf(out, n, "x%d", c->turbo); break;
#ifndef __SWITCH__
	case OPT_FULLSCREEN: snprintf(out, n, "%s", (SDL_GetWindowFlags(a->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? T("S\xC3\xAD") : T("No")); break;
#endif
	case OPT_LANG: snprintf(out, n, "%s", LANG_NAME[g_lang]); break;
	case OPT_MUSIC: snprintf(out, n, "%d %%", c->musicVolume); break;
	case OPT_ROMS: snprintf(out, n, "%s", c->romDir); break;
	default: out[0] = 0;
	}
}

static const char* optLabel(int id) {
	switch (id) {
	case OPT_THEME: return T("Tema de los men\xC3\xBAs");
	case OPT_CONSOLES: return T("Consolas visibles");
	case OPT_ACCENT: return T("Color de los men\xC3\xBAs");
	case OPT_COLS: return T("Cartuchos por fila");
	case OPT_ZOOM: return T("Aumento del elegido");
	case OPT_RESUME: return T("Seguir donde lo dej\xC3\xA9");
	case OPT_COVERS: return T("Imagen del cartucho");
	case OPT_SCALE: return T("Tama\xC3\xB1o del juego");
	case OPT_BORDER: return T("Costados del juego");
	case OPT_VOLUME: return T("Volumen de los juegos");
	case OPT_SWAPAB: return T("Intercambiar A y B");
	case OPT_TURBO: return T("Velocidad del turbo");
#ifndef __SWITCH__
	case OPT_FULLSCREEN: return T("Pantalla completa");
#endif
	case OPT_LANG: return T("Idioma");
	case OPT_UPDATE: return T("Buscar actualizaciones");
	case OPT_MUSIC: return T("Volumen de la m\xC3\xBAsica");
	case OPT_ROMS: return T("Carpeta de juegos");
	case OPT_RESCAN: return T("Actualizar biblioteca y portadas");
	case OPT_ABOUT: return T("Acerca de SwapShelf");
	default: return "";
	}
}

static const char* optHelp(int id) {
	switch (id) {
	case OPT_CONSOLES: return T("Elige qu\xC3\xA9 consolas aparecen arriba y en el inicio. Las que no tienen juegos se ocultan solas.");
	case OPT_THEME: return T("Oscuro para jugar de noche o en la tele; claro para lugares con mucha luz.");
	case OPT_ACCENT: return T("Color de la selecci\xC3\xB3n y los botones. Cian combina con la S del logo.");
	case OPT_COLS: return T("Menos por fila = cartuchos m\xC3\xA1s grandes en cada consola (5 a 9).");
	case OPT_ZOOM: return T("Cu\xC3\xA1nto se agranda el cartucho donde est\xC3\xA1s, para leerlo mejor.");
	case OPT_RESUME: return T("Al volver a abrir un juego, sigue desde el punto exacto donde saliste.");
	case OPT_COVERS: return T("Portada de la caja (TheGamesDB) o la pantalla de t\xC3\xADtulo del juego.");
	case OPT_SCALE: return T("Ajustar mantiene la forma; Entero usa p\xC3\xADxeles exactos; Estirar llena la pantalla.");
	case OPT_BORDER: return T("Qu\xC3\xA9 se ve a los lados de la imagen del juego.");
	case OPT_VOLUME: return T("Volumen del sonido de los juegos.");
	case OPT_SWAPAB: return T("Cambia la funci\xC3\xB3n de A y B en los men\xC3\xBAs (aceptar y volver).");
	case OPT_TURBO: return T("Cu\xC3\xA1ntas veces m\xC3\xA1s r\xC3\xA1pido va el juego mientras mantienes el turbo.");
#ifndef __SWITCH__
	case OPT_FULLSCREEN: return T("Ventana o pantalla completa (tambi\xC3\xA9n con F11).");
#endif
	case OPT_MUSIC: return T("M\xC3\xBAsica del men\xC3\xBA (la pones con musica_menu= en swapshelf.ini). Se aplica al volver a abrir.");
	case OPT_ROMS: return T("D\xC3\xB3nde busca tus juegos, con una carpeta por consola. Se cambia en swapshelf.ini (carpeta_roms=).");
	case OPT_RESCAN: return T("Vuelve a revisar las carpetas y busca nombres y portadas que falten.");
	case OPT_LANG: return T("Idioma de los men\xC3\xBAs de SwapShelf.");
	case OPT_UPDATE: return T("Al abrir, revisa en GitHub si hay una versi\xC3\xB3n nueva y te pregunta antes de instalarla.");
	case OPT_ABOUT: return T("Versi\xC3\xB3n, autor, licencia y cr\xC3\xA9" "ditos.");
	default: return "";
	}
}

/* Cambia una opción (dir = -1 / +1), la aplica y la guarda en el ini */
static void optChange(struct App* a, int id, int dir) {
	struct Settings* c = &a->cfg;
	char v[32];
	switch (id) {
	case OPT_THEME:
		c->lightTheme = !c->lightTheme;
		themeApply(c->lightTheme, c->accent);
		iniSet("tema", c->lightTheme ? "claro" : "oscuro");
		break;
	case OPT_ACCENT:
		c->accent = (c->accent + (dir > 0 ? 1 : ACC_COUNT - 1)) % ACC_COUNT;
		themeApply(c->lightTheme, c->accent);
		iniSet("color", ACC_KEY[c->accent]);
		break;
	case OPT_COLS:
		c->gridCols = clampi(c->gridCols + dir, 5, 9);
		g_gridCols = c->gridCols; g_gridScroll = 0; g_listsDirty = true;
		snprintf(v, sizeof(v), "%d", c->gridCols); iniSet("por_fila", v);
		break;
	case OPT_ZOOM:
		c->gridZoom = clampi(c->gridZoom + dir * 10, 100, 170);
		g_gridZoom = c->gridZoom / 100.0f;
		snprintf(v, sizeof(v), "%d", c->gridZoom); iniSet("aumento", v);
		break;
	case OPT_RESUME: c->resume = !c->resume; iniSet("continuar", c->resume ? "si" : "no"); break;
	case OPT_UPDATE: c->autoUpdate = !c->autoUpdate; iniSet("actualizar", c->autoUpdate ? "si" : "no"); break;
	case OPT_COVERS:
		c->boxArt = !c->boxArt;
		g_preferBox = c->boxArt;
		iniSet("portadas", c->boxArt ? "caja" : "pantalla");
		for (int i = 0; i < g_libN; ++i) {  /* volver a cargar las imágenes */
			struct LibGame* g = &g_lib[i];
			if (g->card) { SDL_DestroyTexture(g->card); g->card = NULL; }
			if (g->label) { SDL_FreeSurface(g->label); g->label = NULL; }
			g->labelChecked = false;
		}
		break;
	case OPT_SCALE:
		c->scale = (c->scale + (dir > 0 ? 1 : 2)) % 3;
		iniSet("escala", c->scale == SCALE_INTEGER ? "entero" : c->scale == SCALE_STRETCH ? "estirar" : "ajustar");
		break;
	case OPT_BORDER:
		c->border = (c->border + (dir > 0 ? 1 : 2)) % 3;
		iniSet("borde", c->border == BORDER_BLUR ? "difuminado" : c->border == BORDER_BLACK ? "negro" : "tema");
		break;
	case OPT_VOLUME:
		c->volume = clampi(c->volume + dir * 10, 0, 100);
		snprintf(v, sizeof(v), "%d", c->volume); iniSet("volumen", v);
		break;
	case OPT_SWAPAB: c->swapAB = !c->swapAB; iniSet("intercambiar_ab", c->swapAB ? "1" : "0"); break;
	case OPT_MUSIC:
		c->musicVolume = clampi(c->musicVolume + dir * 10, 0, 100);
		snprintf(v, sizeof(v), "%d", c->musicVolume); iniSet("volumen_musica", v);
		break;
	case OPT_TURBO:
		c->turbo = clampi(c->turbo + dir, 2, 10);
		snprintf(v, sizeof(v), "%d", c->turbo); iniSet("turbo", v);
		break;
#ifndef __SWITCH__
	case OPT_FULLSCREEN:
		toggleFullscreen(a);
		iniSet("pantalla_completa", (SDL_GetWindowFlags(a->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? "1" : "0");
		break;
#endif
	case OPT_LANG:
		g_lang = (g_lang + (dir > 0 ? 1 : LANG_COUNT - 1)) % LANG_COUNT;
		c->lang = g_lang;
		iniSet("idioma", LANG_CODE[g_lang]);
		break;
	default: break;
	}
}

static void runAbout(struct App* a) {
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_BACK || in == IN_OK) return;
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		drawBrand(a, F_TITLE, 0.7f, LIB_W / 2, 60, TH.text, 255, AL_CENTER);
		{
			char ver[96];
			snprintf(ver, sizeof(ver), T("Versi\xC3\xB3n %s  \xC2\xB7  por Zadark"), SWS_VERSION);
			text(a, F_SMALL, 0.62f, ver, LIB_W / 2, 150, TH.text2, 255, AL_CENTER);
		}
		text(a, F_SMALL, 0.54f, T("Lanzador de cartuchos homebrew, gratuito y sin fines comerciales."), LIB_W / 2, 186, TH.dim, 255, AL_CENTER);
		static const char* const L[] = {
			"Licencia: GNU GPL versi\xC3\xB3n 3 (archivo LICENSE). El c\xC3\xB3" "digo fuente acompa\xC3\xB1" "a cada versi\xC3\xB3n.",
			"SwapShelf no incluye juegos: usa solo copias de seguridad de cartuchos que tengas.",
			"",
			"N\xC3\xBA" "cleos de emulaci\xC3\xB3n:",
			"  GBA, GB y GBC: mGBA (libmgba), de endrift y colaboradores  \xC2\xB7  MPL 2.0",
			"  NES: FCEUmm  \xC2\xB7  GPLv2 o posterior",
			"  SNES: Supafaust (snes_faust de Mednafen)  \xC2\xB7  GPLv2 o posterior",
			"  Genesis: clownmdemu, de Clownacy  \xC2\xB7  AGPLv3",
			"",
			"Datos: nombres oficiales de No-Intro y portadas de TheGamesDB.net.",
			"Hecho con SDL2 (licencia zlib) y libnx / devkitPro en Nintendo Switch.",
		};
		for (size_t i = 0; i < sizeof(L) / sizeof(L[0]); ++i)
			text(a, F_SMALL, 0.54f, T(L[i]), 160, 250 + (int) i * 32, i == 3 ? TH.text : TH.text2, 255, AL_LEFT);
#ifdef __SWITCH__
		text(a, F_SMALL, 0.52f, T("A / B: Volver"), 160, LIB_H - 50, TH.dim, 255, AL_LEFT);
#else
		text(a, F_SMALL, 0.52f, T("Enter / Esc: Volver"), 160, LIB_H - 50, TH.dim, 255, AL_LEFT);
#endif
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot) { shot = 1; testCapture(a, "acerca_de"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
}

static void doRescan(struct App* a);

/* Mostrar u ocultar cada consola; se guarda en el ini como ocultar=gb,gbc */
static void runConsoles(struct App* a) {
	int sel = 0;
	countSystems();
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_BACK || in == IN_OPTIONS) return;
		if (in == IN_UP) sel = (sel + SYS_COUNT - 1) % SYS_COUNT;
		if (in == IN_DOWN) sel = (sel + 1) % SYS_COUNT;
		if (in == IN_OK || in == IN_LEFT || in == IN_RIGHT) {
			g_sysHidden ^= 1u << sel;
			a->cfg.sysHidden = g_sysHidden;
			char v[64] = "";
			for (int k = 0; k < SYS_COUNT; ++k)
				if (g_sysHidden & (1u << k)) { size_t l = strlen(v); snprintf(v + l, sizeof(v) - l, "%s%s", l ? "," : "", SYS_DIR[k]); }
			iniSet("ocultar", v);
			g_listsDirty = true;
			if (g_libTab && !sysVisible(g_libTab - 1)) g_libTab = 0;
		}
		labelPump();
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		int bw0 = drawBrand(a, F_ITEM, 0.62f, 40, 22, TH.text, 255, AL_LEFT);
		text(a, F_SMALL, 0.6f, T("Consolas visibles"), 40 + bw0 + 14, 32, TH.dim, 255, AL_LEFT);
		fillRR(a, 40, 90, 760, 52 * SYS_COUNT + 24, 20, TH.panel, 255);
		for (int k = 0; k < SYS_COUNT; ++k) {
			float y = 102 + k * 52;
			bool on = k == sel, hidden = (g_sysHidden & (1u << k)) != 0;
			if (on) fillRR(a, 50, y, 740, 46, 23, TH.acc, 255);
			fillRR(a, 74, y + 17, 12, 12, 6, SYS_COLOR[k], hidden ? 90 : 255);
			text(a, F_SMALL, 0.62f, SYS_NAME[k], 98, (int) y + 9, on ? 0x08100C : (hidden ? TH.dim : TH.text), 255, AL_LEFT);
			char c[48];
			snprintf(c, sizeof(c), T("%d juego%s"), g_sysCount[k], g_sysCount[k] == 1 ? "" : "s");
			text(a, F_SMALL, 0.5f, c, 300, (int) y + 13, on ? 0x08100C : TH.dim, 255, AL_LEFT);
			const char* st = hidden ? T("Oculta") : (g_sysCount[k] ? T("Visible") : T("Sin juegos: oculta"));
			text(a, F_SMALL, 0.56f, st, 766, (int) y + 11, on ? 0x08100C : (hidden || !g_sysCount[k] ? TH.dim : TH.acct), 255, AL_RIGHT);
		}
		fillRR(a, 830, 90, LIB_W - 870, 150, 20, TH.panel, 255);
		text(a, F_SMALL, 0.52f, T("Las consolas ocultas no aparecen"), 856, 116, TH.text2, 255, AL_LEFT);
		text(a, F_SMALL, 0.52f, T("arriba, en el inicio ni en Buscar."), 856, 144, TH.text2, 255, AL_LEFT);
		text(a, F_SMALL, 0.52f, T("Tus juegos y partidas no se tocan."), 856, 186, TH.dim, 255, AL_LEFT);
#ifdef __SWITCH__
		text(a, F_SMALL, 0.52f, T("A: Mostrar u ocultar    B: Volver"), 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
#else
		text(a, F_SMALL, 0.52f, T("Enter: Mostrar u ocultar    Esc: Volver"), 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
#endif
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot) { shot = 1; testCapture(a, "consolas_visibles"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
}

static void runOptions(struct App* a) {
	int sel = 0;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_BACK || in == IN_OPTIONS) return;
		if (in == IN_UP) sel = (sel + OPT_COUNT - 1) % OPT_COUNT;
		if (in == IN_DOWN) sel = (sel + 1) % OPT_COUNT;
		if (sel == OPT_ABOUT) { if (in == IN_OK) { runAbout(a); if (a->quit) return; } }
		else if (sel == OPT_CONSOLES) { if (in == IN_OK || in == IN_RIGHT) { runConsoles(a); if (a->quit) return; } }
		else if (sel == OPT_RESCAN) { if (in == IN_OK) { doRescan(a); return; } }
		else if (sel != OPT_ROMS) {
			if (in == IN_LEFT) optChange(a, sel, -1);
			if (in == IN_RIGHT || in == IN_OK) optChange(a, sel, +1);
		}
		labelPump();
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		int bw0 = drawBrand(a, F_ITEM, 0.62f, 40, 22, TH.text, 255, AL_LEFT);
		text(a, F_SMALL, 0.6f, T("Opciones"), 40 + bw0 + 14, 32, TH.dim, 255, AL_LEFT);
		const float px = 40, pw = 760;
		const int rowH = 30;
		fillRR(a, px, 76, pw, rowH * OPT_COUNT + 20, 20, TH.panel, 255);
		for (int i = 0; i < OPT_COUNT; ++i) {
			float y = 86 + i * rowH;
			bool on = i == sel;
			if (on) fillRR(a, px + 10, y, pw - 20, rowH - 2, 17, TH.acc, 255);
			Uint32 ink = on ? 0x08100C : TH.text;
			text(a, F_SMALL, 0.52f, optLabel(i), (int) px + 32, (int) y + 4, ink, 255, AL_LEFT);
			char v[64];
			optValue(a, i, v, sizeof(v));
			if (i == OPT_ABOUT || i == OPT_RESCAN) snprintf(v, sizeof(v), ">");
			if (i == OPT_CONSOLES) { size_t l = strlen(v); snprintf(v + l, sizeof(v) - l, "  >"); }
			if (strlen(v) > 30) { char t2[64]; snprintf(t2, sizeof(t2), "...%s", v + strlen(v) - 27); snprintf(v, sizeof(v), "%s", t2); }
			if (v[0]) {
				bool arrows = on && i != OPT_ABOUT && i != OPT_ROMS && i != OPT_RESCAN && i != OPT_CONSOLES;
				char shown[80];
				snprintf(shown, sizeof(shown), arrows ? "<  %s  >" : "%s", v);
				if (i == OPT_ACCENT) fillRR(a, px + pw - 52 - textW(F_SMALL, shown, 0.52f), y + 9, 14, 14, 7, TH.acc, 255);
				text(a, F_SMALL, 0.52f, shown, (int) (px + pw - 32), (int) y + 4, on ? 0x08100C : (i == OPT_ROMS ? TH.dim : TH.acct), 255, AL_RIGHT);
			}
		}
		/* explicación */
		fillRR(a, 830, 80, LIB_W - 870, 220, 20, TH.panel, 255);
		text(a, F_SMALL, 0.62f, optLabel(sel), 856, 104, TH.text, 255, AL_LEFT);
		{
			/* texto en varias líneas */
			const char* h = optHelp(sel);
			char line[160] = "";
			int y = 146;
			const int maxW = LIB_W - 870 - 52;
			while (*h) {
				const char* sp = strchr(h, ' ');
				size_t wl = sp ? (size_t) (sp - h) : strlen(h);
				char trial[200];
				snprintf(trial, sizeof(trial), "%s%s%.*s", line, line[0] ? " " : "", (int) wl, h);
				if (textW(F_SMALL, trial, 0.52f) > maxW && line[0]) {
					text(a, F_SMALL, 0.52f, line, 856, y, TH.text2, 255, AL_LEFT);
					y += 28;
					snprintf(line, sizeof(line), "%.*s", (int) wl, h);
				} else snprintf(line, sizeof(line), "%s", trial);
				h += wl;
				while (*h == ' ') ++h;
			}
			if (line[0]) text(a, F_SMALL, 0.52f, line, 856, y, TH.text2, 255, AL_LEFT);
		}
		text(a, F_SMALL, 0.48f, T("Los cambios se guardan solos en swapshelf.ini."), 856, 320, TH.dim2, 255, AL_LEFT);
#ifdef __SWITCH__
		text(a, F_SMALL, 0.52f, T("Arriba / Abajo: Elegir    Izquierda / Derecha o A: Cambiar    B: Volver"), 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
#else
		text(a, F_SMALL, 0.52f, T("Flechas: Elegir y cambiar    Enter: Cambiar    Esc o F2: Volver"), 40, LIB_H - 40, TH.dim, 255, AL_LEFT);
#endif
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot) { shot = 1; testCapture(a, "opciones"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
}

/* ---------- navegación ---------- */
static void letterJump(int dir) {
	if (!g_gridN) return;
	if (g_sort != SORT_AZ) {  /* el salto por letra usa el orden A-Z */
		int keep = g_gridList[g_gridSel];
		g_sort = SORT_AZ;
		refreshLists();
		for (int i = 0; i < g_gridN; ++i) if (g_gridList[i] == keep) g_gridSel = i;
	}
	char cur = gameLetter(&g_lib[g_gridList[g_gridSel]]);
	if (dir > 0) {
		for (int i = g_gridSel + 1; i < g_gridN; ++i)
			if (gameLetter(&g_lib[g_gridList[i]]) != cur) { g_gridSel = i; return; }
	} else {
		int i = g_gridSel;
		while (i > 0 && gameLetter(&g_lib[g_gridList[i - 1]]) == cur) --i;  /* inicio del grupo actual */
		if (i != g_gridSel) { g_gridSel = i; return; }
		if (i == 0) return;
		char prev = gameLetter(&g_lib[g_gridList[i - 1]]);
		while (i > 0 && gameLetter(&g_lib[g_gridList[i - 1]]) == prev) --i;
		g_gridSel = i;
	}
}

static void doRescan(struct App* a) {
	if (SDL_AtomicGet(&g_coverBusy) != 0 || SDL_AtomicGet(&g_metaBusy) != 0) {
		setMessage(a, T("La actualizaci\xC3\xB3n de portadas ya est\xC3\xA1 en curso"));
		return;
	}
	stopMeta(false);
	libScan(a);
	metaPrepare();
	metaApply();
	startCovers();
	startMeta(a);
	g_listsDirty = true;
	setMessage(a, a->cfg.tgdbKey[0] ? T("Buscando nombres y portadas...") : T("Agrega tu clave de TheGamesDB en swapshelf.ini para buscar portadas"));
}

/* Borra el archivo de la ROM y lo quita de la biblioteca (las partidas quedan) */
static void deleteGame(struct App* a, int gi) {
	struct LibGame* g = &g_lib[gi];
	char name[160];
	snprintf(name, sizeof(name), "%s", g->name);
	if (remove(g->path) != 0) {
		char m[240];
		snprintf(m, sizeof(m), T("No se pudo borrar %s (la tarjeta puede estar protegida)"), name);
		setMessage(a, m);
		return;
	}
	SDL_Log("Biblioteca: borrado %s", g->path);
	SDL_LockMutex(g_metaLock);
	if (g->card) SDL_DestroyTexture(g->card);
	if (g->label) SDL_FreeSurface(g->label);
	memmove(&g_lib[gi], &g_lib[gi + 1], sizeof(*g_lib) * (size_t) (g_libN - gi - 1));
	--g_libN;
	SDL_UnlockMutex(g_metaLock);
	libCacheSave();
	g_listsDirty = true;
	char m[240];
	snprintf(m, sizeof(m), T("%s: borrado"), name);
	setMessage(a, m);
}

static int launchGame(struct App* a, int gi) {
	struct LibGame* g = &g_lib[gi];
	mmFade(0.0f, 0.4f);
	runInsertAnim(a, g, g_selX, g_selY);
	if (a->quit) return -1;
	playedOnLaunch(g->crc);
	g_listsDirty = true;
	return gi;
}

/* Bucle de la biblioteca: devuelve el índice elegido o -1 para salir */
#include "updater.inc"

static int runLibrary(struct App* a) {
	startMenuMusic(a);
	{
		playedLoad();
		static bool warned = false;
		if (g_appletMode && !warned) {
			warned = true;
			setMessage(a, T("Modo applet: para todo el rendimiento abre SwapShelf manteniendo R sobre un juego"));
			a->messageUntil = SDL_GetTicks() + 9000;
		}
	}
	updStart(a);  /* una sola vez por sesión, en segundo plano */
	Uint32 start = SDL_GetTicks();
	g_listsDirty = true;
	g_gridCols = a->cfg.gridCols;
	g_sysHidden = a->cfg.sysHidden;
	g_gridZoom = a->cfg.gridZoom / 100.0f;
	{
		/* precarga en segundo plano: las primeras filas de cada consola, para que
		 * entrar a una categoría por primera vez no tenga que leer todo de golpe */
		static bool pre = false;
		if (!pre) {
			pre = true;
			static int lst[4096];
			for (int sys = SYS_COUNT - 1; sys >= 0; --sys) {
				int n = consoleList(sys, lst, 4096);
				for (int i = (n < GRID_COLS * 3 ? n : GRID_COLS * 3) - 1; i >= 0; --i) wantLabel(a, &g_lib[lst[i]]);
			}
		}
	}
#ifdef SWS_TEST
	{
		/* pruebas: SWS_TEST_LAUNCH=<parte del nombre de archivo> abre ese juego una vez */
		static bool launched = false;
		const char* want = getenv("SWS_TEST_LAUNCH");
		if (want && !launched)
			for (int i = 0; i < g_libN; ++i)
				if (strstr(g_lib[i].path, want)) { launched = true; playedOnLaunch(g_lib[i].crc); return i; }
	}
#endif
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return -1; }
#endif
		if (g_listsDirty) { g_listsDirty = false; refreshLists(); }
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return -1; }
#ifndef __SWITCH__
		/* En PC se pide confirmación; en Switch no hay opción de salir (se usa HOME) */
		if (in == IN_BACK) {
			if (confirmDialog(a, T("\xC2\xBFSalir de SwapShelf?"), T("Se cerrar\xC3\xA1 el programa. Tus partidas guardadas quedan en la carpeta Partidas."), T("Salir"), NULL)) {
				a->quit = true;
				return -1;
			}
			continue;
		}
#else
		/* en Switch, B en una consola vuelve al inicio */
		if (in == IN_BACK && g_libTab) { g_libTab = 0; g_listsDirty = true; continue; }
#endif
		int gi = currentGame();
		if (in == IN_SEARCH) {
			int r = runSearch(a);
			if (a->quit) return -1;
			if (r >= 0) return launchGame(a, r);
			continue;
		}
		if (in == IN_FAV && gi >= 0) { toggleFav(a, &g_lib[gi]); g_listsDirty = true; }
		if (in == IN_COVER && gi >= 0) { runCoverPicker(a, &g_lib[gi]); if (a->quit) return -1; continue; }
		if (in == IN_CONTROLS && gi >= 0) { runControls(a, g_lib[gi].sys, g_lib[gi].crc, g_lib[gi].name, NULL); if (a->quit) return -1; continue; }
		if (in == IN_MENU && gi >= 0) {
			int r = runGameMenu(a, &g_lib[gi]);
			if (a->quit) return -1;
			if (r == GM_PLAY) return launchGame(a, gi);
			if (r == GM_RESCAN) doRescan(a);
			if (r == GM_DELETED) deleteGame(a, gi);
			continue;
		}
		if (in == IN_RESCAN) doRescan(a);
		if (in == IN_OPTIONS) { runOptions(a); if (a->quit) return -1; continue; }
		if (in == IN_PREV || in == IN_NEXT) {
			countSystems();
			for (int tries = 0; tries <= SYS_COUNT; ++tries) {
				g_libTab = (g_libTab + (in == IN_NEXT ? 1 : SYS_COUNT)) % (SYS_COUNT + 1);
				if (g_libTab == 0 || sysVisible(g_libTab - 1)) break;
			}
			g_gridSel = 0;
			g_gridScroll = 0;
			g_listsDirty = true;
		}
		if (g_libTab == 0) {
			if (g_rowN) {
				struct HomeRow* r = &g_rows[g_homeRow];
				int* col = &g_homeCol[g_homeRow];
				if (in == IN_UP && g_homeRow > 0) --g_homeRow;
				if (in == IN_DOWN && g_homeRow < g_rowN - 1) ++g_homeRow;
				if (in == IN_LEFT && *col > 0) --*col;
				if (in == IN_RIGHT && *col < r->n - 1) ++*col;
				if (in == IN_OK) {
					if (r->kind == ROW_CONSOLES && r->idx[*col] < 0) { runOptions(a); if (a->quit) return -1; g_listsDirty = true; continue; }
					if (r->kind == ROW_CONSOLES) { g_libTab = r->idx[*col] + 1; g_gridSel = 0; g_gridScroll = 0; g_listsDirty = true; }
					else if (gi >= 0) return launchGame(a, gi);
				}
			}
		} else if (g_gridN) {
			if (in == IN_LEFT && g_gridSel > 0) --g_gridSel;
			if (in == IN_RIGHT && g_gridSel < g_gridN - 1) ++g_gridSel;
			if (in == IN_UP && g_gridSel >= GRID_COLS) g_gridSel -= GRID_COLS;
			if (in == IN_DOWN) g_gridSel = g_gridSel + GRID_COLS < g_gridN ? g_gridSel + GRID_COLS : g_gridN - 1;
			if (in == IN_LETTER_PREV) letterJump(-1);
			if (in == IN_LETTER_NEXT) letterJump(1);
			if (in == IN_START) {  /* + / O: cambiar el orden y mantener el juego elegido */
				int keep = g_gridList[g_gridSel];
				g_sort = (g_sort + 1) % SORT_COUNT;
				refreshLists();
				for (int i = 0; i < g_gridN; ++i) if (g_gridList[i] == keep) g_gridSel = i;
				char m[64];
				snprintf(m, sizeof(m), T("Orden: %s"), T(SORT_NAME[g_sort]));
				setMessage(a, m);
			}
			if (in == IN_OK) return launchGame(a, g_gridList[g_gridSel]);
		}
		musicPump(a);
		metaPump(a);
		updPump(a);
		if (a->quit) return -1;
		if (g_listsDirty) { g_listsDirty = false; refreshLists(); }
		drawLibrary(a, (SDL_GetTicks() - start) / 1000.0f);
#ifdef SWS_TEST
		{ static int lastTab = -1; if (lastTab != g_libTab) { char n[32]; snprintf(n, sizeof(n), "pestana_%d", g_libTab); lastTab = g_libTab; testCapture(a, n); } }
		{
			static int shot = 0, frames = 0;
			Uint32 el = SDL_GetTicks() - start;
			if (shot == 0 && el > 1500) { shot = 1; testCapture(a, "biblioteca"); }
			if (shot == 1 && el > 9000) { shot = 2; testCapture(a, "biblioteca_caratulas"); }
			if (++frames % 40 == 0) { char n[48]; snprintf(n, sizeof(n), "lib_t%d_%05d", g_libTab, frames); if (getenv("SWS_TEST_EVERY")) testCapture(a, n); }
		}
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(12);
	}
}

/* ================================================================== */
/* Controles: perfil global por consola + perfil por juego              */
/*  Cada consola tiene sus botones reales. Teclado y mando por separado.*/
/*  (Preparado para un perfil por jugador cuando lleguen SNES/Genesis.) */
/* ================================================================== */
enum CtlBtn { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_C, CB_X, CB_Y, CB_Z, CB_L, CB_R, CB_START, CB_SELECT, CB_COUNT };
static const char* const CB_NAME[CB_COUNT] = { "Arriba", "Abajo", "Izquierda", "Derecha", "A", "B", "C", "X", "Y", "Z", "L", "R", "Start", "Select" };
static const char* const CB_KEYNAME[CB_COUNT] = { "up", "down", "left", "right", "a", "b", "c", "x", "y", "z", "l", "r", "start", "select" };

/* Botones de cada consola, en orden de pantalla */
static const int SYS_BTNS[SYS_COUNT][CB_COUNT + 1] = {
	/* SNES */    { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_X, CB_Y, CB_L, CB_R, CB_START, CB_SELECT, -1 },
	/* Genesis */ { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_C, CB_X, CB_Y, CB_Z, CB_START, CB_SELECT, -1 },
	/* GBA */     { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_L, CB_R, CB_START, CB_SELECT, -1 },
	/* GBC */     { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_START, CB_SELECT, -1 },
	/* GB */      { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_START, CB_SELECT, -1 },
	/* NES */     { CB_UP, CB_DOWN, CB_LEFT, CB_RIGHT, CB_A, CB_B, CB_START, CB_SELECT, -1 },
};

static const char* btnLabel(int sys, int b) {
	if (sys == SYS_GEN && b == CB_SELECT) return "Mode";
	if (sys == SYS_GEN && (b == CB_X || b == CB_Y || b == CB_Z)) return b == CB_X ? T("X (6 botones)") : b == CB_Y ? T("Y (6 botones)") : T("Z (6 botones)");
	return T(CB_NAME[b]);
}

/* Mando: botones de SDL por POSICIÓN (abajo/derecha/izquierda/arriba), así sirve
 * igual para mandos de Nintendo, Xbox o PlayStation. -1 = sin asignar */
struct Controls { int key[CB_COUNT]; int pad[CB_COUNT]; };

static void ctlDefaults(struct Controls* c, int sys) {
	for (int i = 0; i < CB_COUNT; ++i) c->key[i] = c->pad[i] = -1;
	c->key[CB_UP] = SDL_SCANCODE_UP; c->key[CB_DOWN] = SDL_SCANCODE_DOWN;
	c->key[CB_LEFT] = SDL_SCANCODE_LEFT; c->key[CB_RIGHT] = SDL_SCANCODE_RIGHT;
	c->key[CB_START] = SDL_SCANCODE_RETURN; c->key[CB_SELECT] = SDL_SCANCODE_BACKSPACE;
	c->pad[CB_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP; c->pad[CB_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	c->pad[CB_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT; c->pad[CB_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	c->pad[CB_START] = SDL_CONTROLLER_BUTTON_START; c->pad[CB_SELECT] = SDL_CONTROLLER_BUTTON_BACK;
	/* posiciones físicas: B = derecha (east), A = abajo (south) en SDL sin etiquetas */
	const int EAST = SDL_CONTROLLER_BUTTON_B, SOUTH = SDL_CONTROLLER_BUTTON_A, WEST = SDL_CONTROLLER_BUTTON_X, NORTH = SDL_CONTROLLER_BUTTON_Y;
	if (sys == SYS_GEN) {
		c->key[CB_A] = SDL_SCANCODE_A; c->key[CB_B] = SDL_SCANCODE_S; c->key[CB_C] = SDL_SCANCODE_D;
		c->key[CB_X] = SDL_SCANCODE_Q; c->key[CB_Y] = SDL_SCANCODE_W; c->key[CB_Z] = SDL_SCANCODE_E;
		c->pad[CB_A] = WEST; c->pad[CB_B] = SOUTH; c->pad[CB_C] = EAST;
		c->pad[CB_X] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; c->pad[CB_Y] = NORTH; c->pad[CB_Z] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
		return;
	}
	c->key[CB_A] = SDL_SCANCODE_X; c->key[CB_B] = SDL_SCANCODE_Z;
	c->pad[CB_A] = EAST; c->pad[CB_B] = SOUTH;
	if (sys == SYS_SNES || sys == SYS_GBA) {
		c->key[CB_L] = SDL_SCANCODE_A; c->key[CB_R] = SDL_SCANCODE_S;
		c->pad[CB_L] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; c->pad[CB_R] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	}
	if (sys == SYS_SNES) {
		c->key[CB_X] = SDL_SCANCODE_D; c->key[CB_Y] = SDL_SCANCODE_C;
		c->pad[CB_X] = NORTH; c->pad[CB_Y] = WEST;
	}
}

static void ctlFile(char* out, size_t n, int sys, uint32_t crc, bool perGame) {
	char rel[96];
	if (perGame) snprintf(rel, sizeof(rel), "Controles/%08X.ini", (unsigned) crc);
	else snprintf(rel, sizeof(rel), "Controles/global_%s.ini", SYS_NAME[sys]);
	pathJoin(out, n, rel);
}

static bool ctlLoadFile(struct Controls* c, const char* path, int sys) {
	FILE* f = fopen(path, "r");
	if (!f) return false;
	ctlDefaults(c, sys);
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#') continue;
		*eq = 0;
		int kk = -1, pp = -1;
		if (sscanf(eq + 1, " teclado:%d mando:%d", &kk, &pp) < 1) continue;
		for (int i = 0; i < CB_COUNT; ++i)
			if (!strcmp(line, CB_KEYNAME[i])) { c->key[i] = kk; c->pad[i] = pp; }
	}
	fclose(f);
	return true;
}

static void ctlSaveFile(const struct Controls* c, const char* path, int sys, const char* who) {
	char d[1024];
	pathJoin(d, sizeof(d), "Controles");
	MKDIR(d);
	FILE* f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "# Controles de %s (%s)\n# boton = teclado:<scancode SDL> mando:<boton SDL por posicion> (-1 = sin asignar)\n", who, SYS_NAME[sys]);
	for (const int* b = SYS_BTNS[sys]; *b >= 0; ++b)
		fprintf(f, "%s= teclado:%d mando:%d\n", CB_KEYNAME[*b], c->key[*b], c->pad[*b]);
	fclose(f);
}

/* Controles activos de un juego: su perfil propio si existe; si no, el global de la consola */
static bool ctlLoadFor(struct Controls* c, int sys, uint32_t crc) {
	char p[1024];
	ctlFile(p, sizeof(p), sys, crc, true);
	if (ctlLoadFile(c, p, sys)) return true;
	ctlFile(p, sizeof(p), sys, crc, false);
	if (!ctlLoadFile(c, p, sys)) ctlDefaults(c, sys);
	return false;
}

static struct Controls g_ctl;   /* controles del juego que se está jugando */
static int g_gameSys = SYS_GBA;
static uint32_t g_gameCrc;
static bool g_ctlActive;

/* Botón de la consola -> bit de mGBA (GB/GBC/GBA) */
static int ctlToGba(int b) {
	switch (b) {
	case CB_A: return K_A; case CB_B: return K_B; case CB_START: return K_START; case CB_SELECT: return K_SELECT;
	case CB_UP: return K_UP; case CB_DOWN: return K_DOWN; case CB_LEFT: return K_LEFT; case CB_RIGHT: return K_RIGHT;
	case CB_L: return K_L; case CB_R: return K_R;
	case CB_X: return K_X; case CB_Y: return K_Y; case CB_C: return K_C; case CB_Z: return K_Z;
	}
	return -1;
}

static const char* padName(int b) {
	switch (b) {
	case SDL_CONTROLLER_BUTTON_A: return T("Bot\xC3\xB3n de abajo");
	case SDL_CONTROLLER_BUTTON_B: return T("Bot\xC3\xB3n de la derecha");
	case SDL_CONTROLLER_BUTTON_X: return T("Bot\xC3\xB3n de la izquierda");
	case SDL_CONTROLLER_BUTTON_Y: return T("Bot\xC3\xB3n de arriba");
	case SDL_CONTROLLER_BUTTON_BACK: return "Select / -";
	case SDL_CONTROLLER_BUTTON_START: return "Start / +";
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: return T("Clic stick izq.");
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return T("Clic stick der.");
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L";
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return T("Cruceta arriba");
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return T("Cruceta abajo");
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return T("Cruceta izquierda");
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return T("Cruceta derecha");
	case -1: return "\xE2\x80\x94";
	}
	return T("Otro bot\xC3\xB3n");
}

static const char* keyName(int sc) {
	if (sc < 0) return "\xE2\x80\x94";
	const char* n = SDL_GetScancodeName((SDL_Scancode) sc);
	return n && n[0] ? n : "?";
}

/* ---------------- diálogo de confirmación (estilo de la biblioteca) ---------------- */
/* Devuelve true si se confirma. Por defecto queda marcada la opción segura (Cancelar). */
static void drawDialogBox(struct App* a, const char* title, const char* body, const char* yes, int focus) {
	fillRect2(a, 0, 0, LIB_W, LIB_H, 0x000000, 150);
	fillRR(a, 340, 230, 600, 260, 22, TH.panel, 255);
	fillRR(a, 340, 230, 600, 6, 3, 0xE0A030, 255);
	text(a, F_ITEM, 0.62f, title, 380, 262, TH.text, 255, AL_LEFT);
	/* cuerpo en dos líneas como máximo */
	char l1[200], l2[200];
	snprintf(l1, sizeof(l1), "%s", body);
	l2[0] = 0;
	if (textW(F_SMALL, l1, 0.58f) > 520) {
		size_t n = strlen(l1);
		for (size_t i = n / 2; i < n; ++i) if (l1[i] == ' ') { l1[i] = 0; snprintf(l2, sizeof(l2), "%s", l1 + i + 1); break; }
	}
	text(a, F_SMALL, 0.58f, l1, 380, 318, TH.text2, 255, AL_LEFT);
	if (l2[0]) text(a, F_SMALL, 0.58f, l2, 380, 346, TH.text2, 255, AL_LEFT);
	const char* opts[2] = { T("Cancelar"), yes };
	for (int i = 0; i < 2; ++i) {
		float x = 380 + i * 270;
		bool on = focus == i;
		fillRR(a, x, 410, 250, 52, 26, on ? (i ? 0xE05050 : TH.acc) : 0x242A38, 255);
		text(a, F_SMALL, 0.62f, opts[i], (int) (x + 125), 422, on ? 0x0A0C10 : TH.text, 255, AL_CENTER);
	}
}

static bool confirmDialog(struct App* a, const char* title, const char* body, const char* yes, SDL_Texture* backdrop) {
	int focus = 0;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return true; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return true; }
		if (in == IN_BACK) return false;
		if (in == IN_LEFT || in == IN_UP) focus = 0;
		if (in == IN_RIGHT || in == IN_DOWN) focus = 1;
		if (in == IN_OK) return focus == 1;
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		if (backdrop) SDL_RenderCopy(a->renderer, backdrop, NULL, NULL);
		drawDialogBox(a, title, body, yes, focus);
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
}

/* ---------------- pantalla de controles ---------------- */
static void runControls(struct App* a, int sys, uint32_t crc, const char* gameName, SDL_Texture* backdrop) {
	struct Controls c;
	bool perGame = ctlLoadFor(&c, sys, crc);
	int rows = 0;
	while (SYS_BTNS[sys][rows] >= 0) ++rows;
	/* filas: 0 = perfil, 1..rows = botones, rows+1 = restablecer, rows+2 = guardar y volver */
	int sel = 1, col = 0;
	bool capturing = false;
	Uint32 capStart = 0;
#ifdef __SWITCH__
	col = 1;  /* en Switch solo mando */
#endif
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		if (capturing) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT) { a->quit = true; return; }
				int b = SYS_BTNS[sys][sel - 1];
				if (e.type == SDL_KEYDOWN && !e.key.repeat) {
					if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { capturing = false; break; }
					if (col == 0) {
						int sc = e.key.keysym.scancode;
						for (int i = 0; i < CB_COUNT; ++i) if (c.key[i] == sc) c.key[i] = -1;  /* sin repetidos */
						c.key[b] = sc;
						capturing = false;
					}
				}
				if (e.type == SDL_CONTROLLERBUTTONDOWN && col == 1) {
					int pb = e.cbutton.button;
					for (int i = 0; i < CB_COUNT; ++i) if (c.pad[i] == pb) c.pad[i] = -1;
					c.pad[b] = pb;
					capturing = false;
				}
				if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) handleDeviceEvent(a, &e);
			}
			if (SDL_GetTicks() - capStart > 6000) capturing = false;
		} else {
			enum MenuInput in = menuPollInput(a);
			if (in == IN_QUIT) { a->quit = true; return; }
			if (in == IN_UP) sel = sel > 0 ? sel - 1 : rows + 2;
			if (in == IN_DOWN) sel = sel < rows + 2 ? sel + 1 : 0;
#ifndef __SWITCH__
			if ((in == IN_LEFT || in == IN_RIGHT) && sel >= 1 && sel <= rows) col = 1 - col;
#endif
			if ((in == IN_LEFT || in == IN_RIGHT || in == IN_OK) && sel == 0) {
				perGame = !perGame;
				/* al cambiar de perfil se muestran los controles de ese perfil */
				char p[1024];
				ctlFile(p, sizeof(p), sys, crc, perGame);
				if (!ctlLoadFile(&c, p, sys)) {
					if (perGame) { char gp[1024]; ctlFile(gp, sizeof(gp), sys, crc, false); if (!ctlLoadFile(&c, gp, sys)) ctlDefaults(&c, sys); }
					else ctlDefaults(&c, sys);
				}
			} else if (in == IN_OK && sel >= 1 && sel <= rows) {
				capturing = true;
				capStart = SDL_GetTicks();
				SDL_FlushEvents(SDL_KEYDOWN, SDL_CONTROLLERBUTTONUP);
			} else if (in == IN_OK && sel == rows + 1) {
				ctlDefaults(&c, sys);
			} else if ((in == IN_OK && sel == rows + 2) || in == IN_BACK) {
				char p[1024];
				ctlFile(p, sizeof(p), sys, crc, perGame);
				ctlSaveFile(&c, p, sys, perGame ? gameName : T("todos los juegos"));
				if (!perGame) {
					/* volver al perfil global: se quita el perfil propio del juego */
					char gp[1024];
					ctlFile(gp, sizeof(gp), sys, crc, true);
					remove(gp);
				}
				SDL_Log("Controles guardados (%s, %s)", SYS_NAME[sys], perGame ? "este juego" : "global");
				return;
			}
		}
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		themeClearColor(a);
		SDL_RenderClear(a->renderer);
		if (backdrop) { SDL_RenderCopy(a->renderer, backdrop, NULL, NULL); fillRect2(a, 0, 0, LIB_W, LIB_H, TH.bg, 225); }
		char title[160];
		snprintf(title, sizeof(title), T("Controles  \xC2\xB7  %s"), SYS_NAME[sys]);
		text(a, F_ITEM, 0.7f, title, 60, 34, TH.text, 255, AL_LEFT);
		fillRR(a, 60 + textW(F_ITEM, title, 0.7f) + 16, 46, 14, 14, 7, SYS_COLOR[sys], 255);
		/* fila del perfil */
		fillRR(a, 60, 96, 760, 50, 14, sel == 0 ? 0x242A38 : TH.panel, 255);
		text(a, F_SMALL, 0.6f, T("Perfil"), 84, 108, TH.dim, 255, AL_LEFT);
		char prof[200];
		if (perGame) snprintf(prof, sizeof(prof), T("Solo este juego (%s)"), gameName);
		else snprintf(prof, sizeof(prof), T("Global: todos los juegos de %s"), SYS_NAME[sys]);
		textFit(a, F_SMALL, 0.6f, prof, 200, 108, 600, sel == 0 ? TH.acct : TH.text);
		/* cabecera de columnas */
#ifndef __SWITCH__
		text(a, F_SMALL, 0.52f, T("Teclado"), 330, 162, col == 0 ? TH.acct : TH.dim, 255, AL_LEFT);
#endif
		text(a, F_SMALL, 0.52f, T("Mando"), 580, 162, col == 1 ? TH.acct : TH.dim, 255, AL_LEFT);
		const float rh = 34;
		for (int r = 0; r < rows; ++r) {
			int b = SYS_BTNS[sys][r];
			float y = 190 + r * rh;
			bool on = sel == r + 1;
			if (on) fillRR(a, 60, y - 4, 760, rh - 2, 10, 0x242A38, 255);
			text(a, F_SMALL, 0.58f, btnLabel(sys, b), 84, (int) y, TH.text, 255, AL_LEFT);
			for (int cc = 0; cc < 2; ++cc) {
#ifdef __SWITCH__
				if (cc == 0) continue;
#endif
				float x = cc == 0 ? 330 : 580;
				bool here = on && col == cc;
				const char* v = cc == 0 ? keyName(c.key[b]) : padName(c.pad[b]);
				if (here && capturing) v = cc == 0 ? T("Pulsa una tecla...") : T("Pulsa un bot\xC3\xB3n...");
				if (here) fillRR(a, x - 10, y - 2, 230, rh - 6, 9, capturing ? 0xE0A030 : TH.acc, 255);
				textFit(a, F_SMALL, 0.55f, v, (int) x, (int) y, 210, here ? 0x0A0C10 : TH.text2);
			}
		}
		float by = 190 + rows * rh + 14;
		const char* acts[2] = { T("Restablecer de f\xC3\xA1" "brica"), T("Guardar y volver") };
		for (int k = 0; k < 2; ++k) {
			bool on = sel == rows + 1 + k;
			fillRR(a, 60 + k * 300, by, 280, 44, 22, on ? (k ? TH.acc : 0xE0A030) : 0x242A38, 255);
			text(a, F_SMALL, 0.58f, acts[k], 200 + k * 300, (int) by + 9, on ? 0x0A0C10 : TH.text, 255, AL_CENTER);
		}
		/* ayuda lateral */
		fillRR(a, 860, 96, 360, 440, 16, TH.panel, 255);
		text(a, F_SMALL, 0.62f, T("C\xC3\xB3mo funciona"), 884, 116, TH.text, 255, AL_LEFT);
		const char* help[] = {
			T("Global: sirve para todos los"),
			T("juegos de esta consola."),
			T("Solo este juego: para hacks o"),
			T("versiones con otros controles."),
			"",
#ifndef __SWITCH__
			T("Enter: cambiar un bot\xC3\xB3n"),
			T("Izq./Der.: teclado o mando"),
#else
			T("A: cambiar un bot\xC3\xB3n"),
#endif
			T("El stick izquierdo siempre"),
			T("mueve la cruceta."),
		};
		for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i)
			text(a, F_SMALL, 0.52f, help[i], 884, 160 + (int) i * 30, TH.dim, 255, AL_LEFT);
#ifdef SWS_TEST
		{ static int shot = 0; if (!shot) { shot = 1; testCapture(a, "controles"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
}
