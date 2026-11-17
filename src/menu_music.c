/*
 * menu_music.c - música de fondo del menú (MP3 en streaming, en bucle)
 *
 * Decodifica por partes con dr_mp3 (dominio público) y convierte a la
 * frecuencia del dispositivo con SDL_AudioStream. Nunca carga el MP3 entero
 * en memoria (importante en Switch).
 */
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_mp3.h"

#include "menu_music.h"

#include <SDL.h>
#include <string.h>

static struct {
	bool open;
	drmp3 mp3;
	SDL_AudioStream* stream;
	int outRate;
	float volume;   /* 0..1 */
	float gain;     /* rampa actual (fade) */
	float target;   /* hacia dónde va la rampa */
	float step;     /* cambio por muestra */
} M;

bool mmOpen(const char* path, int outRate, float volume) {
	mmClose();
	if (!drmp3_init_file(&M.mp3, path, NULL)) return false;
	M.stream = SDL_NewAudioStream(AUDIO_S16SYS, (Uint8) M.mp3.channels, (int) M.mp3.sampleRate, AUDIO_S16SYS, 2, outRate);
	if (!M.stream) { drmp3_uninit(&M.mp3); return false; }
	M.open = true;
	M.outRate = outRate;
	M.volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
	M.gain = 0;
	M.target = 0;
	M.step = 0;
	SDL_Log("M\xC3\xBAsica del men\xC3\xBA: %s (%u Hz, %u canales)", path, M.mp3.sampleRate, M.mp3.channels);
	return true;
}

void mmClose(void) {
	if (!M.open) return;
	SDL_FreeAudioStream(M.stream);
	drmp3_uninit(&M.mp3);
	memset(&M, 0, sizeof(M));
}

bool mmIsOpen(void) { return M.open; }

void mmFade(float to, float seconds) {
	if (!M.open) return;
	M.target = to;
	float samples = seconds * M.outRate;
	M.step = samples > 1 ? (to - M.gain) / samples : (to - M.gain);
	if (M.step == 0) M.gain = to;
}

/* Rellena hasta 'frames' cuadros estéreo de 16 bits. Devuelve cuántos escribió. */
int mmRead(int16_t* out, int frames) {
	if (!M.open) return 0;
	int got = 0;
	int guard = 0;
	while (got < frames && guard++ < 64) {
		int avail = SDL_AudioStreamAvailable(M.stream) / (int) (2 * sizeof(int16_t));
		if (avail > 0) {
			int want = frames - got < avail ? frames - got : avail;
			int bytes = SDL_AudioStreamGet(M.stream, out + got * 2, want * 2 * (int) sizeof(int16_t));
			if (bytes <= 0) break;
			got += bytes / (int) (2 * sizeof(int16_t));
			continue;
		}
		/* Decodificar otro trozo del MP3 */
		int16_t pcm[1152 * 2 * 4];
		drmp3_uint64 cap = (sizeof(pcm) / sizeof(pcm[0])) / (M.mp3.channels ? M.mp3.channels : 1);
		drmp3_uint64 n = drmp3_read_pcm_frames_s16(&M.mp3, cap, pcm);
		if (n == 0) {
			/* Fin: vuelve al principio (bucle) */
			if (!drmp3_seek_to_pcm_frame(&M.mp3, 0)) { mmClose(); break; }
			continue;
		}
		SDL_AudioStreamPut(M.stream, pcm, (int) (n * M.mp3.channels * sizeof(int16_t)));
	}
	/* Volumen y rampa de entrada/salida */
	for (int i = 0; i < got; ++i) {
		if (M.gain != M.target) {
			M.gain += M.step;
			if ((M.step > 0 && M.gain > M.target) || (M.step < 0 && M.gain < M.target)) M.gain = M.target;
		}
		float g = M.gain * M.volume;
		out[i * 2] = (int16_t) (out[i * 2] * g);
		out[i * 2 + 1] = (int16_t) (out[i * 2 + 1] * g);
	}
	return got;
}
