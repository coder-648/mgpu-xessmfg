// ============================================================================
//  consumer module: frame.cpp
//
//  run_frame_ex: the per-WGC-frame recording path.
//
//  Included by src/xess_mfg_consumer/unity.cpp as part of the unity build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================

#include "../consumer_internal.h"

// ---------------------------------------------------------------------------
// MV/depth copies run on a separate COPY queue. Blocking on its fence froze the
// whole loop for 200-280 ms whenever that queue fell behind (it moves 28 MB per
// frame and hits the pub-queue cap: "10 dropped by overflow" per window). Every wait
// is now a check instead: a publication whose copy has not completed yet is simply
// copied again on our own queue below (copy_inputs_local), which the GPU orders for us.
static UINT g_pf_stale = 0;
// v22: consecutive frames that presented the previous colour because no new publication
// was available (diagnostic; cadence is not advanced on those frames).
static unsigned g_reuse_streak = 0;
// ---- exact pairing (2026-11-15 audit) ---------------------------------------
// The marker is decoded from the colour frame being processed (no one-frame lag,
// no "+1" guess). A frame is only allowed to tag a publication when this exact
// match succeeded; otherwise it presents 1X and the counters below say why.
static bool g_pair_verified = false;
static UINT g_pair_exact = 0;       // marker matched frame_id AND ms12
static UINT g_pair_no_slot = 0;     // no slot carries this seq12 at all
static UINT g_pair_ms_fail = 0;     // seq12 exists but ms12 disagrees (stale/wrong)
static UINT g_pair_ambiguous = 0;   // two slots share seq12 after wrap
static UINT g_pair_unreadable = 0;  // fewer than 4/4 corner blocks / CRC
static UINT g_pair_stale = 0;       // had to read the previous frame's staging
static UINT g_pair_lost = 0;        // unverified -> presented 1X this frame
static UINT g_pair_mismatch = 0;    // verification after slot selection failed
static UINT g_pubq_miss = 0;        // FIFO had no entry whose frame_id matched the colour
static UINT g_dup_dropped = 0;      // duplicate colour dropped at ingress (no GPU work)
static uint64_t g_last_colour_id = 0;   // id of the last colour we actually presented
static double g_last_publish_ms = 0.0;   // last time the producer published a new frame
// How many publications behind the newest one the verified colour was. This is the
// number that sizes kMaxSlots: p99(lag) + 2 must be < kMaxSlots.
static UINT g_pair_lag_n = 0;
static UINT g_pair_lag_max = 0;
static uint64_t g_pair_lag_sum = 0;

static bool copy_fence_done(UINT64 value)
{
    if (g_copy_fence == nullptr) { return false; }
    return g_copy_fence->GetCompletedValue() >= value;
}

// ---------------------------------------------------------------------------
// FG-input pacer (dynamic cadence): the point where we hand a frame to XeFG is
// the only anchor that matters. Batches are laid end to end on the vblank grid,
// so every vblank has exactly one generated output no matter how the game's
// capture timestamps jitter:
//
//     input_n  at  target_n
//     batch_n covers [target_n, target_n + k_n * vblank]
//     target_{n+1} = target_n + k_n * vblank
//
// k_n still comes from the cadence controller (the long-run input rate), but the
// *submission* is no longer "whenever WGC delivered". If the frame arrived early
// we wait for the slot; if it arrived late the slot is already in the past and we
// submit immediately (a real input stall, not something pacing can invent).
static bool   g_fg_pace = true;
static double g_fgi_next_ms = 0.0;
static double g_fgi_wait_ms = 0.0;
static UINT   g_fgi_late = 0;

static void fg_input_pace(UINT k)
{
    if (!g_fg_pace || k < 2) { return; }
    const double vb = refresh_ms();
    if (!(vb > 0.5)) { return; }
    double now = qpc_ms();
    if (g_fgi_next_ms <= 0.0) { g_fgi_next_ms = now; }

    // A long stall must not make us wait for a slot minutes in the past: reset.
    if (now - g_fgi_next_ms > 100.0) { g_fgi_next_ms = now; }

    // 网格必须"按期推进"：目标点是既定的，迟到只能整周期跳过，绝不能以 now 重新起锚。
    // 旧实现 `g_fgi_next_ms = now + k*vb` 会把每批的迟到永久累加进相位 ——
    // 这正是 vphase 在窗口间游走、late 单调增长的原因（43fps 下尤其明显）。
    double due = g_fgi_next_ms;
    if (due > now) {
        const double wait = due - now;
        g_fgi_wait_ms = g_fgi_wait_ms * 0.9 + wait * 0.1;
        // Coarse sleep, then spin for the last millisecond (Sleep(1) can overshoot).
        if (wait > 1.5) { Sleep(static_cast<DWORD>(wait - 1.0)); }
        while ((now = qpc_ms()) < due) { YieldProcessor(); }
    } else {
        ++g_fgi_late;
        // 按整 vblank 跳过（保持 mod vblank 的相位，而不是保持 mod k*vblank）。
        const double behind = now - due;
        due += (std::floor(behind / vb) + 1.0) * vb;
    }

    // End-to-end: this batch occupies exactly k vblanks from its scheduled start.
    g_fgi_next_ms = due + static_cast<double>(k) * vb;
}

// 机会式 vblank 量化（optimal 的 paced 分支）：只在等待 <= Λ 时把本批
// 对齐到上一个 batch 的整数格；过期不补、不追赶；下一批从 now 重新起锚。
static void fg_input_pace_ms(double budget_ms, double latency_budget_ms)
{
    if (!g_fg_pace || budget_ms <= 0.5) { return; }
    double now = qpc_ms();
    if (g_fgi_next_ms <= 0.0) { g_fgi_next_ms = now; }
    // A long stall must not make us wait for a slot minutes in the past: reset.
    if (now - g_fgi_next_ms > 100.0) { g_fgi_next_ms = now; }

    const double due = g_fgi_next_ms;
    if (due > now && latency_budget_ms > 0.05 && (due - now) <= latency_budget_ms) {
        const double wait = due - now;
        g_fgi_wait_ms = g_fgi_wait_ms * 0.9 + wait * 0.1;
        if (wait > 1.5) { Sleep(static_cast<DWORD>(wait - 1.0)); }
        while ((now = qpc_ms()) < due) { YieldProcessor(); }
        g_fgi_next_ms = due + budget_ms;
    } else {
        // 超过 Λ 或已经过期：立即提交，不把迟到补进下一批（无债务）。
        const bool late = (due <= now);
        if (late) { ++g_fgi_late; }
        g_fgi_next_ms = now + budget_ms;
    }
}

// in the game-window setup (an Unreal shipping build is <Project>-Win64-Shipping.exe).
static bool wcs_ends_with_ci(const wchar_t* s, const wchar_t* suffix)
{
    if (!s || !suffix) { return false; }
    const size_t ls = wcslen(s), lf = wcslen(suffix);
    return ls >= lf && _wcsicmp(s + (ls - lf), suffix) == 0;
}

static float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t man  = h & 0x3FFu;
    float v;
    if (exp == 0) { v = (man / 1024.0f) * 0.00006103515625f; }
    else if (exp == 31) { v = 65504.0f; }
    else { v = (1.0f + man / 1024.0f) * std::pow(2.0f, static_cast<float>(exp) - 15.0f); }
    return sign ? -v : v;
}

static void probe_checker(ID3D11Texture2D* colour, ID3D11Texture2D* staging,
                          UINT x0, UINT y0)
{
    const UINT w = 8, h = 8;
    const D3D11_BOX box{ x0, y0, 0, x0 + w, y0 + h, 1 };
    g_ctx11->CopySubresourceRegion(staging, 0, 0, 0, 0, colour, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx11->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { return; }
    double even = 0.0, odd = 0.0, even_a = 0.0, odd_a = 0.0;
    UINT ne = 0, no = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
        for (UINT x = 0; x < w; ++x) {
            const double l = half_to_float(row[x * 4 + 0]) +
                             half_to_float(row[x * 4 + 1]) +
                             half_to_float(row[x * 4 + 2]);
            const double a = half_to_float(row[x * 4 + 3]);
            if (((x0 + x + y0 + y) & 1u) == 0) { even += l; even_a += a; ++ne; }
            else { odd += l; odd_a += a; ++no; }
        }
    }
    g_ctx11->Unmap(staging, 0);
    const double e = ne ? even / ne : 0.0;
    const double o = no ? odd / no : 0.0;
    const double ea = ne ? even_a / ne : 0.0;
    const double oa = no ? odd_a / no : 0.0;
    logf_("[consumer] marker checker: even=%.6f odd=%.6f delta=%+.6f"
          " alpha even=%.6f odd=%.6f delta=%+.6f\n",
          e, o, o - e, ea, oa, oa - ea);
}

static void probe_region(ID3D11Texture2D* colour, ID3D11Texture2D* staging,
                         UINT x0, UINT y0, UINT w, UINT h, const char* name)
{
    const D3D11_BOX box{ x0, y0, 0, x0 + w, y0 + h, 1 };
    g_ctx11->CopySubresourceRegion(staging, 0, 0, 0, 0, colour, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx11->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { return; }
    UINT magenta = 0, yellow = 0, white = 0;
    uint16_t maxr = 0, maxg = 0, maxb = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
        for (UINT x = 0; x < w; ++x) {
            const uint16_t r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
            if (r > maxr) { maxr = r; }
            if (g > maxg) { maxg = g; }
            if (b > maxb) { maxb = b; }
            if (r > 0x3800 && g < 0x2000 && b > 0x3800) { ++magenta; }
            if (r > 0x3800 && g > 0x3800 && b < 0x2000) { ++yellow; }
            if (r > 0x3800 && g > 0x3800 && b > 0x3800) { ++white; }
        }
    }
    g_ctx11->Unmap(staging, 0);
    static int logs = 0;
    if (logs < 16) {
        logf_("[consumer] marker probe %s: maxR=%04X maxG=%04X maxB=%04X"
              " magenta=%u yellow=%u white=%u\n",
              name, maxr, maxg, maxb, magenta, yellow, white);
    }
}

// Marker probe readback: look for the magenta 8x8 block fakerFG writes at the
// top-right of the final backbuffer (stretched by DXGI). Diagnostic only.
static void marker_probe_read(ID3D11Texture2D* colour)
{
    if (!g_marker_probe || !colour || !g_ctx11 || !g_d3d11) { return; }
    static int calls = 0;
    if ((calls++ % 40) != 0) { return; }

    D3D11_TEXTURE2D_DESC sd{};
    colour->GetDesc(&sd);
    if (sd.Width < 256 || sd.Height < 256) { return; }
    const UINT w = 128, h = 128;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = sd.Format; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_d3d11->CreateTexture2D(&d, nullptr, &st))) {
        static bool told = false;
        if (!told) { told = true; logf_("[consumer] marker probe: staging FAILED\n"); }
        return;
    }
    static bool first = true;
    if (first) {
        first = false;
        logf_("[consumer] marker probe: colour %ux%u fmt=%u\n",
              sd.Width, sd.Height, static_cast<unsigned>(sd.Format));
    }
    probe_region(colour, st.Get(), sd.Width - w, 128, w, h, "top-right-y128");
    probe_checker(colour, st.Get(), sd.Width - 8, 128);
    probe_region(colour, st.Get(), mgpu::marker_origin_x(sd.Width),
                 mgpu::marker_origin_y(sd.Height), 32, 32, "marker-origin");
    probe_region(colour, st.Get(), sd.Width / 2 - w / 2, sd.Height / 2 - h / 2,
                 w, h, "centre");
    probe_region(colour, st.Get(), sd.Width - w, 0, w, h, "top-right-y0");
    probe_region(colour, st.Get(), 0, 0, w, h, "top-left");
}

// Plan C: put the pre-marker 8x8 tile back into the HDR10 frame after the
// scRGB->PQ draw. The tile is in the chain buffer's format (R10G10B10A2 here), so
// it matches the XeFG input exactly; no second colour conversion is involved.
static float srgb8_to_linear(float v)
{
    v = (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
    return v;
}

static float pq_encode_scalar(float nits)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::max(0.0f, std::min(1.0f, nits / 10000.0f)), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

// Convert the fakerFG R8 chain tile to the HDR10 R10 format XeFG consumes. The
// game's 8-bit chain is sRGB; the consumer's conversion uses scRGB 1.0 = 80 nits
// and BT.709 -> BT.2020 before PQ, so mirror that here.
static void marker_tile_r8_to_r10(const uint8_t* src, uint8_t* dst)
{
    const float m[3][3] = {
        { 0.6274f, 0.3293f, 0.0433f },
        { 0.0691f, 0.9195f, 0.0114f },
        { 0.0164f, 0.0880f, 0.8956f },
    };
    for (UINT i = 0; i < mgpu::kMarkerSide * mgpu::kMarkerSide; ++i) {
        const float r = srgb8_to_linear(src[i * 4 + 0] / 255.0f) * 80.0f;
        const float g = srgb8_to_linear(src[i * 4 + 1] / 255.0f) * 80.0f;
        const float b = srgb8_to_linear(src[i * 4 + 2] / 255.0f) * 80.0f;
        const float c[3] = {
            m[0][0] * r + m[0][1] * g + m[0][2] * b,
            m[1][0] * r + m[1][1] * g + m[1][2] * b,
            m[2][0] * r + m[2][1] * g + m[2][2] * b,
        };
        const uint32_t rq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[0]))) * 1023.0f + 0.5f);
        const uint32_t gq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[1]))) * 1023.0f + 0.5f);
        const uint32_t bq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[2]))) * 1023.0f + 0.5f);
        const uint32_t px = (rq & 0x3FFu) | ((gq & 0x3FFu) << 10) |
                            ((bq & 0x3FFu) << 20) | (3u << 30);
        memcpy(dst + i * 4, &px, 4);
    }
}

static float r11_float(uint32_t v, bool b10)
{
    const uint32_t exp = b10 ? ((v >> 5) & 0x1Fu) : ((v >> 6) & 0x1Fu);
    const uint32_t man = b10 ? (v & 0x1Fu) : (v & 0x3Fu);
    const float scale = b10 ? 32.0f : 64.0f;
    if (exp == 0) { return (man / scale) * 0.00006103515625f; }
    if (exp == 31) { return 0.0f; }
    return (1.0f + man / scale) * std::pow(2.0f, static_cast<float>(exp) - 15.0f);
}

static void marker_tile_r11_to_r10(const uint8_t* src, uint8_t* dst)
{
    const float m[3][3] = {
        { 0.6274f, 0.3293f, 0.0433f },
        { 0.0691f, 0.9195f, 0.0114f },
        { 0.0164f, 0.0880f, 0.8956f },
    };
    for (UINT i = 0; i < mgpu::kMarkerSide * mgpu::kMarkerSide; ++i) {
        uint32_t px = 0;
        memcpy(&px, src + i * 4, 4);
        const float r = r11_float(px & 0x7FFu, false) * 80.0f;
        const float g = r11_float((px >> 11) & 0x7FFu, false) * 80.0f;
        const float b = r11_float((px >> 22) & 0x3FFu, true) * 80.0f;
        const float c[3] = {
            m[0][0] * r + m[0][1] * g + m[0][2] * b,
            m[1][0] * r + m[1][1] * g + m[1][2] * b,
            m[2][0] * r + m[2][1] * g + m[2][2] * b,
        };
        const uint32_t rq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[0]))) * 1023.0f + 0.5f);
        const uint32_t gq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[1]))) * 1023.0f + 0.5f);
        const uint32_t bq = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f,
            pq_encode_scalar(c[2]))) * 1023.0f + 0.5f);
        const uint32_t out = (rq & 0x3FFu) | ((gq & 0x3FFu) << 10) |
                             ((bq & 0x3FFu) << 20) | (3u << 30);
        memcpy(dst + i * 4, &out, 4);
    }
}

// The WGC colour size the marker code was decoded from. The sidecar rect is in
// the producer's chain space, which normally equals this; keeping it explicit lets
// the restore scale into a resized output (--scale / --den).
static UINT g_marker_cap_w = 0;
static UINT g_marker_cap_h = 0;
// Successful exact restores (the 16x16 copy actually recorded). Should track
// `decoded` 1:1 when the whole chain is healthy.
static uint64_t g_marker_restored = 0;

// ---- diagnostic: dump the presented 32x32 block around the marker -----------
// marker_in.raw  : 16x16 captured (WGC) block, written by marker_decode_code()
// marker_out.raw : 32x32 of the CONVERTED+REPAIRED backbuffer, i.e. what the user
//                  sees. Comparing them tells a remaining block apart from a
//                  marker that was not erased.
static ComPtr<ID3D12Resource> g_dbg_rb;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_dbg_fp{};
static UINT64 g_dbg_value = 0;
static ComPtr<ID3D12Fence> g_dbg_fence;
static bool g_dbg_pending = false;
static unsigned g_dbg_seq = 0;
static unsigned g_dbg_files = 0;

static bool marker_dbg_init()
{
    if (g_dbg_rb) { return true; }
    if (!g_device) { return false; }
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 32;
    td.Height = 32;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0;
    UINT64 row = 0, total = 0;
    g_device->GetCopyableFootprints(&td, 0, 1, 0, &g_dbg_fp, &rows, &row, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_dbg_rb))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dbg_fence)))) {
        g_dbg_rb.Reset(); g_dbg_fence.Reset();
        return false;
    }
    return true;
}

// Called right after the exact restore: copies the visible block into a readback
// buffer. Polls (and writes the file) at the start of the next frame.
static void marker_dbg_record(ID3D12GraphicsCommandList* list, ID3D12Resource* bbres,
                              UINT mx, UINT my)
{
    if (list == nullptr || bbres == nullptr || (++g_dbg_seq % 60u) != 0u) { return; }
    if (!marker_dbg_init()) {
        static bool told = false;
        if (!told) { told = true; logf_("[consumer] marker dump: init FAILED\n"); }
        return;
    }
    const UINT x = (mx >= 8) ? (mx - 8) : 0;
    const UINT y = (my >= 8) ? (my - 8) : 0;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = g_dbg_rb.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = g_dbg_fp;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = bbres;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    const D3D12_BOX box{ x, y, 0, x + 32, y + 32, 1 };
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    g_dbg_pending = true;
    ++g_dbg_value;                      // signalled after ExecuteCommandLists
    logf_("[consumer] marker dump: recording 32x32 at %u,%u (seq=%llu)\n",
          x, y, static_cast<unsigned long long>(g_marker_seq));
}

static void marker_dbg_poll()
{
    if (!g_dbg_pending || !g_dbg_rb || !g_dbg_fence) { return; }
    if (g_dbg_fence->GetCompletedValue() < g_dbg_value) { return; }
    g_dbg_pending = false;
    void* p = nullptr;
    const D3D12_RANGE range{ 0, 0 };
    if (FAILED(g_dbg_rb->Map(0, &range, &p)) || p == nullptr) {
        static bool told = false;
        if (!told) { told = true; logf_("[consumer] marker dump: Map FAILED\n"); }
        return;
    }
    const BYTE* base = static_cast<const BYTE*>(p) + g_dbg_fp.Offset;
    FILE* f = nullptr;
    if (fopen_s(&f, "marker_out.raw", "wb") == 0 && f != nullptr) {
        const UINT rowb = 32u * 4u;
        for (UINT yy = 0; yy < 32; ++yy) {
            fwrite(base + static_cast<size_t>(yy) * g_dbg_fp.Footprint.RowPitch, 1, rowb, f);
        }
        fclose(f);
        if (g_dbg_files++ < 3) {
            logf_("[consumer] marker dump: marker_out.raw (32x32 R10, the presented picture,"
                  " row pitch %u)\n", g_dbg_fp.Footprint.RowPitch);
        }
    }
    g_dbg_rb->Unmap(0, nullptr);
}

// File switch next to the exe (used for diagnostics and A/B runs).
static bool exe_file_exists(const wchar_t* name)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// ---- whole-frame snapshot ---------------------------------------------------
// With dump_frame.on next to the exe, the presented backbuffer is copied to a
// readback buffer (twice per process, not per frame - it is 33 MB). The raw file is
// then turned into a PNG offline, which answers "where exactly is the artefact and
// how big is it" without depending on a screenshot upload.
static ComPtr<ID3D12Resource> g_fdump_rb;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fdump_fp{};
static UINT   g_fdump_rows = 0;
static UINT64 g_fdump_total = 0;
static ComPtr<ID3D12Fence> g_fdump_fence;
static UINT64 g_fdump_value = 0;
static bool   g_fdump_pending = false;
static int    g_fdump_left = 2;

static bool frame_dump_init(ID3D12Resource* bbres)
{
    if (g_fdump_rb) { return true; }
    if (!g_device || !bbres) { return false; }
    const D3D12_RESOURCE_DESC sd = bbres->GetDesc();
    if (sd.Format != DXGI_FORMAT_R10G10B10A2_UNORM) { return false; }
    D3D12_RESOURCE_DESC td = sd;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    UINT64 row = 0;
    g_device->GetCopyableFootprints(&td, 0, 1, 0, &g_fdump_fp, &g_fdump_rows,
                                    &row, &g_fdump_total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = g_fdump_total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_fdump_rb))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fdump_fence)))) {
        g_fdump_rb.Reset(); g_fdump_fence.Reset();
        return false;
    }
    logf_("[consumer] frame dump: readback ready (%u x %u, pitch %u, %.1f MB)\n",
          static_cast<unsigned>(sd.Width), static_cast<unsigned>(sd.Height),
          g_fdump_fp.Footprint.RowPitch, static_cast<double>(g_fdump_total) / 1048576.0);
    return true;
}

static bool frame_dump_wanted()
{
    static int on = -1;
    if (on < 0) {
        on = exe_file_exists(L"dump_frame.on") ? 1 : 0;
        logf_("[consumer] frame dump: %s\n", on ? "ON (dump_frame.on)" : "off");
    }
    return on != 0;
}

static void frame_dump_record(ID3D12GraphicsCommandList* list, ID3D12Resource* bbres)
{
    if (list == nullptr || bbres == nullptr || g_fdump_left <= 0) { return; }
    if (!frame_dump_wanted() || !frame_dump_init(bbres)) { return; }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = g_fdump_rb.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = g_fdump_fp;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = bbres;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    g_fdump_pending = true;
    ++g_fdump_value;                    // signalled after ExecuteCommandLists
    --g_fdump_left;
    logf_("[consumer] frame dump: recording the whole presented frame (%d left)\n",
          g_fdump_left);
}

static void frame_dump_poll()
{
    if (!g_fdump_pending || !g_fdump_rb || !g_fdump_fence) { return; }
    if (g_fdump_fence->GetCompletedValue() < g_fdump_value) { return; }
    g_fdump_pending = false;
    void* p = nullptr;
    const D3D12_RANGE range{ 0, 0 };
    if (FAILED(g_fdump_rb->Map(0, &range, &p)) || p == nullptr) { return; }
    const BYTE* base = static_cast<const BYTE*>(p) + g_fdump_fp.Offset;
    FILE* f = nullptr;
    if (fopen_s(&f, "frame_dump.raw", "wb") == 0 && f != nullptr) {
        const UINT rowb = g_fdump_fp.Footprint.Width * 4u;
        for (UINT y = 0; y < g_fdump_fp.Footprint.Height; ++y) {
            fwrite(base + static_cast<size_t>(y) * g_fdump_fp.Footprint.RowPitch, 1, rowb, f);
        }
        fclose(f);
        logf_("[consumer] frame dump: frame_dump.raw written (%ux%u R10)\n",
              g_fdump_fp.Footprint.Width, g_fdump_fp.Footprint.Height);
    }
    g_fdump_rb->Unmap(0, nullptr);
}

// PQ code (10-bit normalised) -> display linear (0..1 = 0..10000 nits).
static float pq_decode_scalar(float v)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    v = std::max(0.0f, std::min(1.0f, v));
    const float p = std::pow(v, 1.0f / m2);
    const float den = c2 - c3 * p;
    if (den <= 1e-6f) { return 1.0f; }
    const float num = std::max(p - c1, 0.0f);
    return std::pow(num / den, 1.0f / m1);
}

static float linear_to_srgb_scalar(float v)
{
    v = std::max(0.0f, std::min(1.0f, v));
    return (v <= 0.0031308f) ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// The tile arrives normalised to PQ BT.2020 R10. Re-encode it for the consumer's
// actual output encoding: pasting PQ codes into an SDR (gamma) or plain-BT.709
// frame is a colour error inside the repaired patch.
static uint32_t marker_pq_tile_to_output(uint32_t px, int mode)
{
    // 0: HDR10 PQ BT.2020 (as produced). 3: --fg-colour shared, the whole frame is
    // already PQ and the conv pass is a pass-through. Both keep the tile as-is.
    if (mode == 0 || mode == 3) { return px; }
    float v[3] = {
        pq_decode_scalar(((px >> 0) & 0x3FFu) / 1023.0f) * 10000.0f,
        pq_decode_scalar(((px >> 10) & 0x3FFu) / 1023.0f) * 10000.0f,
        pq_decode_scalar(((px >> 20) & 0x3FFu) / 1023.0f) * 10000.0f,
    };
    if (mode == 2) {
        // SDR output: scRGB 1.0 = 80 nits, then the gamma the conv shader writes.
        for (int i = 0; i < 3; ++i) { v[i] = linear_to_srgb_scalar(v[i] / 80.0f); }
    } else {
        // HDR10 without the BT.2020 primaries conversion: PQ(BT.709).
        const float m[3][3] = {
            {  1.6605f, -0.5876f, -0.0728f },
            { -0.1246f,  1.1329f, -0.0083f },
            { -0.0182f, -0.1006f,  1.1187f },
        };
        const float in[3] = { v[0], v[1], v[2] };
        for (int i = 0; i < 3; ++i) {
            v[i] = pq_encode_scalar(m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2]);
        }
    }
    const uint32_t rq = static_cast<uint32_t>(
        std::max(0.0f, std::min(1.0f, v[0])) * 1023.0f + 0.5f);
    const uint32_t gq = static_cast<uint32_t>(
        std::max(0.0f, std::min(1.0f, v[1])) * 1023.0f + 0.5f);
    const uint32_t bq = static_cast<uint32_t>(
        std::max(0.0f, std::min(1.0f, v[2])) * 1023.0f + 0.5f);
    return (rq & 0x3FFu) | ((gq & 0x3FFu) << 10) | ((bq & 0x3FFu) << 20) | (3u << 30);
}

// ---- one-shot visual probe --------------------------------------------------
// With probe_marker.on next to the exe, the consumer writes a 64x64 magenta square
// at (16,16) of the frame it is about to present. If the user does NOT see it, they
// are looking at the game window and no consumer-side repair can ever be visible.
static ComPtr<ID3D12Resource> g_probe_tex;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_probe_fp{};
static int g_probe_state = -1;      // -1 unknown, 0 off, 1 on

static bool probe_file_exists()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, L"probe_marker.on");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Visual probe (probe_marker.on): copy the top-left marker block to the CENTRE of
// the presented backbuffer. The marker itself proves that DRAWs into this buffer are
// displayed; if the centre shows the same black/white block, COPYs into this buffer
// are displayed too. If the centre stays normal, copies are being dropped and the
// repair has to be done with a draw. Runs BEFORE the repair so the source block is
// still the drawn marker.
static void marker_probe_overlay(ID3D12GraphicsCommandList* list, ID3D12Resource* bbres,
                                 UINT /*bbi*/, UINT w, UINT h)
{
    if (list == nullptr || bbres == nullptr) { return; }
    if (g_probe_state < 0) {
        g_probe_state = probe_file_exists() ? 1 : 0;
        logf_("[consumer] visual probe: %s\n", g_probe_state ? "ON (corner block copied to centre)" : "off");
    }
    if (g_probe_state == 0) { return; }
    D3D12_RESOURCE_DESC td = bbres->GetDesc();
    if (td.Format != DXGI_FORMAT_R10G10B10A2_UNORM) { return; }
    const UINT S = mgpu::kMarkerSide;
    if (w < 4 * S || h < 4 * S) { return; }
    static ComPtr<ID3D12Resource> tmp;
    if (!tmp) {
        D3D12_RESOURCE_DESC d = td;
        d.Width = S; d.Height = S; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Flags = D3D12_RESOURCE_FLAG_NONE; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tmp)))) {
            tmp.Reset(); return;
        }
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = bbres;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = tmp.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);

    uint32_t mx = 0, my = 0;
    mgpu::marker_corner(0, w, h, &mx, &my);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tmp.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bbres;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{ mx, my, 0, mx + S, my + S, 1 };
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    b.Transition.pResource = tmp.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = bbres;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);
    const UINT cx = (w / 2u) & ~7u;
    const UINT cy = (h / 2u) & ~7u;
    D3D12_TEXTURE_COPY_LOCATION back{};
    back.pResource = bbres;
    back.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&back, cx, cy, 0, &dst, nullptr);

    b.Transition.pResource = bbres;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = tmp.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
}

// The producer draws the four corner markers ONLY on frames it publishes: fakerFG's
// marker_probe_present() needs g_pending_seq != 0, which needs the title to call
// EvaluateFeature (DLSS/FG on). Repairing unconditionally therefore overwrote real
// scene content at the four corners whenever FG was off - which is exactly what the
// "four 16x16 scars with and without FG" observation turned out to be (measured:
// marker_corners.raw held ordinary scene pixels, and the game log had no marker draw).
static double g_last_pub_ms = -1.0;

// File switches next to the exe, so an A/B run needs no command-line edit:
//   marker_repair_off.on  -> producer still draws the markers, the consumer does NOT
//                             repair them (the four blocks stay visible on purpose)
//   marker_disable.on     -> producer draws nothing at all   (game directory)
static bool marker_drawn_recently()
{
    if (!g_marker_probe) { return false; }
    static int repair_off = -1;
    if (repair_off < 0) {
        repair_off = exe_file_exists(L"marker_repair_off.on") ? 1 : 0;
        if (repair_off) {
            logf_("[consumer] marker repair: OFF by marker_repair_off.on"
                  " (markers stay visible - A/B)\n");
        }
    }
    if (repair_off) { return false; }
    if (g_last_pub_ms < 0.0) { return false; }
    return (qpc_ms() - g_last_publish_ms) < 250.0;
}

// The post-convert copy repair (bbres -> 8x8 temp -> bbres) lived here. It is gone:
// the conversion pass itself now replaces the four corner blocks (conv_consts[8] ->
// ps_main in shaders.cpp), which costs no extra copy or barrier and cannot be
// dropped the way a separate copy pass can.

static uint32_t marker_crc32(uint32_t v)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 32; ++i) {
        const uint32_t bit = ((v >> i) & 1u) ^ (crc & 1u);
        crc >>= 1;
        if (bit) { crc ^= 0xEDB88320u; }
    }
    return ~crc;
}

// Per-pixel luma-sum used only for thresholding the marker code (min/max inside
// the block), so it must be monotonic, not colorimetric. R16F is the normal WGC
// format, but a machine can hand us R10/R8 as well.
static double marker_luma_at(const BYTE* p, DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
        return half_to_float(h[0]) + half_to_float(h[1]) + half_to_float(h[2]);
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v = 0;
        memcpy(&v, p, 4);
        return ((v & 0x3FFu) + ((v >> 10) & 0x3FFu) + ((v >> 20) & 0x3FFu)) / 1023.0;
    }
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return (p[0] + p[1] + p[2]) / 255.0;
    default:
        return 0.0;
    }
}

// Decode the 16x16 marker: one bit per 2x2 cell, seq32 | crc32<<32. Averaging the
// four pixels of a cell (and thresholding against the block's own min/max) is what
// makes the code survive DWM scaling/filtering; a single-bit flip retry covers one
// damaged cell without falling back to timestamp pairing.
// Decode all four corner blocks in ONE staging round trip: four 8x8 copies land in one
// 8x32 staging texture, then a single Map. Four separate Map() calls are four GPU
// synchronisations per frame, which is what dropped 2x generation from 80 to 70 fps.
struct MarkerBlocks
{
    uint32_t bits[mgpu::kMarkerCorners]{};
    bool     ok[mgpu::kMarkerCorners]{};
    double   contrast[mgpu::kMarkerCorners]{};
    double   lo[mgpu::kMarkerCorners]{};
    double   hi[mgpu::kMarkerCorners]{};
    UINT     maps_ok = 0;       // diagnostic: Map results so far
    UINT     maps_busy = 0;
    // True when the id below came from the PREVIOUS frame's staging slot because the
    // current one could not be mapped in time. The caller must not tag a publication
    // from a stale id (it presents 1X instead).
    bool     stale = false;
};

// Diagnostics for the staging read, at file scope so the report can print them even
// when marker_decode_all returns early (that hid the real state once already).
static UINT    g_marker_map_calls = 0;
static UINT    g_marker_map_ok = 0;
static UINT    g_marker_map_busy = 0;
static UINT    g_marker_map_fail = 0;
static HRESULT g_marker_map_last_hr = S_OK;
static int     g_marker_map_last_rd = -2;

static void marker_decode_all(ID3D11Texture2D* colour, UINT w, UINT h, MarkerBlocks* out)
{
    if (out == nullptr) { return; }
    if (!g_marker_probe || !colour || !g_ctx11 || !g_d3d11) { return; }
    D3D11_TEXTURE2D_DESC sd{};
    colour->GetDesc(&sd);
    const UINT bpp = marker_bpp(sd.Format);
    const UINT S = mgpu::kMarkerSide;
    const UINT N = mgpu::kMarkerCorners;
    if (bpp == 0 || S == 0 || N == 0) { return; }

    // TWO staging slots, but the one just filled is read THIS frame (blocking Map).
    // The old design read the PREVIOUS frame's slot to keep Map non-blocking, which made
    // the decoded id lag one colour frame; pairing then compensated with "id + 1", which
    // is only correct while the colour sequence is consecutive (a WGC drop / DWM duplicate
    // silently broke it, and `pf served` still counted a hit). The copy here is 6x6 x 4
    // corners = 144 px, so waiting for it is cheap; if it ever blocks we log the cost.
    // The second slot is kept so a Map that is still busy can fall back to the previous
    // frame's id WITHOUT blocking, and the caller treats that as unverified (1X).
    static ComPtr<ID3D11Texture2D> stg[2];
    static DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN;
    static int fill_slot = 0;
    static int read_slot = -1;
    if (!stg[0] || sfmt != sd.Format) {
        for (int k = 0; k < 2; ++k) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = S;
            d.Height = S * N;
            d.MipLevels = 1; d.ArraySize = 1;
            d.Format = sd.Format; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stg[k].Reset();
            const HRESULT chr = g_d3d11->CreateTexture2D(&d, nullptr, &stg[k]);
            if (FAILED(chr)) {
                static bool told = false;
                if (!told) {
                    told = true;
                    logf_("[consumer] marker decode: staging FAILED hr=0x%08lX "
                          "(%ux%u fmt=%u)\n", static_cast<unsigned long>(chr),
                          S, S * N, static_cast<unsigned>(sd.Format));
                }
                stg[0].Reset(); stg[1].Reset();
                return;
            }
        }
        sfmt = sd.Format;
        fill_slot = 0;
        read_slot = -1;
    }

    UINT mx[mgpu::kMarkerCorners]{}, my[mgpu::kMarkerCorners]{};
    bool inb[mgpu::kMarkerCorners]{};
    bool any = false;
    for (UINT i = 0; i < N; ++i) {
        mgpu::marker_corner(i, w, h, &mx[i], &my[i]);
        inb[i] = (mx[i] + S <= w) && (my[i] + S <= h);
        if (!inb[i]) { continue; }
        const D3D11_BOX box{ mx[i], my[i], 0, mx[i] + S, my[i] + S, 1 };
        g_ctx11->CopySubresourceRegion(stg[fill_slot].Get(), 0, 0, i * S, 0, colour, 0, &box);
        any = true;
    }
    const int rd = any ? fill_slot : -1;
    const int prev = read_slot;                 // last frame's completed slot (fallback)
    read_slot = any ? fill_slot : -1;
    fill_slot ^= 1;
    static bool told_state = false;
    if (!told_state) {
        told_state = true;
        logf_("[consumer] marker decode: staging ready, blocks in bounds=%d, "
              "read slot=%d (bpp=%u, capture %ux%u fmt=%u)\n",
              any ? 1 : 0, rd, bpp, sd.Width, sd.Height, static_cast<unsigned>(sd.Format));
    }
    if (!any || rd < 0) { return; }

    static UINT maps_ok = 0, maps_busy = 0;
    D3D11_MAPPED_SUBRESOURCE m{};
    ++g_marker_map_calls;
    int read_idx = rd;
    g_marker_map_last_rd = rd;
    // NOTE: MapType and MapFlags are SEPARATE parameters. ORing the flag into MapType
    // (0x10001) is an invalid D3D11_MAP and Map() returns E_INVALIDARG (0x80070057) on
    // every call - which is exactly what the counters reported (fail=2879, ok=0).
    HRESULT mhr = g_ctx11->Map(stg[rd].Get(), 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (mhr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // Current-frame decode: wait for the 144-pixel copy instead of reading the
        // previous frame's id. If this ever costs real time it is logged once.
        const double w0 = qpc_ms();
        mhr = g_ctx11->Map(stg[rd].Get(), 0, D3D11_MAP_READ, 0, &m);
        const double w1 = qpc_ms();
        if (SUCCEEDED(mhr) && (w1 - w0) > 1.0) {
            static bool told_wait = false;
            if (!told_wait) {
                told_wait = true;
                logf_("[consumer] marker decode: waited %.2f ms for the current frame's"
                      " 144-px staging copy (current-frame id; no +1 heuristic)\n", w1 - w0);
            }
        }
    }
    if (mhr == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(mhr) || !m.pData) {
        // Still not readable: fall back to the previous frame's completed slot so the
        // caller can keep presenting, but mark it stale (it will NOT tag a publication).
        if (prev >= 0) {
            mhr = g_ctx11->Map(stg[prev].Get(), 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(mhr) && m.pData) {
                read_idx = prev;
                out->stale = true;
                mhr = S_OK;                 // the fallback read did succeed
                ++maps_busy;
            }
        }
        if (read_idx != prev || m.pData == nullptr) {
            ++g_marker_map_fail;
            g_marker_map_last_hr = mhr;
            return;
        }
        ++g_marker_map_busy;
    }
    g_marker_map_last_hr = mhr;
    ++maps_ok;
    ++g_marker_map_ok;
    g_marker_map_last_rd = read_idx;
    const BYTE* base = static_cast<const BYTE*>(m.pData);
    const UINT C = mgpu::kMarkerCells;
    for (UINT i = 0; i < N; ++i) {
        if (!inb[i]) { continue; }
        double luma[mgpu::kMarkerCellsPerBlock]{};   // 9 cells: 8 data bits + 1 reference
        double lo = 1.0e30, hi = -1.0e30;
        for (UINT cy = 0; cy < C; ++cy) {
            for (UINT cx = 0; cx < C; ++cx) {
                double sum = 0.0;
                for (UINT dy = 0; dy < mgpu::kMarkerCellPx; ++dy) {
                    const BYTE* row = base + static_cast<size_t>(i * S + cy * mgpu::kMarkerCellPx + dy) * m.RowPitch;
                    for (UINT dx = 0; dx < mgpu::kMarkerCellPx; ++dx) {
                        sum += marker_luma_at(row + (cx * mgpu::kMarkerCellPx + dx) * bpp, sd.Format);
                    }
                }
                const double v = sum / (mgpu::kMarkerCellPx * mgpu::kMarkerCellPx);
                luma[cy * C + cx] = v;
                if (v < lo) { lo = v; }
                if (v > hi) { hi = v; }
            }
        }
        out->lo[i] = lo;
        out->hi[i] = hi;
        out->contrast[i] = (hi > lo) ? (hi - lo) : 0.0;
        if (hi - lo < 0.05) { continue; }
        // Cells 0..7 are the data bits; cell 8 is the reference cell the marker pass
        // paints opposite to the data, so the block always has two levels.
        const double mid = (lo + hi) * 0.5;
        uint32_t bits = 0;
        for (UINT b = 0; b < mgpu::kMarkerBits; ++b) {
            if (luma[b] > mid) { bits |= (1u << b); }
        }
        out->bits[i] = bits;
        out->ok[i] = true;
    }
    g_ctx11->Unmap(stg[read_idx].Get(), 0);
    out->maps_ok = maps_ok;
    out->maps_busy = maps_busy;
}

// Diagnostic (dump_frame.on only): write the four corner blocks exactly as the capture
// sees them, so "is the marker there, and is it a checker or a solid square?" can be
// answered from the bytes instead of from a screenshot.
static void marker_dump_corners(ID3D11Texture2D* colour, UINT w, UINT h)
{
    if (!colour || !g_ctx11 || !g_d3d11) { return; }
    D3D11_TEXTURE2D_DESC sd{};
    colour->GetDesc(&sd);
    const UINT bpp = marker_bpp(sd.Format);
    const UINT S = mgpu::kMarkerSide;
    if (bpp == 0) { return; }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = S; d.Height = S; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = sd.Format; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(g_d3d11->CreateTexture2D(&d, nullptr, &stg))) { return; }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, L"marker_corners.raw");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == nullptr) { return; }
    for (uint32_t i = 0; i < mgpu::kMarkerCorners; ++i) {
        uint32_t mx = 0, my = 0;
        mgpu::marker_corner(i, w, h, &mx, &my);
        if (mx + S > w || my + S > h) { continue; }
        const D3D11_BOX box{ mx, my, 0, mx + S, my + S, 1 };
        g_ctx11->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, colour, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx11->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT r = 0; r < S; ++r) {
                fwrite(static_cast<const BYTE*>(m.pData) + r * m.RowPitch, 1, S * bpp, f);
            }
            g_ctx11->Unmap(stg.Get(), 0);
        }
    }
    fclose(f);
    logf_("[consumer] marker dump: marker_corners.raw (4 blocks of %ux%u, %u bpp, "
          "corner order TL,TR,BL,BR)\n", S, S, bpp);
}

// Decode the SHORT SNOWFLAKE the producer painted: four 6x6 corner blocks, each with
// 8 data bits (XOR 0x5A) plus one always-white reference cell, giving
//   code32 = payload24 | crc8<<24,   payload24 = sequence12 | low12(ms)
// The full frame id comes from the present ring - 24 bits only has to differ per frame.
static uint32_t marker_decode_payload(ID3D11Texture2D* colour, bool* ok_out,
                                      bool* stale_out = nullptr)
{
    if (ok_out != nullptr) { *ok_out = false; }
    if (stale_out != nullptr) { *stale_out = false; }
    if (!g_marker_probe || !colour) { return 0; }
    D3D11_TEXTURE2D_DESC sd{};
    colour->GetDesc(&sd);
    g_marker_cap_w = sd.Width;
    g_marker_cap_h = sd.Height;
    MarkerBlocks mb{};
    marker_decode_all(colour, sd.Width, sd.Height, &mb);
    if (stale_out != nullptr) { *stale_out = mb.stale; }
    uint32_t code = 0;
    int blocks = 0;
    for (uint32_t i = 0; i < mgpu::kMarkerCorners; ++i) {
        if (!mb.ok[i]) { continue; }
        code |= static_cast<uint32_t>(mb.bits[i] ^ mgpu::kMarkerXor) << (i * mgpu::kMarkerBits);
        ++blocks;
    }
    // Per-corner contrast, every 240 frames: a corner that reads ~0 means the block
    // is not where we look (or something covers it); ~1.0 means the code is there.
    static UINT con_frames = 0;
    if (++con_frames >= 240) {
        con_frames = 0;
        logf_("[consumer] marker corners: #0(%u,%u)=%.2f[%.2f..%.2f] "
              "#1(%u,%u)=%.2f[%.2f..%.2f] #2(%u,%u)=%.2f[%.2f..%.2f] "
              "#3(%u,%u)=%.2f[%.2f..%.2f]  readable=%d/4\n",
              mgpu::marker_origin_x(sd.Width), mgpu::marker_origin_y(sd.Height),
              mb.contrast[0], mb.lo[0], mb.hi[0],
              static_cast<UINT>(sd.Width - mgpu::marker_origin_x(sd.Width)
                                - mgpu::kMarkerSide),
              mgpu::marker_origin_y(sd.Height), mb.contrast[1], mb.lo[1], mb.hi[1],
              mgpu::marker_origin_x(sd.Width),
              static_cast<UINT>(sd.Height - mgpu::marker_origin_y(sd.Height)
                                - mgpu::kMarkerSide),
              mb.contrast[2], mb.lo[2], mb.hi[2],
              static_cast<UINT>(sd.Width - mgpu::marker_origin_x(sd.Width)
                                - mgpu::kMarkerSide),
              static_cast<UINT>(sd.Height - mgpu::marker_origin_y(sd.Height)
                                - mgpu::kMarkerSide),
              mb.contrast[3], mb.lo[3], mb.hi[3], blocks);
        logf_("[consumer] marker read: calls=%u ok=%u busy=%u fail=%u last_hr=0x%08lX rd=%d\n",
              g_marker_map_calls, g_marker_map_ok, g_marker_map_busy, g_marker_map_fail,
              static_cast<unsigned long>(g_marker_map_last_hr), g_marker_map_last_rd);
        if (frame_dump_wanted()) { marker_dump_corners(colour, sd.Width, sd.Height); }
    }

    if (blocks != static_cast<int>(mgpu::kMarkerCorners)) {
        static int told = 0;
        if (told++ < 4) {
            logf_("[consumer] marker decode: only %d/%u corner blocks readable\n",
                  blocks, mgpu::kMarkerCorners);
        }
        return 0;
    }

    uint32_t payload = code & 0xFFFFFFu;
    uint32_t crc = (code >> 24) & 0xFFu;
    bool fixed = false;
    if (crc != mgpu::marker_crc8(payload)) {
        for (UINT b = 0; b < 32; ++b) {
            const uint32_t cand = code ^ (1u << b);
            const uint32_t p2 = cand & 0xFFFFFFu;
            if (((cand >> 24) & 0xFFu) == mgpu::marker_crc8(p2)) {
                code = cand;
                payload = p2;
                crc = (cand >> 24) & 0xFFu;
                fixed = true;
                break;
            }
        }
    }
    if (crc != mgpu::marker_crc8(payload)) {
        static int told = 0;
        if (told++ < 4) {
            logf_("[consumer] marker decode: crc8=0x%02X payload=0x%06X (want 0x%02X)\n",
                  crc, payload, mgpu::marker_crc8(payload));
        }
        return 0;
    }
    if (ok_out != nullptr) { *ok_out = true; }
    static uint32_t last = 0xFFFFFFFFu;
    if (payload != last) {
        last = payload;
        logf_("[consumer] marker code: seq12=%u ms12=%u payload=0x%06X crc8=0x%02X ok%s\n",
              mgpu::marker_seq_from_payload(payload), (payload >> 12) & 0xFFFu,
              payload, crc, fixed ? " (1-bit fix)" : "");
    }
    return payload;
}

static void run_frame_ex(ID3D11Texture2D* colour11)
{
    // Phase stamps for the slow-frame report below (a 250 ms frame must say WHERE it went).
    const double t_m0 = qpc_ms();
    double t_m1 = t_m0, t_m2 = t_m0, t_m3 = t_m0;
    // g_reuse_frame is also set by the duplicate/miss paths below. It must not leak
    // into the next frame; the caller's own paced reuse sets it before the call and
    // clears it after, so only clear what we set here.
    struct ReuseGuard {
        bool on_entry;
        ~ReuseGuard() { if (!on_entry) { g_reuse_frame = false; } }
    } reuse_guard{ g_reuse_frame };

    marker_dbg_poll();
    frame_dump_poll();

    // ---- v22: publication-driven identity -------------------------------------
    // One published slot carries colour + MV + depth for the SAME frame, so the slot
    // index is the pairing key. The main loop picked it and wait_slot() already waited
    // for its fence; there is no marker to decode and no present ring to search.
    int marker_slot = static_cast<int>(g_pub_slot);
    if (g_ctl == nullptr || g_pub_slot >= mgpu::kMaxSlots) { return; }
    const uint32_t slots_now = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    if (g_pub_slot >= slots_now) { marker_slot = 0; }
    const UINT slot = static_cast<UINT>(marker_slot);
    const uint64_t colour_id = g_ctl->frames[marker_slot].frame_id;
    if (colour_id == 0) { g_reuse_frame = true; }   // slot not published yet: keep the last picture
    if ((g_ctl->frames[marker_slot].fg_flags & mgpu::kInputFlagColourActive) == 0) {
        g_reuse_frame = true;    // this publication has no colour: reuse the last sink
    }
    g_marker_seq    = colour_id;
    g_pair_seq      = colour_id;
    g_pair_verified = (colour_id != 0);
    g_marker_payload = 0;
    g_of_miss = false;

    // Freshness / stall: a publication that did not advance means the title stopped
    // evaluating (menu, pause, loading). Keep presenting the last picture; the MV gate
    // below turns FG off after kStallFrames.
    static uint64_t s_last_pub_id = 0;
    const bool pub_fresh = (colour_id != 0 && colour_id != s_last_pub_id);
    if (pub_fresh) { s_last_pub_id = colour_id; }
    g_pub_fresh = pub_fresh;
    if (pub_fresh) { g_last_publish_ms = qpc_ms(); }
    g_pub_stall_frames = pub_fresh ? 0u : (g_pub_stall_frames + 1u);

    // The input clock is the producer's per-presentation interval (the only legal
    // source; DYNAMIC_MF_ANALYSIS §1.3). It used to be a three-way estimate because the
    // colour arrived through WGC; now arrival and decision share one clock.
    {
        const double ft = static_cast<double>(g_ctl->frames[marker_slot].frame_time_ms);
        if (ft > 3.0 && ft < 100.0) { g_prod_interval_ms = ft; }
    }
    g_reuse_streak = g_reuse_frame ? (g_reuse_streak + 1u) : 0u;
    t_m1 = qpc_ms();

    if (!service_shared_layout()) { return; }
    collect_retired_inputs();
    t_m2 = qpc_ms();          // shared layout / retired inputs
    probe_colour_black();

    // 1. colour: capture frame -> shared sink (D3D11, on the AMD card)
    // One frame id for XeLL markers and the XeSS-FG present id, as the guide
    // requires. XeLL sleeps at simulation start and needs the markers to pace.
    const uint32_t present_id = static_cast<uint32_t>(++g_present_id);
    const double t_r0 = qpc_ms();
    // Catch-up burst detector: after a stall, WGC hands over several queued frames and the
    // loop processes them back to back (each with its own Present). This is the "the missing
    // inputs get dumped into the next batch" effect - measurable instead of guessed.
    {
        static double s_prev_proc = 0.0;
        if (s_prev_proc > 0.0) {
            const double dt = t_r0 - s_prev_proc;
            const double nom = input_interval_ms();
            if (nom > 2.0 && dt < nom * 0.6) { ++g_burst_count; }
        }
        s_prev_proc = t_r0;
    }
    // Protocol v7: tell the add-on whether its motion-vector stream is wanted. It is
    // ~14 MB/frame across PCIe, so it is produced only when we will tag it.
    if (g_ctl) {
        // --mv-shadow needs the producer to keep feeding real vectors even though we tag
        // zeroed ones, otherwise the copy would transfer stale data and measure nothing.
        // Auto (default): always ask for MV so the producer sends it when it can; the
        // per-frame decision below picks between it, our flow, and zeros.
        bool want_producer_mv = true;
        if (!g_mv_auto) {
            want_producer_mv = !(g_no_mv && !g_mv_shadow);
#if MGPU_FG_OF
            if (g_fg_of) { want_producer_mv = false; }
#endif
        }
#if MGPU_FG_OF
        // --fg-of builds its own vectors from the colour, so the producer stream (and its
        // ~14.7 MB/frame across PCIe) would be pure waste.
        if (g_fg_of) { want_producer_mv = false; }
#endif
        g_ctl->consumer_request = want_producer_mv ? mgpu::kRequestMv : 0u;
        // v14: the full switch board. --no-mv/--no-depth/--fg-of/--no-colour fold into it, so
        // there is exactly one place that decides what crosses the bus.
        // v22: the colour is the picture itself - it is never optional. The switch board
        // only chooses the motion data (and A/B raw modes).
        uint32_t mask = g_stream_mask | mgpu::kRequestColor;
        if (!want_producer_mv)          { mask &= ~mgpu::kRequestMv; }
        if (g_no_depth)                 { mask &= ~mgpu::kRequestDepth; }
        g_ctl->request_mask = mask;
        if (mask != g_stream_mask_last) {
            g_stream_mask_last = mask;
            logf_("[consumer] streams -> mv=%d depth=%d color=%d ui=%d meta=%d\n",
                  (mask & mgpu::kRequestMv) ? 1 : 0, (mask & mgpu::kRequestDepth) ? 1 : 0,
                  (mask & mgpu::kRequestColor) ? 1 : 0, (mask & mgpu::kRequestUi) ? 1 : 0,
                  (mask & mgpu::kRequestMeta) ? 1 : 0);
        }
    }
    // --dyn-mfg: OptiScaler's controller, once per real frame (before anything is
    // tagged, so a new count applies to this present).
    if (g_dyn_mfg && !g_interp_cadence) { fg_dyn_mfg_frame(t_r0, g_ms_present); }
    // Decision happens here, but the DLL count is applied AFTER gpu_throttle() below:
    // the previous batch must have retired before XeFG is reconfigured, otherwise the
    // pacer is asked to switch model/count in the middle of a batch.
    UINT cad_want = 0;
    bool cad_apply = false;
    // --interp-cadence: pick this frame's multiplier from the vblanks that elapsed
    // since the previous real frame (see cadence_next_slots). Done before anything is
    // tagged, so the count applies to this present.
    //
    // Never advance the phase for a reused/duplicate capture: WGC (and the title's own
    // two presents per record) delivers the same input frame more than once, and each
    // advance would ask for another batch of generated frames for a frame that has
    // already been accounted for. That over-generates in bursts and is exactly the
    // "severe stutter" the dynamic mode showed. A reuse keeps the last count/budget.
    if (g_cad_epoch_ms > 0.5 && g_refresh_hz > 0) {
        // Epoch dynamic: choose an integer multiplier from the input median, then
        // LOCK THE INPUT to target/k so the displayed rate stays exactly on target.
        // The multiplier only changes after a sustained crossing (hysteresis), so the
        // user sees one stable rate instead of 2x/3x flapping.
        static int    s_ep_k = 2;
        static double s_ep_t = 0.0;
        static bool   s_ep_init = false;
        const double target = (g_fg_out_hz > 1.0) ? g_fg_out_hz
                                                  : static_cast<double>(g_refresh_hz);
        const double T = arr_median_ms();
        if (T > 5.0 && target > 1.0) {
            const double fps = 1000.0 / T;
            const int J = static_cast<int>(g_fg_interp_max) + 1;
            if (!s_ep_init) {
                s_ep_k = std::clamp(static_cast<int>(std::floor(target / fps)), 2, J);
                s_ep_init = true;
                s_ep_t = t_r0;
                // Epoch never drops/re-times captures: the original bridge's lesson is
                // that a lock which does not divide the input rate turns one interval
                // into a 2T gap (visible jump). Unlocked is the balanced choice.
                g_lock_fps = 0.0;
            } else if (t_r0 - s_ep_t >= g_cad_epoch_ms) {
                const double out = static_cast<double>(s_ep_k) * fps;
                // No input drops. Keep the current k while the displayed rate stays in a
                // wide band around the cap; only step when it is clearly over the cap,
                // or when one more frame still fits under it. This is the balance the
                // old bridge used: do not manufacture gaps to chase an exact rate.
                if (out > target * 1.10) {
                    if (s_ep_k > 2) { --s_ep_k; }
                } else if (s_ep_k < J &&
                           static_cast<double>(s_ep_k + 1) * fps <= target * 0.92) {
                    ++s_ep_k;
                }
                s_ep_t = t_r0;
            }
            cad_want = static_cast<UINT>(s_ep_k - 1);
            cad_apply = true;
            // No input lock: the batch covers the real input interval.
            g_cad_render_ms = T;
            g_cad_paced = false;
            g_cad_m = 1;
            g_cad_q = 0.0;
            g_cad_out_hz = static_cast<double>(s_ep_k) * fps;
            g_cad_slots = T * g_refresh_hz / 1000.0;
            g_cad_T = T;
            g_cad_can = 1;
            g_cad_reuse = g_reuse_frame ? 1 : 0;
            g_cad_reason = 0;
            g_cad_j = J;
        }
    } else if (g_interp_cadence && g_refresh_hz > 0) {
        if (!g_reuse_frame) {
            // Prefer the game's own frame timestamp from the paired publication. WGC's
            // SystemRelativeTime is compositor-quantised (a locked 40 fps arrives as 2/3/4
            // vblanks), which would make even a steady input switch counts; frame_qpc_ms is
            // the game's own present clock and gives 3,3,3... for a locked 40 fps.
            // The game's own present clock. (v22: there is no WGC timestamp any more.)
            g_cad_input_ts_ms = static_cast<double>(g_ctl->frames[marker_slot].frame_qpc_ms);
            if (g_ctl && marker_slot >= 0 && marker_slot < static_cast<int>(mgpu::kMaxSlots)) {
                const double ts = static_cast<double>(g_ctl->frames[marker_slot].frame_qpc_ms);
                // Keep the game clock monotonic. A one-frame-behind marker/pair slot can hand
                // us the previous frame's timestamp; using it would re-anchor the tick every
                // frame. Accept only forward progress (or a genuine reset after a long gap).
                static double s_last_game_ts = 0.0;
                if (ts > 0.0 && (ts > s_last_game_ts ||
                                 s_last_game_ts - ts > 500.0)) {
                    g_cad_input_ts_ms = ts;
                    s_last_game_ts = ts;
                }
            }
        }
        if (!g_cad_core_ready) { cad_core_configure(); }
        mgpu_cad::Sample s;
        s.nowMs = t_r0;
        // 动态逐帧用来判断"输入多快"的时钟。默认 producer = 游戏自己的 present
        // 间隔（FrameShared.frame_time_ms）：我们自己的 sink 拷贝停顿、颜色丢帧、
        // 追赶突发都不会改变这个数，k 就不会被我们自己的抖动推着走。
        // `--cad-input arrival` 回到 WGC 到达间隔（旧行为，做 A/B）。
        double cad_t_in = (g_arr_last_raw > 0.5) ? g_arr_last_raw : arr_mean_ms();
        if (g_cad_input_producer) {
            const double p = (g_prod_interval_ms > 0.0) ? g_prod_interval_ms
                                                       : producer_interval_ms();
            if (p > 0.0) {
                cad_t_in = p;
                static bool told_prod = false;
                if (!told_prod) {
                    told_prod = true;
                    logf_("[consumer] cadence input: the game's own present interval"
                          " (%.2f ms) instead of our WGC arrival gaps - capture stalls and"
                          " colour drops no longer move k (--cad-input arrival to A/B)\n", p);
                }
            }
        }
        s.inputMs = cad_t_in;

        s.waitMs = g_ms_present;
        // FG 被 MV 门控关掉时，这一帧不会生成任何插值帧；相位与决策都不能推进，
        // 否则记账会按 k 张去减，而实际只出了 1 张（learnedJ 也会被拖垮）。
        // A frame that will present 1X (marker unreadable / publication gone) must not
        // advance the cadence phase: it asks for no generated frames this present.
        const bool pair_unverified_pre = (marker_slot < 0 && pub_fresh && !g_reuse_frame);
        s.reuse = g_reuse_frame || g_mv_gate_off || pair_unverified_pre;

        // "上一批已排空"用 Present 调用数判断（GetLastPresentCount 计的是调用，
        // 不是生成的张数；用 k 累加会让闸门永远不开）。
        bool drained = true;
        UINT last_present = 0;
        if (g_swapchain && SUCCEEDED(g_swapchain->GetLastPresentCount(&last_present))) {
            drained = (static_cast<uint64_t>(last_present) + 2u >= g_present_id);
        }
        s.canSwitch = drained;
        // 会话/资源换代：清空估计器与相位，保留学习值（Magpie 的 reset 语义）。
        if (g_ctl && g_ctl->generation != g_cad_seen_generation) {
            s.reset = true;
            g_cad_seen_generation = g_ctl->generation;
        }
        const mgpu_cad::Decision d = g_cad_core.Update(s);
        g_cad_render_ms = d.renderTimeMs;
        g_cad_paced = d.paced;
        g_cad_m = d.m;
        g_cad_q = d.q;
        g_cad_out_hz = d.outputHz;
        g_cad_slots = (g_refresh_hz > 0) ? (s.inputMs * g_refresh_hz / 1000.0) : 0.0;
        g_cad_T = s.inputMs;
        g_cad_can = drained ? 1 : 0;
        g_cad_reuse = g_reuse_frame ? 1 : 0;
        g_cad_reason = d.reason;
        g_cad_j = d.jEff;
        g_cad_regime = d.regime;
        cad_note_regime(d.regime, d.jEff, d.outputHz, d.targetHz);
        if (d.k >= 2) {
            UINT interp = static_cast<UINT>(d.k) - 1;
            if (interp > g_fg_interp_max) { interp = g_fg_interp_max; }
            cad_want = interp;                 // fg_set_interp takes the interpolated count
            cad_apply = true;
        } else {
            static bool s_fast_warned = false;
            if (!s_fast_warned) {
                s_fast_warned = true;
                logf_("[consumer] cadence core: input is faster than the panel"
                      " (%.1f fps) - 1X is not switched at runtime, running 2x;"
                      " use --no-fg for 1:1\n", 1000.0 / s.inputMs);
            }
        }
    }
    // v22: whether this frame's colour+MV+depth copies went to the input copy queue. Declared
    // before the throttle so the copy call below can fill it.
    bool cq = false;

    // Waiting for the previous frame keeps queue depth at 1: latency stays bounded and
    // "gpu" is a real measurement, but our submissions cannot overlap XeSS-FG's work.
    if (g_no_throttle) { g_ms_throttle = 0.0; } else { gpu_throttle(); }
    // v10: the previous frame's GPU work (which read its colour/MV/depth slot) has
    // completed, so hand that publication back to the producer.
    ack_publication_previous();

    // v22: now the input copies are safe. copy_publication_inputs() waits for the
    // producer's fence on the copy queue; a false return means they will be recorded on
    // this frame's own list after its reset below.
    if (!g_reuse_frame) { cq = copy_publication_inputs(slot); }
    // Apply the cadence count only now: the previous batch is retired, so XeFG sees a
    // clean batch boundary. fg_set_interp() is a no-op when the count is unchanged.
    if (cad_apply) {
        const UINT before = g_fg_interp_actual;
        fg_set_interp(cad_want, "interp-cadence", /*quiet=*/true);
        if (g_fg_interp_actual != before) { ++g_cad_switches; ++g_cad_switches_out; }
        if (g_fg_interp_actual != cad_want) { ++g_cad_fail; }
        if (cad_want < 8) { ++g_cad_hist[cad_want]; }
        g_cad_last_k = g_fg_interp_actual + 1;
    }
    // 记账：只按真正生效的 k 结算（排空失败 / DLL 拒绝时不会丢账）。
    if (g_interp_cadence && g_cad_core_ready && g_cad_epoch_ms <= 0.5) {
        const int appliedK = static_cast<int>(g_fg_interp_actual) + 1;
        const bool ok = cad_apply && (g_fg_interp_actual == cad_want);
        g_cad_core.Commit(appliedK, ok);
        g_cad_expected_k += static_cast<double>(appliedK);
    }
    const double t_work0 = qpc_ms();

    // Real frame interval for XeSS-FG's pacing model.
    const double now_qpc = qpc_ms();
    if (g_last_frame_qpc > 0.0) {
        const double dt = now_qpc - g_last_frame_qpc;
        if (dt > 0.0 && dt < 1000.0) { g_frame_ms_ema = g_frame_ms_ema * 0.9 + dt * 0.1; }
    }
    g_last_frame_qpc = now_qpc;
    if (g_xell && !g_reuse_frame) {
        const uint32_t fid = present_id;
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_START);
        // XeLL's whole job is to delay work to reduce latency, so its sleep is timed:
        // it is the one component inside run_frame that could hide tens of ms.
        const double x0 = qpc_ms();
        xellSleep(g_xell, fid);
        const double x1 = qpc_ms();
        g_ms_xell = g_ms_xell * 0.9 + (x1 - x0) * 0.1;
        if (x1 - x0 > g_max_xell) { g_max_xell = x1 - x0; }
        xellAddMarkerData(g_xell, fid, XELL_SIMULATION_END);
    }

    // v22: there is no WGC sink, no keyed mutex and no 66 MB CopyResource any more. The
    // publication's colour is copied by copy_publication_inputs() (copy queue) or on this
    // frame's own list in the fallback; `colour=` now measures that copy. The convert
    // pass reads the shared sink exactly as before.
    t_m3 = qpc_ms();

    // 3. MV/depth slot paired with this colour: one frame behind, because WGC
    //    only delivers a frame after it has been presented. On a re-presented frame
    //    the slot is unchanged, so nothing has to be waited for.
    // Ring mode: the consumer's local ring is the source of truth. marker_slot is a ring
    // index (see above); the producer transit slot is irrelevant for tagging.
    const UINT slots = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    bool pf_hit = false;          // v22: no local backlog - inputs are copied per publication
    g_of_miss = false;
    // v22: copy this publication's colour + MV/depth AFTER the previous batch has retired
    // (right after gpu_throttle). The sink is a single texture now, so copying it early
    // would overwrite pixels the previous convert pass may still be reading.
    g_pair_seq = g_marker_seq;
    if (!g_reuse_frame) {
        g_last_slot = slot;
        g_have_last = true;
    }

    // Producer ACKs are handled from the ring (prod slot + publication); a ring index is
    // not a producer slot index, so nothing is written here.

    // Cached: see back_buffer(). The buffer identity is fixed for the life of the swap
    // chain, so this also removes the per-frame QueryInterface/refcount traffic.
    ID3D12Resource* bbres = nullptr;
    const UINT bbi = g_swapchain->GetCurrentBackBufferIndex();
    if (!back_buffer(bbi, &bbres)) { return; }
    D3D12_RESOURCE_STATES& bb_state = g_bb_state[bbi & 3];

    g_alloc->Reset();
    g_list->Reset(g_alloc.Get(), nullptr);

    // v22 fallback: the input copy queue was not usable, so record this publication's
    // colour + MV/depth on this frame's own list (they then run before the convert draw).
    if (!g_reuse_frame && !cq) { copy_inputs_local(slot); }

    // A miss with no depth history at all (startup / right after a generation change) needs
    // a valid depth resource for the tag and for the flow's guide: use the flat dummy at
    // the stream size. Steady-state misses use the last depth from the ring.
    if (g_of_miss && g_ring_count == 0) {
        ensure_dummy_depth(slot);
        if (g_depth_local[slot]) { of_bind_depth(0, g_depth_local[slot].Get()); }
    }

    // (The pre-convert sink patch that used to live here is gone: the marker is now
    //  repaired inside the convert pass itself, and its tile is no longer transported.)

    D3D12_RESOURCE_BARRIER bb[2]{};
    bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource = bbres;
    bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[0].Transition.StateBefore = bb_state;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    bb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[1].Transition.pResource = g_shared12.Get();
    bb[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_list->ResourceBarrier(2, bb);

    // --fg-of: motion vectors from the colour we just captured. Recorded before the
    // convert draw so the colour is still in COMMON for the flow dispatch; of_record
    // restores COMMON on the way out, which is what the barrier above assumes.
#if MGPU_FG_OF
    if (g_of_active || g_of_track) {
        const double o0 = qpc_ms();
        if (!g_of_ready) { of_init(); }
        if (of_record(g_list.Get())) {
            g_of_fresh = true;
            const double o1 = qpc_ms();
            g_ms_of = g_ms_of * 0.9 + (o1 - o0) * 0.1;
            if (o1 - o0 > g_max_of) { g_max_of = o1 - o0; }
        }
    }
#endif

    // scRGB (FP16) -> BT.2020 + PQ in R10G10B10A2: the format XeSS-FG requires.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbi & 3) * g_rtv_stride;
    if (!g_rtv_res[bbi & 3]) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(bbres, &rtv_desc, rtv);
        g_rtv_res[bbi & 3] = bbres;
    }

    // Draw to the real back buffer size. XeSS-FG creates the native swap chain
    // itself, so its size need not equal the captured size; a viewport that is too
    // small leaves an uncovered border of uninitialised pixels around the picture.
    const D3D12_RESOURCE_DESC bb_desc = bbres->GetDesc();
    const UINT draw_w = static_cast<UINT>(bb_desc.Width);
    const UINT draw_h = bb_desc.Height;

    // Checked on the first frame, not during init: the window is only resized to the
    // borderless size after the swap chain exists, so checking earlier reported a
    // bogus mismatch. A present size that differs from the client area means DXGI
    // stretches the picture - that is the "everything looks scaled" symptom.
    if (!g_present_size_checked) {
        g_present_size_checked = true;
        RECT cr{};
        if (g_bridge_wnd && GetClientRect(g_bridge_wnd, &cr)) {
            const UINT cw = static_cast<UINT>(cr.right - cr.left);
            const UINT ch = static_cast<UINT>(cr.bottom - cr.top);
            if (cw && ch && (cw != draw_w || ch != draw_h)) {
                logf_("[consumer] WARNING: present size %ux%u != window client %ux%u - DXGI will"
                      " stretch the picture. Align the capture, the output and the monitor"
                      " (see --den/--scale).\n", draw_w, draw_h, cw, ch);
            } else {
                RECT wr{};
                GetWindowRect(g_bridge_wnd, &wr);
                logf_("[consumer] present %ux%u == window client %ux%u (1:1, no stretch)"
                      " window rect=(%ld,%ld)-(%ld,%ld) monitor=%ux%u\n",
                      draw_w, draw_h, cw, ch, wr.left, wr.top, wr.right, wr.bottom,
                      g_full_w, g_full_h);
            }
        }
    }

    D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h),
                       0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(draw_w), static_cast<LONG>(draw_h) };
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &scissor);
    g_list->SetGraphicsRootSignature(g_conv_root.Get());
    g_list->SetPipelineState(g_conv_pso.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srv_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE tbl = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        tbl.ptr += static_cast<SIZE_T>(g_sink) * g_srv_stride;   // this frame's sink
        g_list->SetGraphicsRootDescriptorTable(0, tbl);
    }
    float conv_consts[12];
    conv_constants(conv_consts, draw_w, draw_h);
    // In-pass marker repair: the convert shader replaces the four 8x8 corner blocks with
    // their inward neighbour while it converts, so no extra copy/barrier pass runs per
    // frame (that pass cost ~10 fps at 2x). Constant 11 - 8/9 are the prepare pass's
    // source size.
    conv_consts[11] = marker_drawn_recently() ? 1.0f : 0.0f;
    g_list->SetGraphicsRoot32BitConstants(1, 12, conv_consts, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);

    // The four marker blocks are repaired INSIDE the conversion pass above
    // (conv_consts[8] -> ps_main), so nothing is copied into this backbuffer here.
    marker_probe_overlay(g_list.Get(), bbres, bbi, draw_w, draw_h);
    // Diagnostics that need the presented frame as COPY_SOURCE: the 32x32 block dump
    // (marker_out.raw) and the whole frame (frame_dump.raw). Both are opt-in through
    // dump_frame.on because the transition pair costs frame time.
    if (frame_dump_wanted()) {
        D3D12_RESOURCE_BARRIER db{};
        db.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        db.Transition.pResource = bbres;
        db.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        db.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        db.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_list->ResourceBarrier(1, &db);
        if (marker_drawn_recently()) {
            uint32_t c0x = 0, c0y = 0;
            mgpu::marker_corner(0, draw_w, draw_h, &c0x, &c0y);
            marker_dbg_record(g_list.Get(), bbres, c0x, c0y);
        }
        frame_dump_record(g_list.Get(), bbres);
        db.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        db.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &db);
    }

    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &bb[1]);

    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &bb[0]);
    bb_state = D3D12_RESOURCE_STATE_PRESENT;

    // 4. tag the FG inputs. The producer's resources live on a heap imported from a file
    //    mapping with ALLOW_CROSS_ADAPTER; XeSS-FG fails to copy those ("Failed to
    //    allocate memory for a copy of DEPTH"), so plain local copies are tagged instead.
    //    Those copies pull data that physically lives on the NVIDIA card across to the
    //    AMD one, so both the recording time and the GPU wait for the previous frame are
    //    worth watching.
    //
    //    --input-copy-queue: they are recorded into the copy queue's list here and start
    //    executing while this list's convert draw is still being recorded; the direct
    //    queue is made to wait for them *after* the submit below, so the two overlap and
    //    XeSS-FG's work (submitted at Present) still sees finished data.
    //    --prefetch-inputs: normally they were already recorded during the *previous*
    //    frame's Present, so this frame only queues the wait.
    // The lookup itself happened before the OF/colour list (see above): a prefetch hit
    // needs no copy, and a miss now uses the flow instead of a fallback copy.
    // Ring mode: the inputs were copied into the consumer ring when the publication
    // appeared; there is nothing to copy in-frame. A miss means the ring entry is gone
    // (lag > ring depth) and the frame presents 1X rather than guessing.
    if (g_upsample && pf_hit) { record_upsample(slot); }

    // ---- exact-pair verification (AFTER the input path) --------------------
    // The slot/copy we are about to tag must carry the id this colour's marker decoded.
    // If it does not, nothing is tagged and the frame goes out as 1X. This is what makes
    // a "hit" mean "the RIGHT publication", not merely "some publication".
    // The local copy's id wins over the live slot: the producer may reuse the shared slot
    // as soon as our copy retired, while the local data is still the colour's publication.
    {
        static bool s_prev_verified = false;
        // v23.1: the identity is decided at pick time and is not re-read from the live
        // slot. The producer used to overwrite a slot the consumer had picked (and now
        // skips instead), which made this check fail on frames whose colour/MV were in
        // fact copied from one publication - flapping the MV gate and disabling FG.
        if (g_reuse_frame) {
            g_pair_verified = s_prev_verified;
        } else {
            g_pair_verified = (g_ctl != nullptr && marker_slot >= 0 && g_marker_seq != 0);
            s_prev_verified = g_pair_verified;
        }
        if (!g_pair_verified && marker_slot >= 0 && !g_reuse_frame) { ++g_pair_mismatch; }
        if (!g_pair_verified && !g_reuse_frame && g_pub_fresh) { ++g_pair_lost; }
        if (g_pair_verified && g_ctl) {
            const uint64_t id = g_marker_seq;   // v23.1: identity from pick time
            const uint64_t lag = (g_ctl->frame_published >= id)
                                     ? (g_ctl->frame_published - id) : 0;
            if (lag <= 0xFFFFu) {
                if (lag > g_pair_lag_max) { g_pair_lag_max = static_cast<UINT>(lag); }
                g_pair_lag_sum += lag;
                ++g_pair_lag_n;
            }
        }
        (void)slot;
    }

    // Policy (2026-11-11): no motion vectors -> no frame generation. Menus, loading screens and
    // transitions legitimately carry no MV, and the capture (correctly) refuses to invent a
    // state for a texture the title never transitions. Presenting the real frame there is the
    // right answer; generating from a stale field is what produced artefacts.
    {
        static bool gated_off = false;
        // A stalled producer (menu/pause/loading) means the vectors we would tag are
        // stale: drop to 1X. A single unverified colour is NOT a reason to reconfigure
        // XeFG: doing that per frame (53 toggles in one 43 fps run) produced ERR_OTHER
        // and cut the loop to 23 fps, which made the colour lag worse. Unverified frames
        // simply do not tag (see below) and XeFG keeps its last tagged inputs.
        const bool mv_available = g_ctl &&
                                  (g_ctl->active_mask & mgpu::kRequestMv) != 0 &&
                                  g_pub_stall_frames < 8u &&
                                  g_pair_verified;   // pairing correctness wins: no stale MV
        if (!mv_available && !gated_off) {
            const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 0);
            logf_("[consumer] producer stalled/no MV -> 1X (%s, %d);"
                  " FG resumes when it returns\n", xefg_str(r), (int)r);
            if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { gated_off = true; }
        } else if (mv_available && gated_off) {
            const xefg_swapchain_result_t r = g_api.SetEnabled(g_fg, 1);
            logf_("[consumer] MV back -> FG on (%s, %d)\n", xefg_str(r), (int)r);
            if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) { gated_off = false; }
        }
        // Nothing to tag while the gate is closed: the swap chain is in 1X and the real frame
        // goes straight out. (An early return here would skip Close()/Present.)
        g_mv_gate_off = gated_off;
    }

    xefg_swapchain_d3d12_resource_data_t rd{};
    rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    rd.resourceBase = { 0, 0 };
    // --input-copy-queue: the copies are not in this list, so neither are the tags. The
    // library only needs the list for resources that stop being valid before the next
    // present; ours live until then (see the switch's comment).
    ID3D12CommandList* tag_list = (cq || pf_hit) ? nullptr : g_list.Get();
    // MV and depth can be different sizes (UE5: MVs at render size, depth at full res), so
    // each is tagged with its own stream size - the old code used streams[1] for both.
#if MGPU_FG_OF
    if (g_of_active && g_of_ready && g_of_fresh) {
        // Our own field: 1/8 resolution, screen-UV units, computed from this frame's
        // colour on this GPU (the producer's MV stream measured as garbage). Requiring
        // g_of_fresh keeps a stale field from being tagged when the flow did not run.
        rd.resourceSize = { g_of_out_w, g_of_out_h };
        rd.pResource = g_of_mv.Get();
    } else
#endif
    {
        // --upsample: both streams were rebuilt to the render size by the joint pass, so
        // the pair below is same-size by construction (XeFG rejects mismatches).
        if (g_upsample && g_up_valid && g_up_slot == slot && g_up_mv) {
            rd.resourceSize = { g_up_w, g_up_h };
            rd.pResource = g_up_mv.Get();
        } else {
            rd.resourceSize = { g_ctl->streams[1].width, g_ctl->streams[1].height };
            rd.pResource = g_mv_local[slot].Get();
        }
    }
    rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
    rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    // 资源可能还没建出来（首帧 / generation 刚变 / 局部拷贝失败）。把空指针交给
    // XeFG 会让它返回 ERR_OTHER（每次运行的 frame 1 就是它），不如这一帧不打 tag，
    // 让它按"没有 MV"处理，并计数一次——比静默传 null 好排查。
    if (!g_mv_gate_off && rd.pResource == nullptr) {
        static int told = 0;
        if (told++ < 4) {
            logf_("[consumer] tag: motion vectors not ready for slot %u - skipping the MV tag"
                  " this frame (fg result would be ERR_OTHER)\n", slot);
        }
    }
    // Unverified frame: do NOT tag. XeFG keeps the last verified inputs; this avoids both
    // a wrong (future) MV and the per-frame SetEnabled churn that broke 43 fps runs.
    if (g_pair_verified && !g_mv_gate_off && rd.pResource) {
        g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd);
    }


    if (g_upsample && g_up_valid && g_up_slot == slot && g_up_depth) {
        rd.resourceSize = { g_up_w, g_up_h };
        rd.pResource = g_up_depth.Get();
    } else if (g_pf_pub && g_depth_local[slot]) {
        // Copy-on-publication cache: this is the exact publication the lookup selected.
        rd.resourceSize = { g_ctl->streams[2].width, g_ctl->streams[2].height };
        rd.pResource = g_depth_local[slot].Get();
    } else {
        rd.resourceSize = { g_ctl->streams[2].width, g_ctl->streams[2].height };
        rd.pResource = g_depth_local[slot].Get();
        if (!g_no_depth && g_ring_need > 0 && g_ring_count > 0 && g_depth_ring[0]) {
            UINT back = static_cast<UINT>((g_depth_lag > 0) ? g_depth_lag : 0);
            if (back >= g_ring_count) { back = g_ring_count - 1; }
            const UINT idx = (g_ring_write + g_ring_need - back) % g_ring_need;
            if (g_depth_ring[idx]) {
                rd.pResource = g_depth_ring[idx].Get();
#if MGPU_FG_OF
                g_of_depth_desc = idx;          // guide next frame's upsample with it
#endif
            }
        }
    }
    rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
    if (g_pair_verified && !g_mv_gate_off && rd.pResource) {
        g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd);
    } else if (g_pair_verified && !g_mv_gate_off) {
        static int told_d = 0;
        if (told_d++ < 4) {
            logf_("[consumer] tag: depth not ready for slot %u - skipping the depth tag"
                  " this frame\n", slot);
        }
    }


    // The UI travels as its own stream so the picture above can stay HUDless. Only tagged
    // when the UI stream exists (uiMode AUTO was chosen at init from the same condition).
    // Only tag a UI that the producer actually wrote this frame. A stale UI texture is worse
    // than none: XeFG would composite the previous frame's overlay at the new frame's geometry.
    if (slot < mgpu::kMaxSlots && g_ui_local[slot] &&
        (g_ctl->active_mask & mgpu::kRequestUi) != 0 &&
        g_ctl->streams[3].width >= 64 && g_ctl->streams[3].height >= 64) {
        rd.resourceSize = { g_ctl->streams[3].width, g_ctl->streams[3].height };
        rd.pResource = g_ui_local[slot].Get();
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        rd.type = XEFG_SWAPCHAIN_RES_UI;
        if (!g_mv_gate_off) { g_api.D3D12TagFrameResource(g_fg, tag_list, present_id, &rd); }
    }

    // --dump: record the readback copies in this list, i.e. after everything that
    // feeds XeSS-FG and before Close, so they see exactly what was tagged.
    if (g_dump_frames > 0) { dump_record(g_list.Get(), slot, present_id); }

    g_list->Close();
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_START); }
    // The in-list scar restore reads the cross-adapter slot; order the whole list
    // after the publication that filled it. The colour arrived through WGC, so the
    // publication is already complete in practice - this is a GPU-side no-op that
    // removes the race, not a stall.
    // v22: the colour copy is on the copy queue, and the convert draw (in this list) reads
    // the sink it fills. The direct queue must wait for that copy BEFORE the list runs -
    // otherwise the barrier below transitions a texture that is still being written.
    if (cq) { input_copy_wait(); }
    ID3D12CommandList* lists[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    if (g_dbg_pending && g_dbg_fence) {
        g_queue->Signal(g_dbg_fence.Get(), g_dbg_value);
    }
    if (g_fdump_pending && g_fdump_fence) {
        g_queue->Signal(g_fdump_fence.Get(), g_fdump_value);
    }
    // --input-copy-queue: the wait was queued before the list (the convert pass reads the
    // sink the copies fill), so nothing further is needed here.
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_RENDERSUBMIT_END); }
    g_ms_work = g_ms_work * 0.9 + (qpc_ms() - t_work0) * 0.1;

    // 5. frame constants: jitter from the producer, approximate matrices
    // v11: what the producer *measured* about its own inputs, logged on change. The
    // switches (--mv-flip / --fg-mv-scale / --fg-depth-inv) still win for now; the next
    // step is to let these drive them, which is why they are transported at all
    // (docs/findings/INPUT_SEMANTICS.md).
    {
        static uint32_t logged_sem = 0xFFFFFFFFu;
        const uint32_t sem = g_ctl->frames[slot].semantics;
        if (sem != logged_sem) {
            logged_sem = sem;
            const char* unit = "unknown";
            switch ((sem >> mgpu::kSemMvUnitShift) & 3u) {
            case 1:  unit = "pixels"; break;
            case 2:  unit = "uv"; break;
            case 3:  unit = "ndc"; break;
            default: break;
            }
            const char* sgn = "unknown";
            switch ((sem >> mgpu::kSemMvSignShift) & 3u) {
            case 1:  sgn = "prev-cur"; break;
            case 2:  sgn = "cur-prev"; break;
            default: break;
            }
            const char* dep = "unknown";
            switch ((sem >> mgpu::kSemDepthShift) & 3u) {
            case 1:  dep = "reversed"; break;
            case 2:  dep = "forward"; break;
            default: break;
            }
            const unsigned q = (sem >> mgpu::kSemMvQualityShift) & 0xFFu;
            const bool q_known = (sem & mgpu::kSemQualityKnown) != 0;
            // "(declared)" is not "(unknown)": since 2026-11-10 the producer publishes the
            // API's own declaration for the sign as well (it is the contract), so the probe
            // suffix now distinguishes *measured* from *declared* and from neither.
            const char* sign_src = (sem & mgpu::kSemSignMeasured) ? "(measured)"
                                   : (sgn[0] != 'u' ? "(declared)" : "(unknown)");
            if (q_known) {
                logf_("[consumer] producer semantics: mv=%s%s sign=%s%s depth=%s%s quality=%u/255%s"
                      " valid=%ux%u\n",
                      unit, (sem & mgpu::kSemMvMeasured) ? "(measured)" : "(declared)",
                      sgn, sign_src,
                      dep, (sem & mgpu::kSemDepthMeasured) ? "(measured)" : "(declared)",
                      q, (sem & mgpu::kSemMvUsable) ? "  [MV usable]" : "  [MV NOT usable]",
                      g_ctl->frames[slot].valid_w, g_ctl->frames[slot].valid_h);
            } else {
                // No sign test has run yet (needs two consecutive frames plus depth):
                // say so instead of pretending the quality is zero.
                logf_("[consumer] producer semantics: mv=%s%s sign=%s%s depth=%s%s"
                      " quality=unknown (sign test has not run yet) valid=%ux%u\n",
                      unit, (sem & mgpu::kSemMvMeasured) ? "(measured)" : "(declared)",
                      sgn, sign_src, dep, (sem & mgpu::kSemDepthMeasured) ? "(measured)" : "(declared)",
                      g_ctl->frames[slot].valid_w, g_ctl->frames[slot].valid_h);
            }
        }
    }

    xefg_swapchain_frame_constant_data_t cd{};
    // The projection has to match the aspect the frame was RENDERED at, not the one we
    // capture/present at. They are almost always identical, but the render size is the
    // honest source (and it is the only one that also matches the MV/depth field).
    UINT proj_w = g_ctl->frames[slot].render_w;
    UINT proj_h = g_ctl->frames[slot].render_h;
    if (proj_w < 64 || proj_h < 64) { proj_w = g_width; proj_h = g_height; }
    const float const_aspect = static_cast<float>(proj_w) / static_cast<float>(proj_h);
    // v6: a real camera beats --fg-fov. FSR 3.1 exports near/far/vfov; DLSS-SR does
    // not, so those games keep the --fg-fov / --fg-zn constants.
    const mgpu::FrameShared& fr = g_ctl->frames[slot];
    // 给下一帧的 cadence 决策用：这一帧（已配对）的输入间隔。
    //
    // 信源选择（2026-11-15，第三次修正）：用 FrameShared.frame_time_ms。
    // producer 的注释写明它是「publish 之间」的 EMA —— publish 每*呈现帧*一次，
    // 所以它就是我们要的帧间隔。另外两个候选都被证明更差：
    //   * frame_qpc_ms 是 Evaluate 时刻，一帧会 Evaluate 多次（CP2077 约 3.3 次），
    //     相邻值只隔 7.5ms；按 frame_id 归一化后仍在 12–27ms 之间乱跳（实测 T=19）。
    //   * 我们自己处理两帧的 Δqpc 含丢弃与节流，完全不能用。
    // 这里再对最近 16 个样本取中位数，压掉 EMA 的尖峰。
    {
        static double ring[16] = {};
        static UINT n = 0, w = 0;
        static double prev_qpc = 0.0;
        static uint64_t prev_id = 0;
        const float fms = fr.frame_time_ms;
        if (fms > 3.0f && fms < 100.0f) {
            ring[w] = static_cast<double>(fms);
            w = (w + 1) % 16u;
            if (n < 16) { ++n; }
        }

        // 诊断：用游戏时间戳算的"每条 frame_id 的间隔"，只写日志、不参与决策。
        {
            static double qring[16] = {};
            static UINT qn = 0, qw = 0;
            const double tq = static_cast<double>(fr.frame_qpc_ms);
            const uint64_t id = fr.frame_id;
            if (tq > 1.0) {
                if (prev_qpc > 0.0 && tq > prev_qpc && (tq - prev_qpc) < 250.0 &&
                    id > prev_id && (id - prev_id) <= 8) {
                    const double dt = (tq - prev_qpc) / static_cast<double>(id - prev_id);
                    if (dt > 3.0) {
                        qring[qw] = dt;
                        qw = (qw + 1) % 16u;
                        if (qn < 16) { ++qn; }
                    }
                }
                prev_qpc = tq;
                prev_id = id;
            }
            if (qn > 0) {
                double v[16];
                for (UINT i = 0; i < qn; ++i) { v[i] = qring[i]; }
                for (UINT i = 1; i < qn; ++i) {
                    const double key = v[i];
                    int j = static_cast<int>(i) - 1;
                    while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
                    v[j + 1] = key;
                }
                g_prod_qpc_ms = v[qn / 2];
            }
        }

        if (n > 0) {
            double v[16];
            for (UINT i = 0; i < n; ++i) { v[i] = ring[i]; }
            for (UINT i = 1; i < n; ++i) {
                const double key = v[i];
                int j = static_cast<int>(i) - 1;
                while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
                v[j + 1] = key;
            }
            g_prod_interval_ms = v[n / 2];
        }
    }

    // Bucket the producer's own present interval exactly like the composition-side arrival
    // histogram, so the two can be compared row by row (see g_game_hist).
    if (fr.frame_time_ms > 0.5f && fr.frame_time_ms < 1000.0f) {
        const double vb_g = refresh_ms();
        if (vb_g > 0.5) {
            int kg = static_cast<int>(static_cast<double>(fr.frame_time_ms) / vb_g + 0.5);
            if (kg < 1) { kg = 1; }
            if (kg > 8) { kg = 8; }
            ++g_game_hist[kg];
        }
    }
    const bool cam_ok = ((g_producer_flags & mgpu::kInputFlagHasCamera) != 0) &&
                        std::isfinite(fr.near_plane) && fr.near_plane > 0.0f &&
                        fr.far_plane > fr.near_plane &&
                        fr.vfov_rad > 0.05f && fr.vfov_rad < 2.5f;
    // ---- v12: the game's own view-to-clip, when the producer had one ----
    // DLSS-SR exports no camera, so every NGX title used to get a synthesised projection from
    // --fg-fov (60 deg, 16:9-ish aspect). The add-on now forwards Streamline's sl::Constants,
    // which is documented as jitter-free, so this is the game's real aspect/FOV/far-plane form
    // including the infinite reversed-Z. Sanity checks: the matrix must look like a projection
    // (same shape test OptiScaler uses) and must not be stale.
    bool have_matrix = false;
    float mtx_fov = 0.0f;                       // derived vertical FOV, radians
    float mtx_aspect = const_aspect;            // derived aspect (b/a)
    {
        const float* m = fr.view_to_clip;
        bool finite = true;
        for (int i = 0; i < 16; ++i) { if (!std::isfinite(m[i])) { finite = false; break; } }
        const bool shape = std::fabs(m[15]) < 1.0e-2f &&
                           std::fabs(m[1]) < 1.0e-3f && std::fabs(m[4]) < 1.0e-3f &&
                           (std::fabs(std::fabs(m[11]) - 1.0f) < 1.0e-3f ||
                            std::fabs(std::fabs(m[14]) - 1.0f) < 1.0e-3f);
        const uint64_t now = static_cast<uint64_t>(qpc_ms());
        const bool fresh = fr.matrix_tick_ms != 0 && now >= fr.matrix_tick_ms &&
                           (now - fr.matrix_tick_ms) < 500ull;
        have_matrix = finite && shape && fresh &&
                      (g_producer_flags & mgpu::kInputFlagHasMatrix) != 0 && !g_fg_no_proj;
        if (have_matrix) {
            // Derive the numbers OptiScaler derives (2*atan(1/b) is the vertical FOV; b/a is the
            // aspect), and keep near/far from whatever the source declared - the matrix-derived
            // near/far are the part their code distrusts on UE titles.
            const float a = std::fabs(fr.view_to_clip[0]);
            const float b = std::fabs(fr.view_to_clip[5]);
            if (b > 1.0e-6f) {
                mtx_fov = 2.0f * std::atan(1.0f / b);
            }
            if (a > 1.0e-6f && b > 1.0e-6f) {
                const float asp = b / a;
                if (asp > 0.4f && asp < 4.0f) { mtx_aspect = asp; }
            }
        }
        static bool logged_matrix = false;
        if (have_matrix && !logged_matrix) {
            logged_matrix = true;
            logf_("[consumer] projection: from the game's own view-to-clip (v12): fovY=%.2fdeg"
                  " aspect=%.3f%s\n"
                  "[consumer]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[consumer]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[consumer]   [%+.4f %+.4f %+.4f %+.4f]\n"
                  "[consumer]   [%+.4f %+.4f %+.4f %+.4f]\n",
                  mtx_fov * 57.2957795f, mtx_aspect,
                  g_fg_use_matrix ? " (used verbatim: --fg-use-matrix)" : " (derived; --fg-fov retired)",
                  m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                  m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    const float proj_fov = (have_matrix && mtx_fov > 0.05f)
                               ? mtx_fov
                               : (cam_ok ? fr.vfov_rad : (g_fg_fov_y * 3.14159265f / 180.0f));
    const float proj_aspect = have_matrix ? mtx_aspect : const_aspect;
    // The game's own projection wins, unconditionally (decision 2026-11-10). We do *not*
    // second-guess it against --fg-fov: some integrations hand XeFG a plain 60 deg placeholder,
    // and that is still the game's own answer for its own FG - overriding it with our guess was
    // the worse bet. The line below exists so a mismatch is visible instead of silent.
    if (have_matrix && std::fabs(mtx_fov * 57.2957795f - g_fg_fov_y) > 5.0f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf_("[consumer] note: the game's projection says fovY=%.2fdeg, --fg-fov says %.2f -"
                  " keeping the game's (its own FG uses it)\n",
                  mtx_fov * 57.2957795f, g_fg_fov_y);
        }
    }
    const float proj_zn  = cam_ok ? fr.near_plane : g_fg_near_z;
    const float proj_zf  = cam_ok ? fr.far_plane : 10000.0f;
    // An infinite far plane (what a reversed-Z game reports) only makes sense in the
    // reversed-Z form; the finite form divides by infinity and produces NaNs.
    const bool use_rz = g_fg_proj_rz || !std::isfinite(proj_zf);
    if (have_matrix && g_fg_use_matrix) {
        for (int i = 0; i < 16; ++i) { cd.projectionMatrix[i] = fr.view_to_clip[i]; }
    } else if (use_rz) { approx_projection_rz(cd.projectionMatrix, proj_aspect, proj_fov, proj_zn); }
    else { approx_projection(cd.projectionMatrix, proj_aspect, proj_fov, proj_zn, proj_zf); }
    cd.viewMatrix[0] = 1.0f;
    cd.viewMatrix[5] = 1.0f;
    cd.viewMatrix[10] = 1.0f;
    cd.viewMatrix[15] = 1.0f;
    if (g_fg_no_proj) {
        // Test switch: hand XeFG a plain identity instead of our aspect guess.
        for (int i = 0; i < 16; ++i) { cd.projectionMatrix[i] = 0.0f; }
        cd.projectionMatrix[0] = cd.projectionMatrix[5] = cd.projectionMatrix[10] = 1.0f;
        cd.projectionMatrix[15] = 1.0f;
    }
    // Always the producer's jitter: the game's motion vectors are computed on the jittered
    // render, so this is what XeFG needs to align its history (see g_fg_jitter_zero above).
    cd.jitterOffsetX = g_ctl->frames[slot].jitter_x;
    cd.jitterOffsetY = g_ctl->frames[slot].jitter_y;
    if (g_fg_jitter_zero) {
        // A/B: our colour is the post-upscaler image, i.e. the jitter is already resolved.
        cd.jitterOffsetX = 0.0f;
        cd.jitterOffsetY = 0.0f;
    }
    if (g_fg_jitter_flip) {
        cd.jitterOffsetX = -cd.jitterOffsetX;
        cd.jitterOffsetY = -cd.jitterOffsetY;
    }
    // The producer knows how its vectors are scaled (NGX MV.Scale.X/Y; FSR's
    // motionVectorScale is in pixels, e.g. (2880, 1620)). Pass it through instead of
    // assuming 1.0 and use --mv-flip's sign for the (previous-current) convention.
    cd.motionVectorScaleX = g_ctl->frames[slot].mv_scale_x;
    cd.motionVectorScaleY = g_ctl->frames[slot].mv_scale_y;
    if (cd.motionVectorScaleX == 0.0f && cd.motionVectorScaleY == 0.0f) {
        cd.motionVectorScaleX = 1.0f;
        cd.motionVectorScaleY = 1.0f;
    }
    // --fg-mv-scale: the two conventions seen in the field. CP2077 reports (2560, 1440) and
    // its texels are UV; PBZ reports (1, 1) while XeFG documents its vectors as
    // "screen-space motion in pixels", so 'px' turns the latter into pixels and the two can
    // be compared instead of argued about (docs/findings/UE5_MV_OPTISCALER.md 5.2).
    if (g_fg_mv_scale_mode == 1) {
        const float sx = static_cast<float>(g_ctl->streams[1].width ? g_ctl->streams[1].width : 1u);
        const float sy = static_cast<float>(g_ctl->streams[1].height ? g_ctl->streams[1].height : 1u);
        cd.motionVectorScaleX *= sx;
        cd.motionVectorScaleY *= sy;
    } else if (g_fg_mv_scale_mode == 2) {
        cd.motionVectorScaleX = g_fg_mv_scale_force;
        cd.motionVectorScaleY = g_fg_mv_scale_force;
    }
    // --fg-of: the producers' MV.Scale describes THEIR texture, not ours. Our optical-flow
    // pass writes screen UV of the captured frame (480x270 SINT -> UV), so with the
    // producer's scale - 1.0 on PBZ - the library multiplied those UV values by 1 and saw
    // ~0.009 px of motion instead of ~35 px. That is exactly why "optical flow" looked
    // identical to "no motion vectors" (PBZ, 2026-11-09).
    // The space is the render/MV one, not the 4K output: every game that reports a usable
    // MV.Scale reports its render size there (CP2077: 2560x1440), and XeFG's definition of
    // "pixels" has to agree with the depth/MV grid it gets.
    if (g_of_active && g_of_fresh) {
        cd.motionVectorScaleX = static_cast<float>(g_ctl->streams[1].width ? g_ctl->streams[1].width : 1u);
        cd.motionVectorScaleY = static_cast<float>(g_ctl->streams[1].height ? g_ctl->streams[1].height : 1u);
    }
    if (g_mv_sign < 0.0f) {
        cd.motionVectorScaleX = -cd.motionVectorScaleX;
        cd.motionVectorScaleY = -cd.motionVectorScaleY;
    }
    // v13: the producer's own FG declaration can invalidate XeFG history on scene cuts
    // and teleports. This is metadata from sl::Constants.reset / DLSSG.Reset; it costs
    // nothing and is safer than guessing from a frame-time spike.
    cd.resetHistory = (fr.fg_flags & mgpu::kInputFlagResetHistory) ? 1u : 0u;
    if (cd.resetHistory) { ++g_reset_producer_count; }
    if (g_force_reset_history.exchange(false)) {
        cd.resetHistory = 1u;   // discard history poisoned by the dropped/mismatched input
        static int reset_logs = 0;
        if (reset_logs < 8) {
            // 归因：pair-gap / colour-drop 分开报。carry 迭代的第 2 项是"配对缺口
            // 不 reset，继续复用上一组输入"，先让日志能证明是这一类。
            logf_("[consumer] reset-history: forced for frame seq=%llu"
                  " (pair-gap %u, colour-drop %u, producer %u so far)\n",
                  static_cast<unsigned long long>(fr.frame_id),
                  g_reset_pair_gap_count, g_reset_colour_drop_count.load(),
                  g_reset_producer_count);

            ++reset_logs;
        }
    }

    if (!g_fg_consts_logged) {
        g_fg_consts_logged = true;
        char fov_buf[64]{};
        if (cam_ok) {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=producer fov=%.1fdeg zn=%.2f zf=%.0f",
                        proj_fov * 180.0f / 3.14159265f, proj_zn, proj_zf);
        } else {
            _snprintf_s(fov_buf, sizeof(fov_buf), _TRUNCATE,
                        "camera=synthetic (--fg-fov %.1f, --fg-zn %.2f)",
                        g_fg_fov_y, g_fg_near_z);
        }
        logf_("[consumer] frame constants: jitter=(%.4f, %.4f)%s proj=%s %s"
              " aspect=%.5f (%ux%u) mvscale=(%.4f, %.4f) game_dt=%.2fms\n",
              cd.jitterOffsetX, cd.jitterOffsetY,
              g_fg_jitter_flip ? " (flipped)" : "",
              g_fg_no_proj ? "identity"
                           : (use_rz ? "reversed-Z approximation" : "aspect approximation"),
              fov_buf, const_aspect, proj_w, proj_h,
              cd.motionVectorScaleX, cd.motionVectorScaleY,
              static_cast<double>(fr.frame_time_ms));
    }
    // XeFG's pacing model eats this. OptiScaler lets it be the input frame time,
    // our present interval, or zero ([FrameGen] FTInput). We measured that the
    // input frame time works best for 2x; --fg-rt 0 is there to test whether 0
    // stops the library from budgeting the whole batch.
    // --fg-rt-mode picks *which* estimate of the input frame time: the historical
    // EMA (default), the raw last interval, a 16-sample median that ignores WGC's
    // +-1 vblank quantization, or that median snapped to whole vblanks.
    const double rt_ms = fg_render_time_ms();
    cd.frameRenderTime = static_cast<float>(rt_ms);
    // v6 cross-check: the add-on measures the game's own present interval. A large
    // mismatch means our capture cadence is not the game's (WGC batching, a limiter
    // in between), and frameRenderTime comes from *our* estimate - worth one line.
    //
    // The producer publishes a RAW single interval (FrameShared.frame_time_ms, not an EMA),
    // so comparing it against our *median* fired on a perfectly healthy beat pattern: a game
    // at ~44 fps on a 120 Hz panel alternates 2/3 vblank presents (16.7 / 25.0 ms), our
    // median reads 16.7 and one raw 3-vblank sample reads 25-26 ms -> "WGC sees 60 fps but
    // the game only renders 38". That is a false positive and it cost a debugging session on
    // 2026-11-09 (the game's own frame generation was in fact OFF). Both sides now use a
    // median over the same window, so only a real rate mismatch can trigger it.
    {
        static unsigned dt_frames = 0;
        static bool dt_warned = false;
        // 16-sample medians, one per frame, of the producer's own present interval.
        static float s_ring[16] = {};
        static UINT  s_n = 0, s_i = 0;
        if (fr.frame_time_ms > 0.5f && fr.frame_time_ms < 1000.0f) {
            s_ring[s_i] = fr.frame_time_ms;
            s_i = (s_i + 1) % 16u;
            if (s_n < 16) { ++s_n; }
        }
        double producer_ms = 0.0;
        if (s_n > 0) {
            float v[16];
            for (UINT i = 0; i < s_n; ++i) { v[i] = s_ring[i]; }
            for (UINT i = 1; i < s_n; ++i) {          // n <= 16: insertion sort is fine
                const float k = v[i];
                int j = static_cast<int>(i) - 1;
                while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
                v[j + 1] = k;
            }
            producer_ms = v[s_n / 2];
        }
        ++dt_frames;
        if (!dt_warned && dt_frames > 120 && producer_ms > 1.0 && rt_ms > 1.0 &&
            (producer_ms > rt_ms * 1.35 || producer_ms < rt_ms * 0.74)) {
            dt_warned = true;
            logf_("[consumer] note: producer frame time %.2f ms (median) vs our capture cadence"
                  " %.2f ms - WGC batching or a limiter between them (frameRenderTime"
                  " uses ours, docs/findings/FINAL_CONFIG.md 5.4)\n",
                  producer_ms, rt_ms);
            // The dangerous direction: WGC delivers MORE frames than the game renders.
            // Real causes: the game's own frame generation (DLSS-G / FSR FG), or DWM
            // re-compositing the same content on the composition clock while the window has
            // lost independent flip (our topmost overlay does exactly that - HANDOFF 8.6).
            if (producer_ms > rt_ms * 1.35) {
                logf_("[consumer]   WARNING: WGC sees %.1f fps but the game renders %.1f fps."
                      " Either the game's OWN frame generation is on (DLSS-G / FSR FG - turn it"
                      " off, FINAL_CONFIG 2.1.2), or DWM is re-compositing duplicates because"
                      " the game window lost independent flip (our overlay: try"
                      " --overlay-hidden). Duplicates are dropped, not interpolated\n",
                      1000.0 / rt_ms, 1000.0 / producer_ms);
            }
        }
    }
    const double fg0 = qpc_ms();
    // Anchor the batch to its vblank slot (dynamic cadence only). Everything above
    // (capture, pairing, copies, colour) is already done, so the wait is idle time
    // and the hand-off to XeFG happens exactly at the scheduled boundary.
    // optimal：只有 paced 决策才量化输入（且等待 <= Λ）；否则立即提交。
    if (g_interp_cadence) {
        if (g_cad_paced) {
            fg_input_pace_ms(g_cad_render_ms, g_cad_latency_budget_ms);
        } else if (g_cad_policy != 0) {
            fg_input_pace(g_cad_last_k);   // even/fill 的旧路径（A/B）
        }
    }
    g_api.TagFrameConstants(g_fg, present_id, &cd);

    g_api.SetPresentId(g_fg, present_id);
    const double fg1 = qpc_ms();
    g_ms_fg = g_ms_fg * 0.9 + (fg1 - fg0) * 0.1;
    if (fg1 - fg0 > g_max_fg) { g_max_fg = fg1 - fg0; }
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_START); }
    const double t_present = qpc_ms();
    // Present(1,0) blocked for ~31 ms here, capping the whole loop at ~31 fps and
    // competing with the game's own presents. The guide uses sync interval 0, and
    // XeSS-FG does its own pacing anyway.
    g_swapchain->Present(0, g_present_flags());
    const double pres_ms = qpc_ms() - t_present;
    // 显示端相位：这次提交落在 vblank 周期的哪个位置。校准过才统计。
    if (g_vb_ok) {
        const double ph = vblank_phase_ms(t_present);
        if (ph >= 0.0) {
            g_vb_phase_sum += ph;
            g_vb_phase_sq += ph * ph;
            if (ph < g_vb_phase_min) { g_vb_phase_min = ph; }
            if (ph > g_vb_phase_max) { g_vb_phase_max = ph; }
            ++g_vb_phase_n;
        }
    }

    g_present_ms = static_cast<DWORD>(pres_ms + 0.5);
    g_ms_present = g_ms_present * 0.9 + pres_ms * 0.1;
    {
        // A frame that takes tens of milliseconds is exactly what the eye sees as a
        // stutter, and the 40-frame averages hide it. Name the phase that ate the time.
        const double t_end = qpc_ms();
        const double total = t_end - t_m0;
        if (total > 55.0) {
            static int slow_logs = 0;
            if (slow_logs++ < 40) {
                logf_("[consumer] SLOW frame: total=%.1fms decode=%.1f layout=%.1f"
                      " colour=%.1f inputs=%.1f present=%.1f\n",
                      total, t_m1 - t_m0, t_m2 - t_m1, t_m3 - t_m2,
                      t_present - t_m3, pres_ms);
            }
        }
    }
    // Signal after Present so this also covers XeSS-FG's own submissions.
    g_queue->Signal(g_frame_fence.Get(), ++g_frame_fence_value);
    // This colour was presented: remember its id so the next identical delivery can be
    // discarded at ingress instead of being converted/generated a second time.
    if (colour_id != 0) {
        g_last_colour_id = colour_id;
        // Colour watermark for the producer: publications at or below this id have been
        // consumed and may be recycled; anything newer is still waiting for its colour.
        if (g_ctl) { MemoryBarrier(); g_ctl->colour_seq = colour_id; }
    }
    if (g_xell) { xellAddMarkerData(g_xell, present_id,
                                    XELL_PRESENT_END); }

    // --dump: export this frame's colour/depth/MV so the pairing can be checked
    // offline. Placed after Present so the resources a dump reads are the exact
    // ones XeSS-FG was tagged with for this present id.
    if (g_dump_frames > 0) { dump_flush(); }

    // Did this frame's GPU work fit inside the interval the game gives us? When the sum
    // exceeds it we cannot consume frames as fast as they arrive, the WGC pool backs up
    // and frames get lost - which is what a "dip" looks like on screen.
    if (input_interval_ms() > 1.0 &&
        (g_ms_throttle + g_ms_present) > input_interval_ms()) {
        ++g_slow_frames;
    }
    // Per-frame version of the test above (g_slow_frames is a per-window counter): the main
    // loop's --slack-auto uses this to decide whether to absorb a queued frame instead of
    // dropping it. Same criterion, so the two never disagree about what "overran" means.
    g_last_overran = (input_interval_ms() > 1.0) &&
                     ((g_ms_throttle + g_ms_present) > input_interval_ms());

    // Continue from the slot that was actually consumed; with frame-key pairing this
    // is not necessarily the old round-robin successor.
    if (!g_reuse_frame && slot < mgpu::kMaxSlots) { g_mv_slot = (slot + 1) % slots; }
    ++g_frames;
    // Remember which producer publication this frame consumed, for --slack-auto's
    // duplicate-vs-new-content test in the main loop.
    if (g_ctl) { g_last_content_pub = g_ctl->frame_published; }

    // v22: remember this publication; the next frame ACKs it after gpu_throttle() has
    // waited for this frame's GPU work (which is what makes the ACK mean "copy retired").
    ack_publication_record(slot);

    // Re-assert our z-order every frame batch: the game raises itself on activation.
    assert_topmost();

    // Whole-function duration: whatever is not attributed to a component above shows up
    // here, which is how the "40 ms gap that nothing explains" gets pinned down.
    {
        const double tot = qpc_ms() - t_r0;
        g_ms_total = g_ms_total * 0.9 + tot * 0.1;
        if (tot > g_max_total) { g_max_total = tot; }
    }

    // Every frame, so the on-screen counter is smooth: framesPresented > 1 means
    // XeSS-FG really emitted interpolated frames for that Present, 1 = passthrough.
    xefg_swapchain_present_status_t ps{};
    const xefg_swapchain_result_t sr = g_api.GetLastPresentStatus(g_fg, &ps);
    g_presented_sum += ps.framesPresented;
    g_presented_n   += 1;
    // "xNmiss" counts real frames that produced fewer frames than the configured
    // multiplier, which is how a refused MFG step (old XeLL, model limit) shows up.
    // In --interp-cadence mode the multiplier changes every frame on purpose, so the
    // comparison is meaningless there; the histogram below is the equivalent check.
    if (!g_interp_cadence && ps.framesPresented < g_fg_interp_actual + 1) { ++g_x2_miss; }
    // cadence core 的可实现性反馈：4K 4x 在本机实测会 ERR_OTHER/只出 1 张，
    // 这不能继续当作"理论上限 J 可用"。连续 3 次不满就把 k 上限降一档。
    if (g_interp_cadence && g_cad_core_ready && sr == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
        ps.frameGenResult >= 0 && !g_reuse_frame) {
        // MV 门控（菜单/暂停/加载）期间 FG 被我们主动关掉，每帧只有 1 张输出。
        // 这不是"XeFG 出不满"，把它算进去会让 learnedMaxK 永久降档：
        // 实测 carry15 中途 j 4→2，之后 120 Hz 再也上不去（output 掉到 ~103 Hz）。
        // 所以门控期间既不计入，也把连续计数清零。
        if (g_mv_gate_off) {
            g_cad_core.ForgetShortfall();
        } else {
        const int beforeJ = g_cad_core.LearnedMaxK();
        g_cad_core.NoteOutputShortfall(static_cast<int>(g_fg_interp_actual) + 1,
                                       static_cast<int>(ps.framesPresented));
        const int afterJ = g_cad_core.LearnedMaxK();
        if (afterJ != beforeJ) {
            logf_("[consumer] cadence core: XeFG delivered only %u/%d frames - learned"
                  " max k %d -> %d (4K %dx cannot be kept)\n",
                  ps.framesPresented, static_cast<int>(g_fg_interp_actual) + 1,
                  beforeJ, afterJ, beforeJ);
        }
        }
    }

    // Did somebody else take over presentation? With N+1 frames per real frame on
    // a vblank-paced display the batch cannot honestly cost much less than the
    // input interval; when it does, Present is returning without waiting (an OSD
    // or latency tool hooking it, or our overlay being occluded) and the picture
    // judders even though xN still reads 4.00.
    //
    // One window is not enough evidence: a catch-up burst (two frames processed
    // back to back after a stall) drops the EMA for a single window and used to
    // trigger this on a perfectly healthy run. Require three in a row.
    if (!g_nopace_warned && g_frames > 200 && ps.framesPresented >= 2 &&
        g_arrival_ms_ema > 8.0 && g_ms_total > 0.0 &&
        g_ms_total < 0.35 * g_arrival_ms_ema) {
        if (++g_nopace_windows >= 3) {
            g_nopace_warned = true;
            logf_("[consumer] WARNING: presentation is not vblank-paced - total=%.1fms but the input"
                  " interval is %.1fms while %u frames are presented per real frame.\n",
                  g_ms_total, g_arrival_ms_ema, ps.framesPresented);
            logf_("[consumer]   something else hooks Present or our overlay is occluded"
                  " (OSD / latency tool / FLM, RTSS, Steam overlay, a topmost window).\n");
            logf_("[consumer]   MFG still produces %u frames, but they go out in bursts, so the"
                  " picture judders while the counter reads %.0f fps.\n",
                  ps.framesPresented, 1000.0 / std::max(g_ms_total, 0.001) * ps.framesPresented);
        }
    } else {
        g_nopace_windows = 0;
    }
    {
        const double nq = qpc_ms();
        double gap = 0.0;
        if (g_last_present_qpc > 0.0) {
            gap = nq - g_last_present_qpc;
            if (gap > g_max_gap) { g_max_gap = gap; }
        }
        g_last_present_qpc = nq;
        if (g_ms_throttle > g_max_gpu) { g_max_gpu = g_ms_throttle; }
        if (g_ms_present > g_max_present) { g_max_present = g_ms_present; }

        // Immediate hitch report. A 40-frame average hides exactly the one-frame stalls
        // the eye notices, so anything 45% over the expected spacing is logged at once
        // (rate limited, so a burst cannot flood the file).
        const double expect = (g_pace_ms > 0.0) ? g_pace_ms : g_arrival_ms_ema;
        // Without pacing the gap simply follows the source, and RTSS makes that
        // alternate between 16.7 and 41.7 ms on a 120 Hz panel - a 1.45x threshold
        // would flag that as a hitch every second. Only report real outliers there.
        const double hitch_limit = expect * ((g_pace_ms > 0.0) ? 1.45 : 2.0);
        if (expect > 1.0 && g_hitch_logged < 60) {
            double worst = gap;
            if (g_ms_throttle > worst) { worst = g_ms_throttle; }
            if (g_ms_present > worst) { worst = g_ms_present; }
            if (worst > hitch_limit) {
                ++g_hitch_logged;
                logf_("[consumer] HITCH gap=%.1fms gpu=%.1fms pres=%.1fms wait=%.1fms"
                      " expect=%.1fms xN=%u slot=%u drop=%u\n",
                      gap, g_ms_throttle, g_ms_present, g_ms_wait, expect,
                      ps.framesPresented, slot, g_arr_drops);
            }
        }
    }
    // `show` is what the OSD and the per-frame log line print as "output fps". It used to be
    // loop_fps * framesPresented, i.e. the *loop* rate (measured with GetTickCount64!) times
    // the last batch size. The moment the loop catches up after a stall - WGC hands us two or
    // three queued frames back to back - that reads 148 or even 164 fps on a 120 Hz panel,
    // which is a number the display can never show and which sent people hunting for a bug
    // that does not exist. The swap chain is the only honest source: count what it really gave
    // the compositor per second, in the report window below (see `out:`).
    if (g_show_fps == 0 && g_frame_ms_ema > 0.1) {
        const double loop_fps = 1000.0 / g_frame_ms_ema;
        const double presented = (ps.framesPresented > 0) ? ps.framesPresented : 1;
        g_show_fps = static_cast<int>(loop_fps * presented + 0.5);   // first window only
    }

    if (g_frames == 30 || g_frames == 200 || g_frames == 600) { dump_zorder(); }

    if ((g_frames % g_report_every) == 1) {
        const ULONGLONG now_ms = GetTickCount64();
        const double win = (g_t_report && now_ms > g_t_report)
                         ? (now_ms - g_t_report) / 1000.0 : 0.0;
        g_t_report = now_ms;
        // --stream-auto / --stream-asym: one decision per window, with hysteresis.
        stream_controller_tick(slot);
        const double secs = (now_ms / 1000.0) - g_t0;
        logf_("[consumer] frame %llu  slot %u  jitter=(%.3f, %.3f)  (%.1f fps avg, %.1f recent)"
              "  presented=%u fg=%u %s  hittest=%ld fgwin=%d present=%ums bb=%ux%u show=%d\n",
              (unsigned long long)g_frames, slot,
              cd.jitterOffsetX, cd.jitterOffsetY,
              (secs > 0) ? g_frames / secs : 0.0,
              (win > 0.0) ? static_cast<double>(g_report_every) / win : 0.0,
              ps.framesPresented, ps.isFrameGenEnabled,
              (sr == XEFG_SWAPCHAIN_RESULT_SUCCESS) ? xefg_str(ps.frameGenResult) : "status?",
              g_hittest_count,
              (GetForegroundWindow() == g_game_wnd) ? 1 : 0,
              g_present_ms, draw_w, draw_h, g_show_fps);
        const double xn = (g_presented_n > 0) ? g_presented_sum / g_presented_n : 0.0;
        g_presented_sum = 0.0;
        g_presented_n = 0;

        // Cadence control (both flags off by default, so neither the call nor the log
        // line appears in a normal run):
        //   --interp-auto  re-pick the interpolation count so the output lands on whole
        //                  vblanks (hysteresis lives in fg_auto_interp_tick).
        //   --fg-rt-mode   report which frameRenderTime estimate is being fed.
        if (g_interp_auto && !g_lock_auto) { fg_auto_interp_tick("auto (vblank cadence)"); }
        if (g_lock_auto) { fg_solve_cadence("lock-auto"); }
        // --dyn-mfg runs per real frame in run_frame_ex (its holds count frames).
        if (g_fg_rt_mode != kFgRtEma || g_interp_auto || g_lock_auto || g_lock_fps > 1.0 ||
            g_interp_cadence) {
            // Same estimator the accumulator used (mean of the ring), so this line and `want`
            // can never disagree again - they did, and it read as "input=3 vblanks but 5x".
            const double t_in = arr_mean_ms();

            const double vb = refresh_ms();
            // In cadence mode the multiplier changes per frame, so report the average
            // this window actually used instead of the last frame's count.
            double mul = static_cast<double>(g_fg_interp_actual + 1);
            if (g_interp_cadence) {
                // g_cad_hist[k] counts the frames that asked for k *interpolated* frames, so
                // the number of output frames per input frame is (k + 1). Forgetting the +1
                // made this line print "avg 1.00x, 60.0 fps out" for a run that was actually
                // doing 2.0x and 120 fps - the most misleading log in the program.
                double sum = 0.0, cnt = 0.0;
                for (int i = 0; i < 8; ++i) {
                    sum += static_cast<double>(i + 1) * g_cad_hist[i];
                    cnt += static_cast<double>(g_cad_hist[i]);
                }
                if (cnt > 0.0) { mul = sum / cnt; }
            }
            const double out_ms = (t_in > 0.0 && mul > 0.0) ? t_in / mul : 0.0;
            const double out_vb = (vb > 0.0) ? out_ms / vb : 0.0;
            // 1.00 / 2.00 / 3.00 vblank per output frame = perfectly even motion;
            // anything else is the pacer alternating short and long holds.
            const double whole = static_cast<double>(static_cast<int>(out_vb + 0.5));
            const double frac = dabs_(out_vb - whole);
            logf_("[consumer]   cadence: rt=%.1fms (%s) input=%.1fms = %.2f vblanks ->"
                  " interp=%u (avg %.2fx, %.1f fps out = %.2f vblank/frame%s)"
                  " refresh=%uHz lock=%.2f drops=%u%s\n",
                  fg_render_time_ms(), fg_rt_feed_name(), t_in, t_in / vb,
                  // interp = generated frames for the last real frame (k-1). It used to print
                  // g_fg_interp_actual, i.e. the *ceiling* (--interp N), which read as "we are
                  // asking for 2" while the average below was 5x - the most misleading pair of
                  // numbers in the log (2026-11-10).
                  (g_cad_last_k > 0) ? (g_cad_last_k - 1) : 0u, mul,
                  (out_ms > 0.0) ? 1000.0 / out_ms : 0.0, out_vb,
                  (frac > 0.06) ? " NOT-EXACT" : "", g_refresh_hz,
                  g_lock_fps, g_lock_drops,
                  (g_fg_out_hz > 1.0) ? " (out-hz cap)" : "");
            g_lock_drops = 0;
            if (g_out_clock) {
                const UINT ticks = g_oc_ticks, coasts = g_oc_coasts;
                const double mean = (ticks > 1)
                    ? g_oc_tick_sum / static_cast<double>(ticks - 1) : 0.0;
                logf_("[consumer]   out-clock: ticks=%u period=%.3fms tick mean/max=%.3f/%.3fms"
                      " coast=%u drops=%u skips=%u\n",
                      ticks, oc_period_ms(), mean, g_oc_tick_max,
                      coasts, g_oc_drops, g_oc_skips);
                g_oc_ticks = g_oc_coasts = g_oc_drops = g_oc_skips = 0;
                g_oc_tick_last = 0.0;
                g_oc_tick_sum = 0.0;
                g_oc_tick_max = 0.0;
            }
            if (g_interp_cadence) {
                logf_("[consumer]   cadence core: k=%u m=%d paced=%d q=%.3f out=%.1fHz"
                      " render=%.2fms policy=%s j=%d learnedJ=%d reason=%s(%d)"
                      " dwell=%d swloss=%.2f regime=%s"
                      " can=%d reuse=%d slots=%.2f T=%.2fms\n",
                      (g_cad_last_k > 0) ? g_cad_last_k : 0u, g_cad_m,
                      g_cad_paced ? 1 : 0, g_cad_q, g_cad_out_hz,
                      g_cad_render_ms, cad_policy_name(g_cad_policy),
                      g_cad_j, g_cad_core.LearnedMaxK(),
                      cad_reason_name(g_cad_reason), g_cad_reason,
                      g_cad_core.LearnedDwell(), g_cad_core.SwitchLossPerChange(),
                      cad_regime_name(g_cad_regime),
                      g_cad_can, g_cad_reuse, g_cad_slots, g_cad_T);

                logf_("[consumer]   cadence hist:");
                for (int i = 1; i < 8; ++i) {
                    if (g_cad_hist[i] != 0) { logf_(" %ux=%u", i + 1, g_cad_hist[i]); }
                }
                logf_("  switches=%u deferred=%u refused=%u fgpace=%.1fms late=%u\n",
                      g_cad_switches, g_cad_deferred, g_cad_fail,
                      g_fgi_wait_ms, g_fgi_late);
                for (int i = 0; i < 8; ++i) { g_cad_hist[i] = 0; }
                g_cad_switches = 0;
                g_cad_deferred = 0;
                g_cad_fail = 0;
            } else if (frac > 0.06 && g_refresh_hz > 0) {
                // No VRR: the only fix is locking the input to a divisor of the refresh.
                logf_("[consumer]     whole-vblank at %u Hz needs input %.2f fps (4x) or"
                      " %.2f (3x) or %.2f (2x) - see --list-modes\n",
                      g_refresh_hz, g_refresh_hz / 4.0, g_refresh_hz / 3.0,
                      g_refresh_hz / 2.0);
            }
        }
        const double jit_min = (g_arr_count > 0) ? g_arr_min : 0.0;
        const double jit_max = (g_arr_count > 0) ? g_arr_max : 0.0;
        g_arr_min = 1e9; g_arr_max = 0.0; g_arr_count = 0;
        const UINT drops = g_arr_drops; g_arr_drops = 0;
        const UINT slow = g_slow_frames; g_slow_frames = 0;
        const double budget = (input_interval_ms() > 1.0)
                            ? (g_ms_throttle + g_ms_present) / input_interval_ms() * 100.0 : 0.0;
        const double mx_gpu = g_max_gpu, mx_pres = g_max_present, mx_gap = g_max_gap;
        const double mx_wait = g_max_wait;
        const double mx_cw = g_max_copywait, mx_in = g_max_inputs, mx_tot = g_max_total;
        const double mx_cq = g_max_cq; g_max_cq = 0.0;
        const double mx_xe = g_max_xell, mx_fg = g_max_fg;
        const double mx_col = g_max_colour; g_max_colour = 0.0;
        const double mx_age = g_max_age; g_max_age = 0.0;
        // Everything run_frame() spends that no other field accounts for: the slot walk,
        // the descriptor/barrier setup, the present bookkeeping and this report itself.
        // If this grows, the next thing to look at is ours, not XeSS-FG's.
        const double accounted = g_ms_colour + g_ms_copywait + g_ms_work + g_ms_wait +
                                 g_ms_inputs + g_ms_cq + g_ms_of + g_ms_xell + g_ms_fg +
                                 g_ms_throttle + g_ms_present;
        const double cpu_only = (g_ms_total > accounted) ? (g_ms_total - accounted) : 0.0;
        const UINT x2m = g_x2_miss;
        g_hitch_logged = 0;
        g_max_gpu = 0.0; g_max_present = 0.0; g_max_gap = 0.0; g_x2_miss = 0;
        g_max_wait = 0.0; g_max_copywait = 0.0; g_max_total = 0.0; g_max_inputs = 0.0;
        g_max_xell = 0.0; g_max_fg = 0.0;
        const UINT pace_rep = static_cast<UINT>(g_represents); g_represents = 0.0;
        const UINT pace_sk = g_pace_skips; g_pace_skips = 0;

        // Factual one-shot note about who paces the loop: XeFG can only push
        // (interp+1) frames per input frame into the panel's vblanks, so the input rate
        // is topped out at refresh/(interp+1). On a 59 Hz panel at 4x that is ~15 fps
        // no matter how fast the game itself runs.
        // Only worth saying when the panel itself caps the profile (e.g. 59/60 Hz with
        // 4x): on a healthy 120 Hz run the input is *meant* to be 30 fps, so Present
        // blocking ~10 ms of 33 ms is the design, not a problem.
        if (!g_present_cap_warned && g_refresh_hz > 0 && g_refresh_hz < 100 &&
            g_ms_total > 1.0 && g_arrival_ms_ema > 1.0 &&
            g_ms_present > g_ms_total * 0.35 && g_ms_total > g_arrival_ms_ema * 0.85) {
            g_present_cap_warned = true;
            const double in_fps = 1000.0 / g_arrival_ms_ema;
            const double cap = (g_refresh_hz > 0)
                             ? g_refresh_hz / static_cast<double>(g_fg_interp_actual + 1) : 0.0;
            logf_("[consumer] note: Present blocks %.1f of %.1f ms (%.0f%%), so the display side"
                  " paces the loop.", g_ms_present, g_ms_total,
                  g_ms_present / g_ms_total * 100.0);
            if (cap > 0.0) {
                logf_(" %ux output on a %lu Hz panel tops the input out at %.1f fps"
                      " (measured %.1f fps) - lower --interp to go higher, or raise the"
                      " refresh rate.", static_cast<unsigned>(g_fg_interp_actual + 1),
                      static_cast<unsigned long>(g_refresh_hz), cap, in_fps);
            }
            logf_("\n");
        }
        // Cross-adapter budget: the only per-frame traffic that leaves the render card.
        // MV is R16G16 (4 B/px) and depth R32 (4 B/px), and XeSS-FG wants both at the
        // stream size, so this line is exactly what StreamPercent / --no-depth change -
        // it is the number to watch when the goal is "less bus traffic", not `total`.
        {
            const double mv_mb  = static_cast<double>(g_geo.mv_w) * g_geo.mv_h * 4.0 / (1024.0 * 1024.0);
            const double dep_mb = static_cast<double>(g_geo.depth_w) * g_geo.depth_h * 4.0 / (1024.0 * 1024.0);
            const bool send_mv  = mv_from_producer();
            const bool send_dep = !g_no_depth && !g_no_inputs;
            const double per_frame = (send_mv ? mv_mb : 0.0) + (send_dep ? dep_mb : 0.0);
            const double rate = (g_arrival_ms_ema > 0.1)
                              ? per_frame * (1000.0 / g_arrival_ms_ema) : 0.0;
            logf_("[consumer]   bus: mv %ux%u%s %.1fMB + depth %ux%u%s %.1fMB = %.1fMB/frame"
                  " (%.1f GB/s at %.1f fps; StreamPercent and --no-depth are the levers)\n",
                  g_geo.mv_w, g_geo.mv_h, send_mv ? "" : " (not sent)", send_mv ? mv_mb : 0.0,
                  g_geo.depth_w, g_geo.depth_h, send_dep ? "" : " (not sent)",
                  send_dep ? dep_mb : 0.0, per_frame, rate * 0.001,
                  (g_arrival_ms_ema > 0.1) ? 1000.0 / g_arrival_ms_ema : 0.0);
            // Render-card head room. There is no way to *reserve* a slice of the GPU (WDDM has
            // no per-process budget), so the only two levers are "make the vectors cheaper"
            // and "leave the card some time". Say so when the budget looks tight: 48 MB/frame
            // is ~1.5 GB/s at 30 fps and ~2 GB/s at 40 fps, i.e. a fifth of a frame period
            // spent moving vectors and depth around, on top of two full-screen passes.
            {
                const double share = (g_ms_total > 0.5)
                    ? (g_ms_inputs + g_ms_copywait) / g_ms_total : 0.0;
                if (per_frame >= 48.0 || share >= 0.25) {
                    logf_("[consumer]   headroom: %.0f MB/frame of vectors+depth (%.0f%% of the"
                          " loop is inputs+copy). Lower [capture] StreamPercent (150 halves"
                          " it), drop depth with --no-depth, and cap the *game* frame rate"
                          " (driver Max Frame Rate) so the conversion pass always has a slot"
                          " - dropping frames after render (--lock) does not free GPU time\n",
                          per_frame, share * 100.0);
                }
            }
        }
        // Arrival histogram: where the input actually comes from. `need` is what the current
        // multiplier requires (interp+1 vblanks per input frame), so a bucket below it means
        // the presenter ran faster than we consume, and a bucket above it means frames were
        // late or gated. `n` is the number of arrivals in this window - if `n` is ~20% short
        // of `frames`, the input itself is not being delivered, not merely reprocessed.
        {
            UINT total_arr = 0;
            for (int k = 1; k <= 8; ++k) { total_arr += g_arr_hist[k]; }
            logf_("[consumer]   arr(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_arr_hist[k]) { logf_(" %dx=%u", k, g_arr_hist[k]); }
            }
            const UINT need = g_fg_interp_actual + 1;
            logf_("  n=%u  need %ux (%.1f ms)  frames=%llu  med dwm=%.1f wc=%.1f\n", total_arr,
                  need, need * refresh_ms(), static_cast<unsigned long long>(g_frames),
                  arr_median_ms(), arr_wc_median_ms());
            for (int k = 1; k <= 8; ++k) { g_arr_hist[k] = 0; }
            if (g_fg_rt_mode == kFgRtAuto) {
                const double vb = refresh_ms();
                const double diff = dabs_(g_rt_ema_fast - g_rt_ema_slow);
                const bool moving = diff >= vb * 0.60;
                const int want = moving ? 1 : 0;
                // Decide once per report window (~1 s) and require three windows in a row:
                // the per-frame version flapped the pacer's input 4 times in 18 s.
                if (want != g_rt_auto_state) {
                    if (++g_rt_auto_agree >= 3) {
                        g_rt_auto_state = want;
                        g_rt_auto_agree = 0;
                        ++g_rt_auto_flips;
                        logf_("[consumer] --fg-rt-mode auto: input rate is %s (median %.1f vs"
                              " %.1f ms, %.2f vblanks apart) -> feeding %s\n",
                              moving ? "moving" : "steady", g_rt_ema_fast, g_rt_ema_slow,
                              (vb > 0.5) ? diff / vb : 0.0, moving ? "raw" : "quant");
                    }
                } else {
                    g_rt_auto_agree = 0;
                }
                logf_("[consumer]   rt auto: median %.1f vs %.1f (%.2f vblanks apart) -> %s"
                      "  flips=%u\n", g_rt_ema_fast, g_rt_ema_slow,
                      (vb > 0.5) ? diff / vb : 0.0,
                      (g_rt_auto_state == 1) ? "raw" : "quant", g_rt_auto_flips);
            }
            if (g_spike_count) {
                logf_("[consumer]   rt spikes: %u frame(s) reported at their real length this"
                      " window (--fg-rt-spike %d)\n", g_spike_count, g_fg_rt_spike ? 1 : 0);
                g_spike_count = 0;
            }
            if (g_burst_count) {
                logf_("[consumer]   bursts: %u frame(s) processed faster than 0.6x the input"
                      " interval this window (queued captures drained back to back)\n",
                      g_burst_count);
                g_burst_count = 0;
            }
            // The same buckets for the interval the producer measured between the game's own
            // Present calls. If this is flat while `arr(vb)` is not, the game is presenting
            // evenly and the unevenness appears after it (composition / our overlay).
            UINT game_total = 0;
            for (int k = 1; k <= 8; ++k) { game_total += g_game_hist[k]; }
            logf_("[consumer]   game(vb):");
            for (int k = 1; k <= 8; ++k) {
                if (g_game_hist[k]) { logf_(" %dx=%u", k, g_game_hist[k]); }
            }
            logf_("  n=%u (producer measured)\n", game_total);
            for (int k = 1; k <= 8; ++k) { g_game_hist[k] = 0; }
        }
        // ---- "interpolation cannot help here" -------------------------------------------
        // With the WGC throttle gone (2026-11-10) an unlocked title can run well above half the
        // panel rate. XeFG cannot generate fewer than one frame per input, so 2x of an 80 fps
        // input asks a 120 Hz panel for 160 - it cannot show that, and the surplus becomes
        // duplicates: the picture is *worse* than a plain pass-through. Say so once instead of
        // silently mis-pacing (measured after the throttle fix: 1:1 felt clearly better).
        {
            static bool warned = false;
            const double vb_now = refresh_ms();
            const double tin_now = arr_mean_ms();
            if (!warned && tin_now > 0.5 && vb_now > 0.5) {
                const double per_input = tin_now / vb_now;      // frames needed to fill the panel
                if (per_input < 1.6) {
                    warned = true;
                    logf_("[consumer] note: input ~%.1f fps is already %.0f%% of the %.0f Hz panel"
                          " (%.2f frames per input) - frame generation cannot help at this rate;"
                          " its extra frames only become duplicates. Switch the UI mode to"
                          " \"关闭 (1:1 直通)\" (or add --no-fg).\n",
                          1000.0 / tin_now, 100.0 / per_input, static_cast<double>(g_refresh_hz),
                          per_input);
                }
            }
        }

        // ---- output side: are the generated frames actually reaching the display? -----
        // Nothing above can see this: `xN` is XeFG's own accounting and `cadence` is derived
        // from the input median. Only the swap chain knows what it really handed to the
        // compositor, so count it. `expected` = frames this window * (interp+1) and `vblanks`
        // = refresh periods actually elapsed; presents << expected means XeFG is not
        // emitting the frames it charges us for (the picture judders while every other
        // counter looks perfect), presents > vblanks means they go out in bursts.
        {
            UINT last = 0;
            static UINT s_prev_last = 0;
            if (g_swapchain && SUCCEEDED(g_swapchain->GetLastPresentCount(&last))) {
                const UINT d = (s_prev_last && last >= s_prev_last) ? (last - s_prev_last) : 0;
                s_prev_last = last;
                const double vbs = (win > 0.0) ? win * static_cast<double>(g_refresh_hz) : 0.0;
                // 窗口内 k 可能变化（cap-limited 下 3/4 交替），expected 必须用
                // 本窗口真正应用的 k 之和，否则会把正常换档报成 MISSING。
                const double expect = (g_cad_expected_k > 0.5)
                    ? g_cad_expected_k
                    : static_cast<double>(g_report_every) *
                      static_cast<double>(g_fg_interp_actual + 1);
                g_cad_expected_k = 0.0;
                // ---- 切换成本反馈（bridge 之外我们独有的信息）----------------
                // 一个窗口里发生过 N 次 SetNumInterpolatedFrames，本窗口的
                // expected 是"真正生效的 k"之和，presents 是交换链实际交出去
                // 的张数。差值 >0 就是这些切换吃掉的输出。默认只测量（写进
                // 日志的 swloss 与下面这一行），--cad-switch-loss auto 才让
                // 控制器据此调驻留。
                //
                // present 计数本身可能按"调用"而不是"生成张数"计（见
                // cadence_present_drained 的注释），那种情况下差值会接近 0 或
                // 为负，NoteSwitchLoss 的 [-2, 8] 夹取会直接丢弃，不会误学。
                if (g_cad_switches_out > 0 && d > 0 && expect > 0.5) {
                    const int lost = static_cast<int>(std::lround(expect - static_cast<double>(d)));
                    g_cad_core.NoteSwitchLoss(lost);
                    if (lost > 0) {
                        logf_("[consumer]   switch cost: %u change(s), %d output(s) short"
                              " (%.2f per change) - dwell now %d\n",
                              g_cad_switches_out, lost,
                              static_cast<double>(lost) / static_cast<double>(g_cad_switches_out),
                              g_cad_core.LearnedDwell());
                    }
                }
                g_cad_switches_out = 0;
                logf_("[consumer]   out: presents=%u  expected=%.0f  vblanks=%.0f%s\n",
                      d, expect, vbs,
                      (d > 0 && static_cast<double>(d) + 0.5 < expect)
                          ? "  <== OUTPUT FRAMES MISSING" : "");
                if (g_ctl != nullptr) {
                    logf_("[consumer]   pub: last_processed=%llu pick=%u slots=[%llu %llu %llu %llu]"
                          " global=%llu\n",
                          static_cast<unsigned long long>(g_pair_seq), g_pub_slot,
                          static_cast<unsigned long long>(g_ctl->frames[0].published),
                          static_cast<unsigned long long>(g_ctl->frames[1].published),
                          static_cast<unsigned long long>(g_ctl->frames[2].published),
                          static_cast<unsigned long long>(g_ctl->frames[3].published),
                          static_cast<unsigned long long>(g_ctl->frame_published));
                }
                // The honest output rate: what the swap chain actually handed to the
                // compositor per second of this window. This is what `show=` reports now.
                if (win > 0.05 && d > 0) {
                    g_show_fps = static_cast<int>(static_cast<double>(d) / win + 0.5);
                }
            }
            static bool s_stats_failed = false;
            DXGI_FRAME_STATISTICS fs{};
            if (!s_stats_failed) {
                if (g_swapchain && SUCCEEDED(g_swapchain->GetFrameStatistics(&fs))) {
                    static UINT s_pc = 0, s_prc = 0, s_src = 0;
                    const UINT pc  = (fs.PresentCount >= s_pc) ? fs.PresentCount - s_pc : 0;
                    const UINT prc = (fs.PresentRefreshCount >= s_prc)
                                   ? fs.PresentRefreshCount - s_prc : 0;
                    const UINT src = (fs.SyncRefreshCount >= s_src)
                                   ? fs.SyncRefreshCount - s_src : 0;
                    s_pc = fs.PresentCount; s_prc = fs.PresentRefreshCount;
                    s_src = fs.SyncRefreshCount;
                    // presentRefresh lags syncRefresh when the compositor had to skip a
                    // vblank: a *displayed* drop, invisible to every other counter.
                    logf_("[consumer]   out2: count+%u  presentRefresh+%u  syncRefresh+%u%s\n",
                          pc, prc, src, (src > prc + 1) ? "  <== VBLANK MISSED" : "");
                } else {
                    s_stats_failed = true;
                    logf_("[consumer]   out2: GetFrameStatistics unsupported on this swap chain\n");
                }
            }
        }
        // Process CPU for this report window: user+kernel time of *all* bridge threads
        // (WGC, the D3D driver's workers, our recording), not just the frame loop. This is
        // the number to compare with Task Manager; the per-stage fields below only cover
        // what run_frame() itself timed.
        {
            static ULONGLONG s_cpu_prev = 0, s_wall_prev = 0;
            FILETIME ct{}, et{}, kt{}, ut{};
            ULONGLONG cpu = 0;
            if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                cpu = (static_cast<ULONGLONG>(kt.dwHighDateTime) << 32 | kt.dwLowDateTime) +
                      (static_cast<ULONGLONG>(ut.dwHighDateTime) << 32 | ut.dwLowDateTime);
            }
            const ULONGLONG wall = GetTickCount64();
            if (s_cpu_prev != 0 && wall > s_wall_prev) {
                const double cpu_ms = static_cast<double>(cpu - s_cpu_prev) / 10000.0;
                const double wall_ms = static_cast<double>(wall - s_wall_prev);
                const double pct1 = (wall_ms > 0.0) ? (cpu_ms / wall_ms * 100.0) : 0.0;
                const double per_frame = (g_report_every > 0)
                                       ? (cpu_ms / static_cast<double>(g_report_every)) : 0.0;
                static uint64_t s_wake_prev = 0, s_wf_prev = 0, s_wp_prev = 0;
                static uint64_t s_colour_prev = 0, s_cap_prev = 0, s_drop_prev = 0;
                static double s_arm_ms_prev = 0.0;
                static uint32_t s_arm_calls_prev = 0;
                const uint64_t wakes = g_wake_count - s_wake_prev;
                const uint64_t wf = g_wake_frame - s_wf_prev;
                const uint64_t wp = g_wake_pub - s_wp_prev;
                const uint64_t wc = g_wake_colour - s_colour_prev;
                const uint64_t cap = static_cast<uint64_t>(
                    InterlockedCompareExchange64(&g_cap_wakes, 0, 0)) - s_cap_prev;
                const uint64_t qd = static_cast<uint64_t>(
                    InterlockedCompareExchange64(&g_colour_drops, 0, 0)) - s_drop_prev;
                const double arm_ms = g_arm_ms - s_arm_ms_prev;
                const uint32_t arm_calls = g_arm_calls - s_arm_calls_prev;
                s_wake_prev = g_wake_count;
                s_wf_prev = g_wake_frame;
                s_wp_prev = g_wake_pub;
                s_colour_prev = g_wake_colour;
                s_cap_prev = static_cast<uint64_t>(
                    InterlockedCompareExchange64(&g_cap_wakes, 0, 0));
                s_drop_prev = static_cast<uint64_t>(
                    InterlockedCompareExchange64(&g_colour_drops, 0, 0));
                s_arm_ms_prev = g_arm_ms;
                s_arm_calls_prev = g_arm_calls;
                const double wakes_s = (wall_ms > 0.0)
                                     ? (static_cast<double>(wakes) * 1000.0 / wall_ms) : 0.0;
                logf_("[consumer]   proc: cpu %.1f ms / %.0f ms wall = %.1f%% of one core,"
                      " %.2f ms/frame (all consumer threads, %.0f wakes/s = frame %llu +"
                      " colour %llu + pub %llu, cap %llu, qdrop %llu, arm %.2fms/%u)\n",
                      cpu_ms, wall_ms, pct1, per_frame, wakes_s, wf, wc, wp, cap, qd,
                      arm_ms, arm_calls);
            }
            s_cpu_prev = cpu;
            s_wall_prev = wall;
        }
        // 显示端相位（真实 vblank 校准之后才有）：均值稳定 = 提交锁在面板节拍上；
        // std 大 = 在打滑。这是唯一能反映"眼睛看到的节奏"的显示端信号。
        if (g_vb_ok && g_vb_phase_n > 0) {
            const double n = static_cast<double>(g_vb_phase_n);
            const double mean = g_vb_phase_sum / n;
            const double var = std::max(0.0, g_vb_phase_sq / n - mean * mean);
            logf_("[consumer]   vblank: period=%.4fms phase mean=%.3f std=%.3f"
                  " min=%.3f max=%.3f ms (n=%u)\n",
                  g_vb_period_ms, mean, std::sqrt(var),
                  g_vb_phase_min, g_vb_phase_max, g_vb_phase_n);
            g_vb_phase_sum = g_vb_phase_sq = 0.0;
            g_vb_phase_min = 1e9;
            g_vb_phase_max = -1e9;
            g_vb_phase_n = 0;
        }
        // 本窗口因为"拷贝会阻塞"而复用旧颜色的帧数（colour stall guard）。

        static UINT s_cskip_prev = 0;
        const UINT cskip = g_colour_stall_skips - s_cskip_prev;
        s_cskip_prev = g_colour_stall_skips;
        logf_("[consumer]   breakdown: capture=%.1fms work=%.1fms present=%.1fms gpu=%.1fms"
              " wait=%.1fms copy=%.1fms colour=%.1fms mvs=%.1fms cq=%.1fms of=%.1fms age=%.1fms"
              " xell=%.1fms fg=%.1fms total=%.1fms cpu=%.1fms timeouts=%u mode=%d  xN=%.2f"
              "  arr=%.1fms pin=%.1fms pq=%.1fms  jitter=%.1f..%.1fms  arr_gaps=%u  load=%.0f%%  slow=%u"
              "  pace=%.1fms repres=%u skip=%u cskip=%u"
              "  worst(gpu %.1f/pres %.1f/wait %.1f/copy %.1f/colour %.1f/mvs %.1f/cq %.1f"
              "/of %.1f/age %.1f"
              "/xell %.1f/fg %.1f/total %.1f/gap %.1f) xNmiss=%u\n",
              g_ms_capture, g_ms_work, g_ms_present, g_ms_throttle, g_ms_wait,
              g_ms_copywait, g_ms_colour, g_ms_inputs, g_ms_cq, g_ms_of, g_ms_age, g_ms_xell,
              g_ms_fg, g_ms_total, cpu_only,
              g_slot_timeouts, g_colour_mode, xn, g_arrival_ms_ema, g_prod_interval_ms, g_prod_qpc_ms,
              jit_min, jit_max, drops, budget, slow, g_pace_ms, pace_rep, pace_sk, cskip,

              mx_gpu, mx_pres, mx_wait, mx_cw, mx_col, mx_in, mx_cq, g_max_of, mx_age, mx_xe,
              mx_fg, mx_tot, mx_gap, x2m);
        // --prefetch-inputs hit/miss for this window (only worth a line when it is on).
        if (g_pf_pub) {
            static UINT s_pf_h = 0, s_pf_m = 0, s_pf_d = 0;
            static UINT s_pf_s = 0;
            logf_("[consumer]   pf: %u served, %u missed, %u copy-still-in-flight"
                  " (copied in-frame), %u dropped by overflow; queue now %u (this window)%s\n",
                  g_pf_hits - s_pf_h, g_pf_misses - s_pf_m,
                  g_pf_stale - s_pf_s,
                  g_fifo_drops - s_pf_d, g_pub_q_n,
                  g_pub_fifo ? "" : " [key mode]");
            s_pf_s = g_pf_stale;
            s_pf_h = g_pf_hits;
            s_pf_m = g_pf_misses;
            s_pf_d = g_fifo_drops;
        } else if (g_prefetch_inputs) {
            static UINT s_pf_h = 0, s_pf_m = 0;
            logf_("[consumer]   pf: %u frame(s) served from a prefetched copy, %u had to copy"
                  " in-frame (this window)\n", g_pf_hits - s_pf_h, g_pf_misses - s_pf_m);
            s_pf_h = g_pf_hits;
            s_pf_m = g_pf_misses;
        }
        // Exact-pair health (always printed): `exact` should track the colour frames,
        // `lost` is the number that presented 1X instead of tagging a guess, and
        // `mismatch` must stay 0 (it means the chosen slot did not carry the id).
        {
            static UINT s_ex = 0, s_ms = 0, s_ns = 0, s_am = 0, s_ur = 0, s_st = 0,
                        s_lo = 0, s_mm = 0;
            static UINT s_ln = 0;
            static uint64_t s_lsum = 0;
            static UINT s_pq = 0;
            static UINT s_dup = 0;
            const UINT dn = g_pair_lag_n - s_ln;
            const uint64_t ds = g_pair_lag_sum - s_lsum;
            const double lag_avg = (dn > 0) ? static_cast<double>(ds) / dn : 0.0;
            logf_("[consumer]   pair: exact=%u ms_fail=%u no_slot=%u ambiguous=%u"
                  " unreadable=%u stale=%u lost(1X)=%u mismatch=%u lag(avg=%.2f max=%u"
                  " n=%u, slots=%u)\n",
                  g_pair_exact - s_ex, g_pair_ms_fail - s_ms, g_pair_no_slot - s_ns,
                  g_pair_ambiguous - s_am, g_pair_unreadable - s_ur, g_pair_stale - s_st,
                  g_pair_lost - s_lo, g_pair_mismatch - s_mm,
                  lag_avg, g_pair_lag_max, dn, mgpu::kMaxSlots);
            logf_("[consumer]   pair: pubq_miss=%u this window\n", g_pubq_miss - s_pq);
            s_pq = g_pubq_miss;
            logf_("[consumer]   dup: %u duplicate colour(s) dropped at ingress this window\n",
                  g_dup_dropped - s_dup);
            s_dup = g_dup_dropped;
            s_ex = g_pair_exact; s_ms = g_pair_ms_fail; s_ns = g_pair_no_slot;
            s_am = g_pair_ambiguous; s_ur = g_pair_unreadable; s_st = g_pair_stale;
            s_lo = g_pair_lost; s_mm = g_pair_mismatch;
            s_ln = g_pair_lag_n; s_lsum = g_pair_lag_sum;
        }
        // --slack-auto: how many frames were absorbed instead of dropped this window, and how
        // many were still dropped - split into "carried new content" (a real lost input frame)
        // and "duplicate" (free). Only printed when it is on.
        if (g_slack_auto) {
            static UINT s_hold = 0, s_drop = 0, s_real = 0;
            const UINT dh = g_slack_auto_holds - s_hold;
            const UINT dd = g_slack_auto_drops - s_drop;
            const UINT dr = g_slack_auto_real - s_real;
            logf_("[consumer]   slack-auto: %u held back (would have been dropped), %u dropped"
                  " [%u real loss / %u duplicate]  (this window)\n", dh, dd, dr, dd - dr);
            s_hold = g_slack_auto_holds;
            s_drop = g_slack_auto_drops;
            s_real = g_slack_auto_real;
        }

        // ---- end-to-end latency estimate, measured from inside the process -----
        //
        // Everything below comes from timestamps we already collect, so nothing has
        // to hook Present (an external tool would disturb presentation and change
        // what it measures - see docs/findings/MFG.md §3.3):
        //
        //   age            WGC's own SystemRelativeTime -> the moment we pick the
        //                  frame up. That is the game compose -> our process hop.
        //   total          our whole run_frame, ending when Present() returned.
        //   scanout        0 .. one refresh interval: the newest frame of the batch
        //                  reaches the panel within one vblank.
        //
        // The game's own input->compose latency is NOT visible here (it needs a
        // click timestamp), so it is stated separately as ">= one input frame".
        if (g_ms_total > 0.0) {
            const double scanout = (g_refresh_hz > 0) ? 1000.0 / g_refresh_hz : 8.33;
            const double input_ms = (g_arrival_ms_ema > 1.0) ? g_arrival_ms_ema : 0.0;
            // --slack holds frames back on purpose; that is latency we added, so it
            // has to show up here instead of being hidden.
            const double queue_ms = (input_ms > 0.0) ? g_slack * input_ms : 0.0;
            const double compose_to_present = g_ms_age + g_ms_total + queue_ms;
            if (queue_ms > 0.0) {
                logf_("[consumer] LATENCY: compose->present %.1fms (game->pub %.1f + bridge %.1f"
                      " + slack %.1f) | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, queue_ms, scanout,
                      compose_to_present, compose_to_present + scanout);
            } else {
                logf_("[consumer] LATENCY: compose->present %.1fms (game->pub %.1f + bridge %.1f)"
                      " | +scanout 0..%.1f => **compose->photons %.1f..%.1fms**\n",
                      compose_to_present, g_ms_age, g_ms_total, scanout,
                      compose_to_present, compose_to_present + scanout);
            }
            if (input_ms > 0.0) {
                logf_("[consumer]          + game input->compose >= %.1fms (1 input frame,"
                      " not measurable here) => **est. click->photons >= %.1fms**\n",
                      input_ms, compose_to_present + input_ms);
            }
        }
    }
}


// ------------------------------------------------------------------ fg probe

