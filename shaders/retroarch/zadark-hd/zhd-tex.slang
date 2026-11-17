#version 450
/*
   Zadark HD - filtro de Zadark para juegos de GBA (y otras consolas 2D)
   Color remasterizado + bordes xBR-lv3 + nitidez CAS + resplandor + textura diagonal.

   Partes de terceros (licencia MIT):
     - xBR-lv3: Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com
     - CAS: AMD FidelityFX Contrast Adaptive Sharpening, Copyright (c) AMD
   La textura diagonal está inspirada en el filtro de TV de PCSX2 (reescrita).
*/

layout(push_constant) uniform Push {
	vec4 SourceSize;
	vec4 OriginalSize;
	vec4 OutputSize;
	uint FrameCount;
	float ZHD_TEXTURA;
} params;

#pragma parameter ZHD_TEXTURA "Zadark HD - Textura (1 = sin, 4 = marcada)" 1.0 1.0 4.0 1.0
#define ZHD_TEXTURA params.ZHD_TEXTURA

layout(std140, set = 0, binding = 0) uniform UBO {
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

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
	vec3 c = texture(Source, vTexCoord).rgb;
	FragColor = vec4(zhdTexture(c, floor(vTexCoord * params.OutputSize.xy), ZHD_TEXTURA), 1.0);
}
