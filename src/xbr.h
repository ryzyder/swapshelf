/* xbr: filtro xBR nivel 3 (Hyllian) en CPU, escala x4, formato ABGR8888 */
#pragma once
#include <stdint.h>

#define XBR_MAX_W 320
#define XBR_MAX_H 240

void xbrInit(void);
void xbrShutdown(void);  /* detiene los hilos ayudantes (al salir) */
/* Reparte fn(ctx, y0, y1) en franjas de filas entre varios núcleos */
void xbrParallel(void (*fn)(void* ctx, int y0, int y1), void* ctx, int h);
void xbrScale4(const uint32_t* src, int w, int h, int srcStride, uint32_t* dst, int dstStride);
