#ifndef SWS_POSTFX_H
#define SWS_POSTFX_H

#include <stdint.h>

/* Zadark HD (filtro de Zadark). Niveles 1-4 */
struct ZadarkHD {
	int textura;   /* 1 sin textura .. 4 marcada (filtro diagonal 0/20/35/60 %) */
	int bordes;    /* 1-4: suavizado xBR 35/40/45/50 % */
	int vivo;      /* 1 sutil .. 4 intenso */
	int nitidez;   /* CAS 0-100 */
	int brillo;    /* resplandor 0-100 */
};

int zhdTextura(int nivel);
int zhdSuavizado(int nivel);
/* Color remaster (ABGR8888, R en el byte bajo) */
void zhdColor(const uint32_t* src, uint32_t* dst, int n, int vivo);
/* Resplandor de lo más claro, a 1/4 de tamaño (salida ARGB8888) */
void zhdBloom(const uint32_t* src, int w, int h, uint32_t* out);
/* CAS (nitidez adaptativa) en varios núcleos; src y dst deben ser distintos */
void postfxSharpen(const uint32_t* src, uint32_t* dst, int w, int h, int nitidez);
/* Mezcla la imagen x4 de xBR con el píxel original (menos redondeo); src = cuadro de origen */
void postfxSoften(uint32_t* big, const uint32_t* src, int srcW, int srcH, int suavizado);
/* Rellena la máscara diagonal (ARGB8888) para multiplicar sobre la pantalla */
void postfxBuildMask(uint32_t* px, int w, int h, int pitchPx, int fuerza);

#endif
