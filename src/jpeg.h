/*
 * jpeg: decodificador JPEG pequeño y sin dependencias (baseline y progresivo, Huffman, 8 bits).
 * Se usa para las portadas descargadas. Devuelve RGB24 (malloc) o NULL si no puede.
 */
#pragma once
#include <stddef.h>

unsigned char* jpegDecode(const unsigned char* data, size_t len, int* outW, int* outH);
