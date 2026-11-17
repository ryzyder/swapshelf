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
	float ZHD_BORDES;
	float ZHD_VIVO;
	float XBR_Y_WEIGHT;
	float XBR_EQ_THRESHOLD;
	float XBR_EQ_THRESHOLD2;
	float XBR_LV2_COEFFICIENT;
} params;

#pragma parameter ZHD_BORDES "Zadark HD - Bordes (1-4)" 2.0 1.0 4.0 1.0
#pragma parameter ZHD_VIVO "Zadark HD - Vivo (1-4)" 3.0 1.0 4.0 1.0
#pragma parameter XBR_Y_WEIGHT "xBR Y Weight" 48.0 0.0 100.0 1.0
#pragma parameter XBR_EQ_THRESHOLD "xBR Eq Threshold" 10.0 0.0 50.0 1.0
#pragma parameter XBR_EQ_THRESHOLD2 "xBR Eq Threshold2" 2.0 0.0 4.0 0.1
#pragma parameter XBR_LV2_COEFFICIENT "xBR Lv2 Coefficient" 2.0 1.0 3.0 0.1
#define ZHD_BORDES params.ZHD_BORDES
#define ZHD_VIVO params.ZHD_VIVO
#define XBR_Y_WEIGHT params.XBR_Y_WEIGHT
#define XBR_EQ_THRESHOLD params.XBR_EQ_THRESHOLD
#define XBR_EQ_THRESHOLD2 params.XBR_EQ_THRESHOLD2
#define XBR_LV2_COEFFICIENT params.XBR_LV2_COEFFICIENT

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

/*
   Hyllian's xBR-lv3 Shader
   
   Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.


   Incorporates some of the ideas from SABR shader. Thanks to Joshua Street.
*/

const mat3 yuv = mat3(0.299, 0.587, 0.114, -0.169, -0.331, 0.499, 0.499, -0.418, -0.0813);
const vec4 delta = vec4(0.4, 0.4, 0.4, 0.4);

vec4 df(vec4 A, vec4 B)
{
	return vec4(abs(A-B));
}

float c_df(vec3 c1, vec3 c2) {
	vec3 df = abs(c1 - c2);
	return df.r + df.g + df.b;
}

bvec4 eq(vec4 A, vec4 B)
{
	return lessThan(df(A, B), vec4(XBR_EQ_THRESHOLD));
}

bvec4 eq2(vec4 A, vec4 B)
{
	return lessThan(df(A, B), vec4(XBR_EQ_THRESHOLD2));
}

bvec4 and(bvec4 A, bvec4 B)
{
	return bvec4(A.x && B.x, A.y && B.y, A.z && B.z, A.w && B.w);
}

bvec4 or(bvec4 A, bvec4 B)
{
	return bvec4(A.x || B.x, A.y || B.y, A.z || B.z, A.w || B.w);
}

vec4 weighted_distance(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h)
{
	return (df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0*df(g,h));
}


/* Niveles de VIVO 1-4: vibrance, curva en S, toque cálido, gamma (igual que el lanzador) */
vec4 zhdVivo(float lv) {
	if (lv < 1.5) return vec4(0.15, 0.10, 0.008, 1.00);
	if (lv < 2.5) return vec4(0.30, 0.20, 0.015, 1.02);
	if (lv < 3.5) return vec4(0.45, 0.30, 0.025, 1.04);
	return vec4(0.65, 0.42, 0.035, 1.06);
}

vec3 zhdGrade(vec3 c, float lv) {
	vec4 p = zhdVivo(lv);
	float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
	float sat = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
	float warm = p.z * smoothstep(0.05, 0.25, sat);  /* blancos, grises y negros quedan neutros */
	c = clamp(vec3(y) + (c - vec3(y)) * (1.0 + p.x * (1.0 - sat)), 0.0, 1.0);
	vec3 s = c * c * (3.0 - 2.0 * c);
	c = c + (s - c) * p.y;
	c = pow(c, vec3(1.0 / p.w));
	c.r *= 1.0 + warm;
	c.b *= 1.0 - warm;
	return clamp(c, 0.0, 1.0);
}

float zhdSuavizado(float lv) {
	return 0.30 + 0.05 * clamp(floor(lv + 0.5), 1.0, 4.0);  /* 35/40/45/50 % */
}

void main() {

	bvec4 edr, edr_left, edr_up, edr3_left, edr3_up, px; // px = pixel, edr = edge detection rule
	bvec4 interp_restriction_lv1, interp_restriction_lv2_left, interp_restriction_lv2_up;
	bvec4 interp_restriction_lv3_left, interp_restriction_lv3_up;
	bvec4 nc, nc30, nc60, nc45, nc15, nc75; // new_color
	vec4 fx, fx_left, fx_up, finalfx, fx3_left, fx3_up; // inequations of straight lines.
	vec3 res1, res2, pix1, pix2;
	float blend1, blend2;

    vec2 texSize = params.SourceSize.xy;
	vec2 texCoord = vTexCoord;
	float dx = params.SourceSize.z;
	float dy = params.SourceSize.w;
	vec4 TEX1 = texCoord.xxxy + vec4( -dx, 0, dx,-2.0*dy); // A1 B1 C1
	vec4 TEX2 = texCoord.xxxy + vec4( -dx, 0, dx,    -dy); //  A  B  C
	vec4 TEX3 = texCoord.xxxy + vec4( -dx, 0, dx,      0); //  D  E  F
	vec4 TEX4 = texCoord.xxxy + vec4( -dx, 0, dx,     dy); //  G  H  I
	vec4 TEX5 = texCoord.xxxy + vec4( -dx, 0, dx, 2.0*dy); // G5 H5 I5
	vec4 TEX6 = texCoord.xyyy + vec4(-2.0*dx,-dy, 0,  dy); // A0 D0 G0
	vec4 TEX7 = texCoord.xyyy + vec4( 2.0*dx,-dy, 0,  dy); // C4 F4 I4
	vec2 fp = fract(texCoord * texSize);

    vec3 A1 = texture(Source, TEX1.xw).rgb;
    vec3 B1 = texture(Source, TEX1.yw).rgb;
    vec3 C1 = texture(Source, TEX1.zw).rgb;

    vec3 A = texture(Source, TEX2.xw).rgb;
    vec3 B = texture(Source, TEX2.yw).rgb;
    vec3 C = texture(Source, TEX2.zw).rgb;

    vec3 D = texture(Source, TEX3.xw).rgb;
    vec3 E = texture(Source, TEX3.yw).rgb;
    vec3 F = texture(Source, TEX3.zw).rgb;

    vec3 G = texture(Source, TEX4.xw).rgb;
    vec3 H = texture(Source, TEX4.yw).rgb;
    vec3 I = texture(Source, TEX4.zw).rgb;

    vec3 G5 = texture(Source, TEX5.xw).rgb;
    vec3 H5 = texture(Source, TEX5.yw).rgb;
    vec3 I5 = texture(Source, TEX5.zw).rgb;

    vec3 A0 = texture(Source, TEX6.xy).rgb;
    vec3 D0 = texture(Source, TEX6.xz).rgb;
    vec3 G0 = texture(Source, TEX6.xw).rgb;

    vec3 C4 = texture(Source, TEX7.xy).rgb;
    vec3 F4 = texture(Source, TEX7.xz).rgb;
    vec3 I4 = texture(Source, TEX7.xw).rgb;

	vec4 b = transpose(mat4x3(B, D, H, F)) * (XBR_Y_WEIGHT * yuv[0]);
	vec4 c = transpose(mat4x3(C, A, G, I)) * (XBR_Y_WEIGHT * yuv[0]);
	vec4 e = transpose(mat4x3(E, E, E, E)) * (XBR_Y_WEIGHT * yuv[0]);
	vec4 d = b.yzwx;
	vec4 f = b.wxyz;
	vec4 g = c.zwxy;
	vec4 h = b.zwxy;
	vec4 i = c.wxyz;

	vec4 i4 = transpose(mat4x3(I4, C1, A0, G5)) * (XBR_Y_WEIGHT*yuv[0]);
	vec4 i5 = transpose(mat4x3(I5, C4, A1, G0)) * (XBR_Y_WEIGHT*yuv[0]);
	vec4 h5 = transpose(mat4x3(H5, F4, B1, D0)) * (XBR_Y_WEIGHT*yuv[0]);
	vec4 f4 = h5.yzwx;

	vec4 c1 = i4.yzwx;
	vec4 g0 = i5.wxyz;
	vec4 b1 = h5.zwxy;
	vec4 d0 = h5.wxyz;

	vec4 Ao = vec4( 1.0, -1.0, -1.0, 1.0 );
	vec4 Bo = vec4( 1.0,  1.0, -1.0,-1.0 );
	vec4 Co = vec4( 1.5,  0.5, -0.5, 0.5 );
	vec4 Ax = vec4( 1.0, -1.0, -1.0, 1.0 );
	vec4 Bx = vec4( 0.5,  2.0, -0.5,-2.0 );
	vec4 Cx = vec4( 1.0,  1.0, -0.5, 0.0 );
	vec4 Ay = vec4( 1.0, -1.0, -1.0, 1.0 );
	vec4 By = vec4( 2.0,  0.5, -2.0,-0.5 );
	vec4 Cy = vec4( 2.0,  0.0, -1.0, 0.5 );

	vec4 Az = vec4( 6.0, -2.0, -6.0, 2.0 );
	vec4 Bz = vec4( 2.0, 6.0, -2.0, -6.0 );
	vec4 Cz = vec4( 5.0, 3.0, -3.0, -1.0 );
	vec4 Aw = vec4( 2.0, -6.0, -2.0, 6.0 );
	vec4 Bw = vec4( 6.0, 2.0, -6.0,-2.0 );
	vec4 Cw = vec4( 5.0, -1.0, -3.0, 3.0 );

	fx      = (Ao*fp.y+Bo*fp.x); 
	fx_left = (Ax*fp.y+Bx*fp.x);
	fx_up   = (Ay*fp.y+By*fp.x);
	fx3_left= (Az*fp.y+Bz*fp.x);
	fx3_up  = (Aw*fp.y+Bw*fp.x);

	// It uses CORNER_C if none of the others are defined.
#ifdef CORNER_A
	interp_restriction_lv1 = and(notEqual(e, f), notEqual(e, h));
#elif defined(CORNER_B)
	interp_restriction_lv1      = ((e!=f) && (e!=h)  &&  ( !eq(f,b) && !eq(h,d) || eq(e,i) && !eq(f,i4) && !eq(h,i5) || eq(e,g) || eq(e,c) ) );
#elif defined(CORNER_D)
	interp_restriction_lv1      = ((e!=f) && (e!=h)  &&  ( !eq(f,b) && !eq(h,d) || eq(e,i) && !eq(f,i4) && !eq(h,i5) || eq(e,g) || eq(e,c) ) && (f!=f4 && f!=i || h!=h5 && h!=i || h!=g || f!=c || eq(b,c1) && eq(d,g0)));
#else
	interp_restriction_lv1 = and(and(notEqual(e, f), notEqual(e, h)),
	                             or(or(and(not(eq(f,b)), not(eq(f,c))),
	                                   and(not(eq(h,d)), not(eq(h,g)))),
	                                or(and(eq(e,i), or(and(not(eq(f,f4)), not(eq(f,i4))),
	                                                   and(not(eq(h,h5)), not(eq(h,i5))))),
	                                   or(eq(e,g), eq(e,c)))));
#endif

    interp_restriction_lv2_left = and(notEqual(e, g), notEqual(d, g));
	interp_restriction_lv2_up   = and(notEqual(e, c), notEqual(b, c));
	interp_restriction_lv3_left = and(eq2(g,g0), not(eq2(d0,g0)));
	interp_restriction_lv3_up   = and(eq2(c,c1), not(eq2(b1,c1)));

	vec4 fx45 = smoothstep(Co - delta, Co + delta, fx);
	vec4 fx30 = smoothstep(Cx - delta, Cx + delta, fx_left);
	vec4 fx60 = smoothstep(Cy - delta, Cy + delta, fx_up);
	vec4 fx15 = smoothstep(Cz - delta, Cz + delta, fx3_left);
	vec4 fx75 = smoothstep(Cw - delta, Cw + delta, fx3_up);

	edr = and(lessThan(weighted_distance( e, c, g, i, h5, f4, h, f), weighted_distance( h, d, i5, f, i4, b, e, i)), interp_restriction_lv1);
	edr_left = and(lessThanEqual((XBR_LV2_COEFFICIENT*df(f,g)), df(h,c)), interp_restriction_lv2_left);
	edr_up   = and(greaterThanEqual(df(f,g), (XBR_LV2_COEFFICIENT*df(h,c))), interp_restriction_lv2_up);
	edr3_left = interp_restriction_lv3_left;
	edr3_up = interp_restriction_lv3_up;

	nc45 = and(edr, bvec4(fx45));
	nc30 = and(edr, and(edr_left, bvec4(fx30)));
	nc60 = and(edr, and(edr_up, bvec4(fx60)));
	nc15 = and(and(edr, edr_left), and(edr3_left, bvec4(fx15)));
	nc75 = and(and(edr, edr_up), and(edr3_up, bvec4(fx75)));

    px = lessThanEqual(df(e, f), df(e, h));

    nc = bvec4(nc75.x || nc15.x || nc30.x || nc60.x || nc45.x, nc75.y || nc15.y || nc30.y || nc60.y || nc45.y, nc75.z || nc15.z || nc30.z || nc60.z || nc45.z, nc75.w || nc15.w || nc30.w || nc60.w || nc45.w);

	vec4 final45 = vec4(nc45) * fx45;
	vec4 final30 = vec4(nc30) * fx30;
	vec4 final60 = vec4(nc60) * fx60;
	vec4 final15 = vec4(nc15) * fx15;
	vec4 final75 = vec4(nc75) * fx75;

	vec4 maximo = max(max(max(final15, final75),max(final30, final60)), final45);

	     if (nc.x) {pix1 = px.x ? F : H; blend1 = maximo.x;}
	else if (nc.y) {pix1 = px.y ? B : F; blend1 = maximo.y;}
	else if (nc.z) {pix1 = px.z ? D : B; blend1 = maximo.z;}
	else if (nc.w) {pix1 = px.w ? H : D; blend1 = maximo.w;}

	     if (nc.w) {pix2 = px.w ? H : D; blend2 = maximo.w;}
	else if (nc.z) {pix2 = px.z ? D : B; blend2 = maximo.z;}
	else if (nc.y) {pix2 = px.y ? B : F; blend2 = maximo.y;}
	else if (nc.x) {pix2 = px.x ? F : H; blend2 = maximo.x;}

    res1 = mix(E, pix1, blend1);
    res2 = mix(E, pix2, blend2);
    vec3 res = mix(res1, res2, step(c_df(E, res1), c_df(E, res2)));

    res = mix(E, res, zhdSuavizado(ZHD_BORDES));
    FragColor = vec4(zhdGrade(res, ZHD_VIVO), 1.0);
}
