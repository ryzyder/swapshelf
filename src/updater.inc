/* ================================================================== */
/* Actualizaciones desde GitHub                                        */
/*  Al abrir, si hay internet, pregunta a GitHub cuál es la última       */
/*  versión publicada (Releases de ryzyder/swapshelf). Si es más nueva,  */
/*  ofrece descargarla: en Switch reemplaza el .nro y se vuelve a abrir; */
/*  en PC avisa dónde bajarla. Nunca instala nada sin preguntar, y se    */
/*  apaga con actualizar=no en swapshelf.ini o desde Opciones.           */
/* ================================================================== */
#define UPD_REPO "ryzyder/swapshelf"
#define UPD_API "https://api.github.com/repos/" UPD_REPO "/releases/latest"
#define UPD_PAGE "github.com/" UPD_REPO "/releases"

enum { UPD_IDLE, UPD_CHECKING, UPD_READY, UPD_NONE, UPD_DONE };
static SDL_atomic_t g_updState;
static char g_updVersion[32];     /* "0.7" */
static char g_updNotes[2048];     /* texto de la Release, ya sin markdown */
static char g_updUrl[512];        /* .nro adjunto a la Release */
#ifdef __SWITCH__
static char g_nroPath[512];       /* el .nro que se está ejecutando */
#endif

/* 1 si a > b ("0.10" > "0.9") */
static int updNewer(const char* a, const char* b) {
	while (*a || *b) {
		long x = strtol(a, (char**) &a, 10), y = strtol(b, (char**) &b, 10);
		if (x != y) return x > y;
		if (*a == '.') ++a;
		if (*b == '.') ++b;
		if (!*a && !*b) break;
		if ((*a && !isdigit((unsigned char) *a)) || (*b && !isdigit((unsigned char) *b))) break;
	}
	return 0;
}

/* Lee el valor de texto de "key" en un JSON simple, desde p. Devuelve el
 * puntero después del valor, o NULL. Entiende \n, \" y \\ (lo demás se copia). */
static const char* updJsonStr(const char* p, const char* key, char* out, size_t n) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char* k = strstr(p, pat);
	if (!k) return NULL;
	k += strlen(pat);
	while (*k == ' ' || *k == ':') ++k;
	if (*k != '"') return NULL;
	++k;
	size_t o = 0;
	while (*k && *k != '"') {
		char c = *k++;
		if (c == '\\' && *k) {
			char e = *k++;
			c = e == 'n' ? '\n' : e == 't' ? ' ' : e == 'r' ? '\r' : e;
			if (e == 'u') { k += 4; c = '?'; }
		}
		if (o + 1 < n) out[o++] = c;
	}
	out[o] = 0;
	return *k ? k + 1 : k;
}

/* Deja las notas de la Release en líneas cortas, sin "#", "*" ni "-" de markdown */
static void updCleanNotes(char* s) {
	char* w = s;
	for (char* r = s; *r;) {
		if (*r == '\r') { ++r; continue; }
		bool lineStart = r == s || r[-1] == '\n';
		if (lineStart) {
			while (*r == '#' || *r == ' ') ++r;
			if ((*r == '-' || *r == '*') && r[1] == ' ') { *w++ = '\xC2'; *w++ = '\xB7'; *w++ = ' '; r += 2; continue; }
		}
		if (*r == '*' || *r == '`') { ++r; continue; }
		*w++ = *r++;
	}
	*w = 0;
}

static int updThread(void* ud) {
	(void) ud;
	size_t len = 0;
	const char* err = NULL;
	unsigned char* js = NULL;
#ifdef SWS_TEST
	if (getenv("SWS_TEST_UPDATE")) js = swsReadFile(getenv("SWS_TEST_UPDATE"), &len);  /* respuesta de prueba */
	else
#endif
	js = swsHttpGet(UPD_API, &len, &err, NULL, NULL);
	if (!js) { SDL_Log("Actualizaciones: no se pudo consultar GitHub (%s)", err ? err : "?"); SDL_AtomicSet(&g_updState, UPD_NONE); return 0; }
	char tag[32] = "";
	const char* p = (const char*) js;
	if (!updJsonStr(p, "tag_name", tag, sizeof(tag))) { SDL_Log("Actualizaciones: respuesta sin versión"); free(js); SDL_AtomicSet(&g_updState, UPD_NONE); return 0; }
	const char* v = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
	if (!updNewer(v, SWS_VERSION)) {
		SDL_Log("Actualizaciones: al día (GitHub %s, esta %s)", v, SWS_VERSION);
		free(js);
		SDL_AtomicSet(&g_updState, UPD_NONE);
		return 0;
	}
	snprintf(g_updVersion, sizeof(g_updVersion), "%s", v);
	g_updNotes[0] = 0;
	updJsonStr(p, "body", g_updNotes, sizeof(g_updNotes));
	updCleanNotes(g_updNotes);
	/* el adjunto .nro: buscar cada browser_download_url y quedarse con el que termina en .nro */
	g_updUrl[0] = 0;
	const char* q = p;
	char url[512];
	while ((q = updJsonStr(q, "browser_download_url", url, sizeof(url))) != NULL) {
		size_t n = strlen(url);
		if (n > 4 && !strcmp(url + n - 4, ".nro")) { snprintf(g_updUrl, sizeof(g_updUrl), "%s", url); break; }
	}
	free(js);
	SDL_Log("Actualizaciones: hay una versión nueva %s (esta %s)%s", g_updVersion, SWS_VERSION, g_updUrl[0] ? "" : ", sin .nro adjunto");
	SDL_AtomicSet(&g_updState, UPD_READY);
	return 0;
}

static void updStart(struct App* a) {
	if (!a->cfg.autoUpdate || SDL_AtomicGet(&g_updState) != UPD_IDLE) return;
	SDL_AtomicSet(&g_updState, UPD_CHECKING);
	SDL_Thread* t = SDL_CreateThread(updThread, "actualizar", NULL);
	if (t) SDL_DetachThread(t);
	else SDL_AtomicSet(&g_updState, UPD_NONE);
}

/* Texto en varias líneas que caben en maxW; devuelve la y final */
static int updWrap(struct App* a, const char* s, int x, int y, int maxW, int maxLines, Uint32 col) {
	char line[256];
	int lines = 0;
	while (*s && lines < maxLines) {
		const char* nl = strchr(s, '\n');
		size_t seg = nl ? (size_t) (nl - s) : strlen(s);
		if (seg == 0) { s = nl + 1; y += 12; continue; }  /* línea en blanco: medio renglón */
		size_t take = seg < sizeof(line) - 1 ? seg : sizeof(line) - 1;
		memcpy(line, s, take); line[take] = 0;
		/* recortar por palabras hasta que entre */
		while (textW(F_SMALL, line, 0.56f) > maxW) {
			char* sp = strrchr(line, ' ');
			if (!sp) break;
			*sp = 0;
		}
		size_t used = strlen(line);
		text(a, F_SMALL, 0.56f, line, x, y, col, 255, AL_LEFT);
		y += 28; ++lines;
		s += used;
		while (*s == ' ') ++s;
		if (*s == '\n') ++s;
	}
	return y;
}

static void updDrawPanel(struct App* a, const char* title, const char* sub) {
	themeClearColor(a);
	SDL_RenderClear(a->renderer);
	fillRect2(a, 0, 0, LIB_W, LIB_H, 0x000000, 120);
	fillRR(a, 260, 120, 760, 480, 22, TH.panel, 255);
	fillRR(a, 260, 120, 760, 6, 3, TH.acc, 255);
	drawBrand(a, F_ITEM, 0.5f, 300, 150, TH.text, 255, AL_LEFT);
	text(a, F_ITEM, 0.62f, title, 300, 196, TH.text, 255, AL_LEFT);
	if (sub) text(a, F_SMALL, 0.56f, sub, 300, 244, TH.dim, 255, AL_LEFT);
}

#ifdef __SWITCH__
struct UpdDl { volatile long long done, total; unsigned char* buf; size_t len; const char* err; SDL_atomic_t fin; };
static int updDlThread(void* ud) {
	struct UpdDl* c = ud;
	c->buf = swsHttpGet(g_updUrl, &c->len, &c->err, &c->done, &c->total);
	SDL_AtomicSet(&c->fin, 1);
	return 0;
}
#endif

/* Pantalla de descarga; true si el .nro nuevo quedó en su lugar */
static bool updDownloadAndInstall(struct App* a) {
#ifdef __SWITCH__
	static struct UpdDl ctx;
	memset(&ctx, 0, sizeof(ctx));
	volatile long long* done = &ctx.done;
	volatile long long* total = &ctx.total;
	SDL_Thread* th = SDL_CreateThread(updDlThread, "descarga", &ctx);
	if (!th) return false;
	while (!SDL_AtomicGet(&ctx.fin)) {
		appletMainLoop();
		SDL_PumpEvents();
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		char sub[96];
		if (*total > 0) snprintf(sub, sizeof(sub), T("Descargando la versi\xC3\xB3n %s: %lld de %lld KB"), g_updVersion, *done / 1024, *total / 1024);
		else snprintf(sub, sizeof(sub), T("Descargando la versi\xC3\xB3n %s..."), g_updVersion);
		updDrawPanel(a, T("Actualizando SwapShelf"), sub);
		float k = *total > 0 ? (float) *done / (float) *total : 0;
		fillRR(a, 300, 320, 680, 10, 5, TH.panel3, 255);
		if (k > 0) fillRR(a, 300, 320, 680 * k, 10, 5, TH.acc, 255);
		text(a, F_SMALL, 0.52f, T("No apagues la consola ni cierres la app."), 300, 352, TH.dim2, 255, AL_LEFT);
		SDL_RenderPresent(a->renderer);
		SDL_Delay(30);
	}
	SDL_WaitThread(th, NULL);
	if (!ctx.buf) { SDL_Log("Actualizaciones: fall\xC3\xB3 la descarga (%s)", ctx.err ? ctx.err : "?"); return false; }
	/* un .nro tiene "NRO0" en 0x10; si no, es una página de error */
	if (ctx.len < 1024 * 1024 || memcmp(ctx.buf + 0x10, "NRO0", 4) != 0) {
		SDL_Log("Actualizaciones: el archivo descargado no es un .nro (%u bytes)", (unsigned) ctx.len);
		free(ctx.buf);
		return false;
	}
	char tmp[600], old[600];
	snprintf(tmp, sizeof(tmp), "%s.nuevo", g_nroPath);
	snprintf(old, sizeof(old), "%s.anterior", g_nroPath);
	FILE* f = fopen(tmp, "wb");
	bool ok = f && fwrite(ctx.buf, 1, ctx.len, f) == ctx.len;
	if (f) fclose(f);
	free(ctx.buf);
	if (!ok) { remove(tmp); SDL_Log("Actualizaciones: no se pudo escribir %s", tmp); return false; }
	/* cambiar los archivos: el viejo queda como .anterior por si hay que volver */
	remove(old);
	if (rename(g_nroPath, old) != 0) { remove(tmp); SDL_Log("Actualizaciones: no se pudo apartar %s", g_nroPath); return false; }
	if (rename(tmp, g_nroPath) != 0) { rename(old, g_nroPath); SDL_Log("Actualizaciones: no se pudo poner el .nro nuevo"); return false; }
	SDL_Log("Actualizaciones: instalada la versi\xC3\xB3n %s en %s", g_updVersion, g_nroPath);
	return true;
#else
	(void) a;
	return false;
#endif
}

/* Si ya se sabe que hay una versión nueva, la ofrece una sola vez */
static void updPump(struct App* a) {
	if (SDL_AtomicGet(&g_updState) != UPD_READY) return;
	SDL_AtomicSet(&g_updState, UPD_DONE);
	int focus = 1;
#ifdef __SWITCH__
	bool canInstall = g_updUrl[0] && g_nroPath[0];
#else
	bool canInstall = false;
#endif
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; return; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; return; }
		if (in == IN_BACK) return;
		if (in == IN_LEFT || in == IN_UP) focus = 0;
		if (in == IN_RIGHT || in == IN_DOWN) focus = 1;
		if (in == IN_OK) {
			if (focus == 0 || !canInstall) return;
			break;
		}
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		char t[96], sub[96];
		snprintf(t, sizeof(t), T("Hay una versi\xC3\xB3n nueva: %s"), g_updVersion);
		snprintf(sub, sizeof(sub), T("Tienes la %s. Novedades:"), SWS_VERSION);
		updDrawPanel(a, t, sub);
		updWrap(a, g_updNotes[0] ? g_updNotes : T("Mejoras y correcciones."), 300, 284, 680, 8, TH.text2);
		if (!canInstall) text(a, F_SMALL, 0.54f, T("B\xC3\xA1jala desde " UPD_PAGE), 300, 478, TH.acc, 255, AL_LEFT);
		const char* opts[2] = { T("Ahora no"), canInstall ? T("Actualizar") : T("Entendido") };
		for (int i = 0; i < 2; ++i) {
			float x = 520 + i * 240;
			bool on = focus == i;
			fillRR(a, x, 528, 220, 50, 25, on ? TH.acc : TH.panel3, 255);
			text(a, F_SMALL, 0.6f, opts[i], (int) (x + 110), 540, on ? 0x0A0C10 : TH.text, 255, AL_CENTER);
		}
#ifdef SWS_TEST
		{ static bool shot = false; if (!shot) { shot = true; testCapture(a, "actualizacion"); } }
#endif
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
	if (!updDownloadAndInstall(a)) {
		setMessage(a, T("No se pudo actualizar. Se sigue usando esta versi\xC3\xB3n."));
		a->messageUntil = SDL_GetTicks() + 6000;
		return;
	}
#ifdef __SWITCH__
	/* volver a abrir el .nro nuevo al salir (hbloader lo carga solo) */
	if (envHasNextLoad()) envSetNextLoad(g_nroPath, g_nroPath);
	Uint32 t0 = SDL_GetTicks();
	while (SDL_GetTicks() - t0 < 1800) {
		appletMainLoop();
		SDL_PumpEvents();
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		char sub[96];
		snprintf(sub, sizeof(sub), T("Versi\xC3\xB3n %s instalada. Reiniciando..."), g_updVersion);
		updDrawPanel(a, T("Listo"), sub);
		SDL_RenderPresent(a->renderer);
		SDL_Delay(30);
	}
	a->quit = true;
#endif
}
