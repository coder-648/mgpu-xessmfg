// MV only: R16G16_FLOAT in/out. Separate file from depth on purpose - two texture
// declarations at t0/u0 in one unit make D3D12 reject the pipeline (E_INVALIDARG).
//
// Filter: **medoid**, not an average (2026-11-13). A bilinear/box average across a
// motion boundary writes a vector that never existed in the source frame; no upsampler
// on the consumer side can undo that. Picking the real sample closest to the 3x3 mean
// keeps a vector that actually occurs in the frame and follows the dominant side of the
// boundary; in a smooth block all nine samples are equal, so the result is identical.
// See docs/design/UPSAMPLE_QUALITY.md.

cbuffer Params : register(b0)
{
    uint SrcW; uint SrcH; uint DstW; uint DstH;
};

Texture2D<float2>   Src : register(t0);
RWTexture2D<float2> Dst : register(u0);

int2 clamp_px(int2 p) { return clamp(p, int2(0, 0), int2((int)SrcW - 1, (int)SrcH - 1)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }
    const float2 scale = float2((float)SrcW / (float)DstW, (float)SrcH / (float)DstH);
    const float2 c = (float2(id.xy) + 0.5) * scale - 0.5;
    const float2 o = scale * (1.0 / 3.0);          // 3x3 covers the downsample footprint

    float2 v[9];
    float2 mean = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int idx = (y + 1) * 3 + (x + 1);
            v[idx] = Src[clamp_px(int2(round(c + float2(x, y) * o)))];
            mean += v[idx];
        }
    }
    mean /= 9.0;

    float2 best = v[0];
    float  best_d = 1e30;
    [unroll] for (int i = 0; i < 9; ++i) {
        const float2 d = v[i] - mean;
        const float dd = dot(d, d);
        if (dd < best_d) { best_d = dd; best = v[i]; }
    }
    Dst[id.xy] = best;
}

