// ============================================================================
//  lock_frame module: 085_ui_unblend.inc
//
//  UI unblend: consumer-side reconstruction of the HUDless base.
//
//  The colour that arrives through WGC is the presented backbuffer (UI included). The
//  producer transports one downsampled stream 3:
//    GameUi  - the title's UI-only texture (RGB + alpha);
//    Hudless - a downsampled HUDless scene; v29 synthesises an approximate UI from
//              (present - hudless) before the unblend below.
//
//  This pass rebuilds a HUDless frame:
//
//      premultiplied: H = (P - UI.rgb) / max(1 - UI.a, eps)
//      straight:      H = (P - UI.rgb * UI.a) / max(1 - UI.a, eps)
//
//  The result is tagged as XEFG_SWAPCHAIN_RES_HUDLESS_COLOR while the chain buffer keeps
//  the UI-bearing image. XeFG's BACKBUFFER_HUDLESS mode then interpolates H and extracts
//  the UI from the backbuffer. If this pass cannot run, the caller tags UI (GameUi only)
//  or leaves composition off (Hudless; there is no real UI texture to tag).
//
//  Included by lock_frame/lock_frame.cpp after 080 (make_srv/make_uav/up_transition).
// ============================================================================

static ComPtr<ID3D12RootSignature> g_unblend_rs;   // 3 SRVs (present, UI, occupancy) + 1 UAV
static ComPtr<ID3D12PipelineState>  g_unblend_pso;
static ComPtr<ID3D12PipelineState>  g_synth_pso;
static ComPtr<ID3D12DescriptorHeap> g_unblend_heap;

// A small ring, not one texture per publication slot: 32 x 4K R10 would be ~1 GB. Four
// entries cover the swap-chain depth while the tagged HUDless is still in flight.
static const UINT kUnblendRing = 4;
static ComPtr<ID3D12Resource> g_hudless_ring[kUnblendRing];
static D3D12_RESOURCE_STATES  g_hudless_state[kUnblendRing] = {
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
static UINT g_hudless_w = 0, g_hudless_h = 0;
static UINT g_hudless_ring_i = 0;
static ID3D12Resource* g_hudless_current = nullptr;   // valid for this frame only

// v29: synthesized low-resolution UI (HUDless source only). Same ring depth as the
// HUDless output; it is consumed by the unblend in the same command list.
static ComPtr<ID3D12Resource> g_synth_ring[kUnblendRing];
static D3D12_RESOURCE_STATES  g_synth_state[kUnblendRing] = {
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
static UINT g_synth_w = 0, g_synth_h = 0;
static UINT g_synth_ring_i = 0;
// The UI resource the unblend actually consumed this frame (game UI / decoded synth UI /
// legacy synth ring). --tag-dump reads this instead of assuming g_ui_local[slot].
static ID3D12Resource*        g_ui_used_current = nullptr;
static D3D12_RESOURCE_STATES  g_ui_used_state = D3D12_RESOURCE_STATE_COMMON;

// v29 visual reference dump: --ui-dump N writes the synthesized UI (RGB / alpha /
// composited over a checker) as BMP files. This is a debugging aid only; it records a
// readback copy of the low-resolution synth texture before the unblend.
static int   g_ui_dump_frames = 0;
static int   g_ui_dump_index  = 0;
static int   g_ui_dump_after  = 60;
static bool  g_ui_dump_flushed = false;
static char  g_ui_dump_dir[MAX_PATH] = "ui_dump";

struct UiDumpEntry
{
    ComPtr<ID3D12Resource> rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows = 0;
    UINT64 row_size = 0;
    D3D12_RESOURCE_DESC desc{};
};
static std::vector<UiDumpEntry> g_ui_dump_entries;
static std::vector<UiDumpEntry> g_ui_dump_hud_entries;

static bool ensure_unblend_objects()
{
    if (g_unblend_rs && g_unblend_pso && g_synth_pso && g_unblend_heap) { return true; }

    // ONE root signature for both passes. The SRV table always carries three entries
    // (t0 present, t1 UI, t2 occupancy) because the unblend shader declares all three - a
    // shader may not reference a register the root signature does not describe, or
    // CreateComputePipelineState fails. That is exactly what happened when the sparse pass
    // was bolted onto the old 2-SRV signature: the whole UI path stopped initialising
    // ("unblend pipeline/ring init failed" every frame) and nobody noticed until the
    // consumer was run with --ui-source auto. The synth shader uses only t0/t1 and the
    // plain unblend runs with Sparse=0, so t2 just carries a valid 1-word buffer there.
    auto make_rs = [](UINT srv_count, ComPtr<ID3D12RootSignature>& out) -> bool {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = srv_count;         // present, UI[, Occ]
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;                 // HUDless
        ranges[1].BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0;
        p[0].Constants.Num32BitValues = 8;            // DstW/DstH/UiW/UiH + 4 floats
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderRegister = 0;
        smp.RegisterSpace = 0;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 3;
        rs.pParameters = p;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &smp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        const HRESULT shr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                        &sig, &err);
        if (FAILED(shr) || !sig) {
            logf_("[lock] ui-unblend: root signature FAILED hr=0x%08lX\n",
                  static_cast<unsigned long>(shr));
            return false;
        }
        if (FAILED(g_device->CreateRootSignature(0, sig->GetBufferPointer(),
                                                 sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&out)))) {
            logf_("[lock] ui-unblend: CreateRootSignature FAILED\n");
            return false;
        }
        return true;
    };

    if (!g_unblend_rs && !make_rs(3, g_unblend_rs)) { return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_unblend_rs.Get();
    pd.CS.pShaderBytecode = kUnblendUICSO;
    pd.CS.BytecodeLength = sizeof(kUnblendUICSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_unblend_pso)))) {
        logf_("[lock] ui-unblend: CreateComputePipelineState FAILED\n");
        return false;
    }
    // v29: same root signature/bindings, different shader: present + HUDless -> UI.
    D3D12_COMPUTE_PIPELINE_STATE_DESC sd = pd;
    sd.CS.pShaderBytecode = kSynthUICSO;
    sd.CS.BytecodeLength = sizeof(kSynthUICSO);
    if (FAILED(g_device->CreateComputePipelineState(&sd, IID_PPV_ARGS(&g_synth_pso)))) {
        logf_("[lock] ui-unblend: synth CreateComputePipelineState FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // 0..3 = synth (present, HUDless, occupancy, UI UAV)
    // 4..7 = unblend (present, UI, occupancy, HUDless UAV)
    // Separate regions are required: descriptors are read at execution time, so reusing the
    // same slots in one command list would let a later dispatch overwrite what an earlier
    // one sees.
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_unblend_heap)))) {
        logf_("[lock] ui-unblend: descriptor heap FAILED\n");
        return false;
    }
    logf_("[lock] ui-unblend: UI -> HUDless pass ready (feeds mode 5, mode 4 with --no-ui-mode5)\n");
    return true;
}

static bool ensure_hudless_ring(UINT w, UINT h)
{
    if (w < 64 || h < 64) { return false; }
    if (g_hudless_ring[0] && g_hudless_w == w && g_hudless_h == h) { return true; }

    for (UINT i = 0; i < kUnblendRing; ++i) {
        g_hudless_ring[i].Reset();
        g_hudless_state[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    g_hudless_current = nullptr;
    g_hudless_w = g_hudless_h = 0;

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    for (UINT i = 0; i < kUnblendRing; ++i) {
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_hudless_ring[i])))) {
            logf_("[lock] ui-unblend: HUDless ring allocation FAILED (%ux%u)\n", w, h);
            for (UINT k = 0; k < kUnblendRing; ++k) { g_hudless_ring[k].Reset(); }
            return false;
        }
    }
    g_hudless_w = w;
    g_hudless_h = h;
    g_hudless_ring_i = 0;
    logf_("[lock] ui-unblend: HUDless ring %ux%u x%u (R10 PQ)\n", w, h, kUnblendRing);
    return true;
}

static bool ensure_synth_ring(UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (w < 64 || h < 64 || fmt == DXGI_FORMAT_UNKNOWN) { return false; }
    if (g_synth_ring[0] && g_synth_w == w && g_synth_h == h &&
        g_synth_ring[0]->GetDesc().Format == fmt) {
        return true;
    }

    for (UINT i = 0; i < kUnblendRing; ++i) {
        g_synth_ring[i].Reset();
        g_synth_state[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    g_synth_w = g_synth_h = 0;

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    for (UINT i = 0; i < kUnblendRing; ++i) {
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_synth_ring[i])))) {
            logf_("[lock] ui-unblend: synth UI ring allocation FAILED (%ux%u fmt %u)\n",
                  w, h, static_cast<unsigned>(fmt));
            for (UINT k = 0; k < kUnblendRing; ++k) { g_synth_ring[k].Reset(); }
            return false;
        }
    }
    g_synth_w = w;
    g_synth_h = h;
    g_synth_ring_i = 0;
    logf_("[lock] ui-unblend: synth UI ring %ux%u fmt %u x%u\n",
          w, h, static_cast<unsigned>(fmt), kUnblendRing);
    return true;
}

static bool write_bmp24(const char* path, const std::vector<uint8_t>& rgb, int w, int h)
{
    if (w <= 0 || h <= 0) { return false; }
    FILE* f = fopen(path, "wb");
    if (f == nullptr) { return false; }
    const uint32_t row = static_cast<uint32_t>((w * 3 + 3) & ~3);
    const uint32_t image = row * static_cast<uint32_t>(h);
    const uint32_t file = 54u + image;
    uint8_t hdr[54]{};
    hdr[0] = 'B'; hdr[1] = 'M';
    const uint32_t vals[3] = { file, 0, 54 };
    memcpy(hdr + 2, vals, 12);
    // BITMAPINFOHEADER: the 16-bit planes/bit-count fields must stay 2 bytes each.
    // Writing them as uint32_t shifted every later field by four bytes and produced
    // BMPs that photo viewers reported as corrupt.
    uint8_t info[40]{};
    auto put32 = [](uint8_t* p, uint32_t v) { memcpy(p, &v, 4); };
    auto put16 = [](uint8_t* p, uint16_t v) { memcpy(p, &v, 2); };
    put32(info + 0, 40u);
    put32(info + 4, static_cast<uint32_t>(w));
    put32(info + 8, static_cast<uint32_t>(h));
    put16(info + 12, 1u);
    put16(info + 14, 24u);
    put32(info + 16, 0u);          // BI_RGB
    put32(info + 20, image);
    put32(info + 24, 2835u);
    put32(info + 28, 2835u);
    put32(info + 32, 0u);
    put32(info + 36, 0u);
    memcpy(hdr + 14, info, 40);
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; ok && y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                              static_cast<size_t>(x)) * 3;
            line[static_cast<size_t>(x) * 3 + 0] = rgb[i + 2];   // BMP is BGR
            line[static_cast<size_t>(x) * 3 + 1] = rgb[i + 1];
            line[static_cast<size_t>(x) * 3 + 2] = rgb[i + 0];
        }
        ok = fwrite(line.data(), 1, row, f) == row;
    }
    fclose(f);
    return ok;
}

// v29: build an approximate UI from present (full res) and HUDless (stream 3, low res).
// Leaves `present` in RENDER_TARGET and the returned texture in NON_PIXEL_SRV, ready for
// record_ui_unblend(). Returns nullptr when the source is not Hudless or the resources
// are not ready; the caller then leaves XeFG UI composition off for this frame.
static bool ui_dump_prepare(UiDumpEntry& e, ID3D12Resource* res)
{
    if (res == nullptr) { return false; }
    e.desc = res->GetDesc();
    if (e.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { return false; }
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&e.desc, 0, 1, 0, &e.fp, &e.rows, &e.row_size, &total);
    if (total == 0 || e.rows == 0) { return false; }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    const HRESULT hr = g_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&e.rb));
    if (FAILED(hr)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf_("[lock] ui-dump: readback buffer FAILED hr=0x%08lX (%.1f MB)\n",
                  static_cast<unsigned long>(hr),
                  static_cast<double>(total) / (1024.0 * 1024.0));
        }
        return false;
    }
    return true;
}

static bool write_dump_entry_rgb(const UiDumpEntry& e, const char* path)
{
    if (!e.rb) { return false; }
    const UINT w = static_cast<UINT>(e.desc.Width);
    const UINT h = e.desc.Height;
    if (w == 0 || h == 0) { return false; }
    void* mapped = nullptr;
    if (FAILED(e.rb->Map(0, nullptr, &mapped))) { return false; }
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
    const uint8_t* base = static_cast<const uint8_t*>(mapped) + e.fp.Offset;
    const UINT pitch = e.fp.Footprint.RowPitch;
    const bool r10 = (e.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                      e.desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    const bool rgba8 = (e.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        e.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < w; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (r10) {
                const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4);
                r = static_cast<float>(v & 0x3FFu) / 1023.0f;
                g = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
                b = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
            } else if (rgba8) {
                const uint8_t* p = row + x * 4;
                r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f;
            } else {
                continue;
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            rgb[i + 0] = static_cast<uint8_t>(r * 255.0f + 0.5f);
            rgb[i + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
            rgb[i + 2] = static_cast<uint8_t>(b * 255.0f + 0.5f);
        }
    }
    e.rb->Unmap(0, nullptr);
    return write_bmp24(path, rgb, static_cast<int>(w), static_cast<int>(h));
}

// Called after the present fence is signalled. BMPs are deliberately simple (no image
// library): ui_XX_rgb.bmp shows the premultiplied synth UI, ui_XX_alpha.bmp the mask,
// ui_XX_over.bmp the UI composited over a checker so transparency is visible.
static void ui_dump_flush()
{
    if (g_ui_dump_frames <= 0 || g_ui_dump_flushed ||
        g_ui_dump_index < g_ui_dump_frames) {
        return;
    }
    g_ui_dump_flushed = true;
    if (g_frame_fence) {
        wait_fence_value(g_frame_fence.Get(), g_frame_fence_value, 5000);
    }

    for (size_t k = 0; k < g_ui_dump_entries.size(); ++k) {
        UiDumpEntry& e = g_ui_dump_entries[k];
        if (!e.rb) { continue; }
        const UINT w = static_cast<UINT>(e.desc.Width);
        const UINT h = e.desc.Height;
        if (w == 0 || h == 0) { continue; }
        void* mapped = nullptr;
        if (FAILED(e.rb->Map(0, nullptr, &mapped))) { continue; }

        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        std::vector<uint8_t> alpha(static_cast<size_t>(w) * h * 3, 0);
        std::vector<uint8_t> over(static_cast<size_t>(w) * h * 3, 0);
        const uint8_t* base = static_cast<const uint8_t*>(mapped) + e.fp.Offset;
        const UINT pitch = e.fp.Footprint.RowPitch;
        const bool r10 = (e.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                          e.desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);
        const bool rgba8 = (e.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                            e.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * pitch;
            for (UINT x = 0; x < w; ++x) {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                if (r10) {
                    const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4);
                    r = static_cast<float>(v & 0x3FFu) / 1023.0f;
                    g = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
                    b = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
                    a = static_cast<float>((v >> 30) & 0x3u) / 3.0f;
                } else if (rgba8) {
                    const uint8_t* p = row + x * 4;
                    r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f;
                    a = p[3] / 255.0f;
                } else {
                    continue;
                }
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                rgb[i + 0] = static_cast<uint8_t>(r * 255.0f + 0.5f);
                rgb[i + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
                rgb[i + 2] = static_cast<uint8_t>(b * 255.0f + 0.5f);
                const uint8_t av = static_cast<uint8_t>(a * 255.0f + 0.5f);
                alpha[i + 0] = alpha[i + 1] = alpha[i + 2] = av;
                const float bg = (((x / 32u) + (y / 32u)) & 1u) ? 0.55f : 0.25f;
                float orr = r + (1.0f - a) * bg;
                float orgg = g + (1.0f - a) * bg;
                float orb = b + (1.0f - a) * bg;
                if (orr > 1.0f) { orr = 1.0f; }
                if (orgg > 1.0f) { orgg = 1.0f; }
                if (orb > 1.0f) { orb = 1.0f; }
                over[i + 0] = static_cast<uint8_t>(orr * 255.0f + 0.5f);
                over[i + 1] = static_cast<uint8_t>(orgg * 255.0f + 0.5f);
                over[i + 2] = static_cast<uint8_t>(orb * 255.0f + 0.5f);
            }
        }
        e.rb->Unmap(0, nullptr);

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\ui_%02d_rgb.bmp", g_ui_dump_dir, static_cast<int>(k));
        const bool okr = write_bmp24(path, rgb, static_cast<int>(w), static_cast<int>(h));
        snprintf(path, sizeof(path), "%s\\ui_%02d_alpha.bmp", g_ui_dump_dir, static_cast<int>(k));
        const bool oka = write_bmp24(path, alpha, static_cast<int>(w), static_cast<int>(h));
        snprintf(path, sizeof(path), "%s\\ui_%02d_over.bmp", g_ui_dump_dir, static_cast<int>(k));
        const bool oko = write_bmp24(path, over, static_cast<int>(w), static_cast<int>(h));
        snprintf(path, sizeof(path), "%s\\ui_%02d_meta.txt", g_ui_dump_dir, static_cast<int>(k));
        if (FILE* mf = fopen(path, "w")) {
            fprintf(mf, "index %zu\nwidth %u\nheight %u\nformat %u\n",
                    k, w, h, static_cast<unsigned>(e.desc.Format));
            fclose(mf);
        }
        logf_("[lock] ui-dump %zu: %ux%u fmt %u rgb=%d alpha=%d over=%d -> %s\n",
              k, w, h, static_cast<unsigned>(e.desc.Format), okr ? 1 : 0,
              oka ? 1 : 0, oko ? 1 : 0, g_ui_dump_dir);
        if (k < g_ui_dump_hud_entries.size()) {
            char hp[MAX_PATH];
            snprintf(hp, sizeof(hp), "%s\\ui_%02d_hudless.bmp",
                     g_ui_dump_dir, static_cast<int>(k));
            const bool okh = write_dump_entry_rgb(g_ui_dump_hud_entries[k], hp);
            logf_("[lock] ui-dump %zu: hudless=%d -> %s\n", k, okh ? 1 : 0, hp);
        }
    }
    g_ui_dump_entries.clear();
    g_ui_dump_hud_entries.clear();
    logf_("[lock] ui-dump complete (%d frame(s)) -> %s\n", g_ui_dump_frames, g_ui_dump_dir);
}

// ---- sparse unblend: per-32x8-tile UI occupancy ------------------------------
// The sparse UI codec writes a 4-word header per tile and mode 3 means "empty". The
// consumer derives one flag per tile from those headers (shaders/ui_occ.hlsl) and the
// unblend then writes `present` for empty tiles instead of sampling UI and running two PQ
// pow chains. Measured UI coverage on CP2077 is 6.4-6.8%, so most tiles are empty.
//
// Deriving it here instead of transporting it keeps the protocol at v30: no new field, no
// extra PCIe traffic, and an old producer (sparse codec off) simply never writes mode 3,
// which makes this a no-op rather than a wrong answer.
static const UINT kUiOccTileWords = 264;    // kFrSlotWords in the codec (32x8 tile slot)
static ComPtr<ID3D12Resource>       g_ui_occ;
static D3D12_RESOURCE_STATES        g_ui_occ_state = D3D12_RESOURCE_STATE_COMMON;
static ComPtr<ID3D12RootSignature>  g_occ_rs;
static ComPtr<ID3D12PipelineState>  g_occ_pso;
static ComPtr<ID3D12DescriptorHeap> g_occ_heap;      // t0 codec bytes, u0 occupancy
static UINT                         g_ui_occ_tiles = 0;
static bool                         g_ui_occ_failed = false;
// --no-sparse-unblend: A/B the sparse pass off without touching the producer's ini (the
// consumer alone decides whether its unblend takes the occupancy shortcut).
static bool                         g_ui_sparse_unblend_on = true;

static bool ensure_occ_objects()
{
    if (g_occ_rs && g_occ_pso && g_occ_heap) { return true; }
    if (g_ui_occ_failed) { return false; }

    D3D12_DESCRIPTOR_RANGE rg[2]{};
    rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rg[0].NumDescriptors = 1;
    rg[0].BaseShaderRegister = 0;
    rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rg[1].NumDescriptors = 1;
    rg[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = 2;              // Tiles, SlotWords
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &rg[0];
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &rg[1];
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = p;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        !sig) {
        logf_("[lock] ui-occ: root signature FAILED\n");
        g_ui_occ_failed = true;
        return false;
    }
    if (FAILED(g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                             IID_PPV_ARGS(&g_occ_rs)))) {
        logf_("[lock] ui-occ: CreateRootSignature FAILED\n");
        g_ui_occ_failed = true;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_occ_rs.Get();
    pd.CS.pShaderBytecode = kUiOccCSO;
    pd.CS.BytecodeLength = sizeof(kUiOccCSO);
    if (FAILED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_occ_pso)))) {
        logf_("[lock] ui-occ: CreateComputePipelineState FAILED\n");
        g_ui_occ_failed = true;
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_occ_heap)))) {
        logf_("[lock] ui-occ: descriptor heap FAILED\n");
        g_ui_occ_failed = true;
        return false;
    }
    logf_("[lock] ui-occ: per-tile UI occupancy pass ready\n");
    return true;
}

static bool ensure_occ_buffer(UINT tiles)
{
    if (tiles == 0) { return false; }
    if (g_ui_occ && g_ui_occ_tiles >= tiles) { return true; }
    g_ui_occ.Reset();
    g_ui_occ_tiles = 0;
    g_ui_occ_state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(tiles) * 4ull;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_ui_occ)))) {
        logf_("[lock] ui-occ: buffer FAILED (%u tiles)\n", tiles);
        return false;
    }
    g_ui_occ_tiles = tiles;
    return true;
}

// Structured SRV for the occupancy buffer (1 element when the slot is unused, so the
// descriptor is always valid - the shader only reads it when Sparse=1).
static void make_occ_srv(D3D12_CPU_DESCRIPTOR_HANDLE where)
{
    if (!g_ui_occ) { return; }
    D3D12_SHADER_RESOURCE_VIEW_DESC os{};
    os.Format = DXGI_FORMAT_UNKNOWN;
    os.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    os.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    os.Buffer.StructureByteStride = 4;
    os.Buffer.NumElements = g_ui_occ_tiles ? g_ui_occ_tiles : 1u;
    g_device->CreateShaderResourceView(g_ui_occ.Get(), &os, where);
}

// Fills g_ui_occ from the codec slot's tile headers and leaves it in NON_PIXEL_SRV.
static bool record_ui_occ(ID3D12Resource* codec, UINT w, UINT h, UINT& tiles_out)
{
    tiles_out = 0;
    if (codec == nullptr || w < 32 || h < 8) { return false; }
    if (codec->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) { return false; }
    if (!ensure_occ_objects()) { return false; }
    const UINT tiles = ((w + 31u) / 32u) * ((h + 7u) / 8u);
    if (!ensure_occ_buffer(tiles)) { return false; }

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_occ_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_occ_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;          // raw ByteAddressBuffer
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(codec->GetDesc().Width / 4ull);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g_device->CreateShaderResourceView(codec, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;               // structured (stride 4)
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.StructureByteStride = 4;
    uav.Buffer.NumElements = tiles;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(g_ui_occ.Get(), nullptr, &uav, cpu_uav);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;

    // decode_ui_slot() leaves the codec slot in COPY_DEST; restore that on the way out.
    up_transition(codec, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(g_ui_occ.Get(), g_ui_occ_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_ui_occ_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    g_list->SetComputeRootSignature(g_occ_rs.Get());
    g_list->SetPipelineState(g_occ_pso.Get());
    ID3D12DescriptorHeap* heaps[1] = { g_occ_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    uint32_t params[2] = { tiles, kUiOccTileWords };
    g_list->SetComputeRoot32BitConstants(0, 2, params, 0);
    g_list->SetComputeRootDescriptorTable(1, gpu);
    g_list->SetComputeRootDescriptorTable(2, gpu_uav);
    g_list->Dispatch((tiles + 63u) / 64u, 1, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = g_ui_occ.Get();
    g_list->ResourceBarrier(1, &ub);
    up_transition(g_ui_occ.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_ui_occ_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    up_transition(codec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
    tiles_out = tiles;
    return true;
}

static ID3D12Resource* record_ui_synth(UINT slot, ID3D12Resource* present,
                                       UINT w, UINT h)
{
    if (!g_ui_unblend || ui_source_now() != mgpu::kUiSourceHudless) { return nullptr; }
    if (g_ctl == nullptr || present == nullptr || slot >= mgpu::kMaxSlots) { return nullptr; }
    // Local UI state lives per UI publication slot (see ui_slot_index in 060).
    const UINT uil = ui_slot_index(slot);
    if (g_ui_local[uil] == nullptr || !g_ui_fresh[uil]) { return nullptr; }
    if ((g_ctl->active_mask & mgpu::kRequestUi) == 0) { return nullptr; }

    const mgpu::StreamShared& su = g_ctl->streams[3];
    if (su.width < 64 || su.height < 64 || su.format == 0) { return nullptr; }
    const DXGI_FORMAT ui_fmt = static_cast<DXGI_FORMAT>(su.format);
    if (!ensure_unblend_objects() || !ensure_synth_ring(su.width, su.height, ui_fmt)) {
        return nullptr;
    }

    const UINT ri = g_synth_ring_i;
    g_synth_ring_i = (g_synth_ring_i + 1u) % kUnblendRing;
    ID3D12Resource* ui = g_synth_ring[ri].Get();

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The SRV table is always 3 entries (see ensure_unblend_objects), so the synth binds a
    // valid (unused) occupancy buffer at t2 and puts its UAV at 3.
    (void)ensure_occ_buffer(1);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_unblend_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_unblend_heap->GetGPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu;
    make_srv(present, cpu, DXGI_FORMAT_R10G10B10A2_UNORM);
    cpu.ptr += inc; gpu.ptr += inc;
    make_srv(g_ui_local[uil].Get(), cpu, ui_fmt);
    cpu.ptr += inc; gpu.ptr += inc;
    make_occ_srv(cpu);
    cpu.ptr += inc; gpu.ptr += inc;
    const D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu;
    make_uav(ui, cpu, ui_fmt);

    up_transition(present, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(g_ui_local[uil].Get(), D3D12_RESOURCE_STATE_COMMON,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    up_transition(ui, g_synth_state[ri], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    struct SynthConsts { UINT dst_w, dst_h, src_w, src_h; float threshold, dilate, pad0, pad1; }
        c{ su.width, su.height, w, h, g_ui_synth_threshold, 0.0f, 0.0f, 0.0f };

    g_list->SetComputeRootSignature(g_unblend_rs.Get());
    g_list->SetPipelineState(g_synth_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_unblend_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRoot32BitConstants(0, 8, &c, 0);
    g_list->SetComputeRootDescriptorTable(1, srv_gpu);
    g_list->SetComputeRootDescriptorTable(2, uav_gpu);
    g_list->Dispatch((su.width + 7) / 8, (su.height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = ui;
    g_list->ResourceBarrier(1, &ub);

    // --ui-dump: copy the synth UI to a readback buffer before it is handed to unblend.
    // The BMP is written after Present, when the frame fence covers this copy.
    bool ui_dumped = false;
    if (g_ui_dump_frames > 0 && g_ui_dump_index < g_ui_dump_frames &&
        g_frames >= static_cast<UINT64>(g_ui_dump_after)) {
        if (g_ui_dump_entries.empty()) {
            g_ui_dump_entries.resize(static_cast<size_t>(g_ui_dump_frames));
            g_ui_dump_hud_entries.resize(static_cast<size_t>(g_ui_dump_frames));
        }
        UiDumpEntry& ds = g_ui_dump_entries[static_cast<size_t>(g_ui_dump_index)];
        UiDumpEntry& hs = g_ui_dump_hud_entries[static_cast<size_t>(g_ui_dump_index)];
        if (ui_dump_prepare(ds, ui)) {
            up_transition(ui, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = ui;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = ds.rb.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = ds.fp;
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            up_transition(ui, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ui_dumped = true;
        }
        // Also dump the HUDless input (same frame) so a bad mask can be diagnosed by
        // comparing it with the present.
        if (ui_dump_prepare(hs, g_ui_local[uil].Get())) {
            up_transition(g_ui_local[uil].Get(),
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = g_ui_local[uil].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = hs.rb.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = hs.fp;
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            up_transition(g_ui_local[uil].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (ui_dumped) { ++g_ui_dump_index; }
    }
    if (!ui_dumped) {
        up_transition(ui, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    up_transition(present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    up_transition(g_ui_local[uil].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COMMON);
    g_synth_state[ri] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    static bool logged = false;
    if (!logged) {
        logged = true;
        logf_("[lock] ui-unblend: synth UI %ux%u from %ux%u present + %ux%u HUDless"
              " (threshold=%.3f)\n", su.width, su.height, w, h,
              su.width, su.height, static_cast<double>(g_ui_synth_threshold));
    }
    return ui;
}

// Records the unblend pass and returns the HUDless texture to tag for this frame, or
// nullptr when B cannot run (no UI stream, no local UI, unsupported state). On success
// `present` is left in RENDER_TARGET and the returned texture is in NON_PIXEL_SRV.
static void ui_unblend_note(const char* reason)
{
    static const char* seen[12]{};
    static int n = 0;
    for (int i = 0; i < n; ++i) {
        if (seen[i] == reason) { return; }
    }
    if (n < 12) {
        seen[n++] = reason;
        logf_("[lock] ui-unblend: inactive - %s\n", reason);
    }
}

static void ui_enable_composition_when_ready()
{
    if (!g_ui_unblend || g_fg == nullptr || !g_api.has_ui_composition()) { return; }
    if (g_ctl == nullptr) { return; }
    // The producer selects stream 3 only after the first P/H records, so at consumer
    // init `ui_source` can still be Off and the one-shot init call leaves composition
    // DISABLED. Enable it when the stream actually appears (once per session; XeFG
    // keeps the state).
    static bool applied = false;
    if (applied) { return; }
    const bool ready = g_ctl->streams[3].slot_size != 0 &&
                       g_ctl->streams[3].width >= 64 && g_ctl->streams[3].height >= 64 &&
                       g_ctl->ui_source != mgpu::kUiSourceOff;
    if (!ready) { return; }
    const xefg_swapchain_result_t r = g_api.SetUiCompositionState(
        g_fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
    logf_("[lock] ui: SetUiCompositionState(ENABLED) after the stream appeared: %s (%d)\n",
          xefg_str(r), static_cast<int>(r));
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { applied = true; }
}

static ID3D12Resource* record_ui_unblend(UINT slot, ID3D12Resource* present, UINT w, UINT h)
{
    g_hudless_current = nullptr;
    g_ui_used_current = nullptr;
    if (!g_ui_unblend) { return nullptr; }
    if (g_ctl == nullptr || present == nullptr || slot >= mgpu::kMaxSlots) {
        ui_unblend_note("no shared control / present");
        return nullptr;
    }
    // Local UI state lives per UI publication slot (see ui_slot_index in 060).
    const UINT uil = ui_slot_index(slot);
    if (g_ui_local[uil] == nullptr) {
        ui_unblend_note("no local UI copy this frame");
        return nullptr;
    }
    if (!g_ui_fresh[uil]) {
        ui_unblend_note("UI stream is not fresh for this slot");
        return nullptr;
    }
    if ((g_ctl->active_mask & mgpu::kRequestUi) == 0) {
        ui_unblend_note("producer does not publish a UI stream (active_mask)");
        return nullptr;
    }

    const uint32_t source = ui_source_now();
    if (source != mgpu::kUiSourceGame && source != mgpu::kUiSourceHudless &&
        source != mgpu::kUiSourceSynth) {
        ui_unblend_note("producer UI source is Off/unknown");
        return nullptr;
    }

    const mgpu::StreamShared& su = g_ctl->streams[3];
    if (su.width < 64 || su.height < 64 || su.format == 0) {
        ui_unblend_note("stream 3 has no valid size/format");
        return nullptr;
    }
    ui_enable_composition_when_ready();
    if (!ensure_unblend_objects() || !ensure_hudless_ring(w, h)) {
        ui_unblend_note("unblend pipeline/ring init failed");
        return nullptr;
    }

    // GameUi / Synth: stream 3 is already a UI texture (Synth = full-res UI built on the
    // render card). Hudless: stream 3 is a downsampled HUDless image; build the
    // approximate UI first, on this card, and unblend with that.
    ID3D12Resource* ui_src = g_ui_local[uil].Get();
    D3D12_RESOURCE_STATES ui_before = D3D12_RESOURCE_STATE_COMMON;
    if (source == mgpu::kUiSourceHudless) {
        ui_src = record_ui_synth(slot, present, w, h);
        if (ui_src == nullptr) {
            ui_unblend_note("synth UI pass failed");
            return nullptr;
        }
        ui_before = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    const UINT ri = g_hudless_ring_i;
    g_hudless_ring_i = (g_hudless_ring_i + 1u) % kUnblendRing;
    ID3D12Resource* hudless = g_hudless_ring[ri].Get();

    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_unblend_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_unblend_heap->GetGPUDescriptorHandleForHeapStart();

    // Sparse path: derive per-tile occupancy from the sparse codec's headers so the pass
    // can write `present` for empty tiles. Needs a codec stream (only it has headers), a
    // full-resolution UI texture (an 8x8 dest group must map 1:1 to UI texels, otherwise a
    // bilinear sample could pull a neighbour into an "empty" tile), and a stream that
    // really carries UI (the HUDless source synthesises UI on this card, so its tiles are
    // never empty).
    const UINT uislot_occ = uil;
    UINT occ_tiles = 0;
    // t2 always needs a valid descriptor, so the buffer exists even when the sparse path is
    // off (record_ui_occ grows it when the mask is actually built).
    (void)ensure_occ_buffer(1);
    const bool sparse_ok =
        g_ui_sparse_unblend_on && su.codec != 0 &&
        su.width == w && su.height == h && source != mgpu::kUiSourceHudless &&
        uislot_occ < mgpu::kMaxSlots &&
        record_ui_occ(g_ui[uislot_occ].Get(), su.width, su.height, occ_tiles);
    static bool told_sparse = false;
    if (sparse_ok && !told_sparse) {
        told_sparse = true;
        logf_("[lock] ui-unblend: sparse dispatch ON (%u tiles; an empty tile writes"
              " present directly)\n", occ_tiles);
    }

    // Unblend owns heap region 4..7 (synth owns 0..3).
    cpu.ptr += static_cast<SIZE_T>(inc) * 4;
    gpu.ptr += static_cast<UINT64>(inc) * 4;
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu;
    make_srv(present, cpu, DXGI_FORMAT_R10G10B10A2_UNORM);
    cpu.ptr += inc; gpu.ptr += inc;
    make_srv(ui_src, cpu, static_cast<DXGI_FORMAT>(su.format));
    cpu.ptr += inc; gpu.ptr += inc;
    make_occ_srv(cpu);          // t2: the per-tile UI occupancy
    cpu.ptr += inc; gpu.ptr += inc;
    const D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu;
    make_uav(hudless, cpu, DXGI_FORMAT_R10G10B10A2_UNORM);

    // Present came in as RENDER_TARGET from the convert draw. GameUi is COMMON; the
    // synthesized UI is already NON_PIXEL_SRV. HUDless is COMMON.
    up_transition(present, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (ui_before != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        up_transition(ui_src, ui_before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    up_transition(hudless, g_hudless_state[ri], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // v31/P4b: the producer writes ui_flags bit3 when the UI stream was built for linear
    // subtraction (HDR/R10 PQ). The SDR path stays in code space.
    const float linear = (g_ctl->streams[3].ui_flags & 8u) ? 1.0f : 0.0f;
    struct UnblendConsts { UINT dst_w, dst_h, ui_w, ui_h; float premul, opaque, linear;
                           UINT sparse; }
        c{ w, h, su.width, su.height, 1.0f, 0.99f, linear, sparse_ok ? 1u : 0u };

    g_list->SetComputeRootSignature(g_unblend_rs.Get());
    g_list->SetPipelineState(g_unblend_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_unblend_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRoot32BitConstants(0, 8, &c, 0);
    g_list->SetComputeRootDescriptorTable(1, srv_gpu);
    g_list->SetComputeRootDescriptorTable(2, uav_gpu);
    g_list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = hudless;
    g_list->ResourceBarrier(1, &ub);

    up_transition(present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ui_before != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        up_transition(ui_src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      ui_before);
    }
    up_transition(hudless, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_hudless_state[ri] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    g_hudless_current = hudless;
    g_ui_used_current = ui_src;
    g_ui_used_state = (source == mgpu::kUiSourceHudless)
                          ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                          : D3D12_RESOURCE_STATE_COMMON;
    if (!g_ui_unblend_ready_logged) {
        g_ui_unblend_ready_logged = true;
        logf_("[lock] ui-unblend: HUDless rebuilt from %ux%u present + %ux%u %s"
              " (source=%s)\n", w, h, su.width, su.height,
              source == mgpu::kUiSourceHudless ? "synth UI"
              : source == mgpu::kUiSourceSynth ? "full-res synth UI" : "UI",
              source == mgpu::kUiSourceHudless ? "hudless"
              : source == mgpu::kUiSourceSynth ? "render-synth" : "game");
    }
    return hudless;
}

