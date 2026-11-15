// Consumer-side joint upsample (--upsample): MV guided by depth, depth kept as a REAL
// sample. One pass, 2 SRV + 2 UAV, run on the frame's DIRECT list after the input copies
// and before tagging. See docs/design/UPSAMPLE_QUALITY.md.
//
// The two sources may have DIFFERENT sizes: MV 100 / depth 150-200 is the asymmetric mode
// (XeFG needs the tagged pair to match, so both are rebuilt to the same target here).
// MV is sampled in its own space; each MV tap takes its depth guide from the same screen
// position in depth space.

cbuffer Params : register(b0)
{
    uint DstW; uint DstH; uint MvW; uint MvH; uint DepW; uint DepH; float DepthK;
};

Texture2D<float2>   Mv    : register(t0);   // R16G16_FLOAT
Texture2D<float>    Depth : register(t1);   // R32_FLOAT
RWTexture2D<float2> OutMv    : register(u0);
RWTexture2D<float>  OutDepth : register(u1);

int2 clamp_mv(int2 p)  { return clamp(p, int2(0, 0), int2((int)MvW  - 1, (int)MvH  - 1)); }
int2 clamp_dep(int2 p) { return clamp(p, int2(0, 0), int2((int)DepW - 1, (int)DepH - 1)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) { return; }

    const float2 sc_m = float2((float)MvW / (float)DstW, (float)MvH / (float)DstH);
    const float2 sc_d = float2((float)DepW / (float)DstW, (float)DepH / (float)DstH);
    const float2 uvm  = (float2(id.xy) + 0.5) * sc_m - 0.5;
    const float2 uvd  = (float2(id.xy) + 0.5) * sc_d - 0.5;
    const int2   bm   = (int2)floor(uvm);
    const int2   bd   = (int2)floor(uvd);
    const float2 fm   = uvm - (float2)bm;
    const float2 fd   = uvd - (float2)bd;

    // Reference = the output pixel's nearest depth tap. A plain absolute difference is
    // independent of the Z convention (standard vs reversed-Z).
    const float d0 = Depth[clamp_dep((int2)round(uvd))];

    float2 mv = 0.0;
    float  wsum = 0.0;
    [unroll] for (int y = 0; y <= 1; ++y) {
        [unroll] for (int x = 0; x <= 1; ++x) {
            const int2  p  = clamp_mv(bm + int2(x, y));
            const float ws = (x ? fm.x : 1.0 - fm.x) * (y ? fm.y : 1.0 - fm.y);
            // depth at this MV tap's screen position (sources may use different scales)
            const float2 tuv = (float2(p) + 0.5) / float2((float)MvW, (float)MvH);
            const float  di  = Depth[clamp_dep(int2(tuv * float2((float)DepW, (float)DepH)))];
            const float  wd  = 1.0 / (1.0 + DepthK * abs(di - d0));
            const float  w   = ws * wd + 1e-6;
            mv += Mv[p] * w;
            wsum += w;
        }
    }
    OutMv[id.xy] = mv / max(wsum, 1e-6);

    // Depth: the winning tap is a real value, never an average, so silhouettes survive.
    float best_w = -1.0;
    float best_d = d0;
    [unroll] for (int yy = 0; yy <= 1; ++yy) {
        [unroll] for (int xx = 0; xx <= 1; ++xx) {
            const int2  p  = clamp_dep(bd + int2(xx, yy));
            const float ws = (xx ? fd.x : 1.0 - fd.x) * (yy ? fd.y : 1.0 - fd.y);
            const float wd = 1.0 / (1.0 + DepthK * abs(Depth[p] - d0));
            const float w  = ws * wd + 1e-6;
            if (w > best_w) { best_w = w; best_d = Depth[p]; }
        }
    }
    OutDepth[id.xy] = best_d;
}

