/*
   Hyllian's xBR-lv2 Shader

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
   xbr/shaders/xbr-lv2.glsl
*/

#ifndef XBR_SCALE
#error XBR_SCALE must be defined by the effect wrapper.
#endif

static const float3 XBR_Y = float3(0.2126, 0.7152, 0.0722);
static const float3 XBR_RGB_WEIGHT = float3(14.352, 28.176, 5.472);

static const float4 XBR_AO = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_BO = float4(1.0, 1.0, -1.0, -1.0);
static const float4 XBR_CO = float4(1.5, 0.5, -0.5, 0.5);
static const float4 XBR_AX = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_BX = float4(0.5, 2.0, -0.5, -2.0);
static const float4 XBR_CX = float4(1.0, 1.0, -0.5, 0.0);
static const float4 XBR_AY = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_BY = float4(2.0, 0.5, -2.0, -0.5);
static const float4 XBR_CY = float4(2.0, 0.0, -1.0, 0.5);
static const float4 XBR_CI = 0.25;

float4 XbrAbsDiff(float4 a, float4 b) {
	return abs(a - b);
}

float4 XbrDifferent(float4 a, float4 b) {
	return float4(
		a.x != b.x ? 1.0 : 0.0,
		a.y != b.y ? 1.0 : 0.0,
		a.z != b.z ? 1.0 : 0.0,
		a.w != b.w ? 1.0 : 0.0);
}

float4 XbrEqual(float4 a, float4 b) {
	return step(XbrAbsDiff(a, b), paramXbrEqThreshold.xxxx);
}

float4 XbrNotEqual(float4 a, float4 b) {
	return 1.0 - XbrEqual(a, b);
}

float4 XbrWeightedDistance(
	float4 a, float4 b, float4 c, float4 d,
	float4 e, float4 f, float4 g, float4 h
) {
	return XbrAbsDiff(a, b) + XbrAbsDiff(a, c) + XbrAbsDiff(d, e) +
		XbrAbsDiff(d, f) + 4.0 * XbrAbsDiff(g, h);
}

float4 XbrSmallDetailDistance(
	float4 a, float4 b, float4 c, float4 d,
	float4 e, float4 f, float4 g, float4 h,
	float4 i, float4 j, float4 k, float4 l
) {
	return XbrAbsDiff(a, b) + XbrAbsDiff(a, c) + XbrAbsDiff(d, e) +
		XbrAbsDiff(d, f) + XbrAbsDiff(i, j) + XbrAbsDiff(k, l) +
		2.0 * XbrAbsDiff(g, h);
}

float XbrColorDiff(float3 a, float3 b) {
	const float3 d = abs(a - b);
	return d.r + d.g + d.b;
}

float3 XbrSampleRgb(float2 pos, float2 inputPt, int2 offset) {
	return INPUT.SampleLevel(sam, pos + float2(offset) * inputPt, 0).rgb;
}

void Pass1(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = Rmp8x8(threadId.x) + blockStart;
	const uint2 outputSize = GetOutputSize();
	if (any(gxy >= outputSize)) {
		return;
	}

	const float2 inputPt = GetInputPt();
	const float2 pos = (float2(gxy) + 0.5) * GetOutputPt();
	const float2 fp = frac((float2(gxy) + 0.5) / XBR_SCALE);
	const float4 center = INPUT.SampleLevel(sam, pos, 0);
	#ifdef MP_HDR_COMPATIBILITY
	const float sourceAlpha = center.a;
	#endif

	//    A1 B1 C1
	// A0  A  B  C C4
	// D0  D  E  F F4
	// G0  G  H  I I4
	//    G5 H5 I5
	const float3 A1 = XbrSampleRgb(pos, inputPt, int2(-1, -2));
	const float3 B1 = XbrSampleRgb(pos, inputPt, int2( 0, -2));
	const float3 C1 = XbrSampleRgb(pos, inputPt, int2( 1, -2));
	const float3 A  = XbrSampleRgb(pos, inputPt, int2(-1, -1));
	const float3 B  = XbrSampleRgb(pos, inputPt, int2( 0, -1));
	const float3 C  = XbrSampleRgb(pos, inputPt, int2( 1, -1));
	const float3 D  = XbrSampleRgb(pos, inputPt, int2(-1,  0));
	const float3 E  = center.rgb;
	const float3 F  = XbrSampleRgb(pos, inputPt, int2( 1,  0));
	const float3 G  = XbrSampleRgb(pos, inputPt, int2(-1,  1));
	const float3 H  = XbrSampleRgb(pos, inputPt, int2( 0,  1));
	const float3 I  = XbrSampleRgb(pos, inputPt, int2( 1,  1));
	const float3 G5 = XbrSampleRgb(pos, inputPt, int2(-1,  2));
	const float3 H5 = XbrSampleRgb(pos, inputPt, int2( 0,  2));
	const float3 I5 = XbrSampleRgb(pos, inputPt, int2( 1,  2));
	const float3 A0 = XbrSampleRgb(pos, inputPt, int2(-2, -1));
	const float3 D0 = XbrSampleRgb(pos, inputPt, int2(-2,  0));
	const float3 G0 = XbrSampleRgb(pos, inputPt, int2(-2,  1));
	const float3 C4 = XbrSampleRgb(pos, inputPt, int2( 2, -1));
	const float3 F4 = XbrSampleRgb(pos, inputPt, int2( 2,  0));
	const float3 I4 = XbrSampleRgb(pos, inputPt, int2( 2,  1));

	const float4 b = float4(dot(B, XBR_RGB_WEIGHT), dot(D, XBR_RGB_WEIGHT),
		dot(H, XBR_RGB_WEIGHT), dot(F, XBR_RGB_WEIGHT));
	const float4 c = float4(dot(C, XBR_RGB_WEIGHT), dot(A, XBR_RGB_WEIGHT),
		dot(G, XBR_RGB_WEIGHT), dot(I, XBR_RGB_WEIGHT));
	const float4 d = b.yzwx;
	const float eValue = dot(E, XBR_RGB_WEIGHT);
	const float4 e = float4(eValue, eValue, eValue, eValue);
	const float4 f = b.wxyz;
	const float4 g = c.zwxy;
	const float4 h = b.zwxy;
	const float4 i = c.wxyz;

	float4 i4;
	float4 i5;
	float4 h5;
	if (paramPreserveSmallDetails == 0) {
		i4 = float4(dot(I4, XBR_RGB_WEIGHT), dot(C1, XBR_RGB_WEIGHT),
			dot(A0, XBR_RGB_WEIGHT), dot(G5, XBR_RGB_WEIGHT));
		i5 = float4(dot(I5, XBR_RGB_WEIGHT), dot(C4, XBR_RGB_WEIGHT),
			dot(A1, XBR_RGB_WEIGHT), dot(G0, XBR_RGB_WEIGHT));
		h5 = float4(dot(H5, XBR_RGB_WEIGHT), dot(F4, XBR_RGB_WEIGHT),
			dot(B1, XBR_RGB_WEIGHT), dot(D0, XBR_RGB_WEIGHT));
	} else {
		const float3 weightedY = paramXbrYWeight * XBR_Y;
		i4 = float4(dot(I4, weightedY), dot(C1, weightedY), dot(A0, weightedY), dot(G5, weightedY));
		i5 = float4(dot(I5, weightedY), dot(C4, weightedY), dot(A1, weightedY), dot(G0, weightedY));
		h5 = float4(dot(H5, weightedY), dot(F4, weightedY), dot(B1, weightedY), dot(D0, weightedY));
	}
	const float4 f4 = h5.yzwx;

	const float4 fx = XBR_AO * fp.y + XBR_BO * fp.x;
	const float4 fxL = XBR_AX * fp.y + XBR_BX * fp.x;
	const float4 fxU = XBR_AY * fp.y + XBR_BY * fp.x;

	const float4 irlv0 = XbrDifferent(e, f) * XbrDifferent(e, h);
	const float4 irlv1 = irlv0 * (
		XbrNotEqual(f, b) * XbrNotEqual(f, c) +
		XbrNotEqual(h, d) * XbrNotEqual(h, g) +
		XbrEqual(e, i) * (XbrNotEqual(f, f4) * XbrNotEqual(f, i4) +
			XbrNotEqual(h, h5) * XbrNotEqual(h, i5)) +
		XbrEqual(e, g) + XbrEqual(e, c));
	const float4 irlv2L = XbrDifferent(e, g) * XbrDifferent(d, g);
	const float4 irlv2U = XbrDifferent(e, c) * XbrDifferent(b, c);

	const float4 delta = (1.0 / XBR_SCALE).xxxx;
	const float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) / XBR_SCALE;
	const float4 deltaU = deltaL.yxwz;
	float4 fx45i = saturate((fx + delta - XBR_CO - XBR_CI) / (2.0 * delta));
	float4 fx45 = saturate((fx + delta - XBR_CO) / (2.0 * delta));
	float4 fx30 = saturate((fxL + deltaL - XBR_CX) / (2.0 * deltaL));
	float4 fx60 = saturate((fxU + deltaU - XBR_CY) / (2.0 * deltaU));

	float4 wd1;
	float4 wd2;
	if (paramPreserveSmallDetails == 0) {
		wd1 = XbrWeightedDistance(e, c, g, i, h5, f4, h, f);
		wd2 = XbrWeightedDistance(h, d, i5, f, i4, b, e, i);
	} else {
		wd1 = XbrSmallDetailDistance(e, c, g, i, f4, h5, h, f, b, d, i4, i5);
		wd2 = XbrSmallDetailDistance(h, d, i5, f, b, i4, e, i, g, h5, c, f4);
	}

	const float4 edri = step(wd1, wd2) * irlv0;
	const float4 edr = step(wd1 + 0.1, wd2) * step(0.5, irlv1);
	const float4 edrL = step(paramXbrLv2Coefficient * XbrAbsDiff(f, g), XbrAbsDiff(h, c)) * irlv2L * edr;
	const float4 edrU = step(paramXbrLv2Coefficient * XbrAbsDiff(h, c), XbrAbsDiff(f, g)) * irlv2U * edr;

	fx45 *= edr;
	fx30 *= edrL;
	fx60 *= edrU;
	fx45i *= edri;
	const float4 px = step(XbrAbsDiff(e, f), XbrAbsDiff(e, h));
	const float4 blend = max(max(fx30, fx60), max(fx45, fx45i));

	float3 res1 = E;
	res1 = lerp(res1, lerp(H, F, px.x), blend.x);
	res1 = lerp(res1, lerp(B, D, px.z), blend.z);
	float3 res2 = E;
	res2 = lerp(res2, lerp(F, B, px.y), blend.y);
	res2 = lerp(res2, lerp(D, H, px.w), blend.w);
	const float3 result = lerp(res1, res2, step(XbrColorDiff(E, res1), XbrColorDiff(E, res2)));

	OUTPUT[gxy] = float4(result, MP_HDR_ALPHA);
}
