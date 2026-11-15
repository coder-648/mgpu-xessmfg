// ============================================================================
//  consumer module: dump_of.cpp
//
//  Dump flush, FidelityFX optical-flow setup/record/selftest.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"
static bool dump_write_stream(const DumpStream& s, const char* path)
{
    if (!s.rb) { return false; }
    void* mapped = nullptr;
    if (FAILED(s.rb->Map(0, nullptr, &mapped))) { return false; }
    FILE* f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        const uint32_t hdr[4] = { static_cast<uint32_t>(s.desc.Width), s.desc.Height,
                                  static_cast<uint32_t>(s.desc.Format),
                                  static_cast<uint32_t>(s.fp.Footprint.RowPitch) };
        fwrite(hdr, sizeof(hdr), 1, f);
        const uint8_t* p = static_cast<const uint8_t*>(mapped) + s.fp.Offset;
        ok = true;
        for (UINT y = 0; y < s.rows; ++y) {
            if (fwrite(p + static_cast<size_t>(y) * s.fp.Footprint.RowPitch, 1,
                       static_cast<size_t>(s.row_size), f) != static_cast<size_t>(s.row_size)) {
                ok = false;
                break;
            }
        }
        fclose(f);
    }
    s.rb->Unmap(0, nullptr);
    return ok;
}

// Writes everything out once the last requested frame has been recorded. Called after
// Present, so the fence already covers every copy that was recorded.
static void dump_flush()
{
    if (g_dump_frames <= 0 || g_dump_flushed || g_dump_index < g_dump_frames) { return; }
    g_dump_flushed = true;

    if (g_frame_fence) {
        wait_fence_value(g_frame_fence.Get(), g_frame_fence_value, 5000);
    }

    for (size_t i = 0; i < g_dump_entries.size(); ++i) {
        DumpEntry& e = g_dump_entries[i];
        if (!e.recorded) { continue; }
        char path[MAX_PATH];
        const int n = static_cast<int>(i);
        snprintf(path, sizeof(path), "%s\\f%02d_colour.raw", g_dump_dir, n);
        const bool okc = e.have[0] && dump_write_stream(e.s[0], path);
        snprintf(path, sizeof(path), "%s\\f%02d_depth.raw", g_dump_dir, n);
        const bool okd = e.have[1] && dump_write_stream(e.s[1], path);
        snprintf(path, sizeof(path), "%s\\f%02d_mv.raw", g_dump_dir, n);
        const bool okm = e.have[2] && dump_write_stream(e.s[2], path);

        snprintf(path, sizeof(path), "%s\\f%02d_meta.txt", g_dump_dir, n);
        if (FILE* f = fopen(path, "w")) {
            fprintf(f, "index %d\n", n);
            fprintf(f, "bridge_frame %llu\n", static_cast<unsigned long long>(e.bridge_frame));
            fprintf(f, "present_id %llu\n", static_cast<unsigned long long>(e.present_id));
            fprintf(f, "tick_ms %llu\n", static_cast<unsigned long long>(e.tick_ms));
            fprintf(f, "slot %u\n", e.slot);
            fprintf(f, "producer_frame_id %llu\n",
                    static_cast<unsigned long long>(e.producer_frame_id));
            fprintf(f, "jitter %.6f %.6f\n", e.jitter_x, e.jitter_y);
            fprintf(f, "render %u %u\n", e.render_w, e.render_h);
            fprintf(f, "mv_scale %.8f %.8f\n", e.mv_scale_x, e.mv_scale_y);
            fprintf(f, "stream_mv %u %u\n", e.stream_mv_w, e.stream_mv_h);
            fprintf(f, "stream_depth %u %u\n", e.stream_depth_w, e.stream_depth_h);
            fprintf(f, "present colour=%d depth=%d mv=%d\n", e.have[0] ? 1 : 0,
                    e.have[1] ? 1 : 0, e.have[2] ? 1 : 0);
            fprintf(f, "colour %ux%u fmt %u\n", static_cast<unsigned>(e.s[0].desc.Width),
                    e.s[0].desc.Height, static_cast<unsigned>(e.s[0].desc.Format));
            fprintf(f, "depth %ux%u fmt %u\n", static_cast<unsigned>(e.s[1].desc.Width),
                    e.s[1].desc.Height, static_cast<unsigned>(e.s[1].desc.Format));
            fprintf(f, "mv %ux%u fmt %u\n", static_cast<unsigned>(e.s[2].desc.Width),
                    e.s[2].desc.Height, static_cast<unsigned>(e.s[2].desc.Format));
            fprintf(f, "flags no_mv %u no_depth %u inputs_on %u fov %.3f proj_rz %u\n",
                    e.no_mv, e.no_depth, e.inputs_on, e.fov, e.proj_rz);
            fclose(f);
        }
        logf_("[consumer] dump %d: colour=%d depth=%d mv=%d slot=%u frame_id=%llu -> %s\n",
              n, okc ? 1 : 0, okd ? 1 : 0, okm ? 1 : 0, e.slot,
              static_cast<unsigned long long>(e.producer_frame_id), g_dump_dir);
    }
    g_dump_entries.clear();
    logf_("[consumer] dump complete (%d frames) - analyse with scripts/analyse_dump.sh\n",
          g_dump_frames);
}

// ===================== --fg-of: local optical flow (FidelityFX) ==============
// The producer's motion vectors measured as garbage on PBZ (docs/findings/HANDOFF.md 8.6),
// so the bridge can compute its own from the colour it already has on the AMD
// card: no cross-adapter transfer at all, and the vectors are same-frame.
//
// Cost at 3840x2160 measured 1.2-1.5 ms per frame (temp/ffx_of_test.cpp), i.e.
// cheaper than moving the 29.5-66 MB MV/depth streams across the bus.
//
// FidelityFX writes a coarse R16G16_SINT field at ceil(size/8) in *pixels*; the
// compute pass below bilinearly upscales it to the size of the depth stream and
// converts to the R16G16_FLOAT screen-UV field XeSS-FG expects. XeFG refuses a
// frame whose MV and depth sizes differ ("ERR_INVALID_ARGUMENT"), which is why the
// field cannot be tagged at its native 1/8 size. The first 6 dispatches return
// zeros on purpose (FFX treats FrameIndex() <= 5 as a scene cut), so real MVs only
// start after ~200 ms.
// --- everything from here to the matching #endif is the EXPERIMENTAL flow pass ---
// Compiled out by default (see MGPU_FG_OF next to the globals): MV is a dead input
// for libxess_fg 1.3.1, so this path is kept as a reference/re-test rig, not as a
// shipping feature.
static void run_frame_ex(ID3D11Texture2D* colour11);
// Timing of the flow dispatch. Declared outside the gate because the periodic report
// prints them unconditionally (they simply stay 0.0 when --fg-of is compiled out).
static double g_ms_of = 0.0;
static double g_max_of = 0.0;

#if MGPU_FG_OF
static const char* k_of_hlsl = R"HLSL(
Texture2D<int2>   src   : register(t0);   // FidelityFX field, pixels, ofW x ofH
Texture2D<float4> guide : register(t1);   // the captured colour (scRGB)
Texture2D<float>  dguide: register(t3);   // the depth we tag (same size as dst)
Texture2D<float4> prevc : register(t2);   // the previous presented colour (per-pixel refinement)
RWTexture2D<float2> dst : register(u0);   // tagged MV, screen UV
cbuffer P : register(b0) {
    float2 invSize;   // 1/captureSize
    float2 ofSize;    // the SINT field's own size
    float  signScale; // --fg-of-sign * --fg-of-scale, for convention A/B
    float  guideK;    // edge sensitivity: 0 = plain bilinear
    float  deadzone;  // px: below this the vector is treated as "no motion"
    float  maxLen;    // px: clamp, a bad match can otherwise smear the screen
    float2 constXY;   // px: --fg-of-const override (does XeFG consume our MV at all?)
    float  constOn;
    float  depthK;    // relative-depth weight for the upsample (0 = off)
};

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    uint gtw = 0u, gth = 0u;
    guide.GetDimensions(gtw, gth);
    uint tw = 0u, th = 0u;
    dst.GetDimensions(tw, th);

    float2 uv = (float2(id.xy) + 0.5) / float2(tw, th);
    float2 hp = uv * float2(gtw, gth) - 0.5;
    int2   hi = clamp((int2)floor(hp), int2(0, 0), int2(gtw, gth) - 1);
    const float l0 = luma(guide.Load(int3(hi, 0)).rgb);

    float2 g = uv * ofSize - 0.5;
    int2   i0 = (int2)floor(g);
    float2 f  = g - float2(i0);

    float2 acc = 0.0;
    float  wsum = 0.0;
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = clamp(i0 + int2(dx, dy), int2(0, 0), int2(ofSize) - 1);
            // Same-surface weight: a neighbouring vector only contributes when its
            // colour matches this pixel, which is what stops the field from bleeding
            // across object edges (plain bilinear smears moving objects into the
            // background - that was the extra ghosting).
            float2 tuv = (float2(c) + 0.5) / ofSize;
            int2   hc  = clamp((int2)floor(tuv * float2(gtw, gth)), int2(0, 0),
                               int2(gtw, gth) - 1);
            float  l   = luma(guide.Load(int3(hc, 0)).rgb);
            float  wl  = 1.0 / (1.0 + abs(l - l0) * guideK);
            // Depth is the better surface id: it separates objects from the background
            // even where the colours are similar, and XeFG consumes it anyway. Use the
            // *relative* difference so reverse-Z (UE5) and standard depth both work.
            if (depthK > 0.0) {
                float dc = dguide.Load(int3(clamp((int2)(uv * float2(tw, th)),
                                                 int2(0, 0), int2(tw, th) - 1), 0));
                float dt = dguide.Load(int3(clamp((int2)floor(tuv * float2(tw, th)),
                                                 int2(0, 0), int2(tw, th) - 1), 0));
                float rel = abs(dc - dt) / max(dc, 1e-4);
                wl *= 1.0 / (1.0 + rel * depthK);
            }
            float2 d   = float2(dx ? f.x : 1.0 - f.x, dy ? f.y : 1.0 - f.y);
            float  w   = wl * d.x * d.y;
            acc  += float2(src.Load(int3(c, 0))) * w;
            wsum += w;
        }
    }
    float2 v = acc / max(wsum, 1e-4);

    // ---- per-pixel refinement (the idea behind OptiScaler's camera-motion pass, but
    // supervised by the motion we actually measured): the upsampled vector is only a
    // seed. Take the seed and the four neighbouring OF texels, warp the *previous*
    // colour by each and keep the one that reproduces this pixel's colour best. At an
    // object boundary that selects the surface's own vector instead of the block
    // average, which is what left the narrow halo.
    {
        const float3 c0 = guide.Load(int3(clamp((int2)id.xy, int2(0, 0), int2(tw, th) - 1), 0)).rgb;
        float2 best = v;
        float  bestErr = 1e9;
        [unroll] for (int k = 0; k < 5; ++k) {
            float2 cand = v;
            if (k > 0) {
                int2  step = int2(0, 0);
                if (k == 1) { step = int2(-1, 0); }
                else if (k == 2) { step = int2(1, 0); }
                else if (k == 3) { step = int2(0, -1); }
                else { step = int2(0, 1); }
                int2 c = clamp(i0 + step, int2(0, 0), int2(ofSize) - 1);
                cand = float2(src.Load(int3(c, 0)));
            }
            // the content at this pixel was at (uv - cand/ofSize) one frame ago
            float2 puv = uv - cand / ofSize;
            int2   pp  = clamp((int2)(puv * float2(tw, th)), int2(0, 0), int2(tw, th) - 1);
            float3 cp  = prevc.Load(int3(pp, 0)).rgb;
            float  e   = abs(cp.x - c0.x) + abs(cp.y - c0.y) + abs(cp.z - c0.z);
            if (e < bestErr) { bestErr = e; best = cand; }
        }
        v = best;
    }
    if (constOn > 0.5) {
        v = constXY;                       // diagnostic: a fixed vector everywhere
    }
    float  len = length(v);
    if (len < deadzone) {
        v = float2(0.0, 0.0);
    } else {
        v *= min(1.0, maxLen / max(len, 1e-5));
    }
    dst[id.xy] = v * invSize * signScale;
}
)HLSL";

static FfxOpticalflowContext            g_of_ctx{};
static bool                             g_of_ready = false;
static UINT                             g_of_w = 0, g_of_h = 0;      // SINT field size
static UINT                             g_of_out_w = 0, g_of_out_h = 0; // tagged MV size
static std::vector<unsigned char>       g_of_scratch;
static ComPtr<ID3D12Resource>           g_of_sint;      // FFX output, ceil(size/8)
static ComPtr<ID3D12Resource>           g_of_scd;       // scene-change output, 3x1
static ComPtr<ID3D12Resource>           g_of_mv;        // R16G16_FLOAT UV, tagged
static ComPtr<ID3D12DescriptorHeap>     g_of_heap;      // [0] = SRV sint, [1] = UAV mv
static ComPtr<ID3D12RootSignature>      g_of_root;
static ComPtr<ID3D12PipelineState>      g_of_pso;

// Pixel-field -> UV-field convert pass (the only shader --fg-of adds).
static bool of_build_pso(UINT w, UINT h)
{
    ComPtr<ID3DBlob> cs;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(k_of_hlsl, strlen(k_of_hlsl), "of.hlsl", nullptr, nullptr,
                            "cs_main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) {
        logf_("[consumer] --fg-of: D3DCompile FAILED: %s\n",
              err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                 // t0 = field, t1 = current colour
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;                 // t2 = previous colour (hist 0 or 1)
    ranges[1].BaseShaderRegister = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;                 // t3 = depth guide (one ring slot)
    ranges[2].BaseShaderRegister = 3;
    ranges[2].OffsetInDescriptorsFromTableStart = 4;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors = 1;                 // u0 = the MV we tag
    ranges[3].BaseShaderRegister = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = 4 + mgpu::kMaxSlots;

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &ranges[3];
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.Num32BitValues = 12;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { logf_("[consumer] --fg-of: serialize root FAILED\n"); return false; }
    hr = g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&g_of_root));
    if (FAILED(hr)) { logf_("[consumer] --fg-of: CreateRootSignature FAILED\n"); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_of_root.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_of_pso)))) {
        logf_("[consumer] --fg-of: CreateComputePipelineState FAILED\n");
        return false;
    }
    (void)w; (void)h;
    return true;
}

static bool of_init();

// One attempt only: a failure here is permanent (wrong device, missing scratch),
// and retrying every frame would flood the log.
static bool of_init_once();

static bool of_init()
{
    static bool tried = false;
    if (g_of_ready) { return true; }
    if (tried) { return false; }
    tried = true;
    return of_init_once();
}

static bool of_init_once()
{
    const UINT w = g_full_w, h = g_full_h;
    if (w < 256 || h < 256) { return false; }

    g_of_scratch.assign(ffxGetScratchMemorySizeDX12(1), 0);
    FfxInterface ifc{};
    const FfxErrorCode ifc_rc = ffxGetInterfaceDX12(&ifc, ffxGetDeviceDX12(g_device.Get()),
                                                    g_of_scratch.data(), g_of_scratch.size(), 1);
    if (ifc_rc != FFX_OK) {
        // rc 1 = invalid pointer, 2 = insufficient memory, 5 = backend API error
        unsigned int head = 0;
        if (g_of_scratch.size() >= 4) { memcpy(&head, g_of_scratch.data(), 4); }
        logf_("[consumer] --fg-of: ffxGetInterfaceDX12 FAILED rc=%d"
              " (device=%p scratch=%p size=%zu need=%zu head=%08x align=%zu)\n",
              static_cast<int>(ifc_rc), static_cast<void*>(g_device.Get()),
              static_cast<void*>(g_of_scratch.data()), g_of_scratch.size(),
              ffxGetScratchMemorySizeDX12(1), head,
              static_cast<size_t>(reinterpret_cast<uintptr_t>(g_of_scratch.data()) & 63u));
        return false;
    }

    FfxOpticalflowContextDescription cd{};
    cd.backendInterface = ifc;
    cd.flags = 0;
    cd.resolution = { w, h };
    if (ffxOpticalflowContextCreate(&g_of_ctx, &cd) != FFX_OK) {
        logf_("[consumer] --fg-of: ffxOpticalflowContextCreate FAILED\n");
        return false;
    }

    FfxOpticalflowSharedResourceDescriptions shared{};
    if (ffxOpticalflowGetSharedResourceDescriptions(&g_of_ctx, &shared) != FFX_OK) {
        logf_("[consumer] --fg-of: shared descriptions FAILED\n");
        return false;
    }
    g_of_w = shared.opticalFlowVector.resourceDescription.width;
    g_of_h = shared.opticalFlowVector.resourceDescription.height;

    // XeFG requires the MV and depth resources to have the *same* size (mismatched
    // sizes are rejected with ERR_INVALID_ARGUMENT and nothing is interpolated), so
    // the field is upscaled to whatever the depth stream is.
    g_of_out_w = (g_ctl && g_ctl->streams[2].width >= 256) ? g_ctl->streams[2].width : w;
    g_of_out_h = (g_ctl && g_ctl->streams[2].height >= 256) ? g_ctl->streams[2].height : h;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](ComPtr<ID3D12Resource>& r, DXGI_FORMAT fmt, UINT tw, UINT th,
                    D3D12_RESOURCE_STATES st, D3D12_RESOURCE_FLAGS fl) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = fl;
        return g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr,
                                                 IID_PPV_ARGS(&r));
    };

    if (FAILED(make(g_of_sint, DXGI_FORMAT_R16G16_SINT, g_of_w, g_of_h,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
        FAILED(make(g_of_scd, DXGI_FORMAT_R32_UINT, 3, 1,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
        FAILED(make(g_of_mv, DXGI_FORMAT_R16G16_FLOAT, g_of_out_w, g_of_out_h,
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
        logf_("[consumer] --fg-of: output textures FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 + mgpu::kMaxSlots + 1;   // [0]field [1]cur [2..3]hist [4..7]depth [8]uav
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_of_heap)))) {
        logf_("[consumer] --fg-of: descriptor heap FAILED\n");
        return false;
    }
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16G16_SINT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_of_sint.Get(), &sd, cpu);
    // descriptor 1: the captured colour, used as the guide for the edge-aware upsample
    D3D12_SHADER_RESOURCE_VIEW_DESC gd{};
    gd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    gd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gd.Texture2D.MipLevels = 1;
    cpu.ptr += dstride;
    g_device->CreateShaderResourceView(g_shared12.Get(), &gd, cpu);
    // [2],[3] = the two history colours, [4..7] = the depth ring: all bound lazily, once
    cpu.ptr += static_cast<SIZE_T>(2 + mgpu::kMaxSlots) * dstride;
    // descriptor 6: the MV we tag
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_of_mv.Get(), nullptr, &ud, cpu);

    if (!of_build_pso(w, h)) { return false; }
    g_of_ready = true;
    logf_("[consumer] --fg-of: FidelityFX optical flow at %ux%u, field %ux%u -> tagged at %ux%u"
          " (must match the depth stream)\n", w, h, g_of_w, g_of_h, g_of_out_w, g_of_out_h);
    return true;
}

// Binds the two history colours (t2 selects one of them per frame). Called once.
static void of_bind_hist()
{
    static bool done = false;
    if (done || !g_of_heap || !g_hist[0] || !g_hist[1]) { return; }
    done = true;
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(2) * dstride;
    for (int i = 0; i < 2; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_hist[i].Get(), &sd, cpu);
        cpu.ptr += dstride;
    }
}

// Binds depth ring entry i as the OF upsample's depth guide (t2). Called once per ring
// texture, so no descriptor is rewritten while a frame that reads it may be in flight.
static void of_bind_depth(UINT i, ID3D12Resource* res)
{
    if (!g_of_heap || !res || i >= mgpu::kMaxSlots) { return; }
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_of_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(2 + i) * dstride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;      // the stream is R32_TYPELESS
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(res, &sd, cpu);
}

// Bring-up check for --fg-of-selftest: init + two dispatches and the convert pass,
// on a dummy 4K colour, so a broken barrier/PSO shows up without starting the game.
static bool of_record(ID3D12GraphicsCommandList* list);
static bool of_selftest()
{
    g_full_w = 3840;
    g_full_h = 2160;
    g_fg_of = true;
    if (!of_init()) { return false; }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_full_w; d.Height = g_full_h;
    d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_shared12)))) {
        logf_("[consumer] --fg-of-selftest: dummy colour FAILED\n");
        return false;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&list))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        logf_("[consumer] --fg-of-selftest: command objects FAILED\n");
        return false;
    }

    UINT64 fv = 0;
    for (int i = 0; i < 3; ++i) {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        if (!of_record(list.Get())) {
            logf_("[consumer] --fg-of-selftest: of_record FAILED on frame %d\n", i);
            return false;
        }
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(fence.Get(), ++fv);
        wait_fence_value(fence.Get(), fv, 5000);
    }
    logf_("[consumer] --fg-of-selftest: 3 dispatches + convert OK\n");
    return true;
}

// Runs the optical flow for the current colour (already in g_shared12) and leaves the
// UV field in g_of_mv. The colour must be in COMMON and the caller's list must be open.
static bool of_record(ID3D12GraphicsCommandList* list)
{
    if (!(g_of_active || g_of_track) || !g_of_ready) { return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // colour: COMMON -> compute read
    b.Transition.pResource = g_shared12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);

    FfxOpticalflowDispatchDescription dd{};
    dd.commandList = ffxGetCommandListDX12(list);
    dd.color = ffxGetResourceDX12(g_shared12.Get(), ffxGetResourceDescriptionDX12(g_shared12.Get()),
                                  L"OFColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    dd.opticalFlowVector = ffxGetResourceDX12(
        g_of_sint.Get(), ffxGetResourceDescriptionDX12(g_of_sint.Get(), FFX_RESOURCE_USAGE_UAV),
        L"OFVector", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.opticalFlowSCD = ffxGetResourceDX12(
        g_of_scd.Get(), ffxGetResourceDescriptionDX12(g_of_scd.Get(), FFX_RESOURCE_USAGE_UAV),
        L"OFSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.reset = false;
    // The captured colour is scRGB (linear FP16, 1.0 = 80 nits).
    dd.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    dd.minMaxLuminance = { 0.0f, 1000.0f };
    const FfxErrorCode drc = ffxOpticalflowContextDispatch(&g_of_ctx, &dd);
    if (drc != FFX_OK) {
        if (g_max_of >= 0.0) {
            g_max_of = -1.0;      // log it once
            logf_("[consumer] --fg-of: ffxOpticalflowContextDispatch FAILED rc=%d\n",
                  static_cast<int>(drc));
        }
        return false;
    }

    // colour back to COMMON (the convert draw re-transitions it), SINT -> SRV, mv -> UAV
    D3D12_RESOURCE_BARRIER back[3]{};
    for (int i = 0; i < 3; ++i) {
        back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        back[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    back[0].Transition.pResource = g_shared12.Get();
    back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    back[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    back[1].Transition.pResource = g_of_sint.Get();
    back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    back[2].Transition.pResource = g_of_mv.Get();
    back[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    back[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(3, back);

    // pixels -> UV
    list->SetDescriptorHeaps(1, g_of_heap.GetAddressOf());
    list->SetComputeRootSignature(g_of_root.Get());
    list->SetPipelineState(g_of_pso.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_of_heap->GetGPUDescriptorHandleForHeapStart();
    const UINT dstride = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // [2]/[3] hold hist0/hist1; the previous frame's colour is whichever is not being
    // written this frame.
    of_bind_hist();
    list->SetComputeRootDescriptorTable(0, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_prev = gpu;
    gpu_prev.ptr += static_cast<UINT64>(2 + (1u - (g_hist_write & 1u))) * dstride;
    list->SetComputeRootDescriptorTable(1, gpu_prev);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_depth = gpu;
    gpu_depth.ptr += static_cast<UINT64>(4 + g_of_depth_desc) * dstride;
    list->SetComputeRootDescriptorTable(2, gpu_depth);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += static_cast<UINT64>(4 + mgpu::kMaxSlots) * dstride;
    list->SetComputeRootDescriptorTable(3, gpu_uav);
    // Motion-vector units: the field is in pixels of the captured frame, so
    //   px  -> (1, 1)              pixels, what XeFG expects without the NDC flag
    //   uv  -> (1/w, 1/h)          screen UV
    //   ndc -> (2/w, -2/h)         NDC, y flipped because NDC counts up
    // Units XeSS-FG wants, taken from OptiScaler's camera-motion shader (CM_Common.h):
    //
    //     Output[pixel] = -velocity.xy * size;   // game MV: UV(cur-prev) -> px(prev-cur)
    //     Output[pixel] = (previousUv - uv) * size;   // size = the VELOCITY texture
    //
    // i.e. pixels *of the MV texture's own resolution* and the sign of
    // (previous - current). Our field is pixels of the captured frame with the opposite
    // sign, hence the scale factor and the negation below (mode 3 = "mvpx").
    float ux = 1.0f, uy = 1.0f;
    if (g_of_units == 3) {
        ux = -static_cast<float>(g_of_out_w) / static_cast<float>(g_full_w);
        uy = -static_cast<float>(g_of_out_h) / static_cast<float>(g_full_h);
    } else if (g_of_units == 1) {
        ux = 1.0f / static_cast<float>(g_full_w);
        uy = 1.0f / static_cast<float>(g_full_h);
    } else if (g_of_units == 2) {
        ux = 2.0f / static_cast<float>(g_full_w);
        uy = -2.0f / static_cast<float>(g_full_h);
    }
    const float konst[12] = { ux,
                              uy,
                              static_cast<float>(g_of_w),
                              static_cast<float>(g_of_h),
                              g_of_sign * g_of_scale,
                              g_of_guide,
                              g_of_deadzone,
                              g_of_maxlen,
                              g_of_const_x,
                              g_of_const_y,
                              g_of_const_on ? 1.0f : 0.0f,
                              (g_of_depth_bound[g_of_depth_desc] ? g_of_depth_k : 0.0f) };
    list->SetComputeRoot32BitConstants(4, 12, konst, 0);
    list->Dispatch((g_of_out_w + 7) / 8, (g_of_out_h + 7) / 8, 1);

    // and put them back: SINT -> UAV (FFX expects that next frame), mv -> COMMON (what
    // the XeFG tagging below declares as its incoming state).
    D3D12_RESOURCE_BARRIER after[2]{};
    for (int i = 0; i < 2; ++i) {
        after[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    after[0].Transition.pResource = g_of_sint.Get();
    after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    after[1].Transition.pResource = g_of_mv.Get();
    after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(2, after);
    return true;
}
#endif  // MGPU_FG_OF

