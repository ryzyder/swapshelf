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
uniform float ZHD_NITIDEZ;
uniform float ZHD_BRILLO;
#define SAMPLE(uv) texture2D(tex, uv).rgb

vec3 zhdBright(vec3 c) {
	float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
	return c * clamp((y - 0.86) / 0.14, 0.0, 1.0);
}

vec3 zhdCasBloom(vec2 uv, vec2 TS, float nitidez, float brillo) {
	vec3 a = SAMPLE(uv + vec2(-TS.x, -TS.y));
	vec3 b = SAMPLE(uv + vec2(0.0, -TS.y));
	vec3 c = SAMPLE(uv + vec2(TS.x, -TS.y));
	vec3 d = SAMPLE(uv + vec2(-TS.x, 0.0));
	vec3 e = SAMPLE(uv);
	vec3 f = SAMPLE(uv + vec2(TS.x, 0.0));
	vec3 g = SAMPLE(uv + vec2(-TS.x, TS.y));
	vec3 h = SAMPLE(uv + vec2(0.0, TS.y));
	vec3 i = SAMPLE(uv + vec2(TS.x, TS.y));
	vec3 col = e;
	if (nitidez > 0.0) {
		vec3 mn = min(min(min(b, d), min(f, h)), e);
		vec3 mx = max(max(max(b, d), max(f, h)), e);
		vec3 mn2 = min(mn, min(min(a, c), min(g, i)));
		vec3 mx2 = max(mx, max(max(a, c), max(g, i)));
		mn += mn2;
		mx += mx2;
		vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0);
		float s = clamp(nitidez / 100.0, 0.0, 1.0);
		vec3 w = sqrt(amp) * (-1.0 / mix(8.0, 5.0, s));
		col = clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
	}
	if (brillo > 0.0) {
		/* resplandor: lo más claro, centro + dos anillos (radio 12 y 28 texels de la imagen x4),
		   ajustado para coincidir con el del lanzador */
		vec3 acc = zhdBright(e);
		for (int k = 0; k < 8; ++k) {
			float ang = float(k) * 0.785398;
			vec2 dir = vec2(cos(ang), sin(ang));
			acc += zhdBright(SAMPLE(uv + dir * TS * 12.0));
			acc += zhdBright(SAMPLE(uv + dir * TS * 28.0));
		}
		vec3 bl = acc / 17.0 * (brillo / 100.0);
		col = 1.0 - (1.0 - col) * (1.0 - clamp(bl, 0.0, 1.0));  /* mezcla "pantalla" */
	}
	return col;
}

void main() {
	/* En mGBA texSize es siempre el tamaño de la GBA; la pasada anterior es x4 */
	vec2 TS = vec2(1.0) / (texSize * 4.0);
	gl_FragColor = vec4(zhdCasBloom(texCoord, TS, ZHD_NITIDEZ, ZHD_BRILLO), 1.0);
}
