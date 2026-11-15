/*
   Hyllian's xBR-lv2-noblend Shader

   Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com

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

   Magpie Effect v4 port based on libretro/glsl-shaders commit
   f8e23ff880668f0f0e837a05a316534d82a7f31b:
   xbr/shaders/xbr-lv2-noblend.glsl
*/

//!MAGPIE EFFECT
//!VERSION 4

//!PARAMETER
//!LABEL Lv2 Coefficient
//!DEFAULT 2
//!MIN 1
//!MAX 3
//!STEP 0.1
float paramXbrLv2Coefficient;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH * 3
//!HEIGHT INPUT_HEIGHT * 3
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
//!ADDRESS CLAMP
SamplerState sam;

//!PASS 1
//!IN INPUT
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 64

static const float3 XBR_NOBLEND_Y = float3(0.2126, 0.7152, 0.0722);
static const float4 XBR_NOBLEND_AO = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_NOBLEND_BO = float4(1.0, 1.0, -1.0, -1.0);
static const float4 XBR_NOBLEND_CO = float4(1.5, 0.5, -0.5, 0.5);
static const float4 XBR_NOBLEND_AX = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_NOBLEND_BX = float4(0.5, 2.0, -0.5, -2.0);
static const float4 XBR_NOBLEND_CX = float4(1.0, 1.0, -0.5, 0.0);
static const float4 XBR_NOBLEND_AY = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_NOBLEND_BY = float4(2.0, 0.5, -2.0, -0.5);
static const float4 XBR_NOBLEND_CY = float4(2.0, 0.0, -1.0, 0.5);

float4 XbrNoBlendAbsDiff(float4 a, float4 b) {
	return abs(a - b);
}

float4 XbrNoBlendDifferent(float4 a, float4 b) {
	return float4(
		a.x != b.x ? 1.0 : 0.0,
		a.y != b.y ? 1.0 : 0.0,
		a.z != b.z ? 1.0 : 0.0,
		a.w != b.w ? 1.0 : 0.0);
}

float4 XbrNoBlendWeightedDistance(
	float4 a, float4 b, float4 c, float4 d,
	float4 e, float4 f, float4 g, float4 h
) {
	return XbrNoBlendAbsDiff(a, b) + XbrNoBlendAbsDiff(a, c) +
		XbrNoBlendAbsDiff(d, e) + XbrNoBlendAbsDiff(d, f) +
		4.0 * XbrNoBlendAbsDiff(g, h);
}

float3 XbrNoBlendSampleRgb(float2 pos, float2 inputPt, int2 offset) {
	return INPUT.SampleLevel(sam, pos + float2(offset) * inputPt, 0).rgb;
}

float4 XbrNoBlendLuma4(float3 a, float3 b, float3 c, float3 d) {
	return float4(dot(a, XBR_NOBLEND_Y), dot(b, XBR_NOBLEND_Y),
		dot(c, XBR_NOBLEND_Y), dot(d, XBR_NOBLEND_Y));
}

void Pass1(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = Rmp8x8(threadId.x) + blockStart;
	const uint2 outputSize = GetOutputSize();
	if (any(gxy >= outputSize)) {
		return;
	}

	const float2 inputPt = GetInputPt();
	const float2 pos = (float2(gxy) + 0.5) * GetOutputPt();
	const float2 fp = frac((float2(gxy) + 0.5) / 3.0);
	const float4 center = INPUT.SampleLevel(sam, pos, 0);
	#ifdef MP_HDR_COMPATIBILITY
	const float sourceAlpha = center.a;
	#endif

	const float3 A1 = XbrNoBlendSampleRgb(pos, inputPt, int2(-1, -2));
	const float3 B1 = XbrNoBlendSampleRgb(pos, inputPt, int2( 0, -2));
	const float3 C1 = XbrNoBlendSampleRgb(pos, inputPt, int2( 1, -2));
	const float3 A  = XbrNoBlendSampleRgb(pos, inputPt, int2(-1, -1));
	const float3 B  = XbrNoBlendSampleRgb(pos, inputPt, int2( 0, -1));
	const float3 C  = XbrNoBlendSampleRgb(pos, inputPt, int2( 1, -1));
	const float3 D  = XbrNoBlendSampleRgb(pos, inputPt, int2(-1,  0));
	const float3 E  = center.rgb;
	const float3 F  = XbrNoBlendSampleRgb(pos, inputPt, int2( 1,  0));
	const float3 G  = XbrNoBlendSampleRgb(pos, inputPt, int2(-1,  1));
	const float3 H  = XbrNoBlendSampleRgb(pos, inputPt, int2( 0,  1));
	const float3 I  = XbrNoBlendSampleRgb(pos, inputPt, int2( 1,  1));
	const float3 G5 = XbrNoBlendSampleRgb(pos, inputPt, int2(-1,  2));
	const float3 H5 = XbrNoBlendSampleRgb(pos, inputPt, int2( 0,  2));
	const float3 I5 = XbrNoBlendSampleRgb(pos, inputPt, int2( 1,  2));
	const float3 A0 = XbrNoBlendSampleRgb(pos, inputPt, int2(-2, -1));
	const float3 D0 = XbrNoBlendSampleRgb(pos, inputPt, int2(-2,  0));
	const float3 G0 = XbrNoBlendSampleRgb(pos, inputPt, int2(-2,  1));
	const float3 C4 = XbrNoBlendSampleRgb(pos, inputPt, int2( 2, -1));
	const float3 F4 = XbrNoBlendSampleRgb(pos, inputPt, int2( 2,  0));
	const float3 I4 = XbrNoBlendSampleRgb(pos, inputPt, int2( 2,  1));

	const float4 b = XbrNoBlendLuma4(B, D, H, F);
	const float4 c = XbrNoBlendLuma4(C, A, G, I);
	const float4 d = b.yzwx;
	const float eValue = dot(E, XBR_NOBLEND_Y);
	const float4 e = float4(eValue, eValue, eValue, eValue);
	const float4 f = b.wxyz;
	const float4 g = c.zwxy;
	const float4 h = b.zwxy;
	const float4 i = c.wxyz;
	const float4 i4 = XbrNoBlendLuma4(I4, C1, A0, G5);
	const float4 i5 = XbrNoBlendLuma4(I5, C4, A1, G0);
	const float4 h5 = XbrNoBlendLuma4(H5, F4, B1, D0);
	const float4 f4 = h5.yzwx;

	const float4 fx = XBR_NOBLEND_AO * fp.y + XBR_NOBLEND_BO * fp.x;
	const float4 fxL = XBR_NOBLEND_AX * fp.y + XBR_NOBLEND_BX * fp.x;
	const float4 fxU = XBR_NOBLEND_AY * fp.y + XBR_NOBLEND_BY * fp.x;
	const float4 irlv1 = XbrNoBlendDifferent(e, f) * XbrNoBlendDifferent(e, h);
	const float4 irlv2L = XbrNoBlendDifferent(e, g) * XbrNoBlendDifferent(d, g);
	const float4 irlv2U = XbrNoBlendDifferent(e, c) * XbrNoBlendDifferent(b, c);

	const float4 wd1 = XbrNoBlendWeightedDistance(e, c, g, i, h5, f4, h, f);
	const float4 wd2 = XbrNoBlendWeightedDistance(h, d, i5, f, i4, b, e, i);
	const float4 edr = step(wd1 + 0.1, wd2) * step(0.5, irlv1);
	const float4 edrL = step(paramXbrLv2Coefficient * XbrNoBlendAbsDiff(f, g),
		XbrNoBlendAbsDiff(h, c)) * irlv2L * edr;
	const float4 edrU = step(paramXbrLv2Coefficient * XbrNoBlendAbsDiff(h, c),
		XbrNoBlendAbsDiff(f, g)) * irlv2U * edr;

	const float4 line45 = float4(
		fx.x > XBR_NOBLEND_CO.x ? 1.0 : 0.0,
		fx.y > XBR_NOBLEND_CO.y ? 1.0 : 0.0,
		fx.z > XBR_NOBLEND_CO.z ? 1.0 : 0.0,
		fx.w > XBR_NOBLEND_CO.w ? 1.0 : 0.0);
	const float4 line30 = float4(
		fxL.x > XBR_NOBLEND_CX.x ? 1.0 : 0.0,
		fxL.y > XBR_NOBLEND_CX.y ? 1.0 : 0.0,
		fxL.z > XBR_NOBLEND_CX.z ? 1.0 : 0.0,
		fxL.w > XBR_NOBLEND_CX.w ? 1.0 : 0.0);
	const float4 line60 = float4(
		fxU.x > XBR_NOBLEND_CY.x ? 1.0 : 0.0,
		fxU.y > XBR_NOBLEND_CY.y ? 1.0 : 0.0,
		fxU.z > XBR_NOBLEND_CY.z ? 1.0 : 0.0,
		fxU.w > XBR_NOBLEND_CY.w ? 1.0 : 0.0);
	const float4 nc = step(0.5, edr * (line45 + edrL * line30) + edrU * line60);
	const float4 px = step(XbrNoBlendAbsDiff(e, f), XbrNoBlendAbsDiff(e, h));

	const float3 res1 = nc.x > 0.5 ? (px.x > 0.5 ? F : H) :
		nc.y > 0.5 ? (px.y > 0.5 ? B : F) :
		nc.z > 0.5 ? (px.z > 0.5 ? D : B) : E;
	const float3 res2 = nc.w > 0.5 ? (px.w > 0.5 ? H : D) :
		nc.z > 0.5 ? (px.z > 0.5 ? D : B) :
		nc.y > 0.5 ? (px.y > 0.5 ? B : F) : E;
	const float2 distances = float2(dot(res1, XBR_NOBLEND_Y), dot(res2, XBR_NOBLEND_Y)) - e.xy;
	const float3 result = lerp(res1, res2, step(abs(distances.x), abs(distances.y)));

	OUTPUT[gxy] = float4(result, MP_HDR_ALPHA);
}
