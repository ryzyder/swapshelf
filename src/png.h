/*
 * png: lector PNG pequeño y sin dependencias (inflate propio).
 * Todos los tipos de color, profundidades 1-16 bits y entrelazado Adam7.
 * Devuelve RGB24 (malloc) o NULL; la transparencia se mezcla sobre fondo oscuro.
 */
#pragma once
#include <stddef.h>

unsigned char* pngDecode(const unsigned char* data, size_t len, int* outW, int* outH);
