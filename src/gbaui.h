#ifndef SWS_GBAUI_H
#define SWS_GBAUI_H
/* Interfaz al estilo de un juego de GBA: lienzo de 160 px de alto (ancho según la pantalla),
 * fuente pixel propia, paisaje, ventanas y cuadros de diálogo. Diseño original. */
#include <stdbool.h>
#include <stdint.h>

#define UI_H 160
#define UI_MAX_W 400

/* Colores 0xAARRGGBB */
#define UI_INK      0xFF484850u  /* texto oscuro de ventana */
#define UI_INK_SH   0xFFD0D0C8u  /* su sombra */
#define UI_WHITE    0xFFF8F8F8u
#define UI_BLUE     0xFF5868A0u
#define UI_GREEN    0xFF388848u
#define UI_RED      0xFFD83038u
#define UI_GOLD     0xFFF0C830u

struct Ui {
	uint32_t* px;  /* w x UI_H */
	int w;
	uint32_t* bg;  /* capa fija del paisaje */
	int bgW;
};

void uiSetWidth(struct Ui* u, int w);
void uiFree(struct Ui* u);
void uiClear(struct Ui* u, uint32_t argb);
void uiLandscape(struct Ui* u, float t, int brightness /* 100 = normal */);
void uiRect(struct Ui* u, int x, int y, int w, int h, uint32_t argb);
void uiShade(struct Ui* u, int x, int y, int w, int h, uint32_t rgb, int alpha);
int uiTextW(const char* s, int scale);
/* maxChars < 0 = todo (para el texto letra por letra); shadow/outline 0 = sin */
void uiText(struct Ui* u, const char* s, int x, int y, uint32_t col, uint32_t shadow, int scale, int maxChars);
void uiTextOutline(struct Ui* u, const char* s, int x, int y, uint32_t col, uint32_t outline);
int uiUtf8Len(const char* s);
/* Ajusta a maxW y dibuja; devuelve el nº de líneas. lineH = 15 en diálogos */
int uiTextWrap(struct Ui* u, const char* s, int x, int y, int maxW, int lineH, uint32_t col, uint32_t shadow, int maxChars, int maxLines);
void uiWindow(struct Ui* u, int x, int y, int w, int h);
void uiCursor(struct Ui* u, int x, int y);         /* ▶ */
void uiMore(struct Ui* u, int x, int y, float t);  /* ▼ rojo que salta */
void uiBar(struct Ui* u, int x, int y, int w, double p);  /* barra al estilo de PS */

#endif
