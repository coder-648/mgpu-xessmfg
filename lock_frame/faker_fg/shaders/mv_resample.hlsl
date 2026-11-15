// ============================================================================
//  mv_resample.hlsl - full-resolution motion vectors -> interpolation resolution.
//
//  Titles like Wuchang hand the upscaler motion vectors at the *output* size (their
//  DLSS create flags leave MVLowRes clear) while depth is at the upscaler's render
//  size. XeFG 1.3.1 requires the two resources to have the same size and its
//  HIGH_RES_MV init flag is gone, so the vectors are resampled once per frame onto a
//  render-sized texture. The values are pixels of the source size and XeFG expects
//  pixels of the destination size, so each vector is scaled by dst/src as well -
//  after this pass the input is exactly what a title that declares low-res MV sends.
//
//  Nearest sampling on purpose: the field is smooth, and it keeps the pass free of
//  sampler descriptors (SRV + UAV only).
// ============================================================================
Texture2D<float2> SrcMV : register(t0);
RWTexture2D<float2> DstMV : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint dw, dh;
    DstMV.GetDimensions(dw, dh);
    if (id.x >= dw || id.y >= dh) { return; }

    uint sw, sh;
    SrcMV.GetDimensions(sw, sh);
    if (sw == 0 || sh == 0) { return; }

    uint2 src = uint2((id.x * sw) / dw, (id.y * sh) / dh);
    float2 mv = SrcMV.Load(int3(src, 0));

    float2 scale = float2((float) dw / (float) sw, (float) dh / (float) sh);
    DstMV[id.xy] = mv * scale;
}
