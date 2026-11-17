/*
 * SwapShelf (SWS) - estantería de cartuchos para tus propias copias de juegos retro
 * (PC y Nintendo Switch). Autor: Zadark.
 *
 * Biblioteca por consola (Roms/<consola>), identificación por CRC32 y header,
 * nombres oficiales (No-Intro) y portadas (TheGamesDB), animación de cartucho,
 * filtro Zadark HD y controles por consola o por juego.
 *
 * Por dentro usa el núcleo de mGBA (libmgba, MPL 2.0). No incluye juegos.
 */
#include <mgba/flags.h>
/* En Switch el CMake de mGBA escribe MINIMAL_CORE en flags.h ("Generic"), pero la
 * librería se compila SIN él (sus .c no incluyen flags.h). Si lo dejamos, struct mCore
 * queda 24 bytes más corta aquí (falta inputMap) y core->init apunta a basura. */
#undef MINIMAL_CORE
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>
#include <libretro.h>

#include <SDL.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "netutil.h"
#include "menu_music.h"
#include "xbr.h"
#include "postfx.h"
#include "gbaui.h"
#include "lang.inc"  /* textos en español, inglés y portugués */

#ifdef __SWITCH__
#include <switch.h>
#define BASE_DIR "sdmc:/switch/swapshelf/"
#endif

#define SWS_VERSION  "0.7"
#define APP_TITLE    "SwapShelf (v" SWS_VERSION ")"
#define GBA_W        240
#define GBA_H        160
#define VIDEO_MAX_W  320
#define VIDEO_MAX_H  240
#define SAMPLE_RATE  48000
#define GBA_FPS      59.7275

/* Bits de botones de GBA (orden del registro KEYINPUT) */
enum { K_A = 0, K_B, K_SELECT, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_R, K_L, K_X, K_Y, K_C, K_Z };
#define K_GBA_MASK 0x3FFu  /* mGBA solo usa los 10 primeros bits */


/* ================================================================== */
/* Configuración (swapshelf.ini)                                       */
/* ================================================================== */
enum ScaleMode { SCALE_FIT, SCALE_INTEGER, SCALE_STRETCH };
enum BorderMode { BORDER_THEME, BORDER_BLUR, BORDER_BLACK };

enum { SHADER_NORMAL, SHADER_ZHD };
#define ZHD_NAME "ZADARK HD"  /* nombre del filtro (pausa) */

struct Settings {
	char romDir[256];
	char music[256];            /* música del menú (vacío = sin música) */
	int musicVolume;            /* 0-100 */
	bool fullscreen;
	enum ScaleMode scale;
	bool smooth;
	enum BorderMode border;     /* qué se ve a los costados (pantallas 16:9) */
	int shader;                 /* SHADER_NORMAL | SHADER_ZHD */
	struct ZadarkHD zhd;        /* niveles del filtro Zadark HD */
	int volume;
	bool swapAB;
	int turbo;
	int windowScale;
	char tgdbKey[128];          /* clave de TheGamesDB (vacía = solo nombres, sin portadas) */
	bool boxArt;                /* portada de la caja en vez de la pantalla de título */
	bool resume;                /* continuar donde se dejó cada juego */
	bool autoUpdate;            /* buscar versiones nuevas en GitHub al abrir */
	int gridCols;               /* cartuchos por fila en cada consola (5-9) */
	int gridZoom;               /* aumento del cartucho elegido, en % (100-170) */
	int lang;                   /* LANG_ES, LANG_EN o LANG_PT */
	bool lightTheme;            /* menús claros */
	int accent;                 /* color de acento de los menús (ACC_*) */
	unsigned sysHidden;         /* consolas ocultas por el usuario (bit por consola) */
};

static void settingsDefault(struct Settings* s) {
	memset(s, 0, sizeof(*s));
	s->zhd = (struct ZadarkHD) { .textura = 1, .bordes = 2, .vivo = 3, .nitidez = 60, .brillo = 22 };
	snprintf(s->romDir, sizeof(s->romDir), "Roms");
#ifdef __SWITCH__
	s->fullscreen = true;
#endif
	s->scale = SCALE_FIT;
	s->volume = 100;
	s->turbo = 4;
	s->windowScale = 4;
	/* sin música por defecto: se puede poner una propia con musica_menu= */
	s->musicVolume = 20;
	s->boxArt = true;
	s->resume = true;
	s->autoUpdate = true;
	s->gridCols = 7;
	s->gridZoom = 130;
#ifdef SWS_TGDB_KEY
	snprintf(s->tgdbKey, sizeof(s->tgdbKey), "%s", SWS_TGDB_KEY);  /* clave incluida al compilar */
#endif
}

static char* trim(char* s) {
	while (isspace((unsigned char) *s)) ++s;
	char* e = s + strlen(s);
	while (e > s && isspace((unsigned char) e[-1])) --e;
	*e = '\0';
	return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static Uint32 g_pausedMs;     /* tiempo en pausa durante la partida actual */
static char g_iniPath[1024];  /* swapshelf.ini en uso (las Opciones escriben aquí) */

/* Cambia o agrega "clave=valor" en el ini, sin tocar comentarios ni el resto */
static bool iniSet(const char* key, const char* value) {
	FILE* f = fopen(g_iniPath, "rb");
	char* text = NULL;
	size_t len = 0;
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		text = malloc((size_t) (n > 0 ? n : 0) + 1);
		if (text) { len = fread(text, 1, (size_t) (n > 0 ? n : 0), f); text[len] = 0; }
		fclose(f);
	}
	FILE* o = fopen(g_iniPath, "wb");
	if (!o) { free(text); return false; }
	bool done = false;
	const char* p = text ? text : "";
	while (*p) {
		const char* e = strchr(p, '\n');
		size_t ll = e ? (size_t) (e - p + 1) : strlen(p);
		const char* q = p;
		while (q < p + ll && (*q == ' ' || *q == '\t')) ++q;
		size_t kl = strlen(key);
		const char* r = q + kl;
		bool match = !done && !strncmp(q, key, kl);
		if (match) { while (*r == ' ' || *r == '\t') ++r; match = *r == '='; }
		if (match) {
			fprintf(o, "%s=%s%s", key, value, memchr(p, '\r', ll) ? "\r\n" : "\n");
			done = true;
		} else fwrite(p, 1, ll, o);
		p += ll;
	}
	if (!done) {
		if (len && text[len - 1] != '\n') fputc('\n', o);
		fprintf(o, "%s=%s\n", key, value);
	}
	free(text);
	return fclose(o) == 0;
}

static void settingsLoad(struct Settings* s, const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char* l = trim(line);
		if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
		char* eq = strchr(l, '=');
		if (!eq) continue;
		*eq = '\0';
		char* k = trim(l);
		char* v = trim(eq + 1);
		if (!strcmp(k, "carpeta_roms")) snprintf(s->romDir, sizeof(s->romDir), "%s", v);
		else if (!strcmp(k, "musica_menu")) snprintf(s->music, sizeof(s->music), "%s", v);
		else if (!strcmp(k, "volumen_musica")) s->musicVolume = clampi(atoi(v), 0, 100);
		else if (!strcmp(k, "pantalla_completa")) s->fullscreen = atoi(v) != 0;
		else if (!strcmp(k, "escala")) {
			s->scale = !strcmp(v, "entero") ? SCALE_INTEGER : !strcmp(v, "estirar") ? SCALE_STRETCH : SCALE_FIT;
		}
		else if (!strcmp(k, "filtro")) s->smooth = !strcmp(v, "suave");
		else if (!strcmp(k, "shader")) s->shader = !strcmp(v, "normal") || !*v ? SHADER_NORMAL : SHADER_ZHD;  /* zadark-hd (xbr/ps2 antiguos -> Zadark HD) */
		else if (!strcmp(k, "zhd_textura")) s->zhd.textura = clampi(atoi(v), 1, 4);
		else if (!strcmp(k, "zhd_bordes")) s->zhd.bordes = clampi(atoi(v), 1, 4);
		else if (!strcmp(k, "zhd_vivo")) s->zhd.vivo = clampi(atoi(v), 1, 4);
		else if (!strcmp(k, "zhd_nitidez")) s->zhd.nitidez = clampi(atoi(v), 0, 100);
		else if (!strcmp(k, "zhd_brillo")) s->zhd.brillo = clampi(atoi(v), 0, 100);
		else if (!strcmp(k, "borde")) s->border = !strcmp(v, "difuminado") ? BORDER_BLUR : !strcmp(v, "negro") ? BORDER_BLACK : BORDER_THEME;
		else if (!strcmp(k, "volumen")) s->volume = clampi(atoi(v), 0, 100);
		else if (!strcmp(k, "intercambiar_ab")) s->swapAB = atoi(v) != 0;
		else if (!strcmp(k, "turbo")) s->turbo = clampi(atoi(v), 2, 10);
		else if ((!strcmp(k, "thegamesdb_clave") || !strcmp(k, "thegamesdb_key")) && *v) snprintf(s->tgdbKey, sizeof(s->tgdbKey), "%s", v);  /* la del usuario reemplaza a la incluida */
		else if (!strcmp(k, "portadas")) s->boxArt = strcmp(v, "pantalla") != 0;
		else if (!strcmp(k, "ocultar")) {
			static const char* const K[] = { "snes", "genesis", "gba", "gbc", "gb", "nes" };
			s->sysHidden = 0;
			char buf[128];
			snprintf(buf, sizeof(buf), "%s", v);
			for (char* t = strtok(buf, ", "); t; t = strtok(NULL, ", "))
				for (int i = 0; i < 6; ++i) if (!strcasecmp(t, K[i])) s->sysHidden |= 1u << i;
		}
		else if (!strcmp(k, "tema")) s->lightTheme = !strcmp(v, "claro");
		else if (!strcmp(k, "color")) {
			static const char* const K[] = { "cian", "rojo", "verde", "violeta", "naranja" };
			for (int i = 0; i < 5; ++i) if (!strcmp(v, K[i])) s->accent = i;
		}
		else if (!strcmp(k, "idioma")) s->lang = !strcmp(v, "en") ? LANG_EN : !strcmp(v, "pt") ? LANG_PT : LANG_ES;
		else if (!strcmp(k, "por_fila")) s->gridCols = clampi(atoi(v), 5, 9);
		else if (!strcmp(k, "aumento")) s->gridZoom = clampi(atoi(v), 100, 170);
		else if (!strcmp(k, "continuar")) s->resume = strcmp(v, "no") != 0 && strcmp(v, "0") != 0;
		else if (!strcmp(k, "actualizar")) s->autoUpdate = strcmp(v, "no") != 0 && strcmp(v, "0") != 0;
		else if (!strcmp(k, "tamano_ventana")) s->windowScale = clampi(atoi(v), 1, 10);
	}
	fclose(f);
}

/* ================================================================== */
/* Utilidades                                                          */
/* ================================================================== */
static char g_base[1024];

static void pathJoin(char* out, size_t n, const char* file) {
	snprintf(out, n, "%s%s", g_base, file);
}

static void resolvePath(char* out, size_t n, const char* p) {
	if (p[0] == '/' || p[0] == '\\' || strchr(p, ':')) snprintf(out, n, "%s", p);
	else pathJoin(out, n, p);
}


static bool fileExists(const char* p) {
	FILE* f = fopen(p, "rb");
	if (f) { fclose(f); return true; }
	return false;
}

/* registro.txt junto al juego: cada línea se guarda al instante (sirve aunque se congele) */
static FILE* g_logFile;
static void logToFile(void* ud, int cat, SDL_LogPriority pri, const char* msg) {
	(void) ud; (void) cat; (void) pri;
	if (!g_logFile) return;
	fprintf(g_logFile, "[%8.3f] %s\n", SDL_GetTicks() / 1000.0, msg);
	fflush(g_logFile);
}
static void openLog(void) {
	char p[1100];
	snprintf(p, sizeof(p), "%sregistro.txt", g_base);
	g_logFile = fopen(p, "w");
	SDL_LogSetOutputFunction(logToFile, NULL);
	SDL_LogSetAllPriority(SDL_LOG_PRIORITY_INFO);
}


static void quietLog(struct mLogger* l, int cat, enum mLogLevel lvl, const char* fmt, va_list args) {
	(void) l; (void) cat;
	if (lvl & (mLOG_FATAL | mLOG_ERROR)) {
		char m[512];
		vsnprintf(m, sizeof(m), fmt, args);
		SDL_Log("mGBA: %s", m);
	}
}
static struct mLogger g_logger = { .log = quietLog, .filter = NULL };

static void fatal(const char* msg) {
	fprintf(stderr, "%s\n", msg);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_TITLE, msg, NULL);
#ifdef __SWITCH__
	SDL_Quit();
	consoleInit(NULL);
	printf(T("\n  %s\n\n  Pulsa + para salir.\n"), msg);
	PadState pad;
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	while (appletMainLoop()) {
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
#endif
	exit(1);
}

/* ================================================================== */
/* Estado de la aplicación                                             */
/* ================================================================== */
struct App {
	struct Settings cfg;
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* gameTex;
	int gameTexW, gameTexH;
	Uint32 gameTexFormat;
	SDL_Texture* blurTex;   /* versión diminuta del juego para el borde difuminado */
	struct Ui ui;           /* lienzo al estilo GBA (menús, avisos, pausa) */
	SDL_Texture* uiTex;
	int uiTexW;
	SDL_Texture* uiBig;     /* el lienzo a escala entera, para ampliarlo nítido */
	int uiBigW, uiBigH;
	bool canTarget;
	Uint32 fadeFromWhite;   /* el juego aparece desde blanco tras la animación */
	int vw, vh;             /* tamaño de la pantalla del juego (GBA 240x160, GB 160x144) */
	char saveSlot[160];     /* nombre de la partida del juego elegido */
	char gameTitle[160];
	SDL_Texture* xbrTex;    /* juego escalado x4 con xBR-lv3 */
	int xbrW, xbrH;         /* tamaño real de la textura xBR (GB/GBC no usan 240x160) */
	uint32_t* xbrBuf;
	uint32_t* toneBuf;      /* cuadro con el color de Zadark HD */
	uint32_t* casBuf;       /* x4 tras la nitidez CAS */
	SDL_Texture* maskTex;   /* filtro diagonal, a resolución de pantalla */
	int maskW, maskH, maskStrength;
	SDL_Texture* bloomTex;  /* resplandor a 1/4 (lo amplía la GPU) */
	int bloomW, bloomH;
	uint32_t bloomBuf[(VIDEO_MAX_W / 4) * (VIDEO_MAX_H / 4)];
	bool bloomOn;
	bool casAutoOff;        /* nitidez apagada sola por ir lento */
	Uint64 fxTime; int fxFrames;
	SDL_AudioDeviceID audio;
	SDL_GameController* pad;

	bool musicRunning;      /* la música del menú ya está sonando */

	/* Lo que eligió la biblioteca para jugar */
	char launchPath[1024];
	char launchId[128];
	char message[256];      /* aviso en el menú */
	Uint32 messageUntil;
	bool quit;
};

struct RetroApi;
struct Game {
	struct App* app;
	struct mCore* core;
	color_t* frame;
	bool retro;
	const struct RetroApi* retroApi;
	uint32_t* retroFrame;
	int retroFrameW, retroFrameH;
	enum retro_pixel_format retroFormat;
	double retroFps;
	double retroRate;       /* frecuencia de audio del núcleo */
	double retroPos;
	int16_t retroLast[2];
	bool retroDiscardAudio;
	char sramPath[1024];    /* partida normal (SRAM) de los núcleos libretro */
	Uint32 sramChecked;
	uint32_t sramSum;
	char romPath[1024];
	char statePath[1024];   /* ranura 1; las demás agregan su número; "auto" al salir */
	SDL_Surface* thumb;     /* imagen del juego al pausar (para la ranura) */
	char title[128];
	bool running;
	bool toMenu;
	bool turbo;
	uint32_t keys;
	uint32_t padKeys;
	Uint32 titleReset;
	char osd[160];         /* mensaje sobre el juego (necesario en Switch: no hay título de ventana) */
	Uint32 osdUntil;
	bool pauseRequested;
};

/* API de un núcleo libretro enlazado estáticamente. Los símbolos llevan
 * prefijo (sws_nes_, sws_snes_, sws_genesis_) y cada núcleo se aísla en un
 * único objeto, así pueden convivir dentro del mismo ejecutable. */
struct RetroApi {
	const char* name;
	void (*setEnvironment)(retro_environment_t);
	void (*setVideo)(retro_video_refresh_t);
	void (*setAudio)(retro_audio_sample_t);
	void (*setAudioBatch)(retro_audio_sample_batch_t);
	void (*setInputPoll)(retro_input_poll_t);
	void (*setInputState)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	bool (*loadGame)(const struct retro_game_info*);
	void (*unloadGame)(void);
	void (*run)(void);
	void (*reset)(void);
	void (*getAvInfo)(struct retro_system_av_info*);
	void (*getSystemInfo)(struct retro_system_info*);
	void (*setController)(unsigned, unsigned);
	size_t (*serializeSize)(void);
	bool (*serialize)(void*, size_t);
	bool (*unserialize)(const void*, size_t);
	void* (*getMemoryData)(unsigned);
	size_t (*getMemorySize)(unsigned);
};

#define SWS_RETRO_DECL(P) \
	extern void P##retro_set_environment(retro_environment_t); \
	extern void P##retro_set_video_refresh(retro_video_refresh_t); \
	extern void P##retro_set_audio_sample(retro_audio_sample_t); \
	extern void P##retro_set_audio_sample_batch(retro_audio_sample_batch_t); \
	extern void P##retro_set_input_poll(retro_input_poll_t); \
	extern void P##retro_set_input_state(retro_input_state_t); \
	extern void P##retro_init(void); \
	extern void P##retro_deinit(void); \
	extern bool P##retro_load_game(const struct retro_game_info*); \
	extern void P##retro_unload_game(void); \
	extern void P##retro_run(void); \
	extern void P##retro_reset(void); \
	extern void P##retro_get_system_av_info(struct retro_system_av_info*); \
	extern void P##retro_get_system_info(struct retro_system_info*); \
	extern void P##retro_set_controller_port_device(unsigned, unsigned); \
	extern size_t P##retro_serialize_size(void); \
	extern bool P##retro_serialize(void*, size_t); \
	extern bool P##retro_unserialize(const void*, size_t); \
	extern void* P##retro_get_memory_data(unsigned); \
	extern size_t P##retro_get_memory_size(unsigned);

#define SWS_RETRO_API(P, NAME) { NAME, \
	P##retro_set_environment, P##retro_set_video_refresh, P##retro_set_audio_sample, P##retro_set_audio_sample_batch, \
	P##retro_set_input_poll, P##retro_set_input_state, P##retro_init, P##retro_deinit, P##retro_load_game, \
	P##retro_unload_game, P##retro_run, P##retro_reset, P##retro_get_system_av_info, P##retro_get_system_info, \
	P##retro_set_controller_port_device, P##retro_serialize_size, P##retro_serialize, P##retro_unserialize, \
	P##retro_get_memory_data, P##retro_get_memory_size }

SWS_RETRO_DECL(sws_nes_)
SWS_RETRO_DECL(sws_snes_)
SWS_RETRO_DECL(sws_genesis_)
static const struct RetroApi RETRO_NES = SWS_RETRO_API(sws_nes_, "NES");          /* FCEUmm (GPLv2+) */
static const struct RetroApi RETRO_SNES = SWS_RETRO_API(sws_snes_, "SNES");       /* Supafaust (GPLv2+) */
static const struct RetroApi RETRO_GEN = SWS_RETRO_API(sws_genesis_, "Genesis");  /* clownmdemu (AGPLv3) */

/* Opciones fijas de los núcleos (lo que en RetroArch serían sus "core options") */
/* Genesis europeo: se emula una consola PAL (50 Hz) para que vaya a su velocidad */
static bool g_retroPal;

/* ¿La ROM de Genesis es solo para Europa? Mira la región de la cabecera
 * (0x1F0: letras J/U/E o un dígito hexadecimal) y, si no se puede leer,
 * el nombre del archivo. */
static bool genesisIsPal(const char* path) {
	bool pal = false;
	FILE* f = fopen(path, "rb");
	if (f) {
		unsigned char h[0x200];
		size_t n = fread(h, 1, sizeof(h), f);
		fclose(f);
		if (n == sizeof(h) && !memcmp(h + 0x100, "SEGA", 4)) {
			const unsigned char* r = h + 0x1F0;
			bool J = false, U = false, E = false;
			for (int i = 0; i < 3; ++i) { J |= r[i] == 'J'; U |= r[i] == 'U'; E |= r[i] == 'E'; }
			if (J || U) return false;
			if (E) return true;  /* solo "E": Europa */
			/* formato nuevo: un dígito hexadecimal con bits 1 Japón, 4 EE. UU., 8 Europa */
			int v = r[0] >= '0' && r[0] <= '9' ? r[0] - '0' : r[0] >= 'A' && r[0] <= 'F' ? r[0] - 'A' + 10 : -1;
			if (v >= 0) return (v & 8) && !(v & 5);
		}
	}
	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strstr(base, "(Europe") && !strstr(base, "USA") && !strstr(base, "World") && !strstr(base, "Japan")) pal = true;
	return pal;
}

static const char* retroOption(const char* key) {
	if (!strcmp(key, "clownmdemu_tv_standard") && g_retroPal) return "pal";
	static const struct { const char* k; const char* v; } OPT[] = {
		{ "supafaust_h_filter", "phr256blend" },      /* alta resolución mezclada a 256 de ancho */
		{ "supafaust_audio_rate", "48000" },
		{ "supafaust_renderer", "mt" },               /* PPU en otro hilo: aprovecha los núcleos de la Switch */
		{ "supafaust_thread_affinity_emu", "0x0" },   /* sin fijar afinidad: lo decide el sistema */
		{ "supafaust_thread_affinity_ppu", "0x0" },
		{ "supafaust_correct_aspect", "disabled" },
		{ "supafaust_frame_begin_vblank", "enabled" },
		{ "supafaust_run_ahead", "disabled" },
		{ "fceumm_sndquality", "Low" },
		{ "clownmdemu_overseas_region", "elsewhere" },  /* consola occidental: juegos en inglés, sin bloqueo de región */
		{ "clownmdemu_tv_standard", "ntsc" },  /* "pal" para juegos solo europeos: ver g_retroPal */
		{ "clownmdemu_lowpass_filter", "enabled" },
		{ "clownmdemu_ladder_effect", "enabled" },
		{ "clownmdemu_cd_addon", "disabled" },
	};
	for (size_t i = 0; i < sizeof(OPT) / sizeof(OPT[0]); ++i)
		if (!strcmp(OPT[i].k, key)) return OPT[i].v;
	return NULL;
}

static void retroLog(enum retro_log_level level, const char* fmt, ...) {
	if (level < RETRO_LOG_WARN) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
	SDL_Log("[n\xC3\xBA" "cleo] %s", buf);
}

static struct Game* g_retroGame;

static bool retroEnvironment(unsigned cmd, void* data) {
	struct Game* g = g_retroGame;
	if (!g) return false;
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		g->retroFormat = *(const enum retro_pixel_format*) data;
		return g->retroFormat == RETRO_PIXEL_FORMAT_XRGB8888 || g->retroFormat == RETRO_PIXEL_FORMAT_RGB565 || g->retroFormat == RETRO_PIXEL_FORMAT_0RGB1555;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
#ifdef __SWITCH__
		*(const char**) data = BASE_DIR;
#else
		*(const char**) data = ".";
#endif
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
		const struct retro_system_av_info* av = data;
		if (av->timing.fps > 1) g->retroFps = av->timing.fps;
		if (av->timing.sample_rate > 1000) g->retroRate = av->timing.sample_rate;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		return true;  /* el tamaño real llega con cada cuadro (retroVideo) */
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		v->value = v->key ? retroOption(v->key) : NULL;
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = retroLog;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
		return true;
	default:
		return false;
	}
}

static inline uint32_t retroPixel(const struct Game* g, const uint8_t* row, unsigned x) {
	uint32_t r, gr, b;
	if (g->retroFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
		uint32_t p = ((const uint32_t*) row)[x]; r = (p >> 16) & 255; gr = (p >> 8) & 255; b = p & 255;
	} else if (g->retroFormat == RETRO_PIXEL_FORMAT_RGB565) {
		uint16_t p = ((const uint16_t*) row)[x]; r = (p >> 11) * 255 / 31; gr = ((p >> 5) & 63) * 255 / 63; b = (p & 31) * 255 / 31;
	} else {
		uint16_t p = ((const uint16_t*) row)[x]; r = ((p >> 10) & 31) * 255 / 31; gr = ((p >> 5) & 31) * 255 / 31; b = (p & 31) * 255 / 31;
	}
	return 0xFF000000u | r | (gr << 8) | (b << 16);
}

/* Cuadro del núcleo -> búfer ABGR. Los modos de alta resolución (512 de ancho o
 * 448 líneas entrelazadas) se reducen a la mitad para que el filtro Zadark HD
 * siga trabajando sobre el tamaño nativo de la consola. */
static void retroVideo(const void* data, unsigned w, unsigned h, size_t pitch) {
	struct Game* g = g_retroGame;
	if (!g || !data || !w || !h) return;  /* data NULL = repetir el cuadro anterior */
	unsigned sx = w > VIDEO_MAX_W ? 2 : 1, sy = h > VIDEO_MAX_H ? 2 : 1;
	unsigned ow = w / sx, oh = h / sy;
	if (ow > VIDEO_MAX_W || oh > VIDEO_MAX_H) return;
	if (!g->retroFrame) g->retroFrame = malloc(VIDEO_MAX_W * VIDEO_MAX_H * sizeof(uint32_t));
	if (!g->retroFrame) return;
	for (unsigned y = 0; y < oh; ++y) {
		uint32_t* out = g->retroFrame + y * ow;
		const uint8_t* row = (const uint8_t*) data + (size_t) (y * sy) * pitch;
		if (sx == 1) {
			for (unsigned x = 0; x < ow; ++x) out[x] = retroPixel(g, row, x);
		} else {
			for (unsigned x = 0; x < ow; ++x) {
				uint32_t p0 = retroPixel(g, row, x * 2), p1 = retroPixel(g, row, x * 2 + 1);
				out[x] = 0xFF000000u | (((p0 & 0xFEFEFEu) >> 1) + ((p1 & 0xFEFEFEu) >> 1));
			}
		}
	}
	g->retroFrameW = (int) ow; g->retroFrameH = (int) oh;
	g->app->vw = (int) ow; g->app->vh = (int) oh;
}

/* Audio del núcleo -> dispositivo (48 kHz). Si el núcleo usa otra frecuencia
 * (Genesis, por ejemplo) se remuestrea con interpolación lineal. */
static size_t retroAudioBatch(const int16_t* data, size_t frames) {
	struct Game* g = g_retroGame;
	if (!g || g->retroDiscardAudio || !g->app->audio) return frames;
	static int16_t buf[4096 * 2];
	const int vol = g->app->cfg.volume;
	const double step = g->retroRate > 1000 ? g->retroRate / (double) SAMPLE_RATE : 1.0;
	size_t accepted = frames, n = 0;
	if (step > 0.999 && step < 1.001) {
		for (size_t i = 0; i < frames; ++i) {
			buf[n * 2] = (int16_t) (data[i * 2] * vol / 100);
			buf[n * 2 + 1] = (int16_t) (data[i * 2 + 1] * vol / 100);
			if (++n == 4096) { SDL_QueueAudio(g->app->audio, buf, (Uint32) (n * 4)); n = 0; }
		}
	} else {
		/* g->retroPos: posición fraccionaria dentro del bloque; g->retroLast: última muestra */
		while (g->retroPos < (double) frames) {
			size_t i = (size_t) g->retroPos;
			double f = g->retroPos - (double) i;
			int l0 = i ? data[(i - 1) * 2] : g->retroLast[0], r0 = i ? data[(i - 1) * 2 + 1] : g->retroLast[1];
			int l1 = data[i * 2], r1 = data[i * 2 + 1];
			buf[n * 2] = (int16_t) ((l0 + (l1 - l0) * f) * vol / 100);
			buf[n * 2 + 1] = (int16_t) ((r0 + (r1 - r0) * f) * vol / 100);
			if (++n == 4096) { SDL_QueueAudio(g->app->audio, buf, (Uint32) (n * 4)); n = 0; }
			g->retroPos += step;
		}
		g->retroPos -= (double) frames;
		if (frames) { g->retroLast[0] = data[(frames - 1) * 2]; g->retroLast[1] = data[(frames - 1) * 2 + 1]; }
	}
	if (n) SDL_QueueAudio(g->app->audio, buf, (Uint32) (n * 4));
	return accepted;
}

static void retroAudio(int16_t left, int16_t right) { int16_t s[2] = { left, right }; retroAudioBatch(s, 1); }
static void retroInputPoll(void) { }
static int16_t retroInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) index;
	if (!g_retroGame || port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
	uint32_t k = g_retroGame->keys | g_retroGame->padKeys;
	#define KB(x) ((int16_t) ((k >> (x)) & 1))
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_UP: return KB(K_UP);
	case RETRO_DEVICE_ID_JOYPAD_DOWN: return KB(K_DOWN);
	case RETRO_DEVICE_ID_JOYPAD_LEFT: return KB(K_LEFT);
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: return KB(K_RIGHT);
	case RETRO_DEVICE_ID_JOYPAD_START: return KB(K_START);
	case RETRO_DEVICE_ID_JOYPAD_SELECT: return KB(K_SELECT);  /* en Genesis: Mode */
	}
	if (g_retroGame->retroApi == &RETRO_GEN) {
		/* clownmdemu: Y = A, B = B, A = C, L = X, X = Y, R = Z */
		switch (id) {
		case RETRO_DEVICE_ID_JOYPAD_Y: return KB(K_A);
		case RETRO_DEVICE_ID_JOYPAD_B: return KB(K_B);
		case RETRO_DEVICE_ID_JOYPAD_A: return KB(K_C);
		case RETRO_DEVICE_ID_JOYPAD_L: return KB(K_X);
		case RETRO_DEVICE_ID_JOYPAD_X: return KB(K_Y);
		case RETRO_DEVICE_ID_JOYPAD_R: return KB(K_Z);
		}
		return 0;
	}
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_A: return KB(K_A);
	case RETRO_DEVICE_ID_JOYPAD_B: return KB(K_B);
	case RETRO_DEVICE_ID_JOYPAD_X: return KB(K_X);
	case RETRO_DEVICE_ID_JOYPAD_Y: return KB(K_Y);
	case RETRO_DEVICE_ID_JOYPAD_L: return KB(K_L);
	case RETRO_DEVICE_ID_JOYPAD_R: return KB(K_R);
	}
	#undef KB
	return 0;
}

static void openPad(struct App* a) {
	if (a->pad) return;
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i) && (a->pad = SDL_GameControllerOpen(i))) {
			SDL_Log("Mando: %s", SDL_GameControllerName(a->pad));
			return;
		}
	}
}

static void handleDeviceEvent(struct App* a, const SDL_Event* e) {
	if (e->type == SDL_CONTROLLERDEVICEADDED) openPad(a);
	if (e->type == SDL_CONTROLLERDEVICEREMOVED && a->pad && !SDL_GameControllerGetAttached(a->pad)) {
		SDL_GameControllerClose(a->pad);
		a->pad = NULL;
		openPad(a);
	}
}

static void toggleFullscreen(struct App* a) {
#ifndef __SWITCH__
	a->cfg.fullscreen = !a->cfg.fullscreen;
	SDL_SetWindowFullscreen(a->window, a->cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	SDL_ShowCursor(a->cfg.fullscreen ? SDL_DISABLE : SDL_ENABLE);
#else
	(void) a;
#endif
}

/* Botón "aceptar" / "atrás" del mando, por posición (derecho / abajo) */
static Uint8 padEast(void) {
#ifdef __SWITCH__
	return SDL_CONTROLLER_BUTTON_A;
#else
	return SDL_CONTROLLER_BUTTON_B;
#endif
}
static Uint8 padSouth(void) {
#ifdef __SWITCH__
	return SDL_CONTROLLER_BUTTON_B;
#else
	return SDL_CONTROLLER_BUTTON_A;
#endif
}

/* La biblioteca conserva en Switch el mismo botón de confirmar que el perfil
 * predeterminado de las consolas (B de SDL = A emulado). Así no cambia al
 * insertar un cartucho. Esto solo afecta a la interfaz, no a controles que
 * el usuario haya configurado para un juego. */
static Uint8 menuAcceptButton(void) {
#ifdef __SWITCH__
	return SDL_CONTROLLER_BUTTON_B;
#else
	return padEast();
#endif
}
static Uint8 menuBackButton(void) {
#ifdef __SWITCH__
	return SDL_CONTROLLER_BUTTON_A;
#else
	return padSouth();
#endif
}


static Uint32 g_msgLockUntil;  /* un aviso importante no se pisa con otros hasta esta hora */
static void setMessage(struct App* a, const char* msg) {
	if (g_msgLockUntil && !SDL_TICKS_PASSED(SDL_GetTicks(), g_msgLockUntil)) return;
	snprintf(a->message, sizeof(a->message), "%s", msg);
	a->messageUntil = SDL_GetTicks() + 4000;
}

/* ================================================================== */
/* Lienzo GBA: se dibuja a 160 px de alto y se amplía nítido           */
/* ================================================================== */
static int uiBegin(struct App* a) {
	int ow, oh;
	SDL_GetRendererOutputSize(a->renderer, &ow, &oh);
	int w = oh > 0 ? (int) (((long long) UI_H * ow + oh / 2) / oh) : 240;
	uiSetWidth(&a->ui, w);
	SDL_RenderSetLogicalSize(a->renderer, 0, 0);
	return a->ui.w;
}

static void uiShow(struct App* a, bool overlay) {
	struct Ui* u = &a->ui;
	if (!u->px) return;
	int ow, oh;
	SDL_GetRendererOutputSize(a->renderer, &ow, &oh);
	if (!a->uiTex || a->uiTexW != u->w) {
		if (a->uiTex) SDL_DestroyTexture(a->uiTex);
		a->uiTex = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, u->w, UI_H);
		a->uiTexW = a->uiTex ? u->w : 0;
		if (!a->uiTex) return;
		SDL_SetTextureScaleMode(a->uiTex, SDL_ScaleModeNearest);
	}
	SDL_UpdateTexture(a->uiTex, NULL, u->px, u->w * (int) sizeof(uint32_t));
	double sx = (double) ow / u->w, sy = (double) oh / UI_H, sc = sx < sy ? sx : sy;
	SDL_Rect dst = { 0, 0, (int) (u->w * sc + 0.5), (int) (UI_H * sc + 0.5) };
	dst.x = (ow - dst.w) / 2;
	dst.y = (oh - dst.h) / 2;
	/* Primero x N entero (píxeles iguales) y luego ajuste suave al tamaño real */
	int n = (int) ceil(sc);
	while (n > 1 && u->w * n > 4096) --n;
	SDL_Texture* src = a->uiTex;
	if (n > 1 && a->canTarget) {
		if (!a->uiBig || a->uiBigW != u->w * n || a->uiBigH != UI_H * n) {
			if (a->uiBig) SDL_DestroyTexture(a->uiBig);
			a->uiBig = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, u->w * n, UI_H * n);
			a->uiBigW = u->w * n;
			a->uiBigH = UI_H * n;
			if (a->uiBig) SDL_SetTextureScaleMode(a->uiBig, SDL_ScaleModeLinear);
		}
		if (a->uiBig) {
			SDL_SetRenderTarget(a->renderer, a->uiBig);
			SDL_SetTextureBlendMode(a->uiTex, SDL_BLENDMODE_NONE);
			SDL_RenderCopy(a->renderer, a->uiTex, NULL, NULL);
			SDL_SetRenderTarget(a->renderer, NULL);
			src = a->uiBig;
		}
	}
	SDL_SetTextureBlendMode(src, overlay ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	SDL_RenderCopy(a->renderer, src, NULL, &dst);
}



/* ================================================================== */
/* Menú: idioma -> versión                                             */
/* Devuelve true si se eligió una ROM (a->lang / a->ver), false = salir*/
/* ================================================================== */
static bool g_manualRescan;
static bool g_appletMode;  /* Switch: abierto desde el Álbum (rendimiento limitado) */
enum MenuInput { IN_NONE, IN_UP, IN_DOWN, IN_OK, IN_BACK, IN_QUIT, IN_UPDATE, IN_RESCAN, IN_START, IN_LEFT, IN_RIGHT, IN_PREV, IN_NEXT, IN_CONTROLS, IN_COVER,
                 IN_SEARCH, IN_FAV, IN_MENU, IN_LETTER_PREV, IN_LETTER_NEXT, IN_ERASE, IN_OPTIONS };

/* Mientras hay una caja de texto activa (búsqueda), el texto del teclado físico va aquí */
static char* g_textSink;
static size_t g_textSinkCap;
static bool g_textSinkChanged;

#ifdef SWS_TEST
/* Solo para pruebas automáticas: SWS_TEST_INPUT="D,O,..." (U/D/O/B) y capturas BMP */
static void testCapture(struct App* a, const char* name) {
	const char* dir = getenv("SWS_TEST_DIR");
	if (!dir) return;
	int w, h;
	SDL_GetRendererOutputSize(a->renderer, &w, &h);
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_RenderReadPixels(a->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s.bmp", dir, name);
	SDL_SaveBMP(s, p);
	SDL_FreeSurface(s);
}
static enum MenuInput testInput(void) {
	static int frame = 0, pos = 0;
	const char* seq = getenv("SWS_TEST_INPUT");
	if (!seq || ++frame % 20) return IN_NONE;
	char c = seq[pos];
	if (!c) return IN_QUIT;
	pos += seq[pos + 1] == ',' ? 2 : 1;
	return c == 'U' ? IN_UP : c == 'D' ? IN_DOWN : c == 'O' ? IN_OK : c == 'B' ? IN_BACK : c == 'L' ? IN_LEFT : c == 'R' ? IN_RIGHT :
	       c == 'C' ? IN_CONTROLS : c == 'E' ? IN_NEXT : c == 'Q' ? IN_PREV : c == 'F' ? IN_FAV : c == 'M' ? IN_MENU :
	       c == 'K' ? IN_SEARCH : c == 'T' ? IN_START : c == 'N' ? IN_LETTER_NEXT : c == 'P' ? IN_LETTER_PREV : IN_NONE;
}
#endif

static enum MenuInput menuPollRaw(struct App* a) {
	static int stickDir = 0;
#ifdef SWS_TEST
	enum MenuInput ti = testInput();
	if (ti != IN_NONE) return ti;
#endif
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: return IN_QUIT;
		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED: handleDeviceEvent(a, &e); break;
		case SDL_TEXTINPUT:
			if (g_textSink) {
				size_t l = strlen(g_textSink), add = strlen(e.text.text);
				if (l + add + 1 < g_textSinkCap) { memcpy(g_textSink + l, e.text.text, add + 1); g_textSinkChanged = true; }
			}
			break;
		case SDL_KEYDOWN:
			if (g_textSink) {
				/* con una caja de texto activa, las letras escriben y no mueven el menú */
				SDL_Scancode sc = e.key.keysym.scancode;
				if (sc == SDL_SCANCODE_BACKSPACE) return IN_ERASE;
				if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) return IN_START;
				if (sc == SDL_SCANCODE_ESCAPE) return IN_BACK;
				if (sc == SDL_SCANCODE_UP) return IN_UP;
				if (sc == SDL_SCANCODE_DOWN) return IN_DOWN;
				if (sc == SDL_SCANCODE_LEFT) return IN_LEFT;
				if (sc == SDL_SCANCODE_RIGHT) return IN_RIGHT;
				if (sc == SDL_SCANCODE_PAGEUP) return IN_PREV;
				if (sc == SDL_SCANCODE_PAGEDOWN) return IN_NEXT;
				break;
			}
			if (e.key.keysym.scancode == SDL_SCANCODE_F && (e.key.keysym.mod & KMOD_CTRL)) return IN_SEARCH;
			switch (e.key.keysym.scancode) {
			case SDL_SCANCODE_UP: case SDL_SCANCODE_W: return IN_UP;
			case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: return IN_DOWN;
			case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return IN_LEFT;
			case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return IN_RIGHT;
			case SDL_SCANCODE_Q: return IN_PREV;
			case SDL_SCANCODE_E: return IN_NEXT;
			case SDL_SCANCODE_PAGEUP: return IN_LETTER_PREV;
			case SDL_SCANCODE_PAGEDOWN: return IN_LETTER_NEXT;
			case SDL_SCANCODE_SLASH: case SDL_SCANCODE_KP_DIVIDE: return IN_SEARCH;
			case SDL_SCANCODE_F: return IN_FAV;
			case SDL_SCANCODE_M: return IN_MENU;
			case SDL_SCANCODE_O: return IN_START;  /* orden en la cuadrícula */
			case SDL_SCANCODE_F5: g_manualRescan = true; return IN_RESCAN;
			case SDL_SCANCODE_C: return IN_CONTROLS;
			case SDL_SCANCODE_P: return IN_COVER;  /* elegir portada a mano */
			case SDL_SCANCODE_RETURN:
				if (e.key.keysym.mod & KMOD_ALT) { toggleFullscreen(a); break; }
				return IN_OK;
			case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_X: case SDL_SCANCODE_SPACE: return IN_OK;
			case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_Z: return IN_BACK;
			case SDL_SCANCODE_F11: toggleFullscreen(a); break;
			case SDL_SCANCODE_F3: return IN_UPDATE;
			case SDL_SCANCODE_F2: return IN_OPTIONS;
			default: break;
			}
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) return IN_RESCAN;
			break;
		case SDL_CONTROLLERBUTTONDOWN: {
			Uint8 b = e.cbutton.button;
			Uint8 ok = a->cfg.swapAB ? menuBackButton() : menuAcceptButton();
			Uint8 back = a->cfg.swapAB ? menuAcceptButton() : menuBackButton();
			if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) return IN_UP;
			if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) return IN_DOWN;
			if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) return IN_LEFT;
			if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) return IN_RIGHT;
			/* Botón de arriba (X en mandos de Nintendo): favorito.
			 * Botón de la izquierda (Y en mandos de Nintendo): menú del juego. */
			if (b == SDL_CONTROLLER_BUTTON_Y) return IN_FAV;
			if (b == SDL_CONTROLLER_BUTTON_X) return IN_MENU;
			if (b == SDL_CONTROLLER_BUTTON_BACK) return IN_SEARCH;  /* − */
			if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) return IN_PREV;
			if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) return IN_NEXT;
			if (b == ok) return IN_OK;
			if (b == back) return IN_BACK;
			if (b == SDL_CONTROLLER_BUTTON_START) return IN_START;
			break;
		}
		case SDL_CONTROLLERAXISMOTION:
			if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
				/* ZL / ZR: salto por letra */
				static bool held[2];
				int w = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
				bool on = e.caxis.value > 16000;
				if (on != held[w]) { held[w] = on; if (on) return w ? IN_LETTER_NEXT : IN_LETTER_PREV; }
				break;
			}
			if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
				static int stickX = 0;
				int dir = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : 0;
				if (dir != stickX) {
					stickX = dir;
					if (dir < 0) return IN_LEFT;
					if (dir > 0) return IN_RIGHT;
				}
			}
			if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
				int dir = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : 0;
				if (dir != stickDir) {
					stickDir = dir;
					if (dir < 0) return IN_UP;
					if (dir > 0) return IN_DOWN;
				}
			}
			break;
		}
	}
	return IN_NONE;
}

/* ¿Sigue apretada esa dirección en el mando (cruceta o palanca)? */
static bool padDirHeld(struct App* a, enum MenuInput d) {
	SDL_GameController* p = a->pad;
	if (!p) return false;
	int x = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
	switch (d) {
	case IN_UP: return SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000;
	case IN_DOWN: return SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000;
	case IN_LEFT: return SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -20000;
	case IN_RIGHT: return SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 20000;
	default: return false;
	}
}

/* Entrada de los menús con repetición: mantener una dirección avanza sola
 * (primero una pausa corta y después cada vez más rápido). El teclado ya
 * repite por su cuenta. */
static enum MenuInput menuPollInput(struct App* a) {
	static enum MenuInput held = IN_NONE;
	static Uint32 next;
	static int count;
	enum MenuInput in = menuPollRaw(a);
	Uint32 now = SDL_GetTicks();
	if (in == IN_UP || in == IN_DOWN || in == IN_LEFT || in == IN_RIGHT) {
		held = in;
		next = now + 360;
		count = 0;
		return in;
	}
	if (in != IN_NONE) { held = IN_NONE; return in; }
	if (held != IN_NONE) {
		if (!padDirHeld(a, held)) { held = IN_NONE; return IN_NONE; }
		if ((Sint32) (now - next) >= 0) {
			++count;
			next = now + (count > 10 ? 50 : count > 4 ? 80 : 120);
			return held;
		}
	}
	return IN_NONE;
}

/* Música del menú: mantiene ~120 ms de audio en cola */
static void musicPump(struct App* a) {
	if (!a->audio || !mmIsOpen()) return;
	const Uint32 target = (Uint32) (SAMPLE_RATE * 2 * sizeof(int16_t) * 12 / 100);
	static int16_t buf[1024 * 2];
	int guard = 0;
	while (SDL_GetQueuedAudioSize(a->audio) < target && guard++ < 16) {
		int n = mmRead(buf, 1024);
		if (n <= 0) break;
		SDL_QueueAudio(a->audio, buf, (Uint32) (n * 2 * sizeof(int16_t)));
	}
}

/* Filtro elegido en la pausa (Zadark HD y sus niveles): se recuerda entre sesiones */
static void saveLastChoice(struct App* a) {
	char p[1024];
	pathJoin(p, sizeof(p), "menu.dat");
	FILE* f = fopen(p, "w");
	if (f) { fprintf(f, "0 0 %d %d %d %d\n", a->cfg.shader, a->cfg.zhd.textura, a->cfg.zhd.bordes, a->cfg.zhd.vivo); fclose(f); }
}

static void loadLastChoice(struct App* a) {
	char p[1024];
	pathJoin(p, sizeof(p), "menu.dat");
	FILE* f = fopen(p, "r");
	if (!f) return;
	int l = 0, v = 0, x = -1, tx = 0, bo = 0, vi = 0;
	int n = fscanf(f, "%d %d %d %d %d %d", &l, &v, &x, &tx, &bo, &vi);
	fclose(f);
	if (n >= 6) {
		a->cfg.zhd.textura = clampi(tx, 1, 4);
		a->cfg.zhd.bordes = clampi(bo, 1, 4);
		a->cfg.zhd.vivo = clampi(vi, 1, 4);
	}
	if (n >= 3 && x >= 0) a->cfg.shader = x > 0 ? SHADER_ZHD : SHADER_NORMAL;
}

/* Arranca (o retoma) la música del menú con fundido, sin cortes si ya sonaba */
static void startMenuMusic(struct App* a) {
	if (!a->audio || !mmIsOpen() || a->musicRunning) return;
	SDL_ClearQueuedAudio(a->audio);
	SDL_PauseAudioDevice(a->audio, 0);
	mmFade(1.0f, 1.5f);
	a->musicRunning = true;
}

/* ================================================================== */
/* Partidas                                                            */
/*                                                                     */
/* Se guardan en "Partidas/" como <nombre>_<CRC>.sav, una por juego.   */
/* Si la ROM de una partida cambia (otra versión del mismo hack), la   */
/* partida anterior se respalda en "Partidas/Respaldos/".              */
/* ================================================================== */

static bool copyFile(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if (!in) return false;
	FILE* out = fopen(to, "wb");
	if (!out) { fclose(in); return false; }
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
	}
	fclose(in);
	if (fclose(out) != 0) ok = false;
	return ok;
}

/* Una partida vacía de GBA es todo 0xFF */
static bool saveHasData(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	int c;
	bool data = false;
	while ((c = fgetc(f)) != EOF) if (c != 0xFF) { data = true; break; }
	fclose(f);
	return data;
}

#include "library.inc"

static bool prepareSave(struct App* a, const char* identity, char* savePath, size_t n, char* statePath, size_t sn) {
	const char* slot = a->saveSlot[0] ? a->saveSlot : "juego";
	char dir[1024], bak[1024], marker[1024];
	pathJoin(dir, sizeof(dir), "Partidas");
	MKDIR(dir);
	pathJoin(bak, sizeof(bak), "Partidas/Respaldos");
	MKDIR(bak);
	snprintf(savePath, n, "%s/%s.sav", dir, slot);
	snprintf(statePath, sn, "%s/%s.estado", dir, slot);
	snprintf(marker, sizeof(marker), "%s/%s.rom", dir, slot);

	/* ¿Cambió la ROM de esta opción desde la última vez? -> respaldo */
	char lastRom[512] = "";
	FILE* f = fopen(marker, "r");
	if (f) {
		if (fgets(lastRom, sizeof(lastRom), f)) lastRom[strcspn(lastRom, "\r\n")] = '\0';
		fclose(f);
	}
	const char* cur = identity;
	bool backedUp = false;
	if (lastRom[0] && strcmp(lastRom, cur) != 0 && saveHasData(savePath)) {
		char dst[1200], stamp[32];
		time_t now = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M", localtime(&now));
		char lastNoExt[512];
		snprintf(lastNoExt, sizeof(lastNoExt), "%s", lastRom);
		size_t ln = strlen(lastNoExt);
		if (ln > 4 && !strcmp(lastNoExt + ln - 4, ".gba")) lastNoExt[ln - 4] = 0;
		for (char* c = lastNoExt; *c; ++c) if (*c == ' ' || *c == '/' || *c == '\\' || *c == ':') *c = '_';
		snprintf(dst, sizeof(dst), "%s/%s_%s_%s.sav", bak, slot, lastNoExt, stamp);
		if (copyFile(savePath, dst)) {
			backedUp = true;
			setMessage(a, T("La ROM cambi\xC3\xB3: tu partida anterior se respald\xC3\xB3 en Partidas/Respaldos"));
			a->messageUntil = SDL_GetTicks() + 8000;
		}
	}
	f = fopen(marker, "w");
	if (f) { fprintf(f, "%s\n", cur); fclose(f); }
	return backedUp;
}

/* ================================================================== */
/* Juego                                                               */
/* ================================================================== */
static void notify(struct Game* g, const char* msg) {
	char t[256];
	snprintf(t, sizeof(t), "%s - %s", g->title, msg);
	SDL_SetWindowTitle(g->app->window, t);
	g->titleReset = SDL_GetTicks() + 2000;
	snprintf(g->osd, sizeof(g->osd), "%s", msg);
	g->osdUntil = SDL_GetTicks() + 2500;
	SDL_Log("%s", msg);
}

/* Ranuras de estado: 0 = "al salir" (automático), 1..4 = del jugador.
 * La 1 usa el nombre de siempre (<juego>.estado) para no perder estados viejos. */
#define STATE_SLOTS 4
static void statePathFor(const struct Game* g, int slot, char* out, size_t n) {
	if (slot == 0) {
		snprintf(out, n, "%s", g->statePath);
		char* dot = strrchr(out, '.');
		if (dot) snprintf(dot, n - (size_t) (dot - out), ".auto");
	} else if (slot == 1) snprintf(out, n, "%s", g->statePath);
	else snprintf(out, n, "%s%d", g->statePath, slot);
}

static void thumbPathFor(const struct Game* g, int slot, char* out, size_t n) {
	char p[1024];
	statePathFor(g, slot, p, sizeof(p));
	snprintf(out, n, "%s.bmp", p);
}

static bool saveStateTo(struct Game* g, const char* path) {
	if (g->retro) {
		size_t n = g->retroApi->serializeSize();
		void* data = n ? malloc(n) : NULL;
		bool ok = data && g->retroApi->serialize(data, n);
		FILE* f = ok ? fopen(path, "wb") : NULL;
		ok = f && fwrite(data, 1, n, f) == n;
		if (f) fclose(f);
		free(data);
		return ok;
	}
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
	if (!vf) return false;
	bool ok = mCoreSaveStateNamed(g->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vf->close(vf);
	return ok;
}

static bool loadStateFrom(struct Game* g, const char* path) {
	if (g->retro) {
		size_t n = g->retroApi->serializeSize();
		void* data = n ? malloc(n) : NULL;
		FILE* f = data ? fopen(path, "rb") : NULL;
		bool ok = f && fread(data, 1, n, f) == n && fgetc(f) == EOF && g->retroApi->unserialize(data, n);
		if (f) fclose(f);
		free(data);
		return ok;
	}
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) return false;
	bool ok = mCoreLoadStateNamed(g->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vf->close(vf);
	return ok;
}

static void drawGameLayer(struct Game* g);
static SDL_Rect gameRect(struct App* a);

/* Foto del juego (sin menús encima) para la ranura: 320x180 como máximo */
static void captureThumb(struct Game* g) {
	struct App* a = g->app;
	if (g->thumb) { SDL_FreeSurface(g->thumb); g->thumb = NULL; }
	SDL_RenderSetLogicalSize(a->renderer, 0, 0);
	drawGameLayer(g);
	SDL_Rect r = gameRect(a);
	if (r.w <= 0 || r.h <= 0) return;
	SDL_Surface* full = SDL_CreateRGBSurfaceWithFormat(0, r.w, r.h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!full) return;
	if (SDL_RenderReadPixels(a->renderer, &r, SDL_PIXELFORMAT_ARGB8888, full->pixels, full->pitch) != 0) { SDL_FreeSurface(full); return; }
	int tw = 320, th = r.h * 320 / r.w;
	if (th > 240) { th = 240; tw = r.w * 240 / r.h; }
	SDL_Surface* t = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
	if (t) { SDL_SetSurfaceBlendMode(full, SDL_BLENDMODE_NONE); SDL_BlitScaled(full, NULL, t, NULL); }
	SDL_FreeSurface(full);
	g->thumb = t;
}

static bool saveSlot(struct Game* g, int slot) {
	char p[1024], tp[1024];
	statePathFor(g, slot, p, sizeof(p));
	bool ok = saveStateTo(g, p);
	if (ok && g->thumb) { thumbPathFor(g, slot, tp, sizeof(tp)); SDL_SaveBMP(g->thumb, tp); }
	return ok;
}

/* compatibilidad: guardar / cargar la ranura 1 */
static void saveState(struct Game* g) { notify(g, saveSlot(g, 1) ? T("Estado guardado") : T("Error al guardar el estado")); }
static void loadState(struct Game* g) {
	char p[1024];
	statePathFor(g, 1, p, sizeof(p));
	notify(g, !fileExists(p) ? T("No hay estado guardado") : loadStateFrom(g, p) ? T("Estado cargado") : T("Estado inv\xC3\xA1lido"));
}

/* Al entrar: si hay un estado "al salir", se sigue desde ahí */
static void resumeAuto(struct Game* g) {
	char p[1024];
	statePathFor(g, 0, p, sizeof(p));
	if (!g->app->cfg.resume || !fileExists(p)) return;
	if (loadStateFrom(g, p)) {
		notify(g, T("Continuando donde lo dejaste (pausa: Reiniciar para empezar de cero)"));
		g->osdUntil = SDL_GetTicks() + 4500;
	} else notify(g, T("No se pudo continuar: el estado guardado al salir no es v\xC3\xA1lido"));
}

/* Al salir: guarda el estado "al salir" con su foto */
static void saveAuto(struct Game* g) {
	if (!g->app->cfg.resume) return;
	captureThumb(g);
	if (!saveSlot(g, 0)) SDL_Log("No se pudo guardar el estado al salir");
	if (g->thumb) { SDL_FreeSurface(g->thumb); g->thumb = NULL; }
}

static int keyFromScancode(SDL_Scancode sc) {
	switch (sc) {
	case SDL_SCANCODE_X:         return K_A;
	case SDL_SCANCODE_Z:         return K_B;
	case SDL_SCANCODE_RETURN:    return K_START;
	case SDL_SCANCODE_BACKSPACE: return K_SELECT;
	case SDL_SCANCODE_UP:        return K_UP;
	case SDL_SCANCODE_DOWN:      return K_DOWN;
	case SDL_SCANCODE_LEFT:      return K_LEFT;
	case SDL_SCANCODE_RIGHT:     return K_RIGHT;
	case SDL_SCANCODE_A:         return K_L;
	case SDL_SCANCODE_S:         return K_R;
	default:                     return -1;
	}
}

static void handleKey(struct Game* g, const SDL_KeyboardEvent* e) {
	bool down = e->type == SDL_KEYDOWN;
	if (down && !e->repeat && e->keysym.scancode == SDL_SCANCODE_RETURN && (e->keysym.mod & KMOD_ALT)) {
		toggleFullscreen(g->app);
		return;
	}
	int k = -1;
	if (g_ctlActive) {
		for (const int* b = SYS_BTNS[g_gameSys]; *b >= 0; ++b)
			if (g_ctl.key[*b] == (int) e->keysym.scancode) { k = ctlToGba(*b); break; }
	} else k = keyFromScancode(e->keysym.scancode);
	if (k >= 0) {
		if (down) g->keys |= 1u << k; else g->keys &= ~(1u << k);
		return;
	}
	switch (e->keysym.scancode) {
	case SDL_SCANCODE_TAB:
	case SDL_SCANCODE_SPACE: g->turbo = down; break;
	case SDL_SCANCODE_F2:  if (down && !e->repeat) g->toMenu = true; break;
	case SDL_SCANCODE_F5:  if (down && !e->repeat) saveState(g); break;
	case SDL_SCANCODE_F8:  if (down && !e->repeat) loadState(g); break;
	case SDL_SCANCODE_F11: if (down && !e->repeat) toggleFullscreen(g->app); break;
	case SDL_SCANCODE_ESCAPE: if (down && !e->repeat) g->pauseRequested = true; break;
	default: break;
	}
}

static void pollPad(struct Game* g) {
	g->padKeys = 0;
	SDL_GameController* p = g->app->pad;
	if (!p) return;
	#define BTN(b) SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_##b)
	bool east = SDL_GameControllerGetButton(p, (SDL_GameControllerButton) padEast());
	bool south = SDL_GameControllerGetButton(p, (SDL_GameControllerButton) padSouth());
	bool a = g->app->cfg.swapAB ? south : east;
	bool b = g->app->cfg.swapAB ? east : south;

	int lx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
	int ly = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
	const int dz = 12000;
	bool zl = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
	bool zr = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
	bool lsh = BTN(LEFTSHOULDER), rsh = BTN(RIGHTSHOULDER), start = BTN(START), back = BTN(BACK);

	static bool prevSave, prevLoad, prevQuit, prevMenu, prevPause;
	bool pauseCombo = back && start;
	if (pauseCombo && !prevPause) g->pauseRequested = true;
	prevPause = pauseCombo;
	if (pauseCombo) {
		/* − y + juntos: menú de pausa (no se envían al juego) */
	} else if (zl) {
		/* ZL = modificador: ZL+L guarda, ZL+R carga, ZL+− menú, ZL++ sale */
		if (lsh && !prevSave) saveState(g);
		if (rsh && !prevLoad) loadState(g);
		if (back && !prevMenu) g->toMenu = true;
		if (start && !prevQuit) g->running = false;
		prevSave = lsh; prevLoad = rsh; prevMenu = back; prevQuit = start;
	} else {
		prevSave = prevLoad = prevQuit = prevMenu = false;
		if (g_ctlActive) {
			for (const int* bb = SYS_BTNS[g_gameSys]; *bb >= 0; ++bb) {
				int pb = g_ctl.pad[*bb], k = ctlToGba(*bb);
				if (pb >= 0 && k >= 0 && SDL_GameControllerGetButton(p, (SDL_GameControllerButton) pb)) g->padKeys |= 1u << k;
			}
		} else {
			if (a) g->padKeys |= 1u << K_A;
			if (b) g->padKeys |= 1u << K_B;
			if (lsh) g->padKeys |= 1u << K_L;
			if (rsh) g->padKeys |= 1u << K_R;
			if (start) g->padKeys |= 1u << K_START;
			if (back) g->padKeys |= 1u << K_SELECT;
		}
		if (ly < -dz) g->padKeys |= 1u << K_UP;  /* el stick izquierdo siempre mueve la cruceta */
		if (ly > dz) g->padKeys |= 1u << K_DOWN;
		if (lx < -dz) g->padKeys |= 1u << K_LEFT;
		if (lx > dz) g->padKeys |= 1u << K_RIGHT;
		if (!g_ctlActive && BTN(DPAD_UP)) g->padKeys |= 1u << K_UP;
		if (!g_ctlActive && BTN(DPAD_DOWN)) g->padKeys |= 1u << K_DOWN;
		if (!g_ctlActive && BTN(DPAD_LEFT)) g->padKeys |= 1u << K_LEFT;
		if (!g_ctlActive && BTN(DPAD_RIGHT)) g->padKeys |= 1u << K_RIGHT;
	}
	static bool padTurbo = false;
	if (zr != padTurbo) { g->turbo = zr; padTurbo = zr; }
	#undef BTN
}

static SDL_Rect gameRect(struct App* a) {
	int ow, oh;
	SDL_GetRendererOutputSize(a->renderer, &ow, &oh);
	SDL_Rect dst = { 0, 0, ow, oh };
	if (a->cfg.scale != SCALE_STRETCH) {
		const int VW = a->vw ? a->vw : GBA_W, VH = a->vh ? a->vh : GBA_H;
		double sx = (double) ow / VW, sy = (double) oh / VH;
		double sc = sx < sy ? sx : sy;
		if (a->cfg.scale == SCALE_INTEGER && sc >= 1.0) sc = (int) sc;
		dst.w = (int) (VW * sc);
		dst.h = (int) (VH * sc);
		dst.x = (ow - dst.w) / 2;
		dst.y = (oh - dst.h) / 2;
	}
	return dst;
}

/* Dibuja el juego en su proporción real (3:2) y llena los costados según "borde" */
/* Filtro diagonal de PCSX2: se multiplica sobre la imagen, 1 píxel de máscara = 1 píxel de pantalla */
static void drawDiagonalMask(struct App* a, const SDL_Rect* dst, int strength) {
	if (dst->w <= 0 || dst->h <= 0) return;
	if (!a->maskTex || a->maskW != dst->w || a->maskH != dst->h || a->maskStrength != strength) {
		if (a->maskTex) SDL_DestroyTexture(a->maskTex);
		a->maskTex = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dst->w, dst->h);
		if (!a->maskTex) { a->maskW = a->maskH = 0; return; }
		uint32_t* px = malloc((size_t) dst->w * dst->h * sizeof(uint32_t));
		if (!px) { SDL_DestroyTexture(a->maskTex); a->maskTex = NULL; return; }
		postfxBuildMask(px, dst->w, dst->h, dst->w, strength);
		SDL_UpdateTexture(a->maskTex, NULL, px, dst->w * sizeof(uint32_t));
		free(px);
		SDL_SetTextureBlendMode(a->maskTex, SDL_BLENDMODE_MOD);
		SDL_SetTextureScaleMode(a->maskTex, SDL_ScaleModeNearest);
		a->maskW = dst->w; a->maskH = dst->h; a->maskStrength = strength;
	}
	SDL_RenderCopy(a->renderer, a->maskTex, NULL, dst);
}

static void drawGameLayer(struct Game* g) {
	struct App* a = g->app;
	SDL_Rect dst = gameRect(a);
	int ow, oh;
	SDL_GetRendererOutputSize(a->renderer, &ow, &oh);
	bool bars = dst.x > 0 || dst.y > 0;
	SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 255);
	SDL_RenderClear(a->renderer);

	if (bars && a->cfg.border == BORDER_THEME) {
		uiBegin(a);
		uiLandscape(&a->ui, SDL_GetTicks() / 1000.0f, 70);
		uiShow(a, false);
	} else if (bars && a->cfg.border == BORDER_BLUR) {
		if (!a->blurTex) {
			SDL_RendererInfo ri;
			SDL_GetRendererInfo(a->renderer, &ri);
			if (ri.flags & SDL_RENDERER_TARGETTEXTURE) {
				a->blurTex = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 24, 16);
				if (a->blurTex) SDL_SetTextureScaleMode(a->blurTex, SDL_ScaleModeLinear);
			}
		}
		if (a->blurTex) {
			/* Reducir a 24x16 y ampliar con filtrado = desenfoque barato (vale en Switch) */
			SDL_SetTextureScaleMode(a->gameTex, SDL_ScaleModeLinear);
			SDL_SetRenderTarget(a->renderer, a->blurTex);
			SDL_Rect vsrc = { 0, 0, a->vw ? a->vw : GBA_W, a->vh ? a->vh : GBA_H };
			SDL_RenderCopy(a->renderer, a->gameTex, &vsrc, NULL);
			SDL_SetRenderTarget(a->renderer, NULL);
			SDL_SetTextureScaleMode(a->gameTex, a->cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
			/* Cubrir toda la pantalla manteniendo 3:2 (se recorta arriba/abajo) */
			int cw = ow, chh = ow * GBA_H / GBA_W;
			if (chh < oh) { chh = oh; cw = oh * GBA_W / GBA_H; }
			SDL_Rect cover = { (ow - cw) / 2, (oh - chh) / 2, cw, chh };
			SDL_RenderCopy(a->renderer, a->blurTex, NULL, &cover);
			SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 140);
			SDL_RenderFillRect(a->renderer, NULL);
		}
	}

	if (bars && a->cfg.border != BORDER_BLACK) {
		/* Sombra suave alrededor de la pantalla del juego (sin marco de color) */
		int b = oh / 180 > 2 ? oh / 180 : 2;
		SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 70);
		SDL_Rect sh = { dst.x - b * 3, dst.y - b * 3, dst.w + b * 6, dst.h + b * 6 };
		SDL_RenderFillRect(a->renderer, &sh);
		SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 90);
		SDL_Rect sh2 = { dst.x - b, dst.y - b, dst.w + b * 2, dst.h + b * 2 };
		SDL_RenderFillRect(a->renderer, &sh2);
	}
	{
		bool hd = a->cfg.shader != SHADER_NORMAL && a->xbrTex;
		int VW = a->vw ? a->vw : GBA_W, VH = a->vh ? a->vh : GBA_H;
		SDL_Rect src = { 0, 0, hd ? VW * 4 : VW, hd ? VH * 4 : VH };
		SDL_RenderCopy(a->renderer, hd ? a->xbrTex : a->gameTex, &src, &dst);
	}
	if (a->cfg.shader == SHADER_ZHD && a->xbrTex) {
		/* Resplandor: mezcla "pantalla" (1-(1-a)(1-b)); si la GPU no la tiene, suma */
		if (a->bloomOn && a->bloomTex && a->cfg.zhd.brillo > 0) {
			Uint8 k = (Uint8) (a->cfg.zhd.brillo * 255 / 100);
			SDL_SetTextureColorMod(a->bloomTex, k, k, k);
			SDL_RenderCopy(a->renderer, a->bloomTex, NULL, &dst);
		}
		int strength = zhdTextura(a->cfg.zhd.textura);
		if (strength > 0) {
			drawDiagonalMask(a, &dst, strength);
			/* La máscara deja en promedio (1 + 2·lo)/3 de la luz: se suma el juego otra vez
			 * con lo que falta (x1.2 por lo que se pierde al saturar), color como sin textura */
			float lo = 1.0f - strength / 200.0f;
			Uint8 add = (Uint8) (255.0f * 1.2f * (1.0f - (1.0f + 2.0f * lo) / 3.0f) + 0.5f);
			SDL_SetTextureBlendMode(a->xbrTex, SDL_BLENDMODE_ADD);
			SDL_SetTextureColorMod(a->xbrTex, add, add, add);
			SDL_RenderCopy(a->renderer, a->xbrTex, NULL, &dst);
			SDL_SetTextureColorMod(a->xbrTex, 255, 255, 255);
			SDL_SetTextureBlendMode(a->xbrTex, SDL_BLENDMODE_NONE);
		}
	}
}

/* xBR-lv3: escala el cuadro x4 en CPU y lo sube a su textura (solo color de 32 bits) */
static void updateXbr(struct Game* g) {
#if !defined(COLOR_16_BIT)
	struct App* a = g->app;
	if (a->cfg.shader == SHADER_NORMAL) return;
	const int vw = a->vw ? a->vw : GBA_W, vh = a->vh ? a->vh : GBA_H;
	/* GB y GBC entregan 160x144 dentro de un búfer con paso de GBA. El
	 * filtro debe trabajar con su rectángulo real; usar 240x160 deja restos
	 * de memoria en el cuadro ampliado y se ve como una imagen duplicada. */
	if (!a->xbrTex || a->xbrW != vw * 4 || a->xbrH != vh * 4) {
		if (a->xbrTex) SDL_DestroyTexture(a->xbrTex);
		a->xbrTex = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, vw * 4, vh * 4);
		a->xbrW = a->xbrTex ? vw * 4 : 0;
		a->xbrH = a->xbrTex ? vh * 4 : 0;
		if (!a->xbrBuf) a->xbrBuf = malloc(VIDEO_MAX_W * 4 * VIDEO_MAX_H * 4 * sizeof(uint32_t));
		if (!a->xbrTex || !a->xbrBuf) { SDL_Log("xBR no disponible: %s", SDL_GetError()); a->cfg.shader = SHADER_NORMAL; return; }
		SDL_SetTextureScaleMode(a->xbrTex, SDL_ScaleModeLinear);
		xbrInit();
		SDL_Log("Filtro Zadark HD activado");
	}
	/* Zadark HD: color (tabla) -> xBR-lv3 x4 -> bordes (mezcla) -> nitidez CAS; resplandor y textura en la GPU */
	if (!a->toneBuf) a->toneBuf = malloc(VIDEO_MAX_W * VIDEO_MAX_H * sizeof(uint32_t));
	if (!a->casBuf) a->casBuf = malloc(VIDEO_MAX_W * 4 * VIDEO_MAX_H * 4 * sizeof(uint32_t));
	if (!a->toneBuf || !a->casBuf) { a->cfg.shader = SHADER_NORMAL; return; }
	if (!a->bloomTex || a->bloomW != vw / 4 || a->bloomH != vh / 4) {
		if (a->bloomTex) SDL_DestroyTexture(a->bloomTex);
		a->bloomTex = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, vw / 4, vh / 4);
		a->bloomW = a->bloomTex ? vw / 4 : 0;
		a->bloomH = a->bloomTex ? vh / 4 : 0;
		if (a->bloomTex) {
			SDL_SetTextureScaleMode(a->bloomTex, SDL_ScaleModeLinear);
			SDL_BlendMode screen = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
			                                                  SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
			if (SDL_SetTextureBlendMode(a->bloomTex, screen) != 0) SDL_SetTextureBlendMode(a->bloomTex, SDL_BLENDMODE_ADD);
			a->bloomOn = true;
		}
	}
	const struct ZadarkHD* z = &a->cfg.zhd;
	Uint64 t0 = SDL_GetPerformanceCounter();
	const uint32_t* source = g->retro ? g->retroFrame : (const uint32_t*) g->frame;
	const int sourceStride = g->retro ? vw : GBA_W;
	if (!source) return;
	for (int y = 0; y < vh; ++y)
		zhdColor(source + y * sourceStride, a->toneBuf + y * vw, vw, z->vivo);
	xbrScale4(a->toneBuf, vw, vh, vw, a->xbrBuf, vw * 4);
	postfxSoften(a->xbrBuf, a->toneBuf, vw, vh, zhdSuavizado(z->bordes));
	const uint32_t* out = a->xbrBuf;
	if (z->nitidez > 0 && !a->casAutoOff) {
		postfxSharpen(a->xbrBuf, a->casBuf, vw * 4, vh * 4, z->nitidez);
		out = a->casBuf;
	}
	if (a->bloomTex && z->brillo > 0) zhdBloom(a->toneBuf, vw, vh, a->bloomBuf);
	/* Cada 120 cuadros: anotar el coste y, si se come el cuadro, quitar la nitidez */
	a->fxTime += SDL_GetPerformanceCounter() - t0;
	SDL_UpdateTexture(a->xbrTex, NULL, out, vw * 4 * sizeof(uint32_t));
	if (a->bloomTex && z->brillo > 0) SDL_UpdateTexture(a->bloomTex, NULL, a->bloomBuf, (vw / 4) * sizeof(uint32_t));
	if (++a->fxFrames == 120) {
		double ms = a->fxTime * 1000.0 / SDL_GetPerformanceFrequency() / a->fxFrames;
		static int reports = 0;
		if (reports < 3) { ++reports; SDL_Log("Zadark HD: %.1f ms por cuadro%s", ms, a->casAutoOff ? " (sin nitidez)" : ""); }
		static int slow = 0;
		slow = ms > 10.0 ? slow + 1 : 0;
		if (slow >= 2 && !a->casAutoOff && z->nitidez > 0) {
			a->casAutoOff = true;
			SDL_Log("Zadark HD: nitidez desactivada para mantener la velocidad");
		}
		a->fxTime = 0; a->fxFrames = 0;
	}
#else
	(void) g;
#endif
}

static bool ensureGameTexture(struct App* a, int w, int h, Uint32 format) {
	if (a->gameTex && a->gameTexW == w && a->gameTexH == h && a->gameTexFormat == format) return true;
	if (a->gameTex) SDL_DestroyTexture(a->gameTex);
	a->gameTex = SDL_CreateTexture(a->renderer, format, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!a->gameTex) { a->gameTexW = a->gameTexH = 0; return false; }
	a->gameTexW = w; a->gameTexH = h; a->gameTexFormat = format;
	SDL_SetTextureScaleMode(a->gameTex, a->cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
	return true;
}

static void present(struct Game* g) {
	struct App* a = g->app;
	if (g->retro) {
		if (!g->retroFrame || !ensureGameTexture(a, g->retroFrameW, g->retroFrameH, SDL_PIXELFORMAT_ABGR8888)) return;
		SDL_UpdateTexture(a->gameTex, NULL, g->retroFrame, g->retroFrameW * sizeof(uint32_t));
	} else SDL_UpdateTexture(a->gameTex, NULL, g->frame, GBA_W * sizeof(color_t));
	updateXbr(g);
	drawGameLayer(g);
	Uint32 now = SDL_GetTicks();
	bool toast = g->osd[0] && now < g->osdUntil;
	if (toast || g->turbo) {
		int W = uiBegin(a);
		struct Ui* u = &a->ui;
		uiClear(u, 0);
		if (toast) {
			int tw = uiTextW(g->osd, 1);
			if (tw > W - 40) tw = W - 40;
			int lines = uiTextWrap(NULL, g->osd, 0, 0, tw, 13, 0, 0, -1, 2);
			uiWindow(u, 4, 4, tw + 22, 10 + 13 * lines);
			uiTextWrap(u, g->osd, 14, 7, tw, 13, UI_INK, UI_INK_SH, -1, 2);
		}
		if (g->turbo) {
			char tb[32];
			snprintf(tb, sizeof(tb), "TURBO x%d", a->cfg.turbo);
			int tw = uiTextW(tb, 1);
			uiShade(u, W - tw - 14, UI_H - 16, tw + 10, 13, 0xD83038, 220);
			uiText(u, tb, W - tw - 9, UI_H - 16, UI_WHITE, 0, 1, -1);
		}
		uiShow(a, true);
	}
	if (a->fadeFromWhite) {
		Uint32 el = now - a->fadeFromWhite;
		if (el >= 450) a->fadeFromWhite = 0;
		else {
			SDL_RenderSetLogicalSize(a->renderer, 0, 0);
			SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(a->renderer, 236, 255, 246, (Uint8) (255 - el * 255 / 450));
			SDL_RenderFillRect(a->renderer, NULL);
		}
	}
	SDL_RenderPresent(a->renderer);
}

static void pumpAudio(struct Game* g, bool discard) {
	struct App* a = g->app;
	blip_t* left = g->core->getAudioChannel(g->core, 0);
	blip_t* right = g->core->getAudioChannel(g->core, 1);
	static int16_t buf[4096 * 2];
	int avail;
	while ((avail = blip_samples_avail(left)) > 0) {
		if (avail > 4096) avail = 4096;
		blip_read_samples(left, buf, avail, true);
		blip_read_samples(right, buf + 1, avail, true);
		if (discard || !a->audio) continue;
		if (a->cfg.volume != 100) {
			for (int i = 0; i < avail * 2; ++i) buf[i] = (int16_t) (buf[i] * a->cfg.volume / 100);
		}
		SDL_QueueAudio(a->audio, buf, avail * 2 * sizeof(int16_t));
	}
}

/* ================================================================== */
/* Menú de pausa (Esc en teclado, − y + juntos en el mando)            */
/* ================================================================== */
enum PauseAction { PAUSE_CONTINUE, PAUSE_SAVE, PAUSE_LOAD, PAUSE_FILTER, PAUSE_EDGES, PAUSE_VIVID, PAUSE_CONTROLS, PAUSE_RESET, PAUSE_MENU, PAUSE_QUIT, PAUSE_COUNT };

/* Opciones visibles: en Switch no hay "Salir" (se usa el botón HOME) */
static const enum PauseAction PAUSE_ITEMS[] = {
	PAUSE_CONTINUE, PAUSE_SAVE, PAUSE_LOAD, PAUSE_FILTER, PAUSE_EDGES, PAUSE_VIVID, PAUSE_CONTROLS, PAUSE_RESET, PAUSE_MENU,
#ifndef __SWITCH__
	PAUSE_QUIT,
#endif
};
#define PAUSE_N ((int) (sizeof(PAUSE_ITEMS) / sizeof(PAUSE_ITEMS[0])))

static void pauseLabel(struct App* a, enum PauseAction act, char* out, size_t n) {
	const struct ZadarkHD* z = &a->cfg.zhd;
	switch (act) {
	case PAUSE_CONTINUE: snprintf(out, n, "%s", T("Continuar")); break;
	case PAUSE_SAVE: snprintf(out, n, "%s", T("Guardar estado...")); break;
	case PAUSE_LOAD: snprintf(out, n, "%s", T("Cargar estado...")); break;
	case PAUSE_RESET: snprintf(out, n, "%s", T("Reiniciar juego")); break;
	case PAUSE_FILTER:
		if (a->cfg.shader == SHADER_NORMAL) snprintf(out, n, "%s", T("Filtro:  Normal"));
		else snprintf(out, n, T("Filtro:  Zadark HD %d"), z->textura);
		break;
	case PAUSE_EDGES: snprintf(out, n, T("Bordes:  %d"), z->bordes); break;
	case PAUSE_VIVID: snprintf(out, n, T("Vivo:  %d"), z->vivo); break;
	case PAUSE_CONTROLS: snprintf(out, n, "%s", T("Controles")); break;
	case PAUSE_MENU: snprintf(out, n, "%s", T("Volver a la biblioteca")); break;
	case PAUSE_QUIT: snprintf(out, n, "%s", T("Salir")); break;
	default: out[0] = 0;
	}
}

static const char* pauseHelp(enum PauseAction act) {
	switch (act) {
	case PAUSE_CONTINUE: return T("Vuelve a la partida.");
	case PAUSE_SAVE: return T("Guarda este momento exacto en una de 4 ranuras, con su foto.");
	case PAUSE_LOAD: return T("Vuelve a una ranura o al punto donde saliste la \xC3\xBAltima vez.");
	case PAUSE_RESET: return T("Empieza el juego de cero, como al encender la consola.");
	case PAUSE_FILTER: return T("Normal o Zadark HD 1-4 (el n\xC3\xBAmero es la textura de pantalla).");
	case PAUSE_EDGES: return T("Suavizado de bordes: 1 = 35 %, 2 = 40 %, 3 = 45 %, 4 = 50 %.");
	case PAUSE_VIVID: return T("Color remasterizado: 1 sutil, 2 natural, 3 vivo, 4 intenso.");
	case PAUSE_CONTROLS: return T("Cambia los botones: global para la consola o solo para este juego.");
	case PAUSE_MENU: return T("Vuelve a la biblioteca. Lo que no hayas guardado se pierde.");
	case PAUSE_QUIT: return T("Cierra SwapShelf. Lo que no hayas guardado se pierde.");
	default: return "";
	}
}

/* Menú de pausa con el estilo de la biblioteca; dialog = 0 nada, 1 biblioteca, 2 salir */
static void drawPause(struct Game* g, int sel, int dialog, int focus) {
	struct App* a = g->app;
	drawGameLayer(g);
	SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
	fillRect2(a, 0, 0, LIB_W, LIB_H, 0x0A0C12, 170);
	/* panel izquierdo */
	fillRR(a, 60, 60, 470, 600, 22, TH.panel, 245);
	textFit(a, F_ITEM, 0.62f, g->title, 90, 86, 410, TH.text);
	int bw = textW(F_SMALL, SYS_NAME[g_gameSys], 0.5f) + 22;
	fillRR(a, 90, 134, bw, 26, 13, SYS_COLOR[g_gameSys], 255);
	text(a, F_SMALL, 0.5f, SYS_NAME[g_gameSys], 101, 137, 0x141008, 255, AL_LEFT);
	text(a, F_SMALL, 0.5f, T("En pausa"), 90 + bw + 12, 137, TH.dim, 255, AL_LEFT);
	for (int i = 0; i < PAUSE_N; ++i) {
		enum PauseAction act = PAUSE_ITEMS[i];
		float y = 182 + i * 46;
		bool on = i == sel;
		bool img = act == PAUSE_FILTER || act == PAUSE_EDGES || act == PAUSE_VIVID;
		bool warn = act == PAUSE_MENU || act == PAUSE_QUIT || act == PAUSE_RESET;
		if (on) fillRR(a, 76, y - 6, 438, 44, 22, warn ? 0xE05050 : TH.acc, 255);
		char lab[64];
		pauseLabel(a, act, lab, sizeof(lab));
		Uint32 col = on ? 0x0A0C10 : (img ? TH.acct : (warn ? 0xF0A0A0 : TH.text));
		text(a, F_SMALL, 0.62f, lab, 100, (int) y, col, 255, AL_LEFT);
		if (img && on) text(a, F_SMALL, 0.55f, T("cambiar"), 490, (int) y + 3, 0x0A0C10, 200, AL_RIGHT);
	}
	/* descripción */
	fillRR(a, 560, 560, 660, 100, 22, TH.panel, 245);
	text(a, F_SMALL, 0.6f, pauseHelp(PAUSE_ITEMS[sel]), 590, 584, TH.text2, 255, AL_LEFT);
#ifdef __SWITCH__
	text(a, F_SMALL, 0.5f, T("A: Elegir     B: Continuar"), 590, 622, TH.dim, 255, AL_LEFT);
#else
	text(a, F_SMALL, 0.5f, T("Enter: Elegir     Esc: Continuar"), 590, 622, TH.dim, 255, AL_LEFT);
#endif
#ifdef SWS_TEST
	{
		static int lastSel = -1, lastDlg = -1;
		if (sel != lastSel || dialog != lastDlg) {
			char n[48];
			snprintf(n, sizeof(n), "pausa_%d_%d", sel, dialog);
			lastSel = sel; lastDlg = dialog;
			if (dialog == 0) testCapture(a, n);
		}
	}
#endif
	const char* keep = a->cfg.resume ? T("Se guarda el punto exacto: al volver a abrir el juego sigues desde aqu\xC3\xAD.")
	                                 : T("Lo que no hayas guardado dentro del juego o con un estado r\xC3\xA1pido se perder\xC3\xA1.");
	if (dialog == 1) drawDialogBox(a, T("\xC2\xBFVolver a la biblioteca?"), keep, T("Volver"), focus);
	if (dialog == 2) drawDialogBox(a, T("\xC2\xBFSalir de SwapShelf?"), keep, T("Salir"), focus);
	if (dialog == 3) drawDialogBox(a, T("\xC2\xBFReiniciar el juego?"), T("Empieza desde el encendido. Lo que no hayas guardado se pierde."), T("Reiniciar"), focus);
#ifdef SWS_TEST
	if (dialog) { static int d = 0; if (d != dialog) { d = dialog; testCapture(a, dialog == 1 ? "pausa_confirmar_biblioteca" : "pausa_confirmar_salir"); } }
#endif
	SDL_RenderSetLogicalSize(a->renderer, 0, 0);
	SDL_RenderPresent(a->renderer);
}

/* Elegir ranura de estado. save: 1..4; cargar: "al salir" + 1..4.
 * Devuelve la ranura o -1. */
static int runSlots(struct Game* g, bool save) {
	struct App* a = g->app;
	const int first = save ? 1 : 0, n = STATE_SLOTS + 1 - first;
	SDL_Texture* tex[STATE_SLOTS + 1] = { 0 };
	char when[STATE_SLOTS + 1][48];
	bool used[STATE_SLOTS + 1] = { false };
	int sel = 0, best = -1;
	long long newest = 0;
	for (int k = first; k <= STATE_SLOTS; ++k) {
		char p[1024], tp[1024];
		statePathFor(g, k, p, sizeof(p));
		thumbPathFor(g, k, tp, sizeof(tp));
		struct stat st;
		used[k] = stat(p, &st) == 0;
		when[k][0] = 0;
		if (used[k]) {
			strftime(when[k], sizeof(when[k]), "%d/%m  %H:%M", localtime(&st.st_mtime));
			if ((long long) st.st_mtime > newest) { newest = st.st_mtime; best = k; }
			SDL_Surface* s = fileExists(tp) ? SDL_LoadBMP(tp) : NULL;
			if (s) { tex[k] = SDL_CreateTextureFromSurface(a->renderer, s); SDL_FreeSurface(s); }
		}
	}
	/* al cargar empieza en la más reciente; al guardar, en la primera libre */
	if (!save && best >= 0) sel = best - first;
	if (save) for (int k = 1; k <= STATE_SLOTS; ++k) if (!used[k]) { sel = k - 1; break; }
	int confirm = 0, focus = 0, result = -1;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; break; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { a->quit = true; break; }
		if (confirm) {
			if (in == IN_BACK) confirm = 0;
			if (in == IN_LEFT || in == IN_UP) focus = 0;
			if (in == IN_RIGHT || in == IN_DOWN) focus = 1;
			if (in == IN_OK) { if (focus) { result = sel + first; break; } confirm = 0; }
		} else {
			if (in == IN_BACK) break;
			if (in == IN_LEFT && sel > 0) --sel;
			if (in == IN_RIGHT && sel < n - 1) ++sel;
			if (in == IN_OK) {
				int k = sel + first;
				if (save && used[k]) { confirm = 1; focus = 0; }
				else if (!save && !used[k]) { /* vacía: no hay nada que cargar */ }
				else { result = k; break; }
			}
		}
		drawGameLayer(g);
		SDL_RenderSetLogicalSize(a->renderer, LIB_W, LIB_H);
		fillRect2(a, 0, 0, LIB_W, LIB_H, 0x0A0C12, 200);
		text(a, F_ITEM, 0.66f, save ? T("Guardar estado") : T("Cargar estado"), 60, 120, TH.text, 255, AL_LEFT);
		text(a, F_SMALL, 0.55f, save ? T("Elige una ranura. Si ya tiene algo, se reemplaza.") : T("Elige desde d\xC3\xB3nde seguir."), 60, 170, TH.dim, 255, AL_LEFT);
		const float cw = 220, gap = 20, total = n * cw + (n - 1) * gap, x0 = (LIB_W - total) / 2, y0 = 250;
		for (int i = 0; i < n; ++i) {
			int k = i + first;
			float x = x0 + i * (cw + gap);
			bool on = i == sel;
			if (on) fillRR(a, x - 5, y0 - 5, cw + 10, 230, 18, TH.acc, 255);
			fillRR(a, x, y0, cw, 220, 14, TH.panel, 255);
			SDL_FRect img = { x + 10, y0 + 10, cw - 20, 124 };
			if (tex[k]) {
				/* foto sin deformar, centrada en el recuadro */
				int tw = 1, th = 1;
				SDL_QueryTexture(tex[k], NULL, NULL, &tw, &th);
				fillRR(a, img.x, img.y, img.w, img.h, 8, 0x000000, 255);
				float s = img.w / tw < img.h / th ? img.w / tw : img.h / th;
				SDL_FRect d = { img.x + (img.w - tw * s) / 2, img.y + (img.h - th * s) / 2, tw * s, th * s };
				SDL_RenderCopyF(a->renderer, tex[k], NULL, &d);
			}
			else {
				fillRR(a, img.x, img.y, img.w, img.h, 8, TH.bg, 255);
				text(a, F_SMALL, 0.5f, used[k] ? T("sin foto") : T("vac\xC3\xAD" "a"), (int) (x + cw / 2), (int) (y0 + 60), TH.faint, 255, AL_CENTER);
			}
			char name[32];
			if (k == 0) snprintf(name, sizeof(name), "%s", T("Al salir"));
			else snprintf(name, sizeof(name), T("Ranura %d"), k);
			text(a, F_SMALL, 0.6f, name, (int) x + 14, (int) y0 + 146, TH.text, 255, AL_LEFT);
			text(a, F_SMALL, 0.5f, used[k] ? when[k] : T("Vac\xC3\xAD" "a"), (int) x + 14, (int) y0 + 180, used[k] ? TH.dim : TH.faint, 255, AL_LEFT);
		}
#ifdef __SWITCH__
		text(a, F_SMALL, 0.52f, T("A: Elegir     B: Volver"), 60, 620, TH.dim, 255, AL_LEFT);
#else
		text(a, F_SMALL, 0.52f, T("Enter: Elegir     Esc: Volver"), 60, 620, TH.dim, 255, AL_LEFT);
#endif
		if (confirm) {
			char t[64];
			snprintf(t, sizeof(t), T("\xC2\xBFReemplazar la ranura %d?"), sel + first);
			drawDialogBox(a, t, T("El estado que tiene ahora se pierde."), T("Reemplazar"), focus);
		}
#ifdef SWS_TEST
		{ static int shot[2]; if (!shot[save]) { shot[save] = 1; testCapture(a, save ? "ranuras_guardar" : "ranuras_cargar"); } }
#endif
		SDL_RenderSetLogicalSize(a->renderer, 0, 0);
		SDL_RenderPresent(a->renderer);
		SDL_Delay(16);
	}
	for (int k = 0; k <= STATE_SLOTS; ++k) if (tex[k]) SDL_DestroyTexture(tex[k]);
	return result;
}

static int runSlots(struct Game* g, bool save);

static enum PauseAction runPause(struct Game* g) {
	struct App* a = g->app;
	Uint32 pauseStart = SDL_GetTicks();
	if (a->audio) { SDL_PauseAudioDevice(a->audio, 1); SDL_ClearQueuedAudio(a->audio); }
	captureThumb(g);  /* la foto de este momento, por si se guarda en una ranura */
	int sel = 0, dialog = 0, focus = 0;
	enum PauseAction act = PAUSE_CONTINUE;
	for (;;) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { act = PAUSE_QUIT; break; }
#endif
		enum MenuInput in = menuPollInput(a);
		if (in == IN_QUIT) { act = PAUSE_QUIT; break; }
		if (dialog) {
			/* confirmación: por defecto queda en Cancelar */
			if (in == IN_BACK) dialog = 0;
			if (in == IN_LEFT || in == IN_UP) focus = 0;
			if (in == IN_RIGHT || in == IN_DOWN) focus = 1;
			if (in == IN_OK) {
				if (focus == 1) { act = dialog == 1 ? PAUSE_MENU : dialog == 3 ? PAUSE_RESET : PAUSE_QUIT; break; }
				dialog = 0;
			}
		} else {
			if (in == IN_BACK || in == IN_START) { act = PAUSE_CONTINUE; break; }
			if (in == IN_UP) sel = (sel + PAUSE_N - 1) % PAUSE_N;
			if (in == IN_DOWN) sel = (sel + 1) % PAUSE_N;
			enum PauseAction cur = PAUSE_ITEMS[sel];
			if (in == IN_OK && (cur == PAUSE_FILTER || cur == PAUSE_EDGES || cur == PAUSE_VIVID)) {
				/* cambia la imagen al instante y se queda en la pausa */
				struct ZadarkHD* z = &a->cfg.zhd;
				if (cur == PAUSE_FILTER) {
					if (a->cfg.shader == SHADER_NORMAL) { a->cfg.shader = SHADER_ZHD; z->textura = 1; }
					else if (z->textura < 4) ++z->textura;
					else a->cfg.shader = SHADER_NORMAL;
				} else {
					if (a->cfg.shader == SHADER_NORMAL) a->cfg.shader = SHADER_ZHD;
					int* lv = cur == PAUSE_EDGES ? &z->bordes : &z->vivo;
					*lv = *lv % 4 + 1;
				}
				updateXbr(g);
				saveLastChoice(a);
			} else if (in == IN_OK && cur == PAUSE_CONTROLS) {
				runControls(a, g_gameSys, g_gameCrc, g->title, NULL);
				ctlLoadFor(&g_ctl, g_gameSys, g_gameCrc);  /* aplicar al volver */
				if (a->quit) { act = PAUSE_QUIT; break; }
			} else if (in == IN_OK && (cur == PAUSE_MENU || cur == PAUSE_QUIT || cur == PAUSE_RESET)) {
				dialog = cur == PAUSE_MENU ? 1 : cur == PAUSE_RESET ? 3 : 2;
				focus = 0;
			} else if (in == IN_OK && (cur == PAUSE_SAVE || cur == PAUSE_LOAD)) {
				int slot = runSlots(g, cur == PAUSE_SAVE);
				if (a->quit) { act = PAUSE_QUIT; break; }
				if (slot >= 0) {
					if (cur == PAUSE_SAVE) {
						char msg[64];
						snprintf(msg, sizeof(msg), saveSlot(g, slot) ? T("Guardado en la ranura %d") : T("Error al guardar en la ranura %d"), slot);
						notify(g, msg);
					} else {
						char p[1024];
						statePathFor(g, slot, p, sizeof(p));
						notify(g, loadStateFrom(g, p) ? (slot ? T("Estado cargado") : T("Vuelves a donde saliste")) : T("Ese estado no es v\xC3\xA1lido"));
					}
					act = PAUSE_CONTINUE;
					break;
				}
			} else if (in == IN_OK) { act = cur; break; }
		}
		drawPause(g, sel, dialog, focus);
		SDL_Delay(16);
	}
	if (g->thumb) { SDL_FreeSurface(g->thumb); g->thumb = NULL; }
	g_pausedMs += SDL_GetTicks() - pauseStart;  /* la pausa no cuenta como tiempo jugado */
	/* al volver: sin teclas "pegadas" y el audio en marcha otra vez */
	g->keys = 0;
	g->padKeys = 0;
	g->turbo = false;
	if (a->audio) SDL_PauseAudioDevice(a->audio, 0);
	return act;
}

/* Ejecuta la ROM elegida. Devuelve false si hubo un error al cargarla. */
static bool runMgbaGame(struct App* a) {
	const char* romPath = a->launchPath[0] ? a->launchPath : a->launchId;
	struct Game game;
	memset(&game, 0, sizeof(game));
	struct Game* g = &game;
	g->app = a;
	snprintf(g->romPath, sizeof(g->romPath), "%s", romPath);
	snprintf(g->title, sizeof(g->title), "%s", a->gameTitle[0] ? a->gameTitle : APP_TITLE);
	/* Si se volvió desde NES, restablecer el formato nativo de mGBA. */
#ifdef COLOR_16_BIT
	if (!ensureGameTexture(a, GBA_W, GBA_H, SDL_PIXELFORMAT_BGR555)) return false;
#else
	if (!ensureGameTexture(a, GBA_W, GBA_H, SDL_PIXELFORMAT_ABGR8888)) return false;
#endif

	SDL_Log("Juego: creando n\xC3\xBA" "cleo para %s", g->romPath);
	g->core = mCoreFind(g->romPath);
	if (!g->core || !g->core->init(g->core)) {
		setMessage(a, T("El archivo no es una ROM v\xC3\xA1lida"));
		return false;
	}
	SDL_Log("Juego: n\xC3\xBA" "cleo iniciado");
	mCoreInitConfig(g->core, NULL);
	mCoreConfigSetDefaultIntValue(&g->core->config, "useBios", 0);
	mCoreConfigSetDefaultIntValue(&g->core->config, "skipBios", 1);
	mCoreConfigSetDefaultIntValue(&g->core->config, "idleOptimization", 1);
	mCoreConfigSetDefaultIntValue(&g->core->config, "volume", 0x100);  /* sin esto mGBA queda mudo (volumen 0) */
	mCoreConfigSetDefaultIntValue(&g->core->config, "sgb.borders", 0);  /* GB: sin marco de Super Game Boy */
	mCoreLoadForeignConfig(g->core, &g->core->config);

	g->frame = calloc(GBA_W * GBA_H, sizeof(color_t));
	g->core->setVideoBuffer(g->core, g->frame, GBA_W);
	g->core->setAudioBufferSize(g->core, 2048);
	blip_set_rates(g->core->getAudioChannel(g->core, 0), g->core->frequency(g->core), SAMPLE_RATE);
	blip_set_rates(g->core->getAudioChannel(g->core, 1), g->core->frequency(g->core), SAMPLE_RATE);

	SDL_Log("Juego: configuraci\xC3\xB3n lista, abriendo ROM");
	struct VFile* rom = VFileOpen(g->romPath, O_RDONLY);
	SDL_Log("Juego: ROM abierta (%s), cargando en el n\xC3\xBA" "cleo", rom ? "ok" : "fallo");
	if (!rom || !g->core->loadROM(g->core, rom)) {
		if (rom) rom->close(rom);
		mCoreConfigDeinit(&g->core->config);
		g->core->deinit(g->core);
		free(g->frame);
		setMessage(a, T("No se pudo cargar la ROM"));
		return false;
	}

	{
		unsigned vw = GBA_W, vh = GBA_H;
		g->core->desiredVideoDimensions(g->core, &vw, &vh);
		a->vw = vw > GBA_W ? GBA_W : (int) vw;
		a->vh = vh > GBA_H ? GBA_H : (int) vh;
		SDL_Log("Juego: pantalla %ux%u", vw, vh);
	}
	/* Partida por OPCIÓN (no por nombre de ROM): sobrevive a las actualizaciones */
	char savePath[1024];
	SDL_Log("Juego: ROM cargada, preparando partida");
	bool backedUp = prepareSave(a, a->launchId, savePath, sizeof(savePath), g->statePath, sizeof(g->statePath));
	SDL_Log("Juego: partida en %s", savePath);
	struct VFile* save = VFileOpen(savePath, O_CREAT | O_RDWR);
	if (!save || !g->core->loadSave(g->core, save)) {
		SDL_Log("Aviso: no se pudo abrir %s", savePath);
	}
	g->core->reset(g->core);
	resumeAuto(g);
	SDL_Log("Juego iniciado: %s", g->title);
	SDL_SetWindowTitle(a->window, g->title);
	if (backedUp) {
		notify(g, T("Versi\xC3\xB3n nueva: partida anterior respaldada"));
		g->titleReset = SDL_GetTicks() + 8000;
		g->osdUntil = SDL_GetTicks() + 6000;
	}

	const Uint32 audioTarget = (Uint32) (SAMPLE_RATE / GBA_FPS * 4) * 2 * sizeof(int16_t);
	const double frameTicks = (double) SDL_GetPerformanceFrequency() / GBA_FPS;
	double nextFrame = (double) SDL_GetPerformanceCounter();
	if (a->audio) { SDL_ClearQueuedAudio(a->audio); SDL_PauseAudioDevice(a->audio, 0); }
	g->running = true;

	while (g->running && !g->toMenu) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; break; }
#endif
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT: g->running = false; break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: handleKey(g, &e.key); break;
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED: handleDeviceEvent(a, &e); break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g->keys = 0;
				break;
			}
		}
		pollPad(g);
		if (g->pauseRequested) {
			g->pauseRequested = false;
			switch (runPause(g)) {
			case PAUSE_SAVE: saveState(g); break;
			case PAUSE_LOAD: loadState(g); break;
			case PAUSE_RESET: g->core->reset(g->core); notify(g, T("Juego reiniciado")); break;
			case PAUSE_MENU: g->toMenu = true; continue;
			case PAUSE_QUIT: g->running = false; continue;
			default: break;
			}
			nextFrame = (double) SDL_GetPerformanceCounter();
		}
		g->core->setKeys(g->core, (g->keys | g->padKeys) & K_GBA_MASK);

		if (g->titleReset && SDL_GetTicks() > g->titleReset) {
			SDL_SetWindowTitle(a->window, g->title);
			g->titleReset = 0;
		}

		int frames = g->turbo ? a->cfg.turbo : 1;
		for (int i = 0; i < frames; ++i) {
			g->core->runFrame(g->core);
			pumpAudio(g, g->turbo && i < frames - 1);
		}
		present(g);
		{
			static Uint32 logged = 0, frames = 0, since = 0;
			if (logged < 2) { ++logged; SDL_Log("Juego: cuadro %u dibujado", logged); }
			++frames;
			if (!since) since = SDL_GetTicks();
			if (SDL_GetTicks() - since >= 10000) {
				SDL_Log("Juego: %.1f cuadros/s, audio en cola %u bytes", frames * 1000.0 / (SDL_GetTicks() - since),
				        a->audio ? SDL_GetQueuedAudioSize(a->audio) : 0);
				frames = 0;
				since = SDL_GetTicks();
			}
		}
#ifdef SWS_TEST
		{
			static int gf = 0;
			++gf;
			/* Pulsa A cada 0,5 s a partir de los 15 s para pasar la intro */
			if (gf > 900) g->padKeys = 0, g->keys = (gf / 30) % 2 ? (1u << K_A) : 0;
			if (getenv("SWS_TEST_PAUSE") && gf == 700) g->pauseRequested = true;
			if (gf == 600 || gf == 760 || gf == 1500 || gf == 2400) { char n[32]; snprintf(n, sizeof(n), "game_%d", gf); testCapture(a, n); }
			if (gf == 2400) { gf = 0; g->toMenu = true; }
		}
#endif

		if (a->audio && !g->turbo) {
			/* El audio marca el ritmo; límite de 100 ms por si el dispositivo se traba */
			Uint32 waitStart = SDL_GetTicks();
			while (SDL_GetQueuedAudioSize(a->audio) > audioTarget) {
				if (SDL_GetTicks() - waitStart > 100) { SDL_ClearQueuedAudio(a->audio); break; }
				SDL_Delay(1);
			}
			nextFrame = (double) SDL_GetPerformanceCounter();
		} else {
			if (a->audio && SDL_GetQueuedAudioSize(a->audio) > audioTarget * 2) SDL_ClearQueuedAudio(a->audio);
			nextFrame += frameTicks;
			double now = (double) SDL_GetPerformanceCounter();
			if (nextFrame > now) {
				SDL_Delay((Uint32) ((nextFrame - now) * 1000.0 / SDL_GetPerformanceFrequency()));
			} else if (now - nextFrame > frameTicks * 5) {
				nextFrame = now;
			}
		}
	}
	if (!g->toMenu) a->quit = true;
	saveAuto(g);

	/* deinit sincroniza la partida (.sav) al disco */
	if (a->audio) { SDL_PauseAudioDevice(a->audio, 1); SDL_ClearQueuedAudio(a->audio); }
	mCoreConfigDeinit(&g->core->config);
	g->core->deinit(g->core);
	free(g->frame);
	return true;
}

/* ---- partida normal (SRAM) de los núcleos libretro ---- */
static uint32_t sramSum(const uint8_t* p, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
	return h;
}

static void retroSramLoad(struct Game* g) {
	const struct RetroApi* api = g->retroApi;
	size_t n = api->getMemorySize(RETRO_MEMORY_SAVE_RAM);
	uint8_t* mem = api->getMemoryData(RETRO_MEMORY_SAVE_RAM);
	if (!n || !mem) return;
	size_t len = 0;
	unsigned char* d = swsReadFile(g->sramPath, &len);
	if (d && len) { memcpy(mem, d, len < n ? len : n); SDL_Log("Partida cargada (%u bytes)", (unsigned) (len < n ? len : n)); }
	free(d);
	g->sramSum = sramSum(mem, n);
	g->sramChecked = SDL_GetTicks();
}

static void retroSramSave(struct Game* g) {
	const struct RetroApi* api = g->retroApi;
	size_t n = api->getMemorySize(RETRO_MEMORY_SAVE_RAM);
	uint8_t* mem = api->getMemoryData(RETRO_MEMORY_SAVE_RAM);
	if (!n || !mem || !g->sramPath[0]) return;
	uint32_t s = sramSum(mem, n);
	if (s == g->sramSum && fileExists(g->sramPath)) return;
	FILE* f = fopen(g->sramPath, "wb");
	if (!f) return;
	fwrite(mem, 1, n, f);
	fclose(f);
	g->sramSum = s;
}

/* Cada 2 segundos: si el juego escribió en su memoria de guardado, se graba */
static void retroSramAutosave(struct Game* g) {
	if (SDL_GetTicks() - g->sramChecked < 2000) return;
	g->sramChecked = SDL_GetTicks();
	const struct RetroApi* api = g->retroApi;
	size_t n = api->getMemorySize(RETRO_MEMORY_SAVE_RAM);
	uint8_t* mem = api->getMemoryData(RETRO_MEMORY_SAVE_RAM);
	if (n && mem && sramSum(mem, n) != g->sramSum) retroSramSave(g);
}

/* Ejecuta un núcleo libretro: NES (FCEUmm), SNES (Supafaust) y Genesis
 * (clownmdemu) comparten pantalla, filtro, pausa, controles y partidas. */
static bool runRetroGame(struct App* a, const struct RetroApi* api) {
	const char* romPath = a->launchPath[0] ? a->launchPath : a->launchId;
	struct Game game;
	memset(&game, 0, sizeof(game));
	struct Game* g = &game;
	g->app = a; g->retro = true; g->retroApi = api;
	g->retroFormat = RETRO_PIXEL_FORMAT_0RGB1555;
	g->retroFps = 60.0;
	snprintf(g->romPath, sizeof(g->romPath), "%s", romPath);
	snprintf(g->title, sizeof(g->title), "%s", a->gameTitle[0] ? a->gameTitle : APP_TITLE);

	g_retroGame = g;
	g_retroPal = api == &RETRO_GEN && genesisIsPal(g->romPath);
	if (g_retroPal) SDL_Log("Juego: Genesis europeo, modo PAL (50 Hz)");
	api->setEnvironment(retroEnvironment);
	api->setVideo(retroVideo);
	api->setAudio(retroAudio);
	api->setAudioBatch(retroAudioBatch);
	api->setInputPoll(retroInputPoll);
	api->setInputState(retroInputState);
	api->init();
	api->setController(0, RETRO_DEVICE_JOYPAD);
	/* La ROM se pasa en memoria (Supafaust y clownmdemu no la leen solos) */
	size_t romLen = 0;
	unsigned char* rom = swsReadFile(g->romPath, &romLen);
	if (rom && api == &RETRO_GEN && romLen > 512 && romLen % 16384 == 512) {
		/* .smd: cabecera de 512 bytes y bloques de 16 KB entrelazados (pares/impares) */
		size_t n = romLen - 512;
		unsigned char* plain = malloc(n);
		if (plain) {
			for (size_t b = 0; b < n; b += 16384)
				for (size_t i = 0; i < 8192; ++i) {
					plain[b + i * 2 + 1] = rom[512 + b + i];
					plain[b + i * 2] = rom[512 + b + 8192 + i];
				}
			free(rom); rom = plain; romLen = n;
		}
	}
	struct retro_game_info info = { .path = g->romPath, .data = rom, .size = romLen, .meta = NULL };
	char msg[160];
	if (!rom || !api->loadGame(&info)) {
		free(rom);
		api->deinit(); g_retroGame = NULL;
		snprintf(msg, sizeof(msg), T("No se pudo cargar la ROM de %s"), api->name);
		setMessage(a, msg);
		return false;
	}
	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	api->getAvInfo(&av);
	if (av.geometry.base_width == 0 || av.geometry.base_height == 0) {
		api->unloadGame(); api->deinit(); g_retroGame = NULL; free(rom);
		setMessage(a, T("La resoluci\xC3\xB3n de esta ROM no es compatible"));
		return false;
	}
	a->vw = (int) (av.geometry.base_width > VIDEO_MAX_W ? av.geometry.base_width / 2 : av.geometry.base_width);
	a->vh = (int) (av.geometry.base_height > VIDEO_MAX_H ? av.geometry.base_height / 2 : av.geometry.base_height);
	g->retroFps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
	g->retroRate = av.timing.sample_rate > 1000 ? av.timing.sample_rate : SAMPLE_RATE;
	SDL_Log("Juego: %s %dx%d a %.2f fps, audio %.0f Hz", api->name, a->vw, a->vh, g->retroFps, g->retroRate);
	if (!ensureGameTexture(a, a->vw, a->vh, SDL_PIXELFORMAT_ABGR8888)) {
		api->unloadGame(); api->deinit(); g_retroGame = NULL; free(rom);
		setMessage(a, T("No se pudo crear la pantalla del juego"));
		return false;
	}

	char savePath[1024];
	prepareSave(a, a->launchId, savePath, sizeof(savePath), g->statePath, sizeof(g->statePath));
	/* partida normal (SRAM con pila del cartucho) */
	snprintf(g->sramPath, sizeof(g->sramPath), "%s", savePath);
	retroSramLoad(g);
	api->reset();
	resumeAuto(g);
	SDL_SetWindowTitle(a->window, g->title);
	if (a->audio) { SDL_ClearQueuedAudio(a->audio); SDL_PauseAudioDevice(a->audio, 0); }
	g->running = true;
	const Uint32 audioTarget = (Uint32) (SAMPLE_RATE / g->retroFps * 4) * 2 * sizeof(int16_t);
	double nextFrame = (double) SDL_GetPerformanceCounter();
	const double frameTicks = (double) SDL_GetPerformanceFrequency() / g->retroFps;

	while (g->running && !g->toMenu) {
#ifdef __SWITCH__
		if (!appletMainLoop()) { a->quit = true; break; }
#endif
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT: g->running = false; break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: handleKey(g, &e.key); break;
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED: handleDeviceEvent(a, &e); break;
			case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g->keys = 0; break;
			}
		}
		pollPad(g);
		if (g->pauseRequested) {
			g->pauseRequested = false;
			switch (runPause(g)) {
			case PAUSE_SAVE: saveState(g); break;
			case PAUSE_LOAD: loadState(g); break;
			case PAUSE_RESET: api->reset(); notify(g, T("Juego reiniciado")); break;
			case PAUSE_MENU: g->toMenu = true; continue;
			case PAUSE_QUIT: g->running = false; continue;
			default: break;
			}
			nextFrame = (double) SDL_GetPerformanceCounter();
		}
		if (g->titleReset && SDL_GetTicks() > g->titleReset) { SDL_SetWindowTitle(a->window, g->title); g->titleReset = 0; }
#ifdef SWS_TEST
		{
			/* pruebas: pulsa Start / A cada tanto y captura algunos cuadros */
			static int rf = 0;
			++rf;
			if (rf > 300 && rf < 1700) g->padKeys = ((rf / 40) % 2) ? (1u << ((rf / 80) % 2 ? K_START : K_A)) : 0;
			if (getenv("SWS_TEST_PAUSE") && rf == 500) g->pauseRequested = true;
			if (rf == 240 || rf == 900 || rf == 1700) { char n[32]; snprintf(n, sizeof(n), "retro_%s_%d", api->name, rf); testCapture(a, n); }
			if (rf == 1700) { rf = 0; g->toMenu = true; }
		}
#endif
		int frames = g->turbo ? a->cfg.turbo : 1;
		for (int i = 0; i < frames; ++i) { g->retroDiscardAudio = g->turbo && i < frames - 1; api->run(); }
		g->retroDiscardAudio = false;
		retroSramAutosave(g);
		present(g);
		if (a->audio && !g->turbo) {
			Uint32 waitStart = SDL_GetTicks();
			while (SDL_GetQueuedAudioSize(a->audio) > audioTarget) {
				if (SDL_GetTicks() - waitStart > 100) { SDL_ClearQueuedAudio(a->audio); break; }
				SDL_Delay(1);
			}
			nextFrame = (double) SDL_GetPerformanceCounter();
		} else {
			nextFrame += frameTicks;
			double now = (double) SDL_GetPerformanceCounter();
			if (nextFrame > now) SDL_Delay((Uint32) ((nextFrame - now) * 1000.0 / SDL_GetPerformanceFrequency()));
			else if (now - nextFrame > frameTicks * 5) nextFrame = now;
		}
	}
	if (!g->toMenu) a->quit = true;
	if (a->audio) { SDL_PauseAudioDevice(a->audio, 1); SDL_ClearQueuedAudio(a->audio); }
	saveAuto(g);
	retroSramSave(g);
	api->unloadGame(); api->deinit();
	free(rom);
	free(g->retroFrame);
	g_retroGame = NULL;
	return true;
}

/* Carga de la biblioteca al abrir (en un hilo, mientras corre la intro) */
static SDL_atomic_t g_scanDone;
static int startupScan(void* p) {
	struct App* a = p;
	libScan(a);
	playedLoad();    /* recientes y favoritos: deciden el orden de búsqueda de portadas */
	metaInit();      /* nombres oficiales (No-Intro) y portadas (TheGamesDB) */
	metaPrepare();
	SDL_AtomicSet(&g_scanDone, 1);
	return 0;
}

static bool runGame(struct App* a) {
	if (g_gameSys == SYS_NES) return runRetroGame(a, &RETRO_NES);
	if (g_gameSys == SYS_SNES) return runRetroGame(a, &RETRO_SNES);
	if (g_gameSys == SYS_GEN) return runRetroGame(a, &RETRO_GEN);
	return runMgbaGame(a);
}

/* ================================================================== */
/* Principal                                                           */
/* ================================================================== */
int main(int argc, char** argv) {
	static struct App app;
	struct App* a = &app;

#ifdef __SWITCH__
	socketInitializeDefault();  /* red para la base No-Intro y las portadas */
	/* Abierto desde el Álbum = modo applet: menos memoria y CPU. Se avisa en la biblioteca. */
	{
		AppletType at = appletGetAppletType();
		g_appletMode = at != AppletType_Application && at != AppletType_SystemApplication;
	}
	snprintf(g_base, sizeof(g_base), "%s", BASE_DIR);
	/* el .nro que se está ejecutando (para actualizarlo); desde un forwarder también llega en argv[0] */
	if (argc > 0 && argv[0] && strstr(argv[0], ".nro")) snprintf(g_nroPath, sizeof(g_nroPath), "%s", argv[0]);
	else snprintf(g_nroPath, sizeof(g_nroPath), "%s", BASE_DIR "swapshelf.nro");
	MKDIR("sdmc:/switch");
	MKDIR(BASE_DIR);
#else
	char* bp = SDL_GetBasePath();
	snprintf(g_base, sizeof(g_base), "%s", bp ? bp : "./");
	SDL_free(bp);
#endif
	settingsDefault(&a->cfg);
	char* iniPath = g_iniPath;
	pathJoin(iniPath, sizeof(g_iniPath), "swapshelf.ini");
	if (!fileExists(iniPath)) pathJoin(iniPath, sizeof(g_iniPath), "launcher.ini");  /* nombre de la v0.3 */
	settingsLoad(&a->cfg, iniPath);
	g_lang = a->cfg.lang;
	themeApply(a->cfg.lightTheme, a->cfg.accent);
	openLog();
	SDL_Log("Inicio. Carpeta: %s", g_base);
	loadLastChoice(a);

	SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
		fatal(SDL_GetError());
	}
	SDL_Log("SDL iniciado");
	mLogSetDefaultLogger(&g_logger);

	Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	int ww = 1280, wh = 720;
#ifdef __SWITCH__
	ww = 1280; wh = 720;
	wflags = SDL_WINDOW_FULLSCREEN;
#else
	if (a->cfg.fullscreen) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
	a->window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);
	if (!a->window) fatal(SDL_GetError());
#ifdef SWS_TEST
	if (getenv("SWS_TEST_WIDE")) SDL_SetWindowSize(a->window, 1280, 720);
#endif
#ifndef __SWITCH__
	SDL_SetWindowMinimumSize(a->window, GBA_W * 2, GBA_H * 2);
	if (a->cfg.fullscreen) SDL_ShowCursor(SDL_DISABLE);
	char iconPath[1024];
	pathJoin(iconPath, sizeof(iconPath), "icon.bmp");
	SDL_Surface* icon = SDL_LoadBMP(iconPath);
	if (icon) { SDL_SetWindowIcon(a->window, icon); SDL_FreeSurface(icon); }
#endif
	a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_ACCELERATED);
	if (!a->renderer) a->renderer = SDL_CreateRenderer(a->window, -1, 0);
	if (a->renderer) {
		SDL_RendererInfo ri;
		a->canTarget = SDL_GetRendererInfo(a->renderer, &ri) == 0 && (ri.flags & SDL_RENDERER_TARGETTEXTURE);
	}
	if (a->renderer) {
		SDL_RendererInfo ri;
		SDL_GetRendererInfo(a->renderer, &ri);
		SDL_Log("Renderer: %s (textura m\xC3\xA1x. %dx%d)", ri.name, ri.max_texture_width, ri.max_texture_height);
	}
	if (!a->renderer) fatal(SDL_GetError());

#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	Uint32 fmt = SDL_PIXELFORMAT_BGR565;
#elif defined(COLOR_16_BIT)
	Uint32 fmt = SDL_PIXELFORMAT_BGR555;
#else
	Uint32 fmt = SDL_PIXELFORMAT_ABGR8888;
#endif
	a->gameTex = SDL_CreateTexture(a->renderer, fmt, SDL_TEXTUREACCESS_STREAMING, GBA_W, GBA_H);
	if (!a->gameTex) fatal(SDL_GetError());
	SDL_SetTextureScaleMode(a->gameTex, a->cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
	for (int i = 0; i < 3; ++i) g_fontTex[i] = makeFontTexture(a->renderer, FONTS[i]);
	SDL_Log("Fuentes listas");

	SDL_AudioSpec want = { 0 }, have;
	want.freq = SAMPLE_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	a->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	SDL_Log("Audio: %s", a->audio ? "ok" : SDL_GetError());
	if (a->audio && a->cfg.music[0] && a->cfg.musicVolume > 0) {
		char mp[1100];
		resolvePath(mp, sizeof(mp), a->cfg.music);
		if (fileExists(mp) && !mmOpen(mp, SAMPLE_RATE, a->cfg.musicVolume / 100.0f)) SDL_Log("No se pudo abrir la m\xC3\xBAsica %s", mp);
	}
	if (!a->audio) SDL_Log("Sin audio: %s", SDL_GetError());
	openPad(a);

	/* Biblioteca: escaneo (con caché) y carátulas automáticas en segundo plano */
	{
		char d[1024];
		pathJoin(d, sizeof(d), "Partidas");
		MKDIR(d);
	}
	swsInit();  /* tabla del CRC32 */
	g_preferBox = a->cfg.boxArt;
	g_mainThreadId = SDL_ThreadID();
	/* La biblioteca se carga en segundo plano mientras corre la intro: con
	 * cientos de juegos en la microSD, hacerlo antes dejaba la pantalla en negro. */
	SDL_Thread* scanThr = SDL_CreateThread(startupScan, "biblioteca", a);
	if (!scanThr) startupScan(a);

	/* Con una ROM como argumento se salta la biblioteca (útil para accesos directos) */
	if (argc > 1 && argv[1][0] != '-' && fileExists(argv[1])) {
		snprintf(a->launchPath, sizeof(a->launchPath), "%s", argv[1]);
		snprintf(a->launchId, sizeof(a->launchId), "%s", argv[1]);
		runGame(a);
		a->launchPath[0] = 0;
	}
	runIntro(a);  /* logo SWS */
	if (scanThr) {
		/* si la intro terminó antes, se muestra el progreso */
		while (!SDL_AtomicGet(&g_scanDone) && !a->quit) {
#ifdef __SWITCH__
			if (!appletMainLoop()) { a->quit = true; break; }
#endif
			SDL_PumpEvents();
			scanDraw(a, g_scanFile);
			SDL_Delay(30);
		}
		SDL_WaitThread(scanThr, NULL);
	}
	metaApply();
	startCovers();
	startMeta(a);
	while (!a->quit) {
		int sel = runLibrary(a);
		if (sel < 0) break;
		struct LibGame* g = &g_lib[sel];
		snprintf(a->launchPath, sizeof(a->launchPath), "%s", g->path);
		snprintf(a->launchId, sizeof(a->launchId), "%08X", (unsigned) g->crc);
		snprintf(a->gameTitle, sizeof(a->gameTitle), "%s", g->name);
		/* partida por juego: nombre + CRC (cambiar de versión no pisa la partida) */
		/* si ya hay una partida de este CRC (aunque el nombre mostrado cambie), se usa esa */
		char slot[160] = "";
		{
			char pd[1024], tail[16];
			pathJoin(pd, sizeof(pd), "Partidas");
			snprintf(tail, sizeof(tail), "_%08X.sav", (unsigned) g->crc);
#ifdef _WIN32
			/* nombres en Unicode -> UTF-8 (readdir los da en la página de códigos local: los acentos se romperían) */
			wchar_t wpat[1100];
			char pat[1100];
			snprintf(pat, sizeof(pat), "%s/*%s", pd, tail);
			MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 1100);
			WIN32_FIND_DATAW fd;
			HANDLE fh = FindFirstFileW(wpat, &fd);
			if (fh != INVALID_HANDLE_VALUE) {
				char nm[600];
				if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof(nm), NULL, NULL) > 0) {
					size_t L = strlen(nm);
					if (L > 4 && L - 4 < sizeof(slot)) snprintf(slot, sizeof(slot), "%.*s", (int) (L - 4), nm);
				}
				FindClose(fh);
			}
#else
			DIR* dd = opendir(pd);
			for (struct dirent* de; dd && (de = readdir(dd));) {
				size_t L = strlen(de->d_name), T = strlen(tail);
				if (L > T && L - 4 < sizeof(slot) && !strcasecmp(de->d_name + L - T, tail)) { snprintf(slot, sizeof(slot), "%.*s", (int) (L - 4), de->d_name); break; }
			}
			if (dd) closedir(dd);
#endif
		}
		if (!slot[0]) snprintf(slot, sizeof(slot), "%s_%08X", g->name, (unsigned) g->crc);
		for (char* c = slot; *c; ++c) if (strchr("\\/:*?\"<>| ", *c)) *c = '_';
		snprintf(a->saveSlot, sizeof(a->saveSlot), "%s", slot);
		a->musicRunning = false;
		g_gameSys = g->sys;
		g_gameCrc = g->crc;
		ctlLoadFor(&g_ctl, g->sys, g->crc);
		g_ctlActive = true;
		Uint32 playStart = SDL_GetTicks();
		g_pausedMs = 0;
		runGame(a);
		g_ctlActive = false;
		{
			/* tiempo jugado (sin la pausa) */
			Uint32 ms = SDL_GetTicks() - playStart;
			playedAddTime(g->crc, (long long) ((ms > g_pausedMs ? ms - g_pausedMs : 0) / 1000));
		}
		if (!a->quit) runEjectAnim(a, &g_lib[sel], g_selX, g_selY);  /* sacar el cartucho */
		SDL_SetWindowTitle(a->window, APP_TITLE);
		a->vw = a->vh = 0;
	}
	stopMeta(true);
	stopCovers();

	xbrShutdown();
	mmClose();
	if (a->audio) SDL_CloseAudioDevice(a->audio);
	if (a->pad) SDL_GameControllerClose(a->pad);
	if (a->blurTex) SDL_DestroyTexture(a->blurTex);
	if (a->xbrTex) SDL_DestroyTexture(a->xbrTex);
	if (a->maskTex) SDL_DestroyTexture(a->maskTex);
	if (a->bloomTex) SDL_DestroyTexture(a->bloomTex);
	if (a->uiTex) SDL_DestroyTexture(a->uiTex);
	if (a->uiBig) SDL_DestroyTexture(a->uiBig);
	uiFree(&a->ui);
	free(a->xbrBuf);
	free(a->toneBuf);
	free(a->casBuf);
	SDL_DestroyTexture(a->gameTex);
	SDL_DestroyRenderer(a->renderer);
	SDL_DestroyWindow(a->window);
	SDL_Quit();
#ifdef __SWITCH__
	socketExit();
#endif
	return 0;
}
