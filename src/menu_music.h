/* menu_music: música de fondo del menú (MP3 en streaming, en bucle) */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool mmOpen(const char* path, int outRate, float volume); /* volume 0..1 */
void mmClose(void);
bool mmIsOpen(void);
void mmFade(float to, float seconds);                      /* to 0..1 */
int  mmRead(int16_t* out, int frames);                     /* estéreo S16 */
