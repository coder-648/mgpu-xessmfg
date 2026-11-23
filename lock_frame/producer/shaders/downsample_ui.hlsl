// Producer-side UI downsample (feeds XeFG mode 5; mode 4 with --no-ui-mode5).
//
// One 2x2-footprint box filter in premultiplied-alpha space. The transport target is
// either the source format (R10/RGBA8) or R10G10B10A2_UNORM for FP16 scRGB sources.
// Mode 1 also performs scRGB -> BT.2020 PQ so the consumer's unblend and XeFG see the
// same colour space as the converted backbuffer.
//
// UI colour is low frequency; alpha/text edges are the high-frequency part. Keeping the
// filter in premultiplied space avoids dark fringes on transparent edges.

cbuffer Params : register(b0)
{
    uint  SrcW;
    uint  SrcH;
    uint  DstW;
    uint  DstH;
    uint  Mode;           // 0 = pass-through UI, 1 = UI scRGB -> PQ,
                          // 2 = opaque HUDless pass-through, 3 = opaque HUDless scRGB -> PQ
                          // 4 = UI 1:1 (full-res copy/premultiply), 5 = UI 1:1 + scRGB -> PQ
    uint  Premultiplied;  // 1 = source UI.rgb is already premultiplied
};

Texture2D<float4>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

int2 clamp_src(int2 p)
{
    return clamp(p, int2(0, 0), int2((int)SrcW - 1, (int)SrcH - 1));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    // P1: 1:1 UI path (full resolution, no downsample). A 2x2 box at 1:1 would blur text
    // and 1 px lines, which is exactly what this path exists to avoid.
    if (Mode >= 4) {
        const float4 v = Src.Load(int3(int2(id.xy), 0));
        const float  a = v.a;
        const float3 straight = (Premultiplied != 0) ? v.rgb / max(a, 1e-5) : v.rgb;
        float3 out_rgb = straight;
        if (Mode == 5) {
            const float3x3 k709to2020 = float3x3(
                0.6274, 0.3293, 0.0433,
                0.0691, 0.9195, 0.0114,
                0.0164, 0.0880, 0.8956);
            out_rgb = pq_encode(mul(k709to2020, out_rgb * 80.0));
        }
        Dst[id.xy] = saturate(float4(out_rgb * a, a));
        return;
    }

    // v32: clean HUDless stream. Mode 6 = 1:1 opaque HUDless + scRGB->BT.2020 PQ;
    // mode 7 = 1:1 R10 pass-through.
    if (Mode >= 6) {
        const float3 v = Src.Load(int3(int2(id.xy), 0)).rgb;
        float3 rgb = v;
        if (Mode == 6) {
            const float3x3 k709to2020 = float3x3(
                0.6274, 0.3293, 0.0433,
                0.0691, 0.9195, 0.0114,
                0.0164, 0.0880, 0.8956);
            rgb = pq_encode(mul(k709to2020, rgb * 80.0f));
        }
        Dst[id.xy] = float4(saturate(rgb), 1.0f);
        return;
    }

    const float2 scale = float2((float)SrcW / (float)DstW, (float)SrcH / (float)DstH);
    const float2 c = (float2(id.xy) + 0.5) * scale - 0.5;
    const int2   b = (int2)floor(c);

    const bool opaque = (Mode >= 2);          // HUDless: scene colour, alpha is meaningless
    const bool pq     = (Mode == 1) || (Mode == 3);

    float3 acc = 0.0;
    float  asum = 0.0;
    [unroll] for (int y = 0; y <= 1; ++y) {
        [unroll] for (int x = 0; x <= 1; ++x) {
            float4 v = Src.Load(int3(clamp_src(b + int2(x, y)), 0));
            if (opaque) {
                acc += v.rgb;
                asum += 1.0;
            } else {
                // Average straight colour weighted by alpha. Premultiplying before the
                // average would darken transparent edges; the output is premultiplied again
                // after the optional PQ encode.
                const float a = v.a;
                const float3 straight = (Premultiplied != 0) ? v.rgb / max(a, 1e-5) : v.rgb;
                acc += straight * a;
                asum += a;
            }
        }
    }
    const float alpha = opaque ? 1.0 : saturate(asum * 0.25);
    float3 straight = opaque ? (acc * 0.25) : (acc / max(asum, 1e-5));

    if (pq) {
        // scRGB: 1.0 = 80 nits. The consumer's convert pass encodes the WGC scRGB
        // present as BT.2020 PQ; apply the same BT.709 -> BT.2020 matrix here. Without
        // it, HUDless and present differ in saturated colours across the whole scene,
        // the difference mask becomes all-ones, and the "UI" dump is just the present.
        const float3x3 k709to2020 = float3x3(
            0.6274, 0.3293, 0.0433,
            0.0691, 0.9195, 0.0114,
            0.0164, 0.0880, 0.8956);
        straight = pq_encode(mul(k709to2020, straight * 80.0));
    }

    Dst[id.xy] = saturate(float4(straight * alpha, alpha));
}
