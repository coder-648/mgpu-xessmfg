// ============================================================================
//  bridge module: 050_shaders.inc
//
//  Convert/resample/prepare shader sources and their PSO creation.
//
//  Included by src/bridge/bridge.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

static const char* k_conv_hlsl = R"HLSL(
Texture2D<float4>  src : register(t0);
SamplerState       smp : register(s0);
// down/support/taps/dstW/dstH drive the resampling kernel (see --down). down 0 keeps
// the original single bilinear tap, so the old behaviour is one flag away.
cbuffer Params : register(b0) {
    float sdrWhiteNits;   // scRGB 1.0 = 80 nits
    float mode;           // 0 = HDR10 + BT.2020, 1 = HDR10 plain, 2 = SDR
    float sharpen;        // unsharp amount, applied AFTER the resize
    float down;           // 0 bilinear, 1 Catmull-Rom, 2 Mitchell, 3 Lanczos-3
    float support;        // kernel support, in output-pixel units (0 = bilinear)
    float taps;           // taps per axis that cover 2*support at this scale
    float dstW;           // render target size (the kernel needs the ratio)
    float dstH;
    float pad0; float pad1; float pad2; float pad3;
};

float3 pq_encode(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// ---------------------------------------------------------------- kernels
// All three are evaluated on the *output* grid and stretched by the scale factor,
// which is what turns them into proper low-pass filters instead of a point tap.
float k_catmull(float x)            // support 2, sharp, tiny ringing
{
    x = abs(x);
    if (x < 1.0) { return (1.5 * x - 2.5) * x * x + 1.0; }
    if (x < 2.0) { return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0; }
    return 0.0;
}

float k_mitchell(float x)           // support 2, B = C = 1/3, no ringing
{
    const float B = 1.0 / 3.0, C = 1.0 / 3.0;
    x = abs(x);
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x +
                (-18.0 + 12.0 * B + 6.0 * C) * x * x + (6.0 - 2.0 * B)) / 6.0;
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x +
                (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

float k_lanczos3(float x)           // support 3, sharpest, visible ringing
{
    x = abs(x);
    if (x < 1e-5) { return 1.0; }
    if (x >= 3.0) { return 0.0; }
    const float px = 3.14159265 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

float kw(float x)
{
    if (down < 1.5) { return k_catmull(x); }
    if (down < 2.5) { return k_mitchell(x); }
    return k_lanczos3(x);
}

// ---------------------------------------------------------------- resample
// Separable in maths, done in one pass here: the tap count is small (7 at 0.75x,
// 9 at 0.5x) and this only runs once per *real* frame, so the extra fetches are
// cheap next to the frame-generation work that follows.
float3 downsample(VSOut i, uint tw, uint th)
{
    float2 scl = float2(float(tw) / dstW, float(th) / dstH);   // > 1 when shrinking
    float2 c   = (i.pos.xy + 0.5) * scl - 0.5;
    float2 sup = support * scl;                                // support, in input texels
    int2   lo  = int2(floor(c - sup));
    int    n   = int(taps + 0.5);
    int    mw  = int(tw) - 1;
    int    mh  = int(th) - 1;

    float3 acc = 0.0;
    float  wsum = 0.0;
    [loop] for (int y = 0; y < n; ++y) {
        int   iy = lo.y + y;
        float wy = kw((float(iy) - c.y) / scl.y);
        if (wy == 0.0) { continue; }
        // Load() instead of SampleLevel(): the taps sit on texel centres, so this is
        // exact and it skips the bilinear filter that would blur them together.
        int cy = clamp(iy, 0, mh);
        [loop] for (int x = 0; x < n; ++x) {
            int   ix = lo.x + x;
            float wx = kw((float(ix) - c.x) / scl.x);
            float w  = wx * wy;
            if (w == 0.0) { continue; }
            acc  += max(src.Load(int3(clamp(ix, 0, mw), cy, 0)).rgb, 0.0) * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-4);
}

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOut i) : SV_Target
{
    uint tw = 1u, th = 1u;
    src.GetDimensions(tw, th);
    float2 texel = 1.0 / float2(tw, th);

    // scRGB, 1.0 = 80 nits. Both paths filter in linear light, which is what scRGB
    // already is - resampling after a gamma encode would darken every edge.
    float3 lin = (down < 0.5) ? max(src.SampleLevel(smp, i.uv, 0).rgb, 0.0)
                              : downsample(i, tw, th);

    // Unsharp mask: mainly useful on the bilinear path (it restores what the single
    // tap throws away). With a real kernel it mostly amplifies ringing, so keep it off.
    if (sharpen > 0.0) {
        float3 blur = 0.25 * (src.SampleLevel(smp, i.uv + float2( texel.x, 0.0), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(-texel.x, 0.0), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(0.0,  texel.y), 0).rgb +
                              src.SampleLevel(smp, i.uv + float2(0.0, -texel.y), 0).rgb);
        lin = max(lin + (lin - blur) * sharpen, 0.0);
    }

    // mode 3: the source is ALREADY display-encoded (HDR10 PQ straight from the game's
    // back buffer, via the add-on's colour stream) - pass it through. Re-encoding it
    // with pq_encode below would apply the transfer curve twice and wash the picture out.
    if (mode > 2.5) { return float4(lin, 1.0); }

    // mode 2: SDR output - plain gamma encode of the linear value.
    if (mode > 1.5) {
        float3 c = saturate(lin);
        float3 s = (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
        return float4(s, 1.0);
    }

    float3 nits = lin * sdrWhiteNits;
    float3 v = nits;
    // mode 0: BT.709 -> BT.2020 primaries (rows sum to 1), then PQ.
    // mode 1: same without the primaries conversion, for comparison.
    if (mode < 0.5) {
        const float3x3 k709to2020 = float3x3(
            0.6274, 0.3293, 0.0433,
            0.0691, 0.9195, 0.0114,
            0.0164, 0.0880, 0.8956);
        v = mul(k709to2020, nits);
    }
    return float4(pq_encode(v), 1.0);
}

// ---- dumb producer: the game GPU only copied raw data; the resampling/format work
// happens HERE, on the card that has the spare time (TRANSFER_V2 section 11).
// pad0/pad1 carry the source size, which is also where the copy's content ends.
float4 ps_prep_mv(VSOut i) : SV_Target
{
    // Bilinear: inside a surface the motion field is smooth, and the disocclusion logic
    // in the frame generator - not a nearest tap - is what handles silhouettes.
    float2 mv = src.SampleLevel(smp, i.uv, 0).rg;
    return float4(mv, 0.0, 1.0);
}

float4 ps_prep_depth(VSOut i) : SV_Target
{
    // Nearest: interpolating depth across a silhouette invents geometry.
    int2 xy = int2(i.uv * float2(pad0, pad1));
    float d = src.Load(int3(xy, 0)).x;
    return float4(d, 0.0, 0.0, 1.0);
}
)HLSL";

static DXGI_FORMAT colour_sink_format();   // defined with create_colour_sink()

static bool init_convert()
{
    ComPtr<ID3DBlob> vs, ps, err;

    HRESULT hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
        logf_("[bridge] VS compile FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) {
        logf_("[bridge] PS compile FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 12;        // white nits, mode, sharpen, down,
                                                   // support, taps, dstW, dstH
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, serr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr);
    if (FAILED(hr)) { logf_("[bridge] SerializeRootSignature FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_conv_root));
    if (FAILED(hr)) { logf_("[bridge] CreateRootSignature FAILED\n"); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_conv_root.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    pso.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_conv_pso));
    if (FAILED(hr)) { logf_("[bridge] CreateGraphicsPipelineState FAILED\n"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_heap)))) { return false; }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = colour_sink_format();   // must equal the resource format (D3D12 rule)
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_shared12.Get(), &srv,
                                       g_srv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 6;      // 4 back buffers + dummy depth + dummy MV
    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap)))) { return false; }
    g_rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    logf_("[bridge] HDR10 conversion pipeline ready (scRGB -> BT.2020 PQ)\n");
    return true;
}

// ---------------------------------------------------------------- prepare pass
// Consumer-side half of the dumb producer: the shared slot arrives at the source's own
// resolution/format and is resampled/requantised here. Same root signature as the convert
// pass (1 SRV table + 12 root constants), different PS and render-target format.
static void make_srv(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE where, DXGI_FORMAT fmt);
static ComPtr<ID3D12PipelineState> g_prep_pso[2];      // 0 = mv (RG16F), 1 = depth (R32F)
static ComPtr<ID3D12DescriptorHeap> g_prep_srv_heap;
static ComPtr<ID3D12DescriptorHeap> g_prep_rtv_heap;
static UINT g_prep_desc = 0;
static bool g_prep_ready = false;
static bool g_prep_failed = false;

static bool init_prepare()
{
    if (g_prep_ready) { return true; }
    if (g_prep_failed || !g_conv_root) { return false; }

    const char* entry[2] = { "ps_prep_mv", "ps_prep_depth" };
    const DXGI_FORMAT fmt[2] = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT };
    ComPtr<ID3DBlob> vs, err;
    HRESULT hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
        g_prep_failed = true;
        logf_("[bridge] prepare: VS compile FAILED: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> ps;
        hr = D3DCompile(k_conv_hlsl, strlen(k_conv_hlsl), "conv.hlsl", nullptr, nullptr,
                        entry[k], "ps_5_0", 0, 0, &ps, &err);
        if (FAILED(hr)) {
            g_prep_failed = true;
            logf_("[bridge] prepare: %hs compile FAILED: %s\n", entry[k],
                  err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = g_conv_root.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = 0xFFFFFFFFu;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = fmt[k];
        pso.SampleDesc.Count = 1;
        hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_prep_pso[k]));
        if (FAILED(hr)) {
            g_prep_failed = true;
            logf_("[bridge] prepare: PSO %d FAILED hr=0x%08lX\n", k, (unsigned long)hr);
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 4;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_prep_srv_heap)))) {
        g_prep_failed = true;
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 4;
    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_prep_rtv_heap)))) {
        g_prep_failed = true;
        return false;
    }
    g_prep_desc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_prep_ready = true;
    logf_("[bridge] prepare pass ready (dumb producer: resample/convert on this GPU)\n");
    return true;
}

// raw -> local, one triangle. kind 0 = motion vectors (bilinear), 1 = depth (nearest).
static bool prepare_stream(ID3D12Resource* raw, UINT sw, UINT sh,
                           ID3D12Resource* dst, UINT dw, UINT dh, int kind)
{
    if (!raw || !dst || !init_prepare()) { return false; }
    ID3D12GraphicsCommandList* list = g_list.Get();
    if (!list || sw == 0 || sh == 0 || dw == 0 || dh == 0) { return false; }

    D3D12_CPU_DESCRIPTOR_HANDLE srv = g_prep_srv_heap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(kind) * g_prep_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_prep_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kind) * g_prep_desc;

    // Views are recreated every call: the ring textures are re-created on resize, and the
    // shared source changes whenever the producer re-places a slot.
    const DXGI_FORMAT src_fmt = raw->GetDesc().Format;
    DXGI_FORMAT view = src_fmt;
    if (kind == 1) {
        // The depth slot is R32_TYPELESS today (and would be R32G8X24_TYPELESS with
        // rawdepth). Both sample the depth plane through their typeless format.
        if (src_fmt == DXGI_FORMAT_R32G8X24_TYPELESS)      { view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; }
        else if (src_fmt == DXGI_FORMAT_R32_TYPELESS)      { view = DXGI_FORMAT_R32_FLOAT; }
        else if (src_fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) { view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; }
    }
    make_srv(raw, srv, view);
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = (kind == 0) ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(dst, &rd, rtv);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = raw;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh), 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(dw), static_cast<LONG>(dh) };
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap* heaps[] = { g_prep_srv_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_conv_root.Get());
    list->SetPipelineState(g_prep_pso[kind].Get());
    list->SetGraphicsRootDescriptorTable(0, g_prep_srv_heap->GetGPUDescriptorHandleForHeapStart());
    const float k[12] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          static_cast<float>(dw), static_cast<float>(dh),
                          static_cast<float>(sw), static_cast<float>(sh), 0.0f, 0.0f };
    list->SetGraphicsRoot32BitConstants(1, 12, k, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    b.Transition.pResource = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = raw;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    return true;
}

