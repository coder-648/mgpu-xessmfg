/*
   Hyllian's 2xBR v3.8c + ReverseAA (squared) Shader - Dithering preserved

   Copyright (C) 2011/2012 Hyllian/Jararaca - sergiogdb@gmail.com

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
   more details.
*/

/*
 * ReverseAA part of the code
 *
 * Copyright (c) 2012, Christoph Feck <christoph@maxiom.de>
 * All Rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Magpie Effect v4 port based on libretro/glsl-shaders commit
 * f8e23ff880668f0f0e837a05a316534d82a7f31b:
 * xbr/shaders/xbr-hybrid/2xbr-hybrid-v5-gamma.glsl
 */

//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH * 2
//!HEIGHT INPUT_HEIGHT * 2
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

static const float3 XBR_HYBRID_Y = float3(0.299, 0.587, 0.114) * 48.0;
static const float4 XBR_HYBRID_AO = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_HYBRID_BO = float4(1.0, 1.0, -1.0, -1.0);
static const float4 XBR_HYBRID_CO = float4(1.5, 0.5, -0.5, 0.5);
static const float4 XBR_HYBRID_AX = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_HYBRID_BX = float4(0.5, 2.0, -0.5, -2.0);
static const float4 XBR_HYBRID_CX = float4(1.0, 1.0, -0.5, 0.0);
static const float4 XBR_HYBRID_AY = float4(1.0, -1.0, -1.0, 1.0);
static const float4 XBR_HYBRID_BY = float4(2.0, 0.5, -2.0, -0.5);
static const float4 XBR_HYBRID_CY = float4(2.0, 0.0, -1.0, 0.5);

float4 XbrHybridAbsDiff(float4 a, float4 b) {
	return abs(a - b);
}

float4 XbrHybridDifferent(float4 a, float4 b) {
	return float4(
		a.x != b.x ? 1.0 : 0.0,
		a.y != b.y ? 1.0 : 0.0,
		a.z != b.z ? 1.0 : 0.0,
		a.w != b.w ? 1.0 : 0.0);
}

float4 XbrHybridEqual(float4 a, float4 b, float threshold) {
	return 1.0 - step(threshold, XbrHybridAbsDiff(a, b));
}

float4 XbrHybridOr(float4 a, float4 b) {
	return saturate(a + b);
}

float4 XbrHybridWeightedDistance(
	float4 a, float4 b, float4 c, float4 d,
	float4 e, float4 f, float4 g, float4 h
) {
	return XbrHybridAbsDiff(a, b) + XbrHybridAbsDiff(a, c) +
		XbrHybridAbsDiff(d, e) + XbrHybridAbsDiff(d, f) +
		4.0 * XbrHybridAbsDiff(g, h);
}

float3 XbrHybridSampleRgb(float2 pos, float2 inputPt, int2 offset) {
	return INPUT.SampleLevel(sam, pos + float2(offset) * inputPt, 0).rgb;
}

float4 XbrHybridLuma4(float3 a, float3 b, float3 c, float3 d) {
	return float4(dot(a, XBR_HYBRID_Y), dot(b, XBR_HYBRID_Y),
		dot(c, XBR_HYBRID_Y), dot(d, XBR_HYBRID_Y));
}

float3 XbrHybridGammaIn(float3 color) {
	return pow(max(color, 0.0), 2.4);
}

float3 XbrHybridGammaOut(float3 color) {
	return pow(max(color, 0.0), 1.0 / 2.2);
}

void Pass1(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = Rmp8x8(threadId.x) + blockStart;
	const uint2 outputSize = GetOutputSize();
	if (any(gxy >= outputSize)) {
		return;
	}

	const float2 inputPt = GetInputPt();
	const float2 pos = (float2(gxy) + 0.5) * GetOutputPt();
	const float2 fp = frac((float2(gxy) + 0.5) / 2.0);
	const float4 center = INPUT.SampleLevel(sam, pos, 0);
	#ifdef MP_HDR_COMPATIBILITY
	const float sourceAlpha = center.a;
	// The upstream gamma-domain algorithm is defined for SDR. Keep HDR data
	// untouched and perform a point 2x fallback until a color-space equivalent
	// is validated.
	OUTPUT[gxy] = float4(center.rgb, sourceAlpha);
	return;
	#endif

	const float3 A1 = XbrHybridSampleRgb(pos, inputPt, int2(-1, -2));
	const float3 B1 = XbrHybridSampleRgb(pos, inputPt, int2( 0, -2));
	const float3 C1 = XbrHybridSampleRgb(pos, inputPt, int2( 1, -2));
	const float3 A  = XbrHybridSampleRgb(pos, inputPt, int2(-1, -1));
	const float3 B  = XbrHybridSampleRgb(pos, inputPt, int2( 0, -1));
	const float3 C  = XbrHybridSampleRgb(pos, inputPt, int2( 1, -1));
	const float3 D  = XbrHybridSampleRgb(pos, inputPt, int2(-1,  0));
	const float3 E  = center.rgb;
	const float3 F  = XbrHybridSampleRgb(pos, inputPt, int2( 1,  0));
	const float3 G  = XbrHybridSampleRgb(pos, inputPt, int2(-1,  1));
	const float3 H  = XbrHybridSampleRgb(pos, inputPt, int2( 0,  1));
	const float3 I  = XbrHybridSampleRgb(pos, inputPt, int2( 1,  1));
	const float3 G5 = XbrHybridSampleRgb(pos, inputPt, int2(-1,  2));
	const float3 H5 = XbrHybridSampleRgb(pos, inputPt, int2( 0,  2));
	const float3 I5 = XbrHybridSampleRgb(pos, inputPt, int2( 1,  2));
	const float3 A0 = XbrHybridSampleRgb(pos, inputPt, int2(-2, -1));
	const float3 D0 = XbrHybridSampleRgb(pos, inputPt, int2(-2,  0));
	const float3 G0 = XbrHybridSampleRgb(pos, inputPt, int2(-2,  1));
	const float3 C4 = XbrHybridSampleRgb(pos, inputPt, int2( 2, -1));
	const float3 F4 = XbrHybridSampleRgb(pos, inputPt, int2( 2,  0));
	const float3 I4 = XbrHybridSampleRgb(pos, inputPt, int2( 2,  1));

	const float4 b = XbrHybridLuma4(B, D, H, F);
	const float4 c = XbrHybridLuma4(C, A, G, I);
	const float4 d = b.yzwx;
	const float eValue = dot(E, XBR_HYBRID_Y);
	const float4 e = float4(eValue, eValue, eValue, eValue);
	const float4 f = b.wxyz;
	const float4 g = c.zwxy;
	const float4 h = b.zwxy;
	const float4 i = c.wxyz;
	const float4 i4 = XbrHybridLuma4(I4, C1, A0, G5);
	const float4 i5 = XbrHybridLuma4(I5, C4, A1, G0);
	const float4 h5 = XbrHybridLuma4(H5, F4, B1, D0);
	const float4 f4 = h5.yzwx;

	const float4 fx = XBR_HYBRID_AO * fp.y + XBR_HYBRID_BO * fp.x;
	const float4 fxL = XBR_HYBRID_AX * fp.y + XBR_HYBRID_BX * fp.x;
	const float4 fxU = XBR_HYBRID_AY * fp.y + XBR_HYBRID_BY * fp.x;

	const float4 block1 = (1.0 - XbrHybridEqual(h, h5, 15.0)) * (1.0 - XbrHybridEqual(h, i5, 15.0));
	const float4 block2 = (1.0 - XbrHybridEqual(f, f4, 15.0)) * (1.0 - XbrHybridEqual(f, i4, 15.0));
	const float4 block3 = (1.0 - XbrHybridEqual(h, d, 15.0)) * (1.0 - XbrHybridEqual(h, g, 15.0));
	const float4 block4 = (1.0 - XbrHybridEqual(f, b, 15.0)) * (1.0 - XbrHybridEqual(f, c, 15.0));
	const float4 block5 = XbrHybridEqual(e, i, 15.0) * XbrHybridOr(block2, block1);
	const float4 blockComp = XbrHybridOr(block4, XbrHybridOr(block3,
		XbrHybridOr(block5, XbrHybridOr(XbrHybridEqual(e, g, 15.0), XbrHybridEqual(e, c, 15.0)))));

	const float4 restriction1 = XbrHybridDifferent(e, f) * XbrHybridDifferent(e, h) * blockComp;
	const float4 restrictionL = XbrHybridDifferent(e, g) * XbrHybridDifferent(d, g);
	const float4 restrictionU = XbrHybridDifferent(e, c) * XbrHybridDifferent(b, c);
	const float4 fx45 = smoothstep(XBR_HYBRID_CO - 0.5, XBR_HYBRID_CO + 0.5, fx);
	const float4 fx30 = smoothstep(XBR_HYBRID_CX - 0.5, XBR_HYBRID_CX + 0.5, fxL);
	const float4 fx60 = smoothstep(XBR_HYBRID_CY - 0.5, XBR_HYBRID_CY + 0.5, fxU);

	const float4 wd1 = XbrHybridWeightedDistance(e, c, g, i, h5, f4, h, f);
	const float4 wd2 = XbrHybridWeightedDistance(h, d, i5, f, i4, b, e, i);
	const float4 edr = (1.0 - step(wd2, wd1 + 3.5)) * restriction1;
	const float4 edrL = step(2.0 * XbrHybridAbsDiff(f, g), XbrHybridAbsDiff(h, c)) * restrictionL;
	const float4 edrU = step(2.0 * XbrHybridAbsDiff(h, c), XbrHybridAbsDiff(f, g)) * restrictionU;
	const float4 nc45 = step(0.5, edr * fx45);
	const float4 nc30 = step(0.5, edr * edrL * fx30);
	const float4 nc60 = step(0.5, edr * edrU * fx60);
	const float4 px = step(XbrHybridAbsDiff(e, f), XbrHybridAbsDiff(e, h));

	float3 s = E;
	float3 aa = B - B1;
	float3 bb = s - B;
	float3 cc = H - s;
	float3 dd = H5 - H;
	float3 t = (7.0 * (bb + cc) - 3.0 * (aa + dd)) / 16.0;
	float3 m;
	m.x = s.x < 0.5 ? 2.0 * s.x : 2.0 * (1.0 - s.x);
	m.y = s.y < 0.5 ? 2.0 * s.y : 2.0 * (1.0 - s.y);
	m.z = s.z < 0.5 ? 2.0 * s.z : 2.0 * (1.0 - s.z);
	m = min(m, 0.65 * abs(bb));
	m = min(m, 0.65 * abs(cc));
	t = clamp(t, -m, m);
	const float3 s1 = (2.0 * fp.y - 1.0) * t + s;

	s = s1;
	aa = D - D0;
	bb = s - D;
	cc = F - s;
	dd = F4 - F;
	t = (7.0 * (bb + cc) - 3.0 * (aa + dd)) / 16.0;
	m.x = s.x < 0.5 ? 2.0 * s.x : 2.0 * (1.0 - s.x);
	m.y = s.y < 0.5 ? 2.0 * s.y : 2.0 * (1.0 - s.y);
	m.z = s.z < 0.5 ? 2.0 * s.z : 2.0 * (1.0 - s.z);
	m = min(m, 0.65 * abs(bb));
	m = min(m, 0.65 * abs(cc));
	t = clamp(t, -m, m);
	const float3 reverseAa = (2.0 * fp.x - 1.0) * t + s;

	const float4 nc = XbrHybridOr(nc30, XbrHybridOr(nc60, nc45));
	const float4 r1 = lerp(e, f, edr);
	const bool useReverseAa = all(XbrHybridEqual(r1, e, 5.0) > 0.5);
	float3 result = useReverseAa ? reverseAa : E;
	float3 pixel = result;
	const float4 yes = useReverseAa
		? XbrHybridEqual(e, lerp(f, h, px), 2.0)
		: float4(0.0, 0.0, 0.0, 0.0);
	const float4 blend = max(max(nc30 * fx30, nc60 * fx60), nc45 * fx45);
	float blendAmount = 0.0;

	if (nc.x > 0.5) {
		pixel = px.x > 0.5 ? F : H;
		blendAmount = blend.x;
		result = yes.x > 0.5 ? reverseAa : E;
	} else if (nc.y > 0.5) {
		pixel = px.y > 0.5 ? B : F;
		blendAmount = blend.y;
		result = yes.y > 0.5 ? reverseAa : E;
	} else if (nc.z > 0.5) {
		pixel = px.z > 0.5 ? D : B;
		blendAmount = blend.z;
		result = yes.z > 0.5 ? reverseAa : E;
	} else if (nc.w > 0.5) {
		pixel = px.w > 0.5 ? H : D;
		blendAmount = blend.w;
		result = yes.w > 0.5 ? reverseAa : E;
	}

	result = lerp(XbrHybridGammaIn(result), XbrHybridGammaIn(pixel), blendAmount);
	OUTPUT[gxy] = float4(saturate(XbrHybridGammaOut(result)), 1.0);
}
