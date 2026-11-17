/*
 * netutil: archivos, CRC32 y descargas HTTP(S)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void     swsInit(void);   /* llamar una vez antes de swsCrc32 */
uint32_t swsCrc32(const unsigned char* p, size_t n);

/* Lee un archivo entero (termina en 0 extra). NULL si falla. */
unsigned char* swsReadFile(const char* path, size_t* len);

/* Descarga una URL. progressDone/progressTotal (opcionales) se actualizan durante la descarga.
 * NULL + err si falla (err contiene el código HTTP, ej. "el servidor respondió 403"). */
unsigned char* swsHttpGet(const char* url, size_t* outLen, const char** err,
                          volatile long long* progressDone, volatile long long* progressTotal);
