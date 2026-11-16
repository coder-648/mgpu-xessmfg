// ============================================================================
//  lock_frame module: 115_marker.inc
//
//  WGC 颜色帧 -> marker 解码 -> 与 producer 发布精确配对。
//
//  这是"颜色回到 WGC"这一版的核心：颜色来自 DWM 合成后的画面，MV/depth 来自
//  producer 的发布，两者没有共同的序号字段，唯一可信的身份是 fakerFG 在
//  present 前画在画面四角的 32-bit marker（payload24 = seq12 | low12(ms)，
//  外加 CRC-8）。
//
//  匹配规则（mgpu_pair_core::find_exact）：
//      frames[s].frame_id & 0xFFF == marker.seq12
//  且  circular_distance(frames[s].frame_qpc_ms & 0xFFF, marker.ms12) <= 40 ms
//  取 published 最大的一个；找不到/回绕歧义/CRC 错 都返回"未验证"，
//  调用方本帧只上屏真实帧（1X），绝不用猜的 slot 去打 tag。
//
//  Include order: after 110_pair_dump.inc (qpc_ms) and before 140_frame.inc.
// ============================================================================

// ---------------------------------------------------------------- 解码小工具
static float lock_half_to_float(uint16_t h)
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

static UINT lock_marker_bpp(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return 4;
    default:                             return 4;
    }
}

// 只有阈值意义（块内 min/max 比较），不需要色度正确。
static double lock_marker_luma(const BYTE* p, DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
        return lock_half_to_float(h[0]) + lock_half_to_float(h[1]) + lock_half_to_float(h[2]);
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

struct LockMarkerBlocks
{
    uint32_t bits[mgpu::kMarkerCorners]{};
    bool     ok[mgpu::kMarkerCorners]{};
    double   contrast[mgpu::kMarkerCorners]{};
};

// 四角各 6x6 一次性拷进一张 6x24 staging，再 Map 一次。当前帧解码（没有 +1
// 猜测）：144 px 的等待是亚毫秒级，代价远小于一次错配。
static void lock_marker_decode(ID3D11Texture2D* colour, LockMarkerBlocks* out)
{
    if (out == nullptr || colour == nullptr || g_ctx11 == nullptr || g_d3d11 == nullptr) { return; }
    D3D11_TEXTURE2D_DESC sd{};
    colour->GetDesc(&sd);
    const UINT bpp = lock_marker_bpp(sd.Format);
    const UINT S = mgpu::kMarkerSide;
    const UINT N = mgpu::kMarkerCorners;

    static ComPtr<ID3D11Texture2D> stg;
    static DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN;
    if (!stg || sfmt != sd.Format) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = S;
        d.Height = S * N;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = sd.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stg.Reset();
        if (FAILED(g_d3d11->CreateTexture2D(&d, nullptr, &stg))) {
            logf_("[lock] marker: staging CreateTexture2D FAILED (fmt=%u)\n",
                  static_cast<unsigned>(sd.Format));
            return;
        }
        sfmt = sd.Format;
    }

    bool any = false;
    for (UINT i = 0; i < N; ++i) {
        uint32_t mx = 0, my = 0;
        mgpu::marker_corner(i, sd.Width, sd.Height, &mx, &my);
        if (mx + S > sd.Width || my + S > sd.Height) { continue; }
        const D3D11_BOX box{ mx, my, 0, mx + S, my + S, 1 };
        g_ctx11->CopySubresourceRegion(stg.Get(), 0, 0, i * S, 0, colour, 0, &box);
        any = true;
    }
    if (!any) { return; }

    // 这次拷贝就是当前帧的 marker：先非阻塞 Map，忙则阻塞一次（一次性日志）。
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = g_ctx11->Map(stg.Get(), 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        const double w0 = qpc_ms();
        hr = g_ctx11->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m);
        const double w1 = qpc_ms();
        if (SUCCEEDED(hr) && w1 - w0 > 1.0) {
            static bool told = false;
            if (!told) {
                told = true;
                logf_("[lock] marker: waited %.2f ms for the 144-px staging copy\n", w1 - w0);
            }
        }
    }
    if (FAILED(hr) || m.pData == nullptr) {
        static bool told = false;
        if (!told) {
            told = true;
            logf_("[lock] marker: Map FAILED hr=0x%08lX\n", static_cast<unsigned long>(hr));
        }
        return;
    }

    const BYTE* base = static_cast<const BYTE*>(m.pData);
    const UINT C = mgpu::kMarkerCells;
    for (UINT i = 0; i < N; ++i) {
        uint32_t mx = 0, my = 0;
        mgpu::marker_corner(i, sd.Width, sd.Height, &mx, &my);
        if (mx + S > sd.Width || my + S > sd.Height) { continue; }
        double luma[mgpu::kMarkerCellsPerBlock]{};   // 9 cells: 8 data + 1 reference
        double lo = 1.0e30, hi = -1.0e30;
        for (UINT cy = 0; cy < C; ++cy) {
            for (UINT cx = 0; cx < C; ++cx) {
                double sum = 0.0;
                for (UINT dy = 0; dy < mgpu::kMarkerCellPx; ++dy) {
                    const BYTE* row = base +
                        static_cast<size_t>(i * S + cy * mgpu::kMarkerCellPx + dy) * m.RowPitch;
                    for (UINT dx = 0; dx < mgpu::kMarkerCellPx; ++dx) {
                        sum += lock_marker_luma(
                            row + (cx * mgpu::kMarkerCellPx + dx) * bpp, sd.Format);
                    }
                }
                const double v = sum / (mgpu::kMarkerCellPx * mgpu::kMarkerCellPx);
                luma[cy * C + cx] = v;
                if (v < lo) { lo = v; }
                if (v > hi) { hi = v; }
            }
        }
        out->contrast[i] = (hi > lo) ? (hi - lo) : 0.0;
        if (hi - lo < 0.05) { continue; }
        const double mid = (lo + hi) * 0.5;
        uint32_t bits = 0;
        for (UINT b = 0; b < mgpu::kMarkerBits; ++b) {
            if (luma[b] > mid) { bits |= (1u << b); }
        }
        out->bits[i] = bits;
        out->ok[i] = true;
    }
    g_ctx11->Unmap(stg.Get(), 0);
}

// Last frame whose MARKER DECODED (not "paired"). The repair decision must not depend on
// pairing: during a colour-lag burst every frame is no_slot/1X, the marker is still in the
// picture, and the old 250 ms "last verified" gate turned repair off -> four scar blocks.
static double g_lock_last_marker_ms = -1.0;
static UINT   g_lock_no_slot_streak = 0;

static uint32_t lock_marker_payload(ID3D11Texture2D* colour, bool* ok_out)
{
    if (ok_out != nullptr) { *ok_out = false; }
    if (colour == nullptr) { return 0; }

    LockMarkerBlocks mb{};
    lock_marker_decode(colour, &mb);
    uint32_t code = 0;
    int blocks = 0;
    for (uint32_t i = 0; i < mgpu::kMarkerCorners; ++i) {
        if (!mb.ok[i]) { continue; }
        code |= static_cast<uint32_t>(mb.bits[i] ^ mgpu::kMarkerXor) << (i * mgpu::kMarkerBits);
        ++blocks;
    }
    if (blocks != static_cast<int>(mgpu::kMarkerCorners)) {
        static int told = 0;
        if (told++ < 4) {
            logf_("[lock] marker: only %d/%u corner blocks readable\n",
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
            logf_("[lock] marker: CRC8 failed payload=0x%06X read=0x%02X want=0x%02X\n",
                  payload, crc, mgpu::marker_crc8(payload));
        }
        return 0;
    }
    if (ok_out != nullptr) { *ok_out = true; }
    g_lock_last_marker_ms = qpc_ms();
    static uint32_t last = 0xFFFFFFFFu;
    if (payload != last) {
        last = payload;
        logf_("[lock] marker: seq12=%u ms12=%u code=0x%08X%s\n",
              mgpu::marker_seq_from_payload(payload), (payload >> 12) & 0xFFFu,
              code, fixed ? " (1-bit fix)" : "");
    }
    return payload;
}

// ---------------------------------------------------------------- 精确配对
struct LockPairResult
{
    bool     verified  = false;
    bool     duplicate = false;    // same marker identity as the last processed colour
    int      slot      = -1;
    uint32_t payload   = 0;
    const char* reason = "none";   // exact / no_slot / ambiguous / unreadable / duplicate
};

struct LockPairStats
{
    UINT exact = 0, no_slot = 0, ms_fail = 0, ambiguous = 0, unreadable = 0, lag_max = 0;
    uint64_t lag_sum = 0;
};
static LockPairStats g_lock_pair_stats;
static UINT          g_lock_dup_dropped = 0;
// 重复帧按"身份"判断：seq12 相同就是同一帧。不能比较完整 payload —— loading/pause
// 时 fakerFG 仍按 120 Hz 重画 marker，每次 present 都会重新取 now_qpc_ms，所以
// seq12 不变、ms12 每帧都变；比较完整 payload 会把它们全部当成新输入（实测一轮
// 733 帧同 seq，全部 no_slot → 1X）。
static uint32_t      g_lock_last_seq12 = 0xFFFFFFFFu;
static bool          g_lock_last_seq12_exact = false;
static uint64_t      g_lock_last_exact_pub = 0;
static ULONGLONG     g_lock_miss_since_ms = 0;
// Pairing health is a RATE over a sliding window, not a consecutive-miss streak.
// A sequence such as success -> miss -> success -> success -> miss -> miss is already
// unhealthy even though it never contains a long consecutive run. Keep the last 64
// outcomes; after at least 16 samples, >= 30% misses means the queues have diverged.
static constexpr UINT kPairWindow       = 64;
static constexpr UINT kPairMinSamples   = 16;
static constexpr UINT kPairMissPercent  = 30;
static constexpr ULONGLONG kResetWarmupMs   = 5000;   // startup takes ~5 s to settle
static constexpr ULONGLONG kResetCooldownMs = 15000;  // no back-to-back resets
static uint8_t g_lock_pair_outcome[kPairWindow] = {};   // 1 = miss, 0 = success
static UINT    g_lock_pair_outcome_pos = 0;
static UINT    g_lock_pair_outcome_n   = 0;
static UINT    g_lock_pair_outcome_miss = 0;
static ULONGLONG g_lock_pair_started_ms = 0;

static void lock_reset_pair_window()
{
    memset(g_lock_pair_outcome, 0, sizeof(g_lock_pair_outcome));
    g_lock_pair_outcome_pos = 0;
    g_lock_pair_outcome_n = 0;
    g_lock_pair_outcome_miss = 0;
}

static void lock_note_pair_outcome(bool miss)
{
    if (g_lock_pair_started_ms == 0) { g_lock_pair_started_ms = GetTickCount64(); }
    if (g_lock_pair_outcome_n < kPairWindow) {
        if (miss) { ++g_lock_pair_outcome_miss; }
        g_lock_pair_outcome[g_lock_pair_outcome_pos] = miss ? 1u : 0u;
        g_lock_pair_outcome_pos = (g_lock_pair_outcome_pos + 1) % kPairWindow;
        ++g_lock_pair_outcome_n;
        return;
    }
    const UINT old = g_lock_pair_outcome[g_lock_pair_outcome_pos];
    if (old != 0u) { --g_lock_pair_outcome_miss; }
    g_lock_pair_outcome[g_lock_pair_outcome_pos] = miss ? 1u : 0u;
    if (miss) { ++g_lock_pair_outcome_miss; }
    g_lock_pair_outcome_pos = (g_lock_pair_outcome_pos + 1) % kPairWindow;
}

static bool lock_pair_rate_unhealthy()
{
    if (g_lock_pair_outcome_n < kPairMinSamples) { return false; }
    if (g_lock_pair_started_ms == 0 ||
        GetTickCount64() - g_lock_pair_started_ms < kResetWarmupMs) {
        return false;
    }
    return g_lock_pair_outcome_miss * 100u >=
           g_lock_pair_outcome_n * kPairMissPercent;
}

static ULONGLONG     g_lock_last_session_reset_ms = 0;
static uint32_t      g_lock_session_reset_serial = 0;

static bool lock_request_session_reset()
{
    if (g_ctl == nullptr) { return false; }
    const ULONGLONG now = GetTickCount64();
    if (g_lock_last_session_reset_ms != 0 &&
        now - g_lock_last_session_reset_ms < kResetCooldownMs) {
        return false;
    }
    // A fresh token is how the producer learns that this is a new consumer session.
    // It clears colour_seq/consumed[]/frames[] and bumps generation on its next record.
    const uint64_t token =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
        (static_cast<uint64_t>(now) << 1) ^
        static_cast<uint64_t>(++g_lock_session_reset_serial);
    g_ctl->consumer_session = token != 0 ? token : 1;
    MemoryBarrier();

    const UINT miss_before = g_lock_pair_outcome_miss;
    const UINT total_before = g_lock_pair_outcome_n;
    g_lock_last_seq12 = 0xFFFFFFFFu;
    g_lock_last_seq12_exact = false;
    g_lock_last_exact_pub = 0;
    g_lock_miss_since_ms = 0;
    g_lock_no_slot_streak = 0;
    lock_reset_pair_window();
    g_lock_last_marker_ms = -1.0;
    g_lock_last_session_reset_ms = now;

    logf_("[lock] session reset #%u (pair miss rate %u/%u, window %u) - producer state reset\n",
          g_lock_session_reset_serial, miss_before, total_before, kPairWindow);
    return true;
}


static LockPairResult lock_pair_colour(ID3D11Texture2D* colour)
{
    LockPairResult r;
    if (colour == nullptr || g_ctl == nullptr) { return r; }

    bool ok = false;
    const uint32_t payload = lock_marker_payload(colour, &ok);
    r.payload = payload;
    if (!ok) {
        ++g_lock_pair_stats.unreadable;
        r.reason = "unreadable";
        return r;
    }

    const uint32_t seq12 = mgpu::marker_seq_from_payload(payload);
    // 同一帧身份重复交付：只有上一次已经 exact，且 producer 没有继续发布新帧，
    // 才是真正的 DWM/WGC duplicate。切换帧率限制/暂停后会看到“旧颜色 + 新发布”；
    // 如果这里无条件丢弃，后续颜色会永久停在旧 seq。
    if (seq12 == g_lock_last_seq12 && g_lock_last_seq12_exact) {
        if (g_ctl->frame_published == g_lock_last_exact_pub) {
            ++g_lock_dup_dropped;
            r.duplicate = true;
            r.reason = "duplicate";
            return r;
        }
        // producer advanced: let this stale colour fall through to a miss/re-anchor.
        g_lock_last_seq12_exact = false;
    }
    g_lock_last_seq12 = seq12;
    g_lock_last_seq12_exact = false;

    // 快照所有仍然有效的发布（frame_id 非 0 且 published 非 0）。
    mgpu_pair::SlotView views[mgpu::kMaxSlots]{};
    UINT real[mgpu::kMaxSlots]{};
    int n = 0;
    const UINT slot_count = (g_ctl->slots != 0) ? g_ctl->slots : 1;
    for (UINT s = 0; s < slot_count && s < mgpu::kMaxSlots; ++s) {
        const mgpu::FrameShared& fr = g_ctl->frames[s];
        const uint64_t p1 = fr.published;
        if (fr.frame_id == 0 || p1 == 0) { continue; }
        const uint64_t ps1 = fr.pub_seq;
        const uint64_t fid = fr.frame_id;
        const uint64_t fq  = fr.frame_qpc_ms;
        const uint64_t p2 = fr.published;
        const uint64_t ps2 = fr.pub_seq;
        // Producer writes metadata before published/pub_seq. If either changes while
        // we copy it, this slot is mid-publication; try it on a later colour frame.
        if (p1 != p2 || ps1 != ps2) { continue; }
        views[n].frame_id     = fid;
        views[n].frame_qpc_ms = fq;
        views[n].published    = p1;
        real[n] = s;
        ++n;
    }

    const uint32_t ms12  = (payload >> 12) & 0xFFFu;
    // 40ms 是 40fps+ 场景的余量；锁帧 30x4 时 Evaluate -> present 的间距可以到
    // 33ms 以上，再叠加 present 队列，40ms 会偶尔误判为 ms 失配。120ms 仍然远小于
    // seq12 的回绕周期（4096 帧 ≈ 136 s @30fps），不会引入歧义。
    const mgpu_pair::Match exact = mgpu_pair::find_exact(views, n, seq12, ms12, 120u, true);
    mgpu_pair::Match m = exact;
    if (m.slot < 0) {
        // A WGC timestamp can be a little late; if seq12 is unique, a sequence-only
        // match is still unambiguous and beats dropping the whole colour frame.
        const mgpu_pair::Match loose = mgpu_pair::find_exact(views, n, seq12, ms12, 120u, false);
        if (loose.slot >= 0 && !loose.ambiguous) {
            m = loose;
            ++g_lock_pair_stats.ms_fail;
        }
    }
    if (m.slot < 0) {
        // find_exact 不区分 "没有这个 seq12" 和 "有但 ms12 对不上"；都算未命中。
        ++g_lock_pair_stats.no_slot;
        r.reason = "no_slot";
        return r;
    }
    if (m.ambiguous) {
        ++g_lock_pair_stats.ambiguous;
        r.reason = "ambiguous";
        return r;
    }

    const UINT slot = real[m.slot];
    // lag：这个 verified 发布比最新发布落后几个槽（给 kMaxSlots 定容）。
    UINT lag = 0;
    for (UINT s = 0; s < slot_count && s < mgpu::kMaxSlots; ++s) {
        if (g_ctl->frames[s].published > g_ctl->frames[slot].published) { ++lag; }
    }
    if (lag > g_lock_pair_stats.lag_max) { g_lock_pair_stats.lag_max = lag; }
    g_lock_pair_stats.lag_sum += lag;
    ++g_lock_pair_stats.exact;
    g_lock_last_seq12_exact = true;
    g_lock_last_exact_pub = g_ctl->frame_published;
    g_lock_miss_since_ms = 0;

    r.verified = true;
    r.slot = static_cast<int>(slot);
    r.reason = "exact";
    g_lock_last_marker_ms = qpc_ms();
    return r;
}

// A/B 开关文件（放在 exe 旁边）。
static bool lock_file_exists(const wchar_t* name)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *(slash + 1) = 0; }
    wcscat_s(path, MAX_PATH, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// 画面四角的 marker 必须在本帧的 convert pass 里用内侧邻居补掉。
// marker_repair_off.on 保留为 A/B（标记留在画面上）。
static bool lock_marker_should_repair()
{
    static int off = -1;
    if (off < 0) {
        off = lock_file_exists(L"marker_repair_off.on") ? 1 : 0;
        if (off != 0) {
            logf_("[lock] marker repair OFF by marker_repair_off.on (markers stay visible)\n");
        }
    }
    if (off != 0) { return false; }
    if (g_lock_last_marker_ms < 0.0) { return false; }
    const bool on = (qpc_ms() - g_lock_last_marker_ms) < 250.0;
    static int told = -1;
    if (static_cast<int>(on) != told) {
        told = on ? 1 : 0;
        logf_("[lock] marker repair: %s (last verified marker %.1f ms ago)\n",
              on ? "ON" : "OFF", qpc_ms() - g_lock_last_marker_ms);
    }
    return on;
}

static void lock_pair_report()
{
    const UINT total = g_lock_pair_stats.exact + g_lock_pair_stats.no_slot +
                       g_lock_pair_stats.ambiguous + g_lock_pair_stats.unreadable;
    const double lag_avg = (g_lock_pair_stats.exact != 0)
        ? static_cast<double>(g_lock_pair_stats.lag_sum) / g_lock_pair_stats.exact : 0.0;
    logf_("[lock] pair: exact=%u no_slot=%u ms_fail=%u ambiguous=%u unreadable=%u"
          " dup=%u (total=%u) lag(avg=%.2f max=%u)\n",
          g_lock_pair_stats.exact, g_lock_pair_stats.no_slot, g_lock_pair_stats.ms_fail,
          g_lock_pair_stats.ambiguous, g_lock_pair_stats.unreadable, g_lock_dup_dropped,
          total, lag_avg, g_lock_pair_stats.lag_max);
    g_lock_pair_stats = LockPairStats{};
    g_lock_dup_dropped = 0;
}
