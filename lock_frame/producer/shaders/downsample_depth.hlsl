// Depth only: R32_FLOAT/D32_FLOAT in, R32_FLOAT out.
//
// Filter: **median of the 3x3 footprint** (2026-11-13). Averaging depth across a
// silhouette produces a value that belongs to neither surface, which makes XeFG's
// disocclusion clamp reject the wrong side. An odd-sized median returns a real sample
// and does not need to know the Z convention (standard vs reversed-Z), unlike min/max.
// See docs/design/UPSAMPLE_QUALITY.md.

cbuffer Params : register(b0)
{
    uint SrcW; uint SrcH; uint DstW; uint DstH;
};

Texture2D<float>   Src : register(t0);
RWTexture2D<float> Dst : register(u0);

int2 clamp_px(int2 p) { return clamp(p, int2(0, 0), int2((int)SrcW - 1, (int)SrcH - 1)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }
    const float2 scale = float2((float)SrcW / (float)DstW, (float)SrcH / (float)DstH);
    const float2 c = (float2(id.xy) + 0.5) * scale - 0.5;
    const float2 o = scale * (1.0 / 3.0);

    float v[9];
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            v[(y + 1) * 3 + (x + 1)] = Src[clamp_px(int2(round(c + float2(x, y) * o)))];
        }
    }

    // insertion sort; v[4] is the median (a real sample, never an average)
    for (int i = 1; i < 9; ++i) {
        const float key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    Dst[id.xy] = v[4];
}

