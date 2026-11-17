/* ================================================================== */
/* Metadatos: nombre oficial por CRC32 (No-Intro) y portadas (TheGamesDB)
 *
 *  1. CRC32 -> base No-Intro de libretro-database (se descarga una vez por
 *     consola a Datos/No-Intro/). Si el CRC no está pero el código del header
 *     coincide, el juego se marca como hack o versión modificada.
 *  2. Nombre No-Intro -> TheGamesDB (Games/ByGameName con include=boxart y
 *     filtro de plataforma). Se elige el resultado más parecido.
 *  3. Portada frontal -> Caratulas/<CRC>_caja.bmp (reducida, con su forma original)
 *     y el JPG original en Caratulas/originales/.
 *  Todo queda en Datos/metadatos.txt: cada juego se consulta una sola vez.
 *  Solo se descargan datos e imágenes de portada, nunca juegos.
 * ================================================================== */
#include "jpeg.h"
#include "png.h"

static const char* const SYS_DAT[SYS_COUNT] = {
	"Nintendo - Super Nintendo Entertainment System", "Sega - Mega Drive - Genesis",
	"Nintendo - Game Boy Advance", "Nintendo - Game Boy Color", "Nintendo - Game Boy", "Nintendo - Nintendo Entertainment System",
};
/* IDs de plataforma en TheGamesDB (Genesis y Mega Drive son dos plataformas) */
static const char* const SYS_TGDB[SYS_COUNT] = { "6", "18,36", "5", "41", "4", "7" };
#define DAT_URL  "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/"
#define TGDB_URL "https://api.thegamesdb.net/v1/Games/ByGameName"

struct Meta {
	uint32_t crc;
	char dat;            /* 0 = sin revisar, 'S' = está en No-Intro, 'N' = no está */
	char box;            /* 0 = sin buscar, 'S' = portada guardada, 'N' = no hay */
	int tgdbId;
	char nointro[160];   /* "Donkey Kong Country (USA) (Rev 2)" */
	char base[128];      /* hack: juego original según el código del header */
	char title[128];     /* título en TheGamesDB */
	int rev;             /* cambia cada vez que se actualiza */
};
static struct Meta* g_meta;
static int g_metaN;
static SDL_mutex* g_metaLock;
static SDL_atomic_t g_metaDirty, g_metaBusy;  /* busy: 1 trabajando, -1 cancelar, 0 parado */
static char g_metaStatus[96], g_metaToast[200];
static SDL_Thread* g_metaThr;
static SDL_atomic_t g_metaPaused;  /* tras un error de red o de clave: no reintentar hasta F5 */

/* Cupo mensual de TheGamesDB. Cada respuesta trae cuántas consultas quedan;
 * se guarda en Datos/tgdb_cupo.txt para saberlo también al abrir de nuevo.
 * La búsqueda automática se detiene antes de agotarlo y deja una reserva
 * para elegir portadas a mano. */
#define TGDB_RESERVE 25
static SDL_atomic_t g_tgdbLeft;      /* -1 = todavía no se sabe */
static long long g_tgdbRefreshAt;    /* cuándo se renueva (unix) */
static int gamePriority(uint32_t crc);  /* library.inc: favoritos y jugados primero */

static void boxPath(char* out, size_t n, uint32_t crc) {
	char rel[64];
	snprintf(rel, sizeof(rel), "Caratulas/%08X_caja.bmp", (unsigned) crc);
	pathJoin(out, n, rel);
}

static struct Meta* metaFind(uint32_t crc) {
	for (int i = 0; i < g_metaN; ++i)
		if (g_meta[i].crc == crc) return &g_meta[i];
	return NULL;
}

/* ---- archivo Datos/metadatos.txt ---- */
static void metaFile(char* out, size_t n) { pathJoin(out, n, "Datos/metadatos.txt"); }

static void metaLoad(void) {
	char p[1024];
	metaFile(p, sizeof(p));
	FILE* f = fopen(p, "r");
	if (!f) return;
	char line[1024];
	if (!fgets(line, sizeof(line), f) || strncmp(line, "#metadatos v1", 13) != 0) { fclose(f); return; }
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* fld[7] = { 0 };
		int k = 0;
		char* s = line;
		fld[k++] = s;
		while (*s && k < 7) { if (*s == '\t') { *s = 0; fld[k++] = s + 1; } ++s; }
		if (k < 7) continue;
		struct Meta* nm = realloc(g_meta, sizeof(*g_meta) * (size_t) (g_metaN + 1));
		if (!nm) break;
		g_meta = nm;
		struct Meta* m = &g_meta[g_metaN++];
		memset(m, 0, sizeof(*m));
		m->crc = (uint32_t) strtoul(fld[0], NULL, 16);
		m->dat = fld[1][0] == '-' ? 0 : fld[1][0];
		m->box = fld[2][0] == '-' ? 0 : fld[2][0];
		m->tgdbId = atoi(fld[3]);
		snprintf(m->nointro, sizeof(m->nointro), "%s", fld[4]);
		snprintf(m->base, sizeof(m->base), "%s", fld[5]);
		snprintf(m->title, sizeof(m->title), "%s", fld[6]);
		m->rev = 1;
	}
	fclose(f);
}

static void metaSave(void) {  /* llamar con g_metaLock tomado */
	char p[1024], tmp[1100];
	metaFile(p, sizeof(p));
	snprintf(tmp, sizeof(tmp), "%s.tmp", p);
	FILE* f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "#metadatos v1\tcrc\tno-intro\tportada\tid_thegamesdb\tnombre_no-intro\tjuego_base\ttitulo\n");
	for (int i = 0; i < g_metaN; ++i) {
		struct Meta* m = &g_meta[i];
		fprintf(f, "%08X\t%c\t%c\t%d\t%s\t%s\t%s\n", (unsigned) m->crc, m->dat ? m->dat : '-', m->box ? m->box : '-',
		        m->tgdbId, m->nointro, m->base, m->title);
	}
	fclose(f);
	remove(p);
	rename(tmp, p);
}

/* ---- nombres ---- */
/* "Legend of Zelda, The - A Link to the Past (USA)" -> "The Legend of Zelda - A Link to the Past" */
static void cleanNoIntro(const char* in, char* out, size_t n) {
	char t[256];
	snprintf(t, sizeof(t), "%s", in);
	char* cut = strstr(t, " (");
	if (cut) *cut = 0;
	cut = strstr(t, " [");
	if (cut) *cut = 0;
	char res[256] = "";
	char* seg = t;
	while (seg) {
		char* next = strstr(seg, " - ");
		if (next) *next = 0;
		size_t L = strlen(seg);
		char part[256];
		if (L > 5 && !strcmp(seg + L - 5, ", The")) { seg[L - 5] = 0; snprintf(part, sizeof(part), "The %s", seg); }
		else if (L > 3 && !strcmp(seg + L - 3, ", A")) { seg[L - 3] = 0; snprintf(part, sizeof(part), "A %s", seg); }
		else snprintf(part, sizeof(part), "%s", seg);
		size_t rl = strlen(res);
		snprintf(res + rl, sizeof(res) - rl, "%s%s", rl ? " - " : "", part);
		seg = next ? next + 3 : NULL;
	}
	snprintf(out, n, "%s", res);
}

/* Palabras normalizadas (minúsculas, sin acentos, números romanos -> cifras) */
static int nameTokens(const char* s, char tok[24][24]) {
	int n = 0, l = 0;
	char cur[24];
	for (const unsigned char* p = (const unsigned char*) s;; ++p) {
		int c = *p;
		if (c == 0xC3 && p[1]) {  /* letras latinas acentuadas más comunes */
			int d = *++p;
			c = (d >= 0x80 && d <= 0x86) || (d >= 0xA0 && d <= 0xA6) ? 'a' : (d == 0x87 || d == 0xA7) ? 'c'
			  : (d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB) ? 'e' : (d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF) ? 'i'
			  : (d == 0x91 || d == 0xB1) ? 'n' : (d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6) ? 'o'
			  : (d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC) ? 'u' : ' ';
		}
		if (c == '\'') continue;  /* "Kirby's" = "kirbys" */
		if (c == '&') c = ' ';
		if (isalnum(c) && c < 0x80) { if (l < 23) cur[l++] = (char) tolower(c); continue; }
		if (l) {
			cur[l] = 0;
			if (!strcmp(cur, "ii")) strcpy(cur, "2");
			else if (!strcmp(cur, "iii")) strcpy(cur, "3");
			else if (!strcmp(cur, "iv")) strcpy(cur, "4");
			else if (!strcmp(cur, "and")) cur[0] = 0;
			if (cur[0] && n < 24) strcpy(tok[n++], cur);
			l = 0;
		}
		if (!c) break;
	}
	return n;
}

/* Parecido entre dos nombres (coeficiente de Dice sobre palabras): 0..1 */
static double nameScore(const char* a, const char* b) {
	char ta[24][24], tb[24][24];
	int na = nameTokens(a, ta), nb = nameTokens(b, tb);
	if (!na || !nb) return 0;
	bool used[24] = { false };
	int common = 0;
	for (int i = 0; i < na; ++i)
		for (int k = 0; k < nb; ++k)
			if (!used[k] && !strcmp(ta[i], tb[k])) { used[k] = true; ++common; break; }
	return 2.0 * common / (na + nb);
}

/* ---- red ---- */
static void urlEncode(const char* in, char* out, size_t n) {
	size_t o = 0;
	for (const unsigned char* p = (const unsigned char*) in; *p && o + 4 < n; ++p) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out[o++] = (char) *p;
		else o += (size_t) snprintf(out + o, n - o, "%%%02X", *p);
	}
	out[o] = 0;
}

static unsigned char* netGet(const char* url, size_t* len, const char** err) {
#ifdef SWS_TEST
	/* pruebas: las respuestas salen de una carpeta (sin internet) */
	const char* dir = getenv("SWS_TEST_NET");
	if (dir) {
		char name[512] = "", p[1200];
		const char* q = strstr(url, "ByGameName");
		if (q) {
			const char* nm = strstr(url, "&name=");
			size_t k = 0;
			memcpy(name, "tgdb_", 5);
			k = 5;
			for (nm = nm ? nm + 6 : ""; *nm && *nm != '&' && k < 200; ++nm) {
				if (*nm == '%' && nm[1] && nm[2]) { nm += 2; name[k++] = '_'; }
				else name[k++] = isalnum((unsigned char) *nm) ? (char) tolower((unsigned char) *nm) : '_';
			}
			snprintf(name + k, sizeof(name) - k, ".json");
			const char* key = strstr(url, "apikey=");
			SDL_Log("[prueba red] ByGameName -> %s (clave %s)", name, key && key[7] != '&' ? "presente" : "falta");
		} else {
			const char* b = strrchr(url, '/');
			size_t k = 0;
			for (b = b ? b + 1 : url; *b && *b != '?' && k < 400; ++b) {
				if (*b == '%' && b[1] && b[2]) { char hx[3] = { b[1], b[2], 0 }; name[k++] = (char) strtol(hx, NULL, 16); b += 2; }
				else name[k++] = *b;
			}
			name[k] = 0;
			SDL_Log("[prueba red] %s", name);
		}
		snprintf(p, sizeof(p), "%s/%s", dir, name);
		unsigned char* d = swsReadFile(p, len);
		if (!d) *err = "no encontrado (prueba)";
		if (d && !strncmp((const char*) d, "HTTP403", 7)) { free(d); *err = T("el servidor respondi\xC3\xB3 403"); return NULL; }
		return d;
	}
#endif
	return swsHttpGet(url, len, err, NULL, NULL);
}

static bool writeAll(const char* path, const void* d, size_t n) {
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fwrite(d, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	if (!ok) remove(path);
	return ok;
}

/* ---- JSON mínimo (solo lectura) ---- */
enum { JN_NULL, JN_BOOL, JN_NUM, JN_STR, JN_ARR, JN_OBJ };
struct JNode { int type; double num; char* str; char* key; struct JNode* kid; struct JNode* next; };

static void jFree(struct JNode* n) {
	while (n) {
		struct JNode* nx = n->next;
		jFree(n->kid);
		free(n->str);
		free(n->key);
		free(n);
		n = nx;
	}
}

static void jWs(const char** p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') ++*p; }

static void jPutUtf8(char** o, unsigned cp) {
	if (cp < 0x80) *(*o)++ = (char) cp;
	else if (cp < 0x800) { *(*o)++ = (char) (0xC0 | cp >> 6); *(*o)++ = (char) (0x80 | (cp & 63)); }
	else if (cp < 0x10000) { *(*o)++ = (char) (0xE0 | cp >> 12); *(*o)++ = (char) (0x80 | ((cp >> 6) & 63)); *(*o)++ = (char) (0x80 | (cp & 63)); }
	else { *(*o)++ = (char) (0xF0 | cp >> 18); *(*o)++ = (char) (0x80 | ((cp >> 12) & 63)); *(*o)++ = (char) (0x80 | ((cp >> 6) & 63)); *(*o)++ = (char) (0x80 | (cp & 63)); }
}

static char* jString(const char** p) {
	if (**p != '"') return NULL;
	const char* s = ++*p;
	size_t cap = 0;
	while (s[cap] && s[cap] != '"') { if (s[cap] == '\\' && s[cap + 1]) ++cap; ++cap; }
	char* out = malloc(cap * 2 + 8);  /* \uXXXX nunca ocupa más que su texto en UTF-8... salvo pares: margen */
	if (!out) return NULL;
	char* o = out;
	while (**p && **p != '"') {
		char c = *(*p)++;
		if (c != '\\') { *o++ = c; continue; }
		c = *(*p)++;
		if (!c) break;
		switch (c) {
		case 'n': *o++ = '\n'; break;
		case 't': *o++ = '\t'; break;
		case 'r': *o++ = '\r'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'u': {
			unsigned cp = 0;
			for (int i = 0; i < 4 && isxdigit((unsigned char) **p); ++i, ++*p) cp = cp * 16 + (unsigned) (isdigit((unsigned char) **p) ? **p - '0' : (tolower((unsigned char) **p) - 'a' + 10));
			if (cp >= 0xD800 && cp < 0xDC00 && (*p)[0] == '\\' && (*p)[1] == 'u') {
				unsigned lo = 0;
				const char* q = *p + 2;
				for (int i = 0; i < 4 && isxdigit((unsigned char) *q); ++i, ++q) lo = lo * 16 + (unsigned) (isdigit((unsigned char) *q) ? *q - '0' : (tolower((unsigned char) *q) - 'a' + 10));
				if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); *p = q; }
			}
			jPutUtf8(&o, cp);
			break;
		}
		default: *o++ = c;
		}
	}
	if (**p == '"') ++*p;
	*o = 0;
	return out;
}

static struct JNode* jValue(const char** p, int depth) {
	jWs(p);
	if (depth > 48) return NULL;
	struct JNode* n = calloc(1, sizeof(*n));
	if (!n) return NULL;
	char c = **p;
	if (c == '{' || c == '[') {
		n->type = c == '{' ? JN_OBJ : JN_ARR;
		++*p;
		struct JNode** tail = &n->kid;
		for (;;) {
			jWs(p);
			if (**p == (c == '{' ? '}' : ']')) { ++*p; break; }
			char* key = NULL;
			if (c == '{') {
				key = jString(p);
				if (!key) { jFree(n); return NULL; }
				jWs(p);
				if (**p != ':') { free(key); jFree(n); return NULL; }
				++*p;
			}
			struct JNode* v = jValue(p, depth + 1);
			if (!v) { free(key); jFree(n); return NULL; }
			v->key = key;
			*tail = v;
			tail = &v->next;
			jWs(p);
			if (**p == ',') { ++*p; continue; }
			if (**p == (c == '{' ? '}' : ']')) { ++*p; break; }
			jFree(n);
			return NULL;
		}
	} else if (c == '"') {
		n->type = JN_STR;
		n->str = jString(p);
		if (!n->str) { free(n); return NULL; }
	} else if (c == 't' || c == 'f' || c == 'n') {
		n->type = c == 'n' ? JN_NULL : JN_BOOL;
		n->num = c == 't';
		while (isalpha((unsigned char) **p)) ++*p;
	} else if (c == '-' || isdigit((unsigned char) c)) {
		n->type = JN_NUM;
		char* e;
		n->num = strtod(*p, &e);
		*p = e;
	} else { free(n); return NULL; }
	return n;
}

static struct JNode* jGet(struct JNode* o, const char* key) {
	if (!o || o->type != JN_OBJ) return NULL;
	for (struct JNode* k = o->kid; k; k = k->next)
		if (k->key && !strcmp(k->key, key)) return k;
	return NULL;
}
static const char* jStr(struct JNode* n) { return n && n->type == JN_STR ? n->str : NULL; }
static int jInt(struct JNode* n) {
	if (!n) return 0;
	if (n->type == JN_NUM) return (int) n->num;
	if (n->type == JN_STR) return atoi(n->str);
	return 0;
}

/* ---- imagen de la etiqueta ---- */
/* Promedio de área: (cx,cy,cw,ch) del origen -> dw x dh en dst (stride en bytes) */
static void areaResize(const uint8_t* src, int sw, int sh, float cx, float cy, float cw, float ch,
                       uint8_t* dst, int dw, int dh, int stride) {
	for (int y = 0; y < dh; ++y) {
		int y0 = (int) (cy + y * ch / dh), y1 = (int) ceilf(cy + (y + 1) * ch / dh);
		if (y0 < 0) y0 = 0;
		if (y1 > sh) y1 = sh;
		if (y1 <= y0) y1 = y0 + 1 <= sh ? y0 + 1 : y0, y0 = y1 - 1;
		for (int x = 0; x < dw; ++x) {
			int x0 = (int) (cx + x * cw / dw), x1 = (int) ceilf(cx + (x + 1) * cw / dw);
			if (x0 < 0) x0 = 0;
			if (x1 > sw) x1 = sw;
			if (x1 <= x0) x1 = x0 + 1 <= sw ? x0 + 1 : x0, x0 = x1 - 1;
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int yy = y0; yy < y1; ++yy) {
				const uint8_t* s = src + ((size_t) yy * sw + x0) * 3;
				for (int xx = x0; xx < x1; ++xx, s += 3) { r += s[0]; g += s[1]; b += s[2]; ++n; }
			}
			uint8_t* d = dst + (size_t) y * stride + x * 3;
			if (!n) n = 1;
			d[0] = (uint8_t) (r / n); d[1] = (uint8_t) (g / n); d[2] = (uint8_t) (b / n);
		}
	}
}

/* Guarda la portada reducida (máx. 400 px), sin recortar: la etiqueta de cada
 * consola decide cómo encajarla. */
static bool saveBoxArt(const uint8_t* rgb, int w, int h, const char* outPath) {
	float k = w > h ? 400.0f / w : 400.0f / h;
	if (k > 1) k = 1;
	int dw = (int) (w * k + 0.5f), dh = (int) (h * k + 0.5f);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	uint8_t* dst = malloc((size_t) dw * dh * 3);
	if (!dst) return false;
	areaResize(rgb, w, h, 0, 0, (float) w, (float) h, dst, dw, dh, dw * 3);
	bool ok = false;
	SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(dst, dw, dh, 24, dw * 3, SDL_PIXELFORMAT_RGB24);
	if (sf) { ok = SDL_SaveBMP(sf, outPath) == 0; SDL_FreeSurface(sf); }
	free(dst);
	return ok;
}


/* ---- portadas: formato y tipo de lanzamiento ---- */
/* TheGamesDB guarda portadas en JPG y en PNG */
static bool coverFileOk(const char* fn) {
	size_t fl = strlen(fn);
	return (fl > 4 && (!strcasecmp(fn + fl - 4, ".jpg") || !strcasecmp(fn + fl - 4, ".png"))) || (fl > 5 && !strcasecmp(fn + fl - 5, ".jpeg"));
}

static uint8_t* decodeImage(const unsigned char* d, size_t len, int* w, int* h) {
	if (len > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return pngDecode(d, len, w, h);
	return jpegDecode(d, len, w, h);
}

/* 0 = lanzamiento oficial, 1 = variante ("[Classic Series]", prototipo...),
 * 2 = hack, traducción o homebrew. Sirve para ordenar: lo oficial primero. */
static int releaseKind(struct JNode* g, const char* title) {
	char low[160];
	size_t i = 0;
	for (; title[i] && i < sizeof(low) - 1; ++i) low[i] = (char) tolower((unsigned char) title[i]);
	low[i] = 0;
	static const char* const HACK[] = { "hack", " mod", "redux", "homebrew", "translation", "traduc", "romhack", "demake", "remix", "multiplayer" };
	for (size_t k = 0; k < sizeof(HACK) / sizeof(HACK[0]); ++k)
		if (strstr(low, HACK[k])) return 2;
	if (jInt(jGet(g, "region_id")) == 9) return 2;  /* región "otra": casi siempre homebrew o hacks */
	if (strchr(title, '[')) return 1;
	return 0;
}

static int releaseYear(struct JNode* g) {
	const char* d = jStr(jGet(g, "release_date"));
	return d && strlen(d) >= 4 ? atoi(d) : 0;
}

/* ---- TheGamesDB ---- */
enum { TG_FOUND = 1, TG_NONE = 0, TG_NET = -1, TG_KEY = -2, TG_QUOTA = -3 };

/* 429 = demasiadas consultas: el cupo de la clave se agotó por ahora. Se deja
 * de consultar unas horas en vez de seguir insistiendo. */
static void tgdbQuotaHit(void);

/* Busca el juego y devuelve la URL de la portada frontal (JPG) */
/* ¿El id de plataforma de TheGamesDB corresponde a esta consola? */
static bool tgdbPlatformOk(int sys, int platform) {
	if (platform <= 0) return true;  /* respuesta sin el dato: se acepta */
	const char* p = SYS_TGDB[sys];
	while (*p) {
		if (atoi(p) == platform) return true;
		while (*p && *p != ',') ++p;
		if (*p == ',') ++p;
	}
	return false;
}

static void tgdbSaveAllowance(void) {
	char p[1024];
	pathJoin(p, sizeof(p), "Datos/tgdb_cupo.txt");
	FILE* f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "%d %lld\n", SDL_AtomicGet(&g_tgdbLeft), g_tgdbRefreshAt);
	fclose(f);
}

static void tgdbLoadAllowance(void) {
	SDL_AtomicSet(&g_tgdbLeft, -1);
	char p[1024];
	pathJoin(p, sizeof(p), "Datos/tgdb_cupo.txt");
	FILE* f = fopen(p, "r");
	if (!f) return;
	int left = -1;
	long long at = 0;
	if (fscanf(f, "%d %lld", &left, &at) == 2 && at > (long long) time(NULL)) { SDL_AtomicSet(&g_tgdbLeft, left); g_tgdbRefreshAt = at; }
	fclose(f);
}

static void tgdbNoteAllowance(struct JNode* root) {
	struct JNode* r = jGet(root, "remaining_monthly_allowance");
	if (!r) return;
	int left = jInt(r) + jInt(jGet(root, "extra_allowance"));
	int timer = jInt(jGet(root, "allowance_refresh_timer"));
	SDL_AtomicSet(&g_tgdbLeft, left);
	g_tgdbRefreshAt = (long long) time(NULL) + (timer > 0 ? timer : 30 * 86400);
	tgdbSaveAllowance();
}

static void tgdbQuotaHit(void) {
	SDL_AtomicSet(&g_tgdbLeft, 0);
	if (g_tgdbRefreshAt < (long long) time(NULL) + 3600) g_tgdbRefreshAt = (long long) time(NULL) + 12 * 3600;
	tgdbSaveAllowance();
}

/* "quedan 2350 consultas" o "" si no se sabe */
static void tgdbAllowanceText(char* out, size_t n) {
	int left = SDL_AtomicGet(&g_tgdbLeft);
	if (left < 0) { out[0] = 0; return; }
	long long days = (g_tgdbRefreshAt - (long long) time(NULL) + 86399) / 86400;
	snprintf(out, n, T("quedan %d consultas este mes (se renueva en %lld d\xC3\xAD" "a%s)"), left, days, days == 1 ? "" : "s");
}

static int tgdbFind(const char* key, int sys, const char* nointro, int* id, char* title, size_t tn, char* img, size_t in, const char** err) {
	char clean[256], q[3][256];
	cleanNoIntro(nointro, clean, sizeof(clean));
	int nq = 0;
	/* 1) nombre completo sin " - "; 2) solo el subtítulo; 3) solo el título principal */
	snprintf(q[nq], sizeof(q[nq]), "%s", clean);
	for (char* d; (d = strstr(q[nq], " - "));) memmove(d + 1, d + 3, strlen(d + 3) + 1);
	++nq;
	char* dash = strstr(clean, " - ");
	if (dash) {
		if (strchr(dash + 3, ' ')) snprintf(q[nq++], sizeof(q[0]), "%s", dash + 3);
		snprintf(q[nq], sizeof(q[0]), "%.*s", (int) (dash - clean), clean);
		++nq;
	}
	double bestScore = 0;
	int bestHasImg = 0;
	for (int qi = 0; qi < nq; ++qi) {
		char ek[256], en[600], plat[32], url[1400];
		urlEncode(key, ek, sizeof(ek));
		urlEncode(q[qi], en, sizeof(en));
		snprintf(plat, sizeof(plat), "%s", SYS_TGDB[sys]);  /* la coma va tal cual (18,36) */
		snprintf(url, sizeof(url), "%s?apikey=%s&name=%s&filter%%5Bplatform%%5D=%s&include=boxart&fields=region_id", TGDB_URL, ek, en, plat);
		size_t len = 0;
		unsigned char* body = netGet(url, &len, err);
		if (!body) {
			if (*err && strstr(*err, "403")) return TG_KEY;
			if (*err && strstr(*err, "429")) { tgdbQuotaHit(); return TG_QUOTA; }
#ifdef SWS_TEST
			if (*err && strstr(*err, "prueba")) continue;  /* sin archivo de prueba = sin resultados */
#endif
			return TG_NET;
		}
		const char* p = (const char*) body;
		struct JNode* root = jValue(&p, 0);
		free(body);
		if (!root) continue;
		tgdbNoteAllowance(root);
		struct JNode* games = jGet(jGet(root, "data"), "games");
		struct JNode* boxart = jGet(jGet(root, "include"), "boxart");
		struct JNode* bu = jGet(boxart, "base_url");
		const char* base = jStr(jGet(bu, "large"));
		if (!base) base = jStr(jGet(bu, "medium"));
		if (!base) base = jStr(jGet(bu, "original"));
		struct JNode* bdata = jGet(boxart, "data");
		for (struct JNode* g = games && games->type == JN_ARR ? games->kid : NULL; g; g = g->next) {
			const char* t = jStr(jGet(g, "game_title"));
			int gid = jInt(jGet(g, "id"));
			if (!t || !gid) continue;
			if (!tgdbPlatformOk(sys, jInt(jGet(g, "platform")))) continue;  /* otra consola (p. ej. la versión de SNES) */
			/* lo oficial gana a variantes y hacks con el mismo nombre */
			int kind = releaseKind(g, t);
			double sc = nameScore(clean, t) - (kind == 1 ? 0.12 : kind == 2 ? 0.35 : 0);
			/* imagen frontal (JPG o PNG) */
			char ids[16];
			snprintf(ids, sizeof(ids), "%d", gid);
			const char* file = NULL;
			struct JNode* imgs = jGet(bdata, ids);
			for (struct JNode* im = imgs && imgs->type == JN_ARR ? imgs->kid : NULL; im; im = im->next) {
				const char* side = jStr(jGet(im, "side"));
				const char* fn = jStr(jGet(im, "filename"));
				const char* ty = jStr(jGet(im, "type"));
				if (!fn || (ty && strcmp(ty, "boxart")) || (side && strcmp(side, "front"))) continue;
				if (coverFileOk(fn)) { file = fn; break; }
			}
			int hasImg = file && base;
			if (sc > bestScore + 1e-6 || (sc > bestScore - 1e-6 && hasImg > bestHasImg)) {
				bestScore = sc;
				bestHasImg = hasImg;
				*id = gid;
				snprintf(title, tn, "%s", t);
				if (hasImg) {
					size_t bl = strlen(base);
					snprintf(img, in, "%s%s%s", base, bl && base[bl - 1] == '/' ? "" : "/", file[0] == '/' ? file + 1 : file);
				} else img[0] = 0;
			}
		}
		jFree(root);
		if (bestScore >= 0.6 && bestHasImg) break;  /* encontrado: no gastar más consultas */
	}
	if (bestScore < 0.6) { *id = 0; title[0] = 0; img[0] = 0; return TG_NONE; }
	return TG_FOUND;
}

/* Resultados para la elección manual de portada. Se mantienen varias
 * coincidencias ordenadas por similitud, no solo la primera automática. */
#define TGDB_CHOICES 8
struct TgdbChoice { int id; double score; int kind; int year; char title[128]; char image[700]; };

static int tgdbChoices(const char* key, int sys, const char* query, struct TgdbChoice* out, int cap, const char** err) {
	char clean[256], ek[256], en[600], plat[32], url[1400];
	cleanNoIntro(query, clean, sizeof(clean));
	urlEncode(key, ek, sizeof(ek));
	urlEncode(clean, en, sizeof(en));
	snprintf(plat, sizeof(plat), "%s", SYS_TGDB[sys]);
	/* filter[platform]: solo juegos de esta consola */
	snprintf(url, sizeof(url), "%s?apikey=%s&name=%s&filter%%5Bplatform%%5D=%s&include=boxart&fields=region_id", TGDB_URL, ek, en, plat);
	size_t len = 0;
	unsigned char* body = netGet(url, &len, err);
	if (!body) {
		if (*err && strstr(*err, "429")) { tgdbQuotaHit(); return TG_KEY; }
		return *err && strstr(*err, "403") ? TG_KEY : TG_NET;
	}
	const char* p = (const char*) body;
	struct JNode* root = jValue(&p, 0);
	free(body);
	if (!root) return TG_NONE;
	tgdbNoteAllowance(root);
	struct JNode* games = jGet(jGet(root, "data"), "games");
	struct JNode* boxart = jGet(jGet(root, "include"), "boxart");
	const char* base = jStr(jGet(jGet(boxart, "base_url"), "large"));
	if (!base) base = jStr(jGet(jGet(boxart, "base_url"), "medium"));
	struct JNode* bdata = jGet(boxart, "data");
	/* se juntan todos los resultados con portada, se ordenan y quedan los mejores */
	struct TgdbChoice all[40];
	int n = 0;
	for (struct JNode* g = games && games->type == JN_ARR ? games->kid : NULL; g && n < 40; g = g->next) {
		const char* title = jStr(jGet(g, "game_title"));
		int id = jInt(jGet(g, "id"));
		if (!title || !id || !base) continue;
		if (!tgdbPlatformOk(sys, jInt(jGet(g, "platform")))) continue;
		char ids[16]; snprintf(ids, sizeof(ids), "%d", id);
		const char* file = NULL;
		struct JNode* imgs = jGet(bdata, ids);
		for (struct JNode* im = imgs && imgs->type == JN_ARR ? imgs->kid : NULL; im; im = im->next) {
			const char* side = jStr(jGet(im, "side"));
			const char* fn = jStr(jGet(im, "filename"));
			const char* ty = jStr(jGet(im, "type"));
			if (!fn || (ty && strcmp(ty, "boxart")) || (side && strcmp(side, "front"))) continue;
			if (coverFileOk(fn)) { file = fn; break; }
		}
		if (!file) continue;
		all[n].id = id;
		all[n].kind = releaseKind(g, title);
		all[n].year = releaseYear(g);
		all[n].score = nameScore(clean, title) - (all[n].kind == 1 ? 0.12 : all[n].kind == 2 ? 0.35 : 0);
		snprintf(all[n].title, sizeof(all[n].title), "%s", title);
		snprintf(all[n].image, sizeof(all[n].image), "%s%s", base, file);
		++n;
	}
	jFree(root);
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
			if (all[j].score > all[i].score) { struct TgdbChoice t = all[i]; all[i] = all[j]; all[j] = t; }
	if (n > cap) n = cap;
	memcpy(out, all, sizeof(*out) * (size_t) n);
	return n ? TG_FOUND : TG_NONE;
}

/* ---- trabajo en segundo plano ---- */
struct MetaJob { uint32_t crc; int sys; char code[8]; char filename[128]; bool upgrade; /* ya tiene portada: probar la de RetroArch */ };
struct MetaWork {
	struct MetaJob* jobs; int n; char key[128];
	struct MetaJob* chk; int nchk;  /* portadas ya bajadas a revisar (una sola vez) */
	int nupgrade;                   /* portadas de TheGamesDB que se cambian por las de RetroArch (una vez) */
};

static void metaSetStatus(const char* s) {
	SDL_LockMutex(g_metaLock);
	snprintf(g_metaStatus, sizeof(g_metaStatus), "%s", s);
	SDL_UnlockMutex(g_metaLock);
}
static void metaSetToast(const char* s) {
	SDL_LockMutex(g_metaLock);
	snprintf(g_metaToast, sizeof(g_metaToast), "%s", s);
	SDL_UnlockMutex(g_metaLock);
}
static void metaCommit(void) {  /* con g_metaLock tomado */
	metaSave();
	SDL_AtomicSet(&g_metaDirty, 1);
}

/* Recorre la base No-Intro buscando los CRC pedidos (y el código, para hacks) */
static void datLookup(const char* datPath, struct MetaJob* jobs, int n, int sys) {
	size_t len = 0;
	unsigned char* d = swsReadFile(datPath, &len);
	if (!d) return;
	char* found = calloc((size_t) n, 160);
	char* base = calloc((size_t) n, 128);
	if (!found || !base) { free(found); free(base); free(d); return; }
	char cur[160] = "";
	char* s = (char*) d;
	while (s && *s) {
		char* e = strchr(s, '\n');
		if (e) *e = 0;
		char* l = s;
		while (*l == ' ' || *l == '\t') ++l;
		if (!strncmp(l, "game (", 6)) cur[0] = 0;
		else if (!strncmp(l, "name \"", 6) && !cur[0]) {
			char* q = strchr(l + 6, '"');
			if (q) snprintf(cur, sizeof(cur), "%.*s", (int) (q - (l + 6)), l + 6);
		} else if (!strncmp(l, "rom (", 5) && cur[0]) {
			char* c = strstr(l, " crc ");
			uint32_t crc = c ? (uint32_t) strtoul(c + 5, NULL, 16) : 0;
			char serial[16] = "";
			char* sr = strstr(l, "serial \"");
			if (sr) { char* q = strchr(sr + 8, '"'); if (q && q - (sr + 8) < 15) snprintf(serial, sizeof(serial), "%.*s", (int) (q - (sr + 8)), sr + 8); }
			for (int i = 0; i < n; ++i) {
				if (jobs[i].sys != sys) continue;
				if (c && crc == jobs[i].crc && !found[i * 160]) snprintf(found + i * 160, 160, "%s", cur);
				if (serial[0] && jobs[i].code[0] && !strcmp(serial, jobs[i].code) && !base[i * 128]) cleanNoIntro(cur, base + i * 128, 128);
			}
		}
		s = e ? e + 1 : NULL;
	}
	free(d);
	SDL_LockMutex(g_metaLock);
	for (int i = 0; i < n; ++i) {
		if (jobs[i].sys != sys) continue;
		struct Meta* m = metaFind(jobs[i].crc);
		if (!m || m->dat) continue;
		if (found[i * 160]) { m->dat = 'S'; snprintf(m->nointro, sizeof(m->nointro), "%s", found + i * 160); }
		else { m->dat = 'N'; snprintf(m->base, sizeof(m->base), "%s", base + i * 128); }
		++m->rev;
		SDL_Log("No-Intro %08X: %s%s", (unsigned) m->crc, m->dat == 'S' ? m->nointro : "no est\xC3\xA1 en la base", m->base[0] ? " (hack?)" : "");
	}
	metaCommit();
	SDL_UnlockMutex(g_metaLock);
	free(found);
	free(base);
}

static bool metaCancelled(void) { return SDL_AtomicGet(&g_metaBusy) < 0; }

/* Portada de RetroArch (libretro-thumbnails) por nombre No-Intro.
 * Devuelve 1 si la guardó, 0 si no existe, -1 si hubo un error de red. */
static int libretroCover(int sys, const char* nointro, uint32_t crc, const char** err) {
	char name[256], enc[800], sysEnc[200], sysRepo[160], url[1400];
	/* caracteres que RetroArch reemplaza por _ en los nombres de archivo */
	snprintf(name, sizeof(name), "%s", nointro);
	for (char* c = name; *c; ++c) if (strchr("&*/:`<>?\\|\"", *c)) *c = '_';
	urlEncode(name, enc, sizeof(enc));
	urlEncode(SYS_DAT[sys], sysEnc, sizeof(sysEnc));
	snprintf(sysRepo, sizeof(sysRepo), "%s", SYS_DAT[sys]);
	for (char* c = sysRepo; *c; ++c) if (*c == ' ') *c = '_';
	bool netErr = false;
	for (int src = 0; src < 2; ++src) {
		if (src == 0) snprintf(url, sizeof(url), "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png", sysEnc, enc);
		else snprintf(url, sizeof(url), "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s.png", sysRepo, enc);
		size_t len = 0;
		*err = NULL;
		unsigned char* d = netGet(url, &len, err);
		if (!d) {
			if (!(*err && strstr(*err, "404"))) netErr = true;
			continue;
		}
		int w = 0, h = 0;
		uint8_t* rgb = decodeImage(d, len, &w, &h);
		char out[1024];
		boxPath(out, sizeof(out), crc);
		bool ok = rgb && saveBoxArt(rgb, w, h, out);
		if (ok) {
			char orig[1024], rel[80];
			snprintf(rel, sizeof(rel), "Caratulas/originales/%08X.png", (unsigned) crc);
			pathJoin(orig, sizeof(orig), rel);
			writeAll(orig, d, len);
			SDL_Log("Portada %08X: RetroArch (%s) %dx%d", (unsigned) crc, src ? "GitHub" : "thumbnails.libretro.com", w, h);
		}
		free(rgb);
		free(d);
		if (ok) return 1;
	}
	return netErr ? -1 : 0;
}

static int metaThread(void* p) {
	struct MetaWork* w = p;
	char msg[200];
	/* 1) nombres oficiales por CRC32 */
	for (int sys = 0; sys < SYS_COUNT && !metaCancelled(); ++sys) {
		bool need = false;
		SDL_LockMutex(g_metaLock);
		for (int i = 0; i < w->n; ++i) {
			struct Meta* m = metaFind(w->jobs[i].crc);
			if (w->jobs[i].sys == sys && m && !m->dat) need = true;
		}
		SDL_UnlockMutex(g_metaLock);
		if (!need) continue;
		char rel[200], path[1024];
		snprintf(rel, sizeof(rel), "Datos/No-Intro/%s.dat", SYS_DAT[sys]);
		pathJoin(path, sizeof(path), rel);
		if (!fileExists(path)) {
			snprintf(msg, sizeof(msg), T("Descargando la base No-Intro de %s..."), SYS_NAME[sys]);
			metaSetStatus(msg);
			char enc[300], url[600];
			urlEncode(SYS_DAT[sys], enc, sizeof(enc));
			snprintf(url, sizeof(url), "%s%s.dat", DAT_URL, enc);
			size_t len = 0;
			const char* err = NULL;
			unsigned char* d = netGet(url, &len, &err);
			if (!d || len < 1000 || !strstr((const char*) d, "game (")) {
				snprintf(msg, sizeof(msg), T("No se pudo descargar la base No-Intro de %s (%s)"), SYS_NAME[sys], err ? T(err) : T("archivo inv\xC3\xA1lido"));
				metaSetToast(msg);
				SDL_AtomicSet(&g_metaPaused, 1);
				free(d);
				continue;  /* se reintenta con F5 o en el próximo inicio */
			}
			writeAll(path, d, len);
			free(d);
		}
		snprintf(msg, sizeof(msg), T("Identificando juegos de %s..."), SYS_NAME[sys]);
		metaSetStatus(msg);
		datLookup(path, w->jobs, w->n, sys);
	}
	/* 1b) una sola vez: versiones anteriores podían bajar la portada de otra
	 * consola (p. ej. la caja de SNES para un juego de Genesis). Se consulta la
	 * plataforma de cada portada guardada, de a 40 por consulta, y las que no
	 * corresponden se borran y se vuelven a buscar. */
	if (w->key[0] && w->nchk && !metaCancelled()) {
		SDL_Log("Revision de plataforma: %d portadas guardadas para revisar", w->nchk);
		bool ok = true;
		int fixed = 0;
		for (int b0 = 0; b0 < w->nchk && ok && !metaCancelled(); b0 += 40) {
			int left = SDL_AtomicGet(&g_tgdbLeft);
			if (left >= 0 && left <= TGDB_RESERVE) { ok = false; break; }
			char ids[600] = "", ek[256], url[1000];
			int nb = 0;
			for (int i = b0; i < w->nchk && nb < 40; ++i, ++nb) {
				SDL_LockMutex(g_metaLock);
				struct Meta* m = metaFind(w->chk[i].crc);
				int id = m ? m->tgdbId : 0;
				SDL_UnlockMutex(g_metaLock);
				size_t l = strlen(ids);
				snprintf(ids + l, sizeof(ids) - l, "%s%d", l ? "," : "", id);
			}
			snprintf(msg, sizeof(msg), T("Revisando portadas %d/%d..."), b0, w->nchk);
			metaSetStatus(msg);
			urlEncode(w->key, ek, sizeof(ek));
			snprintf(url, sizeof(url), "https://api.thegamesdb.net/v1/Games/ByGameID?apikey=%s&id=%s&fields=platform", ek, ids);
			size_t len = 0;
			const char* err = NULL;
			unsigned char* body = netGet(url, &len, &err);
			if (!body) {
				SDL_Log("Revision de plataforma: sin respuesta (%s)", err ? err : "?");
				if (err && strstr(err, "429")) tgdbQuotaHit();
				ok = false;
				break;
			}
			const char* p = (const char*) body;
			struct JNode* root = jValue(&p, 0);
			free(body);
			if (!root) { SDL_Log("Revision de plataforma: respuesta ilegible"); ok = false; break; }
			tgdbNoteAllowance(root);
			struct JNode* games = jGet(jGet(root, "data"), "games");
			for (struct JNode* g = games && games->type == JN_ARR ? games->kid : NULL; g; g = g->next) {
				int gid = jInt(jGet(g, "id")), plat = jInt(jGet(g, "platform"));
				for (int i = b0; i < w->nchk && i < b0 + 40; ++i) {
					SDL_LockMutex(g_metaLock);
					struct Meta* m = metaFind(w->chk[i].crc);
					bool wrong = m && m->tgdbId == gid && !tgdbPlatformOk(w->chk[i].sys, plat);
					if (m && m->tgdbId == gid) SDL_Log("Revision de plataforma: %08X id %d plataforma %d -> %s", (unsigned) m->crc, gid, plat, wrong ? "otra consola, se borra" : "bien");
					if (wrong) {
						char box[1024];
						boxPath(box, sizeof(box), m->crc);
						remove(box);
						m->box = 0; m->tgdbId = 0; m->title[0] = 0; ++m->rev;
						metaCommit();
						w->jobs[w->n++] = w->chk[i];  /* se vuelve a buscar abajo */
						++fixed;
					}
					SDL_UnlockMutex(g_metaLock);
				}
			}
			jFree(root);
		}
		SDL_Log("Revision de plataforma: %d portadas revisadas, %d de otra consola%s", w->nchk, fixed, ok ? "" : " (incompleta: se reintenta al abrir)");
		if (ok) {
			char mk[1024];
			pathJoin(mk, sizeof(mk), "Datos/portadas_plataforma.ok");
			FILE* f = fopen(mk, "w");
			if (f) fclose(f);
			if (fixed) { snprintf(msg, sizeof(msg), T("%d portadas eran de otra consola: se buscan de nuevo"), fixed); metaSetToast(msg); }
		}
	}
	/* 2) portadas: primero las de RetroArch (libretro-thumbnails: por nombre
	 * No-Intro y carpeta de la consola, sin clave ni cupo); TheGamesDB solo para
	 * lo que no se encuentre ahí (hacks, ROMs sin identificar). */
	{
		int total = 0, done = 0, got = 0, upgraded = 0;
		bool tgdbOff = false;  /* TheGamesDB falló (clave, cupo o red): solo RetroArch */
		bool upgradeComplete = true;
		SDL_LockMutex(g_metaLock);
		for (int i = 0; i < w->n; ++i) { struct Meta* m = metaFind(w->jobs[i].crc); if (m && (w->jobs[i].upgrade || !m->box) && w->jobs[i].filename[0]) ++total; }
		SDL_UnlockMutex(g_metaLock);
		for (int i = 0; i < w->n && !metaCancelled(); ++i) {
			struct MetaJob* j = &w->jobs[i];
			char search[160], nointro[200] = "";
			SDL_LockMutex(g_metaLock);
			struct Meta* m = metaFind(j->crc);
			/* Una copia con cabecera o revisión distinta puede no coincidir con
			 * No-Intro. En ese caso se busca por el nombre del archivo: así una
			 * ROM legítima de Zelda sigue obteniendo su portada de TheGamesDB. */
			bool todo = m && j->filename[0] && (j->upgrade ? (m->dat == 'S' && m->nointro[0]) : !m->box);
			if (todo) {
				snprintf(search, sizeof(search), "%s", m->dat == 'S' && m->nointro[0] ? m->nointro : j->filename);
				if (m->dat == 'S') snprintf(nointro, sizeof(nointro), "%s", m->nointro);
			}
			SDL_UnlockMutex(g_metaLock);
			if (!todo) continue;
			++done;
			char out[1024];
			boxPath(out, sizeof(out), j->crc);
			if (!j->upgrade && fileExists(out)) {  /* portada propia puesta por el usuario */
				SDL_LockMutex(g_metaLock);
				m->box = 'S'; ++m->rev; metaCommit();
				SDL_UnlockMutex(g_metaLock);
				continue;
			}
			/* 2a) RetroArch */
			if (nointro[0]) {
				snprintf(msg, sizeof(msg), j->upgrade ? T("Mejorando portadas %d/%d...") : T("Buscando portadas %d/%d..."), done, total);
				metaSetStatus(msg);
				const char* err = NULL;
				int lr = libretroCover(j->sys, nointro, j->crc, &err);
				if (lr == 1) {
					SDL_LockMutex(g_metaLock);
					m->box = 'S'; m->tgdbId = 0; ++m->rev;
					metaCommit();
					SDL_UnlockMutex(g_metaLock);
					if (j->upgrade) ++upgraded; else ++got;
					continue;
				}
				if (lr < 0 && j->upgrade) upgradeComplete = false;
			}
			if (j->upgrade) continue;  /* la portada que ya tenía se queda */
			/* 2b) TheGamesDB */
			if (tgdbOff) continue;  /* se intenta en el próximo inicio */
			if (!w->key[0]) {
				SDL_LockMutex(g_metaLock);
				m->box = 'N'; ++m->rev; metaCommit();
				SDL_UnlockMutex(g_metaLock);
				continue;
			}
			{
				int left = SDL_AtomicGet(&g_tgdbLeft);
				if (left >= 0 && left <= TGDB_RESERVE) {
					char cupo[120];
					tgdbAllowanceText(cupo, sizeof(cupo));
					snprintf(msg, sizeof(msg), T("Pausado para no agotar TheGamesDB: %s"), cupo);
					metaSetToast(msg);
					tgdbOff = true;
					continue;  /* RetroArch sigue con los demás juegos */
				}
				if (left >= 0) snprintf(msg, sizeof(msg), T("Buscando portadas %d/%d  \xC2\xB7  quedan %d consultas"), done, total, left);
				else snprintf(msg, sizeof(msg), T("Buscando portadas %d/%d..."), done, total);
			}
			metaSetStatus(msg);
			int id = 0;
			char title[128], img[700];
			const char* err = NULL;
			int r = tgdbFind(w->key, j->sys, search, &id, title, sizeof(title), img, sizeof(img), &err);
			if (r == TG_KEY) { metaSetToast(T("TheGamesDB rechaz\xC3\xB3 la clave o se acab\xC3\xB3 el cupo del mes")); tgdbOff = true; continue; }
			if (r == TG_QUOTA) { metaSetToast(T("TheGamesDB: se agotaron las consultas por ahora. Se sigue en unas horas.")); tgdbOff = true; continue; }
			if (r == TG_NET) { snprintf(msg, sizeof(msg), T("Sin conexi\xC3\xB3n con TheGamesDB (%s)"), err ? T(err) : T("error")); metaSetToast(msg); tgdbOff = true; continue; }
			char box = 'N';
			if (r == TG_FOUND && img[0]) {
				size_t len = 0;
				unsigned char* jpg = netGet(img, &len, &err);
				int iw = 0, ih = 0;
				uint8_t* rgb = jpg ? decodeImage(jpg, len, &iw, &ih) : NULL;
				if (rgb && saveBoxArt(rgb, iw, ih, out)) {
					box = 'S';
					++got;
					char orig[1024], rel[80];
					snprintf(rel, sizeof(rel), "Caratulas/originales/%08X.%s", (unsigned) j->crc, len > 4 && jpg[0] == 0x89 ? "png" : "jpg");
					pathJoin(orig, sizeof(orig), rel);
					writeAll(orig, jpg, len);
				} else if (!jpg) {
					snprintf(msg, sizeof(msg), T("No se pudo descargar una portada (%s)"), err ? T(err) : T("error"));
					metaSetToast(msg);
					tgdbOff = true;
					free(rgb);
					free(jpg);
					continue;  /* se reintenta luego */
				}
				SDL_Log("Portada %08X: %s (%dx%d) -> %s", (unsigned) j->crc, title, iw, ih, box == 'S' ? "guardada" : "no se pudo leer");
				free(rgb);
				free(jpg);
			} else SDL_Log("Portada %08X: sin resultado en TheGamesDB para \"%s\"", (unsigned) j->crc, search);
			SDL_LockMutex(g_metaLock);
			m->box = box;
			m->tgdbId = id;
			snprintf(m->title, sizeof(m->title), "%s", r == TG_FOUND ? title : "");
			++m->rev;
			metaCommit();
			SDL_UnlockMutex(g_metaLock);
		}
		if (got) { snprintf(msg, sizeof(msg), T("%d portada%s nueva%s"), got, got == 1 ? "" : "s", got == 1 ? "" : "s"); metaSetToast(msg); }
		if (upgraded) SDL_Log("Portadas: %d reemplazadas por las de RetroArch", upgraded);
		if (w->nupgrade && upgradeComplete && !metaCancelled()) {
			char mk[1024];
			pathJoin(mk, sizeof(mk), "Datos/portadas_libretro.ok");
			FILE* f = fopen(mk, "w");
			if (f) fclose(f);
			if (upgraded) { snprintf(msg, sizeof(msg), T("%d portadas actualizadas con las de RetroArch"), upgraded); metaSetToast(msg); }
		}
	}
	free(w->jobs);
	free(w->chk);
	free(w);
	metaSetStatus("");
	SDL_AtomicSet(&g_metaBusy, 0);
	return 0;
}

/* ---- hilo principal ---- */
static void metaInit(void) {
	if (!g_metaLock) g_metaLock = SDL_CreateMutex();
	char d[1024];
	pathJoin(d, sizeof(d), "Datos"); MKDIR(d);
	pathJoin(d, sizeof(d), "Datos/No-Intro"); MKDIR(d);
	pathJoin(d, sizeof(d), "Caratulas"); MKDIR(d);
	pathJoin(d, sizeof(d), "Caratulas/originales"); MKDIR(d);
	metaLoad();
	tgdbLoadAllowance();
	/* v0.3 guardaba la portada ya encajada (<CRC>_portada.bmp): se rehace desde el JPG original.
	 * Se hace una sola vez: revisar miles de archivos en cada inicio demoraba la apertura. */
	char mk[1024];
	pathJoin(mk, sizeof(mk), "Datos/portadas_v04.ok");
	if (fileExists(mk)) return;
	for (int i = 0; i < g_metaN; ++i) {
		if (g_meta[i].box != 'S') continue;
		char box[1024], old[1024], jpg[1024], rel[80];
		boxPath(box, sizeof(box), g_meta[i].crc);
		snprintf(rel, sizeof(rel), "Caratulas/%08X_portada.bmp", (unsigned) g_meta[i].crc);
		pathJoin(old, sizeof(old), rel);
		snprintf(rel, sizeof(rel), "Caratulas/originales/%08X.jpg", (unsigned) g_meta[i].crc);
		pathJoin(jpg, sizeof(jpg), rel);
		if (!fileExists(box) && fileExists(jpg)) {
			size_t len = 0;
			unsigned char* d2 = swsReadFile(jpg, &len);
			int w = 0, h = 0;
			uint8_t* rgb = d2 ? decodeImage(d2, len, &w, &h) : NULL;
			if (rgb && saveBoxArt(rgb, w, h, box)) SDL_Log("Portada %08X convertida al formato nuevo", (unsigned) g_meta[i].crc);
			free(rgb);
			free(d2);
		}
		if (fileExists(box) && fileExists(old)) remove(old);
		if (!fileExists(box)) g_meta[i].box = 0;  /* sin archivo: se vuelve a buscar */
	}
	FILE* mf = fopen(mk, "w");
	if (mf) fclose(mf);
}

/* Una entrada por CRC de la biblioteca (con el hilo parado) */
static void metaPrepare(void) {
	for (int i = 0; i < g_libN; ++i) {
		if (metaFind(g_lib[i].crc)) continue;
		struct Meta* nm = realloc(g_meta, sizeof(*g_meta) * (size_t) (g_metaN + 1));
		if (!nm) return;
		g_meta = nm;
		memset(&g_meta[g_metaN], 0, sizeof(*g_meta));
		g_meta[g_metaN].crc = g_lib[i].crc;
		g_meta[g_metaN].rev = 1;
		++g_metaN;
	}
}

/* Pasa los metadatos a la biblioteca (nombres y aviso de portada nueva) */
static void metaApply(void) {
	SDL_LockMutex(g_metaLock);
	for (int i = 0; i < g_libN; ++i) {
		struct LibGame* g = &g_lib[i];
		struct Meta* m = metaFind(g->crc);
		if (!m || m->rev == g->metaRev) continue;
		g->metaRev = m->rev;
		if (m->dat == 'S') {
			snprintf(g->official, sizeof(g->official), "%s", m->nointro);
			if (m->title[0]) snprintf(g->name, sizeof(g->name), "%s", m->title);
			else cleanNoIntro(m->nointro, g->name, sizeof(g->name));
			g->note[0] = 0;
		} else if (m->dat == 'N' && m->base[0]) {
			snprintf(g->note, sizeof(g->note), T("Hack o versi\xC3\xB3n modificada de %s"), m->base);
		}
		/* volver a cargar la etiqueta por si llegó la portada */
		g->labelChecked = false;
		if (g->label) { SDL_FreeSurface(g->label); g->label = NULL; }
		if (g->card) { SDL_DestroyTexture(g->card); g->card = NULL; }
	}
	SDL_UnlockMutex(g_metaLock);
}

/* Portadas elegidas a mano: no se reemplazan automáticamente */
static void manualPickAdd(uint32_t crc) {
	char p[1024];
	pathJoin(p, sizeof(p), "Datos/portadas_manuales.txt");
	FILE* f = fopen(p, "a");
	if (f) { fprintf(f, "%08X\n", (unsigned) crc); fclose(f); }
}

static bool manualPickHas(const uint32_t* list, int n, uint32_t crc) {
	for (int i = 0; i < n; ++i) if (list[i] == crc) return true;
	return false;
}

static void startMeta(struct App* a) {
	if (g_metaThr) return;
	/* F5 también debe dar una segunda oportunidad a una portada que no se
	 * encontró. Sin esto, el estado 'N' queda guardado para siempre y F5 solo
	 * reintenta los errores de red o de clave. */
	bool manualRescan = g_manualRescan;
	if (manualRescan) SDL_AtomicSet(&g_metaPaused, 0);
	g_manualRescan = false;
	if (SDL_AtomicGet(&g_metaPaused)) return;
	struct MetaWork* w = calloc(1, sizeof(*w));
	if (!w) return;
	w->jobs = calloc((size_t) g_libN * 3 + 1, sizeof(*w->jobs));  /* lugar para las que se vuelven a buscar */
	if (!w->jobs) { free(w); return; }
	snprintf(w->key, sizeof(w->key), "%s", a->cfg.tgdbKey);
	{
		/* revisión única de plataforma de las portadas ya bajadas */
		char mk[1024];
		pathJoin(mk, sizeof(mk), "Datos/portadas_plataforma.ok");
		if (w->key[0] && !fileExists(mk) && (w->chk = calloc((size_t) g_libN + 1, sizeof(*w->chk)))) {
			int noId = 0;
			for (int i = 0; i < g_libN; ++i) {
				struct Meta* m = metaFind(g_lib[i].crc);
				if (m && m->box == 'S' && m->tgdbId <= 0) ++noId;
				if (!m || m->box != 'S' || m->tgdbId <= 0) continue;
				if (m->dat == 'S' && m->nointro[0]) continue;  /* esas se cambian por las de RetroArch, sin gastar cupo */
				w->chk[w->nchk].crc = g_lib[i].crc;
				w->chk[w->nchk].sys = g_lib[i].sys;
				snprintf(w->chk[w->nchk].filename, sizeof(w->chk[0].filename), "%s", g_lib[i].name);
				++w->nchk;
			}
			SDL_Log("Revision de plataforma: %d portadas con id de TheGamesDB, %d sin id (no se pueden revisar)", w->nchk, noId);
		}
	}
	for (int i = 0; i < g_libN; ++i) {
		struct Meta* m = metaFind(g_lib[i].crc);
		if (!m) continue;
		bool retryBox = manualRescan && m->box == 'N';
		bool need = !m->dat || !m->box || retryBox;  /* RetroArch no necesita clave */
		bool dup = false;
		for (int k = 0; k < w->n; ++k) if (w->jobs[k].crc == g_lib[i].crc) dup = true;
		if (!need || dup) continue;
		w->jobs[w->n].crc = g_lib[i].crc;
		w->jobs[w->n].sys = g_lib[i].sys;
		snprintf(w->jobs[w->n].code, sizeof(w->jobs[0].code), "%s", g_lib[i].sys == SYS_GBA ? g_lib[i].code : "");
		snprintf(w->jobs[w->n].filename, sizeof(w->jobs[0].filename), "%s", g_lib[i].name);
		++w->n;
	}
	/* con cupo limitado, primero lo que más se usa: favoritos y jugados */
	for (int i = 1; i < w->n; ++i) {
		struct MetaJob t = w->jobs[i];
		int pt = gamePriority(t.crc), k = i - 1;
		while (k >= 0 && gamePriority(w->jobs[k].crc) < pt) { w->jobs[k + 1] = w->jobs[k]; --k; }
		w->jobs[k + 1] = t;
	}
	{
		/* una sola vez: las portadas de TheGamesDB (y las no encontradas) de juegos
		 * identificados por No-Intro se cambian por las de RetroArch, que siempre son
		 * de la consola correcta. Las elegidas a mano se respetan. Van al final. */
		char mk[1024];
		pathJoin(mk, sizeof(mk), "Datos/portadas_libretro.ok");
		if (!fileExists(mk)) {
			uint32_t* manual = NULL;
			int nm = 0;
			char p[1024];
			pathJoin(p, sizeof(p), "Datos/portadas_manuales.txt");
			FILE* f = fopen(p, "r");
			if (f) {
				char line[64];
				while (fgets(line, sizeof(line), f)) {
					uint32_t* t = realloc(manual, sizeof(*manual) * (size_t) (nm + 1));
					if (!t) break;
					manual = t;
					manual[nm++] = (uint32_t) strtoul(line, NULL, 16);
				}
				fclose(f);
			}
			for (int i = 0; i < g_libN; ++i) {
				struct Meta* m = metaFind(g_lib[i].crc);
				if (!m || m->dat != 'S' || !m->nointro[0] || !(m->box == 'S' || m->box == 'N')) continue;
				if (manualPickHas(manual, nm, g_lib[i].crc)) continue;
				bool dup = false;
				for (int k = 0; k < w->n; ++k) if (w->jobs[k].crc == g_lib[i].crc) dup = true;
				if (dup) continue;
				struct MetaJob* j = &w->jobs[w->n++];
				j->crc = g_lib[i].crc;
				j->sys = g_lib[i].sys;
				snprintf(j->filename, sizeof(j->filename), "%s", g_lib[i].name);
				j->upgrade = true;
				++w->nupgrade;
			}
			free(manual);
			SDL_Log("Portadas: %d para cambiar por las de RetroArch", w->nupgrade);
		}
	}
	if (!w->n && !w->nchk) { free(w->jobs); free(w->chk); free(w); return; }
	SDL_AtomicSet(&g_metaBusy, 1);
	g_metaThr = SDL_CreateThread(metaThread, "metadatos", w);
	if (!g_metaThr) { free(w->jobs); free(w->chk); free(w); SDL_AtomicSet(&g_metaBusy, 0); }
	else SDL_Log("Metadatos: %d juego(s) por revisar%s", w->n, a->cfg.tgdbKey[0] ? "" : " (sin clave de TheGamesDB: solo nombres)");
}

/* exiting: no esperar más de unos segundos a una descarga lenta */
static void stopMeta(bool exiting) {
	if (!g_metaThr) return;
	if (SDL_AtomicGet(&g_metaBusy) > 0) SDL_AtomicSet(&g_metaBusy, -1);
	if (exiting) {
		Uint32 t0 = SDL_GetTicks();
		while (SDL_AtomicGet(&g_metaBusy) != 0 && SDL_GetTicks() - t0 < 3000) SDL_Delay(20);
		if (SDL_AtomicGet(&g_metaBusy) != 0) { SDL_DetachThread(g_metaThr); g_metaThr = NULL; return; }
	}
	SDL_WaitThread(g_metaThr, NULL);
	g_metaThr = NULL;
}

/* Cada cuadro: aplicar resultados nuevos y mostrar avisos */
static void metaPump(struct App* a) {
	if (g_metaThr && SDL_AtomicGet(&g_metaBusy) == 0) { SDL_WaitThread(g_metaThr, NULL); g_metaThr = NULL; }
	if (SDL_AtomicGet(&g_metaDirty)) {
		SDL_AtomicSet(&g_metaDirty, 0);
		metaApply();
		libCacheSave();
	}
	if (!g_metaLock) return;
	SDL_LockMutex(g_metaLock);
	if (g_metaToast[0]) { setMessage(a, g_metaToast); g_metaToast[0] = 0; }
	SDL_UnlockMutex(g_metaLock);
}
