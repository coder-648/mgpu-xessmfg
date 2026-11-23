// ============================================================================
//  lock_frame module: 118_tag_dump.inc
//
//  Test-phase dump: at the moment the frame is about to be handed to XeFG, copy the
//  three colour resources it will consume and write them as BMPs:
//
//      tag_XX_backbuffer.bmp  the chain buffer P handed to Present (UI inside)
//      tag_XX_hudless.bmp     the rebuilt HUDless (tagged in mode 4/5)
//      tag_XX_ui.bmp          the UI texture (decoded/copied or render-card synth)
//      tag_XX_patch.bmp       v33: the HUDless support patch (when ui_flags bit4 is set)
//      tag_XX_meta.txt        present id / slot / source / mode5 / derived / linear / patch / sizes
//
//  In patch mode the third image is the *reconstruction material*: H' is present outside
//  the support and the patch inside it, so black tiles in patch.bmp are the tiles the
//  consumer copied from `present`. No alpha/UI is involved.
//
//  Visual check only; never changes tagging or composition. Use --tag-dump N
//  (--tag-dump-after M, --tag-dump-dir DIR).
// ============================================================================

static int  g_tag_dump_frames = 0;
static int  g_tag_dump_index  = 0;
static int  g_tag_dump_after  = 60;
static char g_tag_dump_dir[MAX_PATH] = "tag_dump";
static bool g_tag_dump_flushed = false;

struct TagDumpEntry
{
    DumpStream s[3];                 // 0 = backbuffer, 1 = HUDless, 2 = UI
    bool have[3] = { false, false, false };
    uint64_t present_id = 0;
    uint32_t slot = 0, mode5 = 0, derived = 0, linear = 0, patch = 0, source = 0;
    uint32_t w = 0, h = 0;
    uint32_t ui_w = 0, ui_h = 0, ui_fmt = 0, ui_quality = 0xFFFFFFFFu;
};

static std::vector<TagDumpEntry> g_tag_dump_entries;

static void tag_dump_copy(ID3D12GraphicsCommandList* list, DumpStream& s, int which,
                          ID3D12Resource* res, D3D12_RESOURCE_STATES before)
{
    if (list == nullptr || res == nullptr || !dump_prepare(s, res, which)) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = s.fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = before;
        list->ResourceBarrier(1, &b);
    }
}

static void record_tag_dump(ID3D12GraphicsCommandList* list, UINT slot,
                            uint32_t mode5,
                            ID3D12Resource* backbuffer,
                            D3D12_RESOURCE_STATES backbuffer_state,
                            ID3D12Resource* hudless,
                            D3D12_RESOURCE_STATES hudless_state,
                            ID3D12Resource* ui,
                            D3D12_RESOURCE_STATES ui_state)
{
    if (g_tag_dump_frames <= 0 || g_tag_dump_index >= g_tag_dump_frames ||
        g_tag_dump_flushed) {
        return;
    }
    if (g_frames < static_cast<uint64_t>(g_tag_dump_after)) { return; }
    if (g_ctl == nullptr) { return; }
    if (backbuffer == nullptr && hudless == nullptr && ui == nullptr) { return; }

    if (g_tag_dump_entries.empty()) {
        g_tag_dump_entries.resize(static_cast<size_t>(g_tag_dump_frames));
    }
    TagDumpEntry& e = g_tag_dump_entries[static_cast<size_t>(g_tag_dump_index)];

    tag_dump_copy(list, e.s[0], 0, backbuffer, backbuffer_state);
    tag_dump_copy(list, e.s[1], 1, hudless, hudless_state);
    tag_dump_copy(list, e.s[2], 2, ui, ui_state);
    e.have[0] = (e.s[0].rb != nullptr);
    e.have[1] = (e.s[1].rb != nullptr);
    e.have[2] = (e.s[2].rb != nullptr);

    e.present_id = g_ctl->frame_published;
    e.slot       = slot;
    e.mode5      = mode5;
    e.derived    = (g_ctl->streams[3].ui_flags & mgpu::kUiFlagDerived) ? 1u : 0u;
    e.linear     = (g_ctl->streams[3].ui_flags & mgpu::kUiFlagLinear) ? 1u : 0u;
    e.patch      = ((g_ctl->streams[3].ui_flags & mgpu::kUiFlagPatchPayload) ||
                    (g_ctl->streams[4].ui_flags & mgpu::kHudlessFlagPatchPayload)) ? 1u : 0u;
    e.source     = g_ctl->ui_source;
    if (backbuffer != nullptr) {
        const D3D12_RESOURCE_DESC bd = backbuffer->GetDesc();
        e.w = static_cast<uint32_t>(bd.Width);
        e.h = static_cast<uint32_t>(bd.Height);
    }
    e.ui_w       = g_ctl->streams[3].width;
    e.ui_h       = g_ctl->streams[3].height;
    e.ui_fmt     = g_ctl->streams[3].format;
    e.ui_quality = g_ctl->streams[3].ui_quality;

    ++g_tag_dump_index;
}

static bool write_dumpstream_bmp(const DumpStream& s, const char* path)
{
    if (!s.rb) { return false; }
    const UINT w = static_cast<UINT>(s.desc.Width);
    const UINT h = static_cast<UINT>(s.desc.Height);
    if (w == 0 || h == 0) { return false; }
    void* mapped = nullptr;
    if (FAILED(s.rb->Map(0, nullptr, &mapped)) || mapped == nullptr) { return false; }

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
    const uint8_t* base = static_cast<const uint8_t*>(mapped) + s.fp.Offset;
    const UINT pitch = s.fp.Footprint.RowPitch;
    const bool r10 = (s.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                      s.desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    const bool rgba8 = (s.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        s.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0;
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
    s.rb->Unmap(0, nullptr);
    return write_bmp24(path, rgb, static_cast<int>(w), static_cast<int>(h));
}

static bool write_dumpstream_alpha_bmp(const DumpStream& s, const char* path)
{
    if (!s.rb) { return false; }
    const UINT w = static_cast<UINT>(s.desc.Width);
    const UINT h = static_cast<UINT>(s.desc.Height);
    if (w == 0 || h == 0) { return false; }
    void* mapped = nullptr;
    if (FAILED(s.rb->Map(0, nullptr, &mapped)) || mapped == nullptr) { return false; }
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
    const uint8_t* base = static_cast<const uint8_t*>(mapped) + s.fp.Offset;
    const UINT pitch = s.fp.Footprint.RowPitch;
    const bool r10 = (s.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                      s.desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    const bool rgba8 = (s.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        s.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < w; ++x) {
            float a = 0.0f;
            if (r10) {
                const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4);
                a = static_cast<float>((v >> 30) & 3u) / 3.0f;
            } else if (rgba8) {
                a = row[x * 4 + 3] / 255.0f;
            } else {
                continue;
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            const uint8_t g = static_cast<uint8_t>(a * 255.0f + 0.5f);
            rgb[i + 0] = rgb[i + 1] = rgb[i + 2] = g;
        }
    }
    s.rb->Unmap(0, nullptr);
    return write_bmp24(path, rgb, static_cast<int>(w), static_cast<int>(h));
}

static void tag_dump_flush()
{
    if (g_tag_dump_frames <= 0 || g_tag_dump_flushed ||
        g_tag_dump_index < g_tag_dump_frames) {
        return;
    }
    g_tag_dump_flushed = true;
    if (g_frame_fence) {
        wait_fence_value(g_frame_fence.Get(), g_frame_fence_value, 5000);
    }
    // v33: the patch calibration copies its gain here every frame; after the fence it is
    // the converged value for the session. Log it once with the dump.
    if (g_patch_gain_rb) {
        void* mapped = nullptr;
        if (SUCCEEDED(g_patch_gain_rb->Map(0, nullptr, &mapped)) && mapped != nullptr) {
            const float* g = static_cast<const float*>(mapped);
            logf_("[lock] ui-patch: LUT16 R=%.3f G=%.3f B=%.3f confidence=%.3f\n",
                  static_cast<double>(g[16]), static_cast<double>(g[32 + 16]),
                  static_cast<double>(g[64 + 16]), static_cast<double>(g[96]));
            g_patch_gain_rb->Unmap(0, nullptr);
        }
    }
    for (size_t k = 0; k < g_tag_dump_entries.size(); ++k) {
        TagDumpEntry& e = g_tag_dump_entries[k];
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\tag_%02d_backbuffer.bmp",
                 g_tag_dump_dir, static_cast<int>(k));
        const bool okb = write_dumpstream_bmp(e.s[0], path);
        snprintf(path, sizeof(path), "%s\\tag_%02d_hudless.bmp",
                 g_tag_dump_dir, static_cast<int>(k));
        const bool okh = write_dumpstream_bmp(e.s[1], path);
        snprintf(path, sizeof(path), "%s\\tag_%02d_%s.bmp",
                 g_tag_dump_dir, static_cast<int>(k), e.patch ? "patch" : "ui");
        const bool oku = write_dumpstream_bmp(e.s[2], path);
        snprintf(path, sizeof(path), "%s\\tag_%02d_alpha.bmp",
                 g_tag_dump_dir, static_cast<int>(k));
        const bool oka = write_dumpstream_alpha_bmp(e.s[2], path);
        snprintf(path, sizeof(path), "%s\\tag_%02d_meta.txt",
                 g_tag_dump_dir, static_cast<int>(k));
        if (FILE* mf = fopen(path, "w")) {
            fprintf(mf, "present_id %llu\nslot %u\nsource %u\nmode5 %u\nderived %u\n"
                        "linear %u\npatch %u\nw %u\nh %u\nui_w %u\nui_h %u\nui_fmt %u\n"
                        "ui_quality_bp %u\nbackbuffer_bmp %d\nhudless_bmp %d\n"
                        "reconstruct_bmp %d\nalpha_bmp %d\n",
                    static_cast<unsigned long long>(e.present_id), e.slot, e.source,
                    e.mode5, e.derived, e.linear, e.patch, e.w, e.h, e.ui_w, e.ui_h,
                    e.ui_fmt, e.ui_quality, okb ? 1 : 0, okh ? 1 : 0, oku ? 1 : 0,
                    oka ? 1 : 0);
            fclose(mf);
        }
        logf_("[lock] tag-dump %zu: bb=%d hud=%d %s=%d mode5=%u derived=%u patch=%u"
              " -> %s\n", k, okb ? 1 : 0, okh ? 1 : 0,
              e.patch ? "patch" : "ui", oku ? 1 : 0,
              e.mode5, e.derived, e.patch, g_tag_dump_dir);
    }
    g_tag_dump_entries.clear();
    logf_("[lock] tag-dump complete (%d frame(s)) -> %s\n",
          g_tag_dump_frames, g_tag_dump_dir);
}

