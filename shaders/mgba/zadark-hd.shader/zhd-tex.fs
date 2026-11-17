/*
   Zadark HD - filtro de Zadark para juegos de GBA (y otras consolas 2D)
   Color remasterizado + bordes xBR-lv3 + nitidez CAS + resplandor + textura diagonal.

   Partes de terceros (licencia MIT):
     - xBR-lv3: Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com
     - CAS: AMD FidelityFX Contrast Adaptive Sharpening, Copyright (c) AMD
   La textura diagonal está inspirada en el filtro de TV de PCSX2 (reescrita).
*/
#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#endif
uniform sampler2D tex;
uniform vec2 texSize;
varying vec2 texCoord;
uniform float ZHD_TEXTURA;

vec3 zhdTexture(vec3 c, vec2 PX, float textura) {
	float lv = clamp(floor(textura + 0.5), 1.0, 4.0);
	float fuerza = lv < 1.5 ? 0.0 : lv < 2.5 ? 20.0 : lv < 3.5 ? 35.0 : 60.0;
	if (fuerza <= 0.0) return c;
	float lo = 1.0 - fuerza / 200.0;
	float idx = mod(PX.x + mod(PX.y, 3.0), 3.0);
	vec3 m = vec3(lo);
	if (idx < 0.5) m.r = 1.0; else if (idx < 1.5) m.g = 1.0; else m.b = 1.0;
	float add = 1.2 * (1.0 - (1.0 + 2.0 * lo) / 3.0);  /* devuelve la luz que quita la máscara */
	return clamp(c * m + c * add, 0.0, 1.0);
}

void main() {
	vec3 c = texture2D(tex, texCoord).rgb;
	gl_FragColor = vec4(zhdTexture(c, floor(gl_FragCoord.xy), ZHD_TEXTURA), 1.0);
}
