// ============================================================================
//  xess_mfg_producer.dll - the transport half of the new producer.
//
//  What it is: a plain DLL loaded by fakerFG (src/faker_fg/producer_link.cpp). It
//  owns the cross-process block and nothing else:
//
//     fakerFG record (Evaluate, list open)  ->  barrier + CopyTextureRegion
//     fakerFG present   (list submitted)    ->  frames[slot] + Signal(fwd)
//
//  v22: the colour is a first-class stream. The record's back buffer is copied verbatim
//  into stream 0, in the same slot and under the same fence as MV/depth, so one
//  publication is one frame in every stream and the consumer no longer captures the
//  window (docs/design/COLOUR_TRANSPORT_V22.md).
//
//  Protocol: src/mgpu_shared.h v14, unchanged (slot + fence, dumb producer).
//  Design: docs/design/XESS_MFG_PRODUCER.md.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <wrl/client.h>

#include "mgpu_fg_api.h"
#include "mgpu_producer_api.h"
#include "mgpu_shared.h"
#include "downsample_mv_cso.h"
#include "downsample_depth_cso.h"
#include "downsample_ui_cso.h"
#include "fr_encode_cso.h"
#include "fr_encode32_cso.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- log
FILE* g_log = nullptr;
bool  g_log_tried = false;

void log_open()
{
    if (g_log != nullptr || g_log_tried) { return; }
    g_log_tried = true;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* dot = wcsrchr(path, L'.')) { *dot = L'\0'; }
    wcscat_s(path, MAX_PATH, L"_mfg_producer.log");
    g_log = _wfsopen(path, L"a", _SH_DENYNO);
    if (g_log == nullptr) { return; }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_log, "\n==== xess_mfg_producer pid=%lu %04u-%02u-%02u %02u:%02u:%02u ====\n",
            GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond);
    fflush(g_log);
}

void logf(const char* fmt, ...)
{
    log_open();
    if (g_log == nullptr) { return; }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------- state
const MgpuFgApi* g_fg = nullptr;

HANDLE  g_map = nullptr;
void*   g_view = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12Heap>   g_heap;
ComPtr<ID3D12Resource> g_slot[mgpu::kMaxStreams][mgpu::kMaxSlots];
ComPtr<ID3D12Fence>    g_fwd[mgpu::kMaxSlots];
ComPtr<ID3D12Fence>    g_back[mgpu::kMaxSlots];
uint64_t g_fwd_value[mgpu::kMaxSlots] = {};

bool     g_ready = false;
// When g_ready flipped: the transport can publish, but nothing is published until fakerFG's
// Present detour calls MgpuProducerPresent. The watchdog below reports a ready transport
// that never receives that tick.
ULONGLONG g_ready_ms = 0;
volatile LONG g_init_started = 0;
volatile LONG g_init_failed = 0;
uint32_t g_render_w = 0, g_render_h = 0;      // the game's render size (published as render)
// Per-stream content rectangle (= what is copied this frame). Index 1 = MV, 2 = depth.
// They may differ: the consumer can ask for MV at 1:1 and depth at 150/200 (XeFG needs
// them the same size, so the bridge rebuilds the smaller one before tagging).
uint32_t g_stream_w[mgpu::kMaxStreams] = {}, g_stream_h[mgpu::kMaxStreams] = {};
uint32_t g_slot_w = 0, g_slot_h = 0;          // placed capacity (= render size, see ensure_shared)
uint32_t g_mv_format = 0, g_depth_format = 0; // slot formats (follow the source)

// v22: the colour stream. It is the frame's OUTPUT image, copied verbatim (source
// format, no shader): FP16 scRGB stays FP16 scRGB, R10 PQ stays R10 PQ. Placed once
// at cap_dim(output), so a resolution or upscaler-preset change only rewrites the
// live rectangle (same rule as MV/depth).
uint32_t g_colour_w = 0, g_colour_h = 0;      // live content size (output size)
uint32_t g_colour_slot_w = 0, g_colour_slot_h = 0;   // placed capacity
uint32_t g_colour_format = 0;
// B mode 4: stream 3 is the UI-only texture, downsampled and optionally colour-converted
// on the render card. The consumer unblends it from the presented backbuffer to rebuild a
// HUDless frame. It never receives HUDless or colour from this producer.
uint32_t g_ui_w = 0, g_ui_h = 0;               // source UI size
uint32_t g_ui_format = 0;                     // transport format (R10 or RGBA8)
uint32_t g_ui_slot_w = 0, g_ui_slot_h = 0;     // placed capacity (downsampled)
bool     g_want_ui_session = false;            // UI stream is OFF by default (unverified, see below)
bool     g_ui_premultiplied = true;            // source UI.rgb already premultiplied (Streamline default)
bool     g_ui_premultiplied_cfg = true;        // fakerFG.ini UiPremultiplied (GameUi only)
// v29: which resource feeds stream 3.
//   mode 0 Auto    - title UI tag first, then HUDless
//   mode 1 Game    - title UI tag only
//   mode 2 Hudless - downsampled HUDless only (consumer synthesises UI)
//   mode 3 Off     - no stream 3
uint32_t g_ui_source_mode = 0;
uint32_t g_ui_source = mgpu::kUiSourceOff;     // published in SharedControl::ui_source
uint32_t g_ui_source_type = mgpu::kUiSourceOff; // stable type once first selected
// Local A/B override for the closed synthesis gate (`fakerFG.ini UiSynthesis=1`). It is
// never a release default: UiStream=0/UiSource=Off remain the shipped configuration.
static bool g_ui_synthesis_override = false;
// 锁帧版：颜色走消费者侧的 WGC，producer 不再发布 colour 流。默认关闭；
// 需要 A/B 时在 fakerFG.ini 写 ColourStream=1。
bool     g_want_colour_session = false;       // ColourStream=0 (lock_frame default)

// One frame in flight. The record itself is copied so present() can publish the
// metadata after the title has closed/executed its lists.
MgpuFgFrame g_frame{};
bool        g_frame_valid = false;
uint32_t    g_copied_mask = 0;     // kRequestMv / kRequestDepth copied this frame
bool        g_slot_locked = false;
// (The 16x16 scar tile transport was removed: the marker is drawn by fakerFG as a
//  self-contained 32-bit code and read back by the consumer, nothing is copied here.)
uint32_t    g_cur_slot = 0;
uint32_t    g_next_slot = 0;

// Producer frame interval (EMA), published in FrameShared.frame_time_ms. Measured between
// publish_frame() calls (once per presented frame), NOT between records: a title can call
// Evaluate several times per frame (CP2077: 3.3x), so the record interval (~7.5 ms) is not
// the frame time the consumer's controller needs.
uint64_t g_qpc_freq = 0;
uint64_t g_last_publish_qpc = 0;
uint64_t g_last_publish_frame_qpc = 0;   // f.frame_qpc_ms of the last counted publish
uint64_t g_last_published_seq = 0;       // last frame_id actually written to frames[]
double   g_publish_ms_ema = 0.0;
// v22 diagnostics: record/present/publish counters + copy-queue retirement latency.
uint64_t g_record_in = 0;
uint64_t g_present_calls = 0;
uint64_t g_publish_calls = 0;
double   g_copy_wait_ema = 0.0;
double   g_copy_wait_max = 0.0;
uint64_t g_publish_skips = 0;
uint64_t g_publish_seq_dedup = 0;
uint64_t g_record_replaced = 0;
uint64_t g_record_dropped = 0;
uint64_t g_pub_seq = 0;      // v23: unique, monotonic publication counter
ULONGLONG g_t0 = 0;   // first record timestamp, ms (diagnostics)

// Async copy queue (second DIRECT queue). The copy used to be recorded on the game's own
// command list, i.e. in the frame's critical path: measured on the render card as
// `game=25.1 ms @150` vs `27.2 ms @100`. Here the frame only records into this list; at
// present the game's queue signals g_frame_fence, this queue waits for it, copies, and only
// then signals fwd. The game's present is never delayed by the transfer.
// DIRECT (not COPY) is deliberate: the source needs a state transition to COPY_SOURCE,
// which a COPY list cannot express. D3D12 runs the second queue concurrently with the
// game's; with a cross-adapter destination there is no shared-resource hazard.
ComPtr<ID3D12CommandQueue>     g_copy_q;      // second DIRECT queue (needs barriers)
ComPtr<ID3D12CommandAllocator> g_copy_alloc;
ComPtr<ID3D12GraphicsCommandList> g_copy_list;
ComPtr<ID3D12CommandQueue>     g_dma_q;       // COPY queue (DMA engine, no barriers)
ComPtr<ID3D12CommandAllocator> g_dma_alloc;
ComPtr<ID3D12GraphicsCommandList> g_dma_list;
ComPtr<ID3D12Fence>            g_copy_done;   // copy queue finished (allocator reuse)
ComPtr<ID3D12Fence>            g_frame_fence; // game queue -> copy queue (frame submitted)
uint64_t g_copy_done_value = 0;
uint64_t g_frame_fence_value = 0;
bool     g_copy_enabled = false;
bool     g_dma_enabled = false;
bool     g_force_inline = false;      // DLSSG lane: copy on the title's own list
bool     g_copy_list_open = false;   // DIRECT list reset this frame and not closed yet
bool     g_dma_list_open = false;    // COPY list reset this frame and not closed yet
bool     g_copy_open = false;        // this frame recorded at least one copy
bool     g_copy_use_dma = false;     // this frame's copies are on the COPY (DMA) queue
ID3D12CommandQueue*      g_copy_active_q = nullptr;
ID3D12GraphicsCommandList* g_copy_active_list = nullptr;

// ---------------------------------------------------------------- StreamPercent
// 100 = 1:1. 150 / 200 / 400 scale a stream to 2/3, 1/2, 1/4 of the render size with one
// compute pass, so the cross-adapter copy shrinks quadratically. The two streams are
// independent (the bridge may ask for MV 100 / depth 200 and rebuild depth on its side).
// Requires a DIRECT or COMPUTE list (Dispatch is illegal on a COPY list).
// 锁帧版的预算：MV/depth 默认 StreamPercent=150（每个维度 2/3，字节 4/9）。
// 颜色不再跨卡，省下的带宽留给渲染卡；1 和 2 分别是 MV/depth。
// 消费端 lock_frame.exe 默认 --upsample，把 150% 的 MV/depth 联合重建到输入画面
// 尺寸后再交给 XeFG，所以 150 的精度不会原样带进插帧。
uint32_t g_percent[mgpu::kMaxStreams] = { 100, 150, 150, 200, 100 };   // effective (request or ini)
uint32_t g_ini_percent[mgpu::kMaxStreams] = { 100, 150, 150, 200, 100 }; // fakerFG.ini values, restored when request = 0
bool     g_scaled[mgpu::kMaxStreams] = {};
// A COPY-only command list cannot take the downscale Dispatch. The first time that is
// seen we fall back to 1:1 for the rest of the session instead of publishing nothing;
// this keeps StreamPercent=150 as the default safe for UE5-style tag lists.
bool     g_force_full_size = false;
uint32_t g_ds_w[mgpu::kMaxStreams] = {}, g_ds_h[mgpu::kMaxStreams] = {};
ComPtr<ID3D12RootSignature> g_ds_rs;
ComPtr<ID3D12DescriptorHeap> g_ds_heap;
ComPtr<ID3D12PipelineState> g_ds_pso[mgpu::kMaxStreams];      // 1 = MV, 2 = depth
ComPtr<ID3D12Resource>      g_ds_tex[mgpu::kMaxStreams];      // scaled outputs, same formats as the source
D3D12_RESOURCE_STATES       g_ds_state[mgpu::kMaxStreams] = { D3D12_RESOURCE_STATE_COMMON,
                                              D3D12_RESOURCE_STATE_COMMON,
                                              D3D12_RESOURCE_STATE_COMMON };

// ------------------------------------------------------- fr codec (protocol v30)
// Lossless MV transport (docs/design/LOSSLESS_TRANSPORT.md sections 6.1/14.3/17).
// Off unless fakerFG.ini says MvCodec=1; with 0 everything below is unused and the
// texture slot + copy_stream path stays bit-identical to 1.2.2.
uint32_t mv_codec();      // MvCodec= from fakerFG.ini, read once per session (defined below)
uint32_t depth_codec();   // DepthCodec= (the same codec on the R32 depth stream)
uint32_t stream_codec(int stream);   // 1 = MV, 2 = depth, 0 = texture path
void fr_resolve_codec(ID3D12Device* device);   // resolves "auto" before the layout
constexpr uint32_t kFrTileW = 32, kFrTileH = 8;
constexpr uint32_t kFrSlotWords = 264;          // (4 + 256*4) bytes / 4, rounded up
ComPtr<ID3D12RootSignature>  g_fr_rs;
ComPtr<ID3D12DescriptorHeap> g_fr_heap;         // srv(texture) | uav(slot) | uav(TileLen)
ComPtr<ID3D12PipelineState>  g_fr_pso;     // MV: fp16 pairs
ComPtr<ID3D12PipelineState>  g_fr_pso_d32; // depth: one R32 value per pixel
ComPtr<ID3D12Resource>       g_fr_tilelen[2];   // [0] = MV, [1] = depth
// MvCodec=2: private encode target (bisect: is a UAV write to the cross-adapter slot the
// invalid call?). Never read - it exists only so the kernel has a legal destination.
ComPtr<ID3D12Resource>       g_fr_stage;
uint64_t                     g_fr_stage_bytes = 0;

// ----------------------------------------------------------- d3d12 validation (D3dDebug)
// A cross-adapter resource that one side treats as illegal only surfaces as "device
// removed (INVALID_CALL)". D3dDebug=1 turns the debug layer on for this process (before the
// title creates its device - that is why it hangs off MgpuProducerStart) and mirrors the
// validation messages into this log. Expect a much slower session; diagnosis only.
uint32_t ini_int(const wchar_t* key, uint32_t def);   // defined with the other ini readers

bool                     g_d3d_debug = false;
ComPtr<ID3D12InfoQueue>  g_info_q;

void fr_debug_enable()
{
    g_d3d_debug = ini_int(L"D3dDebug", 0) != 0;
    if (!g_d3d_debug) { return; }
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
        dbg->EnableDebugLayer();
        logf("d3d12: debug layer ENABLED (D3dDebug=1) - this session runs much slower,"
             " validation messages go to this log\n");
    } else {
        g_d3d_debug = false;
        logf("d3d12: D3D12GetDebugInterface FAILED (Graphics Tools not installed?) -"
             " no validation\n");
    }
}

void fr_drain_msgs(const char* where)
{
    if (!g_d3d_debug) { return; }
    if (!g_info_q) {
        if (!g_device) { return; }
        if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&g_info_q)))) {
            g_d3d_debug = false;
            return;
        }
        logf("d3d12: InfoQueue attached (%s)\n", where);
    }
    const UINT64 n = g_info_q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_info_q->GetMessage(i, nullptr, &len)) || len == 0) { continue; }
        D3D12_MESSAGE* m = static_cast<D3D12_MESSAGE*>(malloc(len));
        if (m == nullptr) { continue; }
        if (SUCCEEDED(g_info_q->GetMessage(i, m, &len))) {
            logf("[d3d12 %s] %hs\n", where, m->pDescription);
        }
        free(m);
    }
    g_info_q->ClearStoredMessages();
}

// ------------------------------------------------------- coded-size measurement (1 Hz)
// The consumer's `bus:` line is derived from the stream geometry, so on the codec path it
// reports the RAW size (3840x2160x4 B). The real cross-adapter traffic is what the encoder
// wrote per tile, which only TileLen[] knows. Sample it once a second: copy TileLen into a
// readback buffer and sum it while the NEXT sample is being recorded (a second later, so
// the previous copy has long retired - no fence, no stall).
ComPtr<ID3D12Resource> g_fr_rb[2];              // [0] = MV, [1] = depth
uint32_t               g_fr_rb_tiles[2] = { 0, 0 };
ULONGLONG              g_fr_rb_ms[2] = { 0, 0 };

void fr_bytes_tick(ID3D12GraphicsCommandList* list, uint32_t tiles, int stream)
{
    if (list == nullptr || tiles == 0 || stream < 0 || stream > 1) { return; }
    const ULONGLONG now = GetTickCount64();
    if (g_fr_rb_ms[stream] != 0 && (now - g_fr_rb_ms[stream]) < 1000) { return; }

    if (g_fr_rb[stream] && g_fr_rb_tiles[stream] != 0) {
        void* p = nullptr;
        const D3D12_RANGE rr{ 0, static_cast<SIZE_T>(g_fr_rb_tiles[stream]) * 4u };
        if (SUCCEEDED(g_fr_rb[stream]->Map(0, &rr, &p)) && p != nullptr) {
            const uint32_t* v = static_cast<const uint32_t*>(p);
            uint64_t sum = 0;
            for (uint32_t i = 0; i < g_fr_rb_tiles[stream]; ++i) { sum += v[i]; }
            const double coded = static_cast<double>(sum) / 1048576.0;
            const double raw = static_cast<double>(g_fr_rb_tiles[stream]) * 1024.0 / 1048576.0;
            logf("fr: coded %s %.2f MB/frame (raw %.2f, %.2fx)\n",
                 stream == 0 ? "mv   " : "depth", coded, raw,
                 coded > 0.001 ? raw / coded : 0.0);
            const D3D12_RANGE none{ 0, 0 };
            g_fr_rb[stream]->Unmap(0, &none);
        }
    }

    if (!g_fr_rb[stream]) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 65536ull * 4ull;   // TileLen upper bound (see ensure_fr_objects)
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g_fr_rb[stream])))) {
            g_fr_rb_ms[stream] = now;                    // try again in a second
            return;
        }
    }

    // TileLen is a UAV the dispatch just wrote: one UAV barrier for visibility, then the
    // actual state change - a copy SOURCE may not be in UNORDERED_ACCESS, and the driver
    // drops the copy silently when it is (measured: readback came back all zeros).
    ID3D12Resource* tl = g_fr_tilelen[stream].Get();
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = tl;
    list->ResourceBarrier(1, &ub);
    // No transition: TileLen lives in COMMON (buffers always do) and D3D12 promotes it for
    // both the UAV write and this copy. Adding a UAV->COPY_SOURCE barrier here was what made
    // the driver drop the copy silently (the readback kept reading zeros).
    list->CopyBufferRegion(g_fr_rb[stream].Get(), 0, tl, 0,
                           static_cast<UINT64>(tiles) * 4ull);
    g_fr_rb_tiles[stream] = tiles;
    g_fr_rb_ms[stream] = now;
}


uint32_t fr_tiles(uint32_t w, uint32_t h)
{
    return ((w + kFrTileW - 1u) / kFrTileW) * ((h + kFrTileH - 1u) / kFrTileH);
}
uint64_t fr_slot_bytes(uint32_t slot_w, uint32_t slot_h)
{
    return mgpu::align64k(static_cast<uint64_t>(fr_tiles(slot_w, slot_h)) *
                          kFrSlotWords * 4u);
}

// credit window (v10): never overwrite a slot the consumer is still reading.
// Start "off": with no consumer yet, every slot must be usable (the old add-on
// started ON and dropped its first ~30 publications).
uint64_t g_ack_sum = 0;
uint64_t g_ack_idle = 30;
bool     g_credit_on = false;
const uint64_t kAckGraceFrames = 30;

uint32_t cap_dim(uint32_t v)
{
    uint32_t c = (v + 255u) & ~255u;
    if (c < 256u) { c = 256u; }
    if (c > mgpu::kStreamMaxW) { c = mgpu::kStreamMaxW; }
    return c;
}

int format_family(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT: return 32;
    default:                    return static_cast<int>(f);
    }
}

bool copy_formats_ok(DXGI_FORMAT a, DXGI_FORMAT b)
{
    return format_family(a) == format_family(b);
}

// Wide depth (R32G8X24 / D32_FLOAT_S8X24) is 8 bytes per pixel and cannot be copied
// directly into the R32 slot. The downscale shader reads its R32 plane and writes
// R32_FLOAT; it must run even when StreamPercent=100 (where no scaling was requested).
bool wide_depth_format(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32G8X24_TYPELESS ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// ---------------------------------------------------------------- shared block
// Same v9/v14 shape the add-on used: one named mapping, capacity-sized slots that
// never move, a 64 KB-aligned data start, per-slot placed resources and two
// cross-adapter fences. A render-size change only rewrites streams[1..2].width/
// height - nothing is re-placed, nobody is waited for.
mgpu::SharedControl* ctl()
{
    return g_view != nullptr ? reinterpret_cast<mgpu::SharedControl*>(g_view) : nullptr;
}

bool layout_streams(ID3D12Device* device, mgpu::SharedControl* c,
                    uint32_t slot_w, uint32_t slot_h,
                    uint32_t colour_w, uint32_t colour_h,
                    uint32_t colour_slot_w, uint32_t colour_slot_h, uint32_t colour_format,
                    uint32_t ui_w, uint32_t ui_h, uint32_t ui_slot_w, uint32_t ui_slot_h,
                    uint32_t ui_format)
{
    struct Spec { DXGI_FORMAT format; uint32_t w, h, sw, sh; };
    Spec specs[mgpu::kMaxStreams] = {
        // v22: colour is the frame's OUTPUT image. w/h is the live content (the consumer
        // builds its sink/swap chain at this) and sw/sh is the placed capacity (cap_dim
        // rounds 2160 up to 2304 - never confuse the two).
        { static_cast<DXGI_FORMAT>(colour_format), colour_w, colour_h,
          colour_slot_w, colour_slot_h },
        { static_cast<DXGI_FORMAT>(g_mv_format),    g_stream_w[1], g_stream_h[1], slot_w, slot_h },
        { static_cast<DXGI_FORMAT>(g_depth_format), g_stream_w[2], g_stream_h[2], slot_w, slot_h },
        // B mode 4: UI-only texture, downsampled on the render card and unblended on the
        // consumer. Its live rectangle is the downsampled size, not the output size.
        { static_cast<DXGI_FORMAT>(ui_format), ui_w, ui_h, ui_slot_w, ui_slot_h },
        // v22: the scar/marker stream is retired with WGC; kept as an empty slot.
        { DXGI_FORMAT_UNKNOWN,      0, 0, 0, 0 },
    };
    // v30: the codec MV slot holds bytes for exactly this content size, so it is sized and
    // published from (w,h) - not from the shared texture capacity, which now follows the
    // depth stream. The consumer clamps the live MV size against slot_w/h, so publishing a
    // smaller capacity here would silently cut a 4K MV stream down to the depth size.
    if (mv_codec() != 0 && specs[mgpu::kStreamMv].format == DXGI_FORMAT_R16G16_FLOAT) {
        specs[mgpu::kStreamMv].sw = specs[mgpu::kStreamMv].w;
        specs[mgpu::kStreamMv].sh = specs[mgpu::kStreamMv].h;
    }
    if (depth_codec() != 0 &&
        (specs[mgpu::kStreamDepth].format == DXGI_FORMAT_R32_TYPELESS ||
         specs[mgpu::kStreamDepth].format == DXGI_FORMAT_R32_FLOAT)) {
        specs[mgpu::kStreamDepth].sw = specs[mgpu::kStreamDepth].w;
        specs[mgpu::kStreamDepth].sh = specs[mgpu::kStreamDepth].h;
    }
    uint64_t cursor = mgpu::kDataStart;
    for (int i = 0; i < static_cast<int>(mgpu::kMaxStreams); ++i) {
        const bool needed = (i == static_cast<int>(mgpu::kStreamColour) ||
                             i == static_cast<int>(mgpu::kStreamMv) ||
                             i == static_cast<int>(mgpu::kStreamDepth) ||
                             i == static_cast<int>(mgpu::kStreamUi));
        uint64_t slot_size = 0;
        const bool want_fr = (stream_codec(i) != 0);
        const bool fr_fmt_ok = (i == static_cast<int>(mgpu::kStreamMv))
            ? (specs[i].format == DXGI_FORMAT_R16G16_FLOAT)
            : (specs[i].format == DXGI_FORMAT_R32_TYPELESS ||
               specs[i].format == DXGI_FORMAT_R32_FLOAT);
        const bool fr = want_fr && fr_fmt_ok;
        if (want_fr && !fr) {
            logf("fr: codec needs %s, source is fmt %u - keeping the texture slot\n",
                 i == static_cast<int>(mgpu::kStreamMv) ? "R16G16_FLOAT"
                                                       : "R32_TYPELESS/R32_FLOAT",
                 specs[i].format);
        }
        if (fr) {
            // v30: the MV slot carries fr-codec bytes, not pixels.
            slot_size = fr_slot_bytes(specs[i].sw, specs[i].sh);
        } else if (needed && specs[i].sw >= 16 && specs[i].sh >= 16 &&
            specs[i].format != DXGI_FORMAT_UNKNOWN) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = specs[i].sw;
            rd.Height           = specs[i].sh;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = specs[i].format;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            const D3D12_RESOURCE_ALLOCATION_INFO ai =
                device->GetResourceAllocationInfo(0, 1, &rd);
            slot_size = mgpu::align64k(ai.SizeInBytes);
        }
        c->streams[i].width     = needed ? specs[i].w : 0;
        c->streams[i].height    = needed ? specs[i].h : 0;
        c->streams[i].format    = needed ? static_cast<uint32_t>(specs[i].format) : 0;
        c->streams[i].offset    = cursor;
        c->streams[i].slot_size = slot_size;
        c->streams[i].slot_w    = needed ? specs[i].sw : 0;
        c->streams[i].slot_h    = needed ? specs[i].sh : 0;
        c->streams[i].seq       = 0;
        c->streams[i].codec     = fr ? 1u : 0u;
        cursor += slot_size * mgpu::kMaxSlots;
    }
    return true;
}

bool place_slots(ID3D12Device* device, uint32_t stream, uint32_t format,
                 uint32_t w, uint32_t h, uint64_t offset, uint64_t slot_size)
{
    // Must match layout_streams(): the byte slot exists only for the format the kernel
    // implements, otherwise the size laid out is a texture's allocation size.
    const bool as_buffer = (stream_codec(static_cast<int>(stream)) != 0 &&
                            (stream != mgpu::kStreamMv ||
                             format == static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT)) &&
                            (stream != mgpu::kStreamDepth ||
                             format == static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS) ||
                             format == static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT)));
    for (uint32_t s = 0; s < mgpu::kMaxSlots; ++s) {
        D3D12_RESOURCE_DESC rd{};
        if (as_buffer) {
            // v30: raw byte buffer, sized from the slot capacity (see layout_streams).
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = slot_size;
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        } else {
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = w;
        rd.Height           = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = static_cast<DXGI_FORMAT>(format);
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        }
        const D3D12_RESOURCE_STATES init_state =
            as_buffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
        const HRESULT hr = device->CreatePlacedResource(
            g_heap.Get(), offset + slot_size * s, &rd, init_state,
            nullptr, IID_PPV_ARGS(&g_slot[stream][s]));
        if (FAILED(hr)) {
            logf("shared: CreatePlacedResource FAILED (stream %u %ux%u fmt %u slot %u) hr=0x%08lX\n",
                 stream, w, h, format, s, static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}


// StreamPercent from fakerFG.ini. Per stream: StreamPercentMv / StreamPercentDepth fall
// back to StreamPercent, then 150 (lock_frame default). Read once per session (rule 7: no per-frame file
// access); the bridge's dynamic request arrives through shared memory, not the file.
uint32_t read_percent(int stream)
{
    // v30: the fr codec is lossless at 1:1, so a codec stream always runs at 100%.
    if (stream == 1 && mv_codec() != 0) { return 100; }
    if (stream == 2 && depth_codec() != 0) { return 100; }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *slash = L'\0'; }
    wcscat_s(path, MAX_PATH, L"\\fakerFG.ini");
    const int fallback = GetPrivateProfileIntW(L"fakerFG", L"StreamPercent", 150, path);
    const wchar_t* key = (stream == 1) ? L"StreamPercentMv"
                       : (stream == 2) ? L"StreamPercentDepth"
                                       : L"StreamPercentUi";
    int v = GetPrivateProfileIntW(L"fakerFG", key, fallback, path);
    if (v < 100) { v = 100; }
    if (v > 400) { v = 400; }
    return static_cast<uint32_t>(v);
}

// render -> stream size. Rounded down to a multiple of 8 (XeFG ignores unaligned MV at
// odd sizes) and never below 256.
uint32_t target_dim(uint32_t v, uint32_t percent)
{
    uint32_t t = v * 100u / (percent ? percent : 100u);
    t &= ~7u;
    if (t < 256u) { t = 256u; }
    return t;
}

// '%' is quadratic in cost: 200 halves each dimension (1/4 of the bytes). The two streams
// may end up at different sizes; that is intentional (see g_stream_w).
void compute_target(uint32_t render_w, uint32_t render_h)
{
    static bool s_percent_read = false;   // rule 7: the ini is read once per session
    g_render_w = render_w;
    g_render_h = render_h;
    if (!s_percent_read) {
        s_percent_read = true;
        for (int i = 1; i <= 2; ++i) {
            g_percent[i] = read_percent(i);
            g_ini_percent[i] = g_percent[i];
        }
    }
    for (int i = 1; i <= 2; ++i) {
        g_scaled[i] = (g_percent[i] > 100);
        if (!g_scaled[i]) {
            g_stream_w[i] = render_w;
            g_stream_h[i] = render_h;
            continue;
        }
        g_stream_w[i] = target_dim(render_w, g_percent[i]);
        g_stream_h[i] = target_dim(render_h, g_percent[i]);
        if (g_stream_w[i] >= render_w || g_stream_h[i] >= render_h) {
            g_scaled[i] = false;
            g_stream_w[i] = render_w;
            g_stream_h[i] = render_h;
        }
    }
}

// UI transport format. R10/RGBA8 keep their format; FP16 scRGB becomes R10 PQ (the
// downsample shader converts); anything else falls back to RGBA8.
DXGI_FORMAT ui_transport_format(DXGI_FORMAT src)
{
    switch (src) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;   // downsample_ui.hlsl applies PQ
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void compute_ui_target(uint32_t ui_w, uint32_t ui_h, DXGI_FORMAT src_format)
{
    if (ui_w < 64 || ui_h < 64) {
        g_ui_w = g_ui_h = 0;
        g_stream_w[3] = g_stream_h[3] = 0;
        g_ui_format = 0;
        g_scaled[3] = false;
        return;
    }
    static bool s_ui_percent_read = false;
    if (!s_ui_percent_read) {
        s_ui_percent_read = true;
        g_percent[3] = read_percent(3);
        g_ini_percent[3] = g_percent[3];
    }
    g_ui_w = ui_w;
    g_ui_h = ui_h;
    g_ui_format = static_cast<uint32_t>(ui_transport_format(src_format));
    const bool convert = (g_ui_format != static_cast<uint32_t>(src_format));
    g_scaled[3] = (g_percent[3] > 100) || convert;
    if (!g_scaled[3]) {
        g_stream_w[3] = ui_w;
        g_stream_h[3] = ui_h;
        return;
    }
    g_stream_w[3] = target_dim(ui_w, g_percent[3]);
    g_stream_h[3] = target_dim(ui_h, g_percent[3]);
    if (g_stream_w[3] >= ui_w && g_stream_h[3] >= ui_h && !convert) {
        g_scaled[3] = false;
        g_stream_w[3] = ui_w;
        g_stream_h[3] = ui_h;
    }
}


// One compute dispatch per stream, on the title's list (must be DIRECT or COMPUTE).
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);   // below
// Root signature: b0 = 4 root constants (src w/h, dst w/h), t0 = source, u0 = target.
// Root descriptors carry no format, so the resource's own format decides: R16G16_FLOAT
// for MV, R32_FLOAT/D32_FLOAT for depth.
bool ensure_scale_objects(ID3D12Device* device)
{
    if (g_ds_rs && g_ds_pso[1] && g_ds_pso[2] && g_ds_pso[3]) { return true; }

    D3D12_DESCRIPTOR_RANGE range_srv{};
    range_srv.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_srv.NumDescriptors                    = 1;
    range_srv.BaseShaderRegister                = 0;
    D3D12_DESCRIPTOR_RANGE range_uav{};
    range_uav.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range_uav.NumDescriptors                    = 1;
    range_uav.BaseShaderRegister                = 0;

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister  = 0;
    p[0].Constants.Num32BitValues  = 6;      // src w/h, dst w/h, mode, premultiplied
    p[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    // Textures cannot be root descriptors (only raw/structured buffers can), so both the
    // source and the target go through a small descriptor table.
    p[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges   = &range_srv;
    p[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges   = &range_uav;
    p[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters   = p;
    rs.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        logf("downscale: D3D12SerializeRootSignature FAILED\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_ds_rs)))) {
        logf("downscale: CreateRootSignature FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 6;          // mv srv,uav | depth srv,uav | ui srv,uav
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ds_heap)))) {
        logf("downscale: CreateDescriptorHeap FAILED\n");
        return false;
    }

    static const unsigned char* kCs[4] = { nullptr, kDownsampleMVCSO, kDownsampleDepthCSO,
                                          kDownsampleUICSO };
    static const size_t kCsSize[4] = { 0, sizeof(kDownsampleMVCSO),
                                       sizeof(kDownsampleDepthCSO),
                                       sizeof(kDownsampleUICSO) };
    for (int stream = 1; stream <= 3; ++stream) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature    = g_ds_rs.Get();
        pd.CS.pShaderBytecode = kCs[stream];
        pd.CS.BytecodeLength  = kCsSize[stream];
        const HRESULT hr = device->CreateComputePipelineState(&pd,
                                                              IID_PPV_ARGS(&g_ds_pso[stream]));
        if (FAILED(hr)) {
            logf("downscale: CreateComputePipelineState FAILED (stream %d, %zu bytes)"
                 " hr=0x%08lX\n", stream, kCsSize[stream], static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

// failure). The caller then simply stops publishing.
bool ensure_scale_target(ID3D12Device* device, int stream, DXGI_FORMAT fmt)
{
    if (g_ds_tex[stream] && g_ds_w[stream] == g_stream_w[stream] &&
        g_ds_h[stream] == g_stream_h[stream]) {
        if (g_ds_tex[stream]->GetDesc().Format == fmt) { return true; }
    }
    g_ds_tex[stream].Reset();
    g_ds_state[stream] = D3D12_RESOURCE_STATE_COMMON;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = g_stream_w[stream];
    rd.Height           = g_stream_h[stream];
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&g_ds_tex[stream]));
    if (FAILED(hr)) {
        logf("downscale: CreateCommittedResource FAILED (stream %d %ux%u fmt %u) hr=0x%08lX\n",
             stream, g_stream_w[stream], g_stream_h[stream], static_cast<unsigned>(fmt),
             static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool scale_dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                    uint32_t declared_state, int stream)
{
    if (list == nullptr || src == nullptr) { return false; }
    ID3D12Device* device = g_device.Get();
    if (!ensure_scale_objects(device)) { return false; }

    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    // Depth: read via the R32 plane and write an R32_FLOAT target. A depth format can be
    // neither an SRV (D32_FLOAT) nor a UAV target, and both are INVOCATION errors that
    // remove the device - measured on CP2077.
    //
    // R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT (wide depth, 8 bytes) needs the X8X24
    // plane format; using plain R32_FLOAT here is an invalid SRV and removed the device
    // on PBZ. R32_TYPELESS / D32_FLOAT / R32_FLOAT use plain R32_FLOAT.
    const bool wide_depth = (sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                             sd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    const bool is_ui = (stream == 3);
    DXGI_FORMAT ui_srv_fmt = sd.Format;
    if (is_ui) {
        if (sd.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) { ui_srv_fmt = DXGI_FORMAT_R10G10B10A2_UNORM; }
        if (sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)    { ui_srv_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
    }
    const DXGI_FORMAT srv_fmt = is_ui
        ? ui_srv_fmt
        : ((stream == 2)
               ? (wide_depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT)
               : sd.Format);
    const DXGI_FORMAT target_fmt = is_ui
        ? static_cast<DXGI_FORMAT>(g_ui_format)
        : ((stream == 2) ? DXGI_FORMAT_R32_FLOAT : sd.Format);
    if (!ensure_scale_target(device, stream, target_fmt)) { return false; }
    g_ds_w[stream] = g_stream_w[stream];
    g_ds_h[stream] = g_stream_h[stream];

    const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool state_unknown = (declared_state == 0xFFFFFFFFu);
    const D3D12_RESOURCE_STATES before =
        (declared_state != 0 && !state_unknown)
            ? static_cast<D3D12_RESOURCE_STATES>(declared_state)
            : read_state;
    if (before != read_state) { barrier(list, src, before, read_state); }
    if (g_ds_state[stream] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        barrier(list, g_ds_tex[stream].Get(), g_ds_state[stream],
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_ds_state[stream] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    uint32_t mode = 0;
    if (is_ui && g_ui_source == mgpu::kUiSourceHudless) {
        // 2 = opaque HUDless pass-through, 3 = opaque HUDless scRGB -> PQ. The source
        // alpha of a HUDless target is not meaningful, so the shader forces alpha = 1.
        mode = 2u + ((sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                      g_ui_format == DXGI_FORMAT_R10G10B10A2_UNORM) ? 1u : 0u);
    } else if (is_ui && sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
               g_ui_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        mode = 1u;
    }
    uint32_t params[6] = {
        static_cast<uint32_t>(sd.Width), static_cast<uint32_t>(sd.Height),
        g_stream_w[stream], g_stream_h[stream],
        mode,
        g_ui_premultiplied ? 1u : 0u
    };

    // Descriptors: textures need a table (root SRV/UAV would only accept raw buffers).
    ID3D12Resource* dst = g_ds_tex[stream].Get();
    const uint32_t slot = (stream == 1) ? 0u : (stream == 2) ? 2u : 4u;
    const UINT inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        g_ds_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(inc) * slot;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        g_ds_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(inc) * slot;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    // A depth resource cannot be viewed with its own format (D32_FLOAT as an SRV is
    // DXGI_ERROR_INVALID_CALL and removes the device - measured). R32_FLOAT is the
    // documented SRV format for the D32/R32 family and is accepted for both D32_FLOAT and
    // R32_TYPELESS sources. MV keeps its own format (R16G16_FLOAT).
    srv.Format                  = srv_fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = dst->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu_uav);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;

    list->SetComputeRootSignature(g_ds_rs.Get());
    list->SetPipelineState(g_ds_pso[stream].Get());
    // A root descriptor table is only valid while its heap is bound; the title sets its
    // own heaps before every pass, so this is restored by its next draw/dispatch.
    ID3D12DescriptorHeap* heaps[1] = { g_ds_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->Dispatch((g_stream_w[stream] + 7) / 8, (g_stream_h[stream] + 7) / 8, 1);

    // UAV write -> copy read needs both a UAV barrier and the state change.
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = g_ds_tex[stream].Get();
    list->ResourceBarrier(1, &ub);
    barrier(list, g_ds_tex[stream].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_ds_state[stream] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (before != read_state) { barrier(list, src, read_state, before); }

    static bool told[4] = {};
    if (!told[stream]) {
        told[stream] = true;
        logf("downscale: %s %llux%llu -> %ux%u fmt %u (StreamPercent=%u)\n",
             stream == 1 ? "mv" : (stream == 2 ? "depth" : "ui"),
             static_cast<unsigned long long>(sd.Width),
             static_cast<unsigned long long>(sd.Height),
             g_stream_w[stream], g_stream_h[stream], static_cast<unsigned>(target_fmt),
             g_percent[stream]);
    }
    return true;
}

// ---------------------------------------------------------------- fr codec encode
// One compute pass per frame: read the game's MV texture, write the compressed bytes into
// the slot. Recorded on the *game's* list (like scale_dispatch) because the MV texture is
// only valid there; the copy queue waits on the game's fence before publishing, so the
// forward fence still covers this write.
bool ensure_fr_objects(ID3D12Device* device)
{
    if (g_fr_rs && g_fr_pso && g_fr_pso_d32 && g_fr_tilelen[0] && g_fr_tilelen[1]) { return true; }

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (int i = 0; i < 3; ++i) {
        ranges[i].NumDescriptors     = 1;
        ranges[i].BaseShaderRegister = static_cast<UINT>(i == 0 ? 0 : (i - 1));
        ranges[i].RangeType          = (i == 0) ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                                : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
    D3D12_ROOT_PARAMETER rp[4]{};
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 6;
    rp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 3; ++i) {
        rp[i + 1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        rp[i + 1].DescriptorTable.pDescriptorRanges   = &ranges[i];
        rp[i + 1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters   = rp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_fr_rs)))) {
        logf("fr: root signature FAILED\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;                 // srv(texture) | uav(slot) | uav(TileLen)
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fr_heap)))) {
        logf("fr: descriptor heap FAILED\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = g_fr_rs.Get();
    pd.CS.pShaderBytecode = kFrEncodeCSO;
    pd.CS.BytecodeLength  = sizeof(kFrEncodeCSO);
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_pso)))) {
        logf("fr: CreateComputePipelineState FAILED (%zu bytes)\n", sizeof(kFrEncodeCSO));
        return false;
    }
    pd.CS.pShaderBytecode = kFrEncodeD32CSO;
    pd.CS.BytecodeLength  = sizeof(kFrEncodeD32CSO);
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fr_pso_d32)))) {
        logf("fr: CreateComputePipelineState(D32) FAILED (%zu bytes)\n",
             sizeof(kFrEncodeD32CSO));
        return false;
    }

    // TileLen[] is bookkeeping the shader always writes; give it a real buffer. Sized for
    // a generous upper bound (4096x4096 / 32x8 = 65536 entries = 256 KB).
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = fr_tiles(4096, 4096) * 4ull;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Buffers cannot be created in COMMON: the shader writes this as a UAV every frame, so
    // it is born in UNORDERED_ACCESS (a UAV write from COMMON was the prime suspect for
    // the INVALID_CALL device removal, 2026-11-21).
    for (int i = 0; i < 2; ++i) {
        // A buffer is effectively COMMON whatever the initial state says (the D3D12 debug
        // layer reports that explicitly), and COMMON is implicitly promoted for both a UAV
        // write and a copy read - so no transition is needed anywhere.
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr,
                                                   IID_PPV_ARGS(&g_fr_tilelen[i])))) {
            logf("fr: TileLen buffer FAILED (%d)\n", i);
            return false;
        }
    }
    logf("fr: encode pass ready (mode %u, tile %ux%u, %u words per slot tile)\n",
         mv_codec(), kFrTileW, kFrTileH, kFrSlotWords);
    return true;
}

bool fr_encode_dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                        uint32_t declared_state, int stream, uint32_t slot)
{
    if (list == nullptr || src == nullptr) { return false; }
    ID3D12Device* device = g_device.Get();
    if (!ensure_fr_objects(device)) { return false; }
    ID3D12Resource* dst = (slot < mgpu::kMaxSlots) ? g_slot[stream][slot].Get() : nullptr;
    if (dst == nullptr || dst->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return false;
    }
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    // stream 1 = MV (fp16 pairs), stream 2 = depth (one R32 value per pixel, read through
    // an R32_FLOAT view; R32_TYPELESS is the usual source format).
    const bool d32 = (stream == 2);
    const DXGI_FORMAT codec_srv_fmt = d32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    if (d32 ? (sd.Format != DXGI_FORMAT_R32_TYPELESS && sd.Format != DXGI_FORMAT_R32_FLOAT)
            : (sd.Format != DXGI_FORMAT_R16G16_FLOAT)) {
        return false;
    }

    const uint32_t w = g_stream_w[stream] ? g_stream_w[stream] : static_cast<uint32_t>(sd.Width);
    const uint32_t h = g_stream_h[stream] ? g_stream_h[stream] : static_cast<uint32_t>(sd.Height);
    const uint32_t tiles = fr_tiles(w, h);
    // The slot is sized for the content that was current at layout time. A later preset
    // change that grows the render size must not write past it - drop MV instead.
    const uint64_t cap_tiles = dst->GetDesc().Width / (kFrSlotWords * 4u);
    if (tiles > cap_tiles) {
        static bool told_overflow = false;
        if (!told_overflow) {
            told_overflow = true;
            logf("fr: MV content %ux%u needs %u tiles but the slot holds %llu - MV dropped"
                 " (raise StreamPercentMv to 100 at the largest render size, or restart)\n",
                 w, h, tiles, static_cast<unsigned long long>(cap_tiles));
        }
        return false;
    }

    // MvCodec 1 = UAV straight into the shared slot (the real path); 2 = same but into a
    // private buffer (bisect: is a UAV write to a cross-adapter buffer the invalid call?);
    // 3 = everything except the Dispatch. See docs/design/LOSSLESS_TRANSPORT.md 17.6.
    const uint32_t fr_mode = mv_codec();
    ID3D12Resource* uav_target = dst;
    if (fr_mode == 2) {
        const uint64_t need = dst->GetDesc().Width;
        if (!g_fr_stage || g_fr_stage_bytes < need) {
            g_fr_stage.Reset();
            D3D12_HEAP_PROPERTIES shp{};
            shp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC sbd{};
            sbd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            sbd.Width            = need;
            sbd.Height           = 1;
            sbd.DepthOrArraySize = 1;
            sbd.MipLevels        = 1;
            sbd.Format           = DXGI_FORMAT_UNKNOWN;
            sbd.SampleDesc.Count = 1;
            sbd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            sbd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &shp, D3D12_HEAP_FLAG_NONE, &sbd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr, IID_PPV_ARGS(&g_fr_stage)))) {
                logf("fr: staging buffer FAILED (%llu B)\n",
                     static_cast<unsigned long long>(need));
                return false;
            }
            g_fr_stage_bytes = need;
            logf("fr: MvCodec=2 bisect - the encode target is a private %llu B buffer\n",
                 static_cast<unsigned long long>(need));
        }
        uav_target = g_fr_stage.Get();
    }

    const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool state_unknown = (declared_state == 0xFFFFFFFFu);
    const D3D12_RESOURCE_STATES before =
        (declared_state != 0 && !state_unknown)
            ? static_cast<D3D12_RESOURCE_STATES>(declared_state)
            : read_state;
    if (before != read_state) { barrier(list, src, before, read_state); }
    if (fr_mode == 1) {
        barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const UINT inc = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_fr_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_fr_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = codec_srv_fmt;          // R16G16_FLOAT / R32_FLOAT
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    // A raw (ByteAddressBuffer) UAV is Format R32_TYPELESS + stride 0 + FLAG_RAW. UNKNOWN
    // is rejected by the debug layer ("When creating a RAW Unordered Access View, the
    // format must be DXGI_FORMAT_R32_TYPELESS") and undefined at execution time - that was
    // the DXGI_ERROR_INVALID_CALL device removal measured on 2026-11-21.
    uav.Format                     = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_RAW;
    uav.Buffer.NumElements = static_cast<UINT>(uav_target->GetDesc().Width / 4ull);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += inc;
    device->CreateUnorderedAccessView(uav_target, nullptr, &uav, cpu_uav);

    // TileLen is an RWStructuredBuffer<uint> in the shader, so the view must be STRUCTURED
    // (stride 4). A raw view silently drops every write - that is why this readback used to
    // report 0.00 MB while the transport itself was fine.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavtl{};
    uavtl.Format                     = DXGI_FORMAT_UNKNOWN;
    uavtl.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    uavtl.Buffer.StructureByteStride = 4;
    uavtl.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;
    uavtl.Buffer.NumElements = static_cast<UINT>(g_fr_tilelen[d32 ? 1 : 0]->GetDesc().Width / 4ull);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_tl = cpu_uav;
    cpu_tl.ptr += inc;
    device->CreateUnorderedAccessView(g_fr_tilelen[d32 ? 1 : 0].Get(), nullptr, &uavtl, cpu_tl);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_tl = gpu_uav;
    gpu_tl.ptr += inc;

    uint32_t params[6] = { w, h, 1u,
                           (w + kFrTileW - 1u) / kFrTileW,
                           (h + kFrTileH - 1u) / kFrTileH,
                           kFrSlotWords };
    list->SetComputeRootSignature(g_fr_rs.Get());
    list->SetPipelineState(d32 ? g_fr_pso_d32.Get() : g_fr_pso.Get());
    ID3D12DescriptorHeap* heaps[1] = { g_fr_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, 6, params, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, gpu_uav);
    list->SetComputeRootDescriptorTable(3, gpu_tl);
    if (fr_mode != 3) { list->Dispatch(tiles, 1, 1); }

    if (fr_mode != 3) { fr_bytes_tick(list, tiles, d32 ? 1 : 0); }
    if (fr_mode == 1 || fr_mode == 2) {
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = uav_target;
        list->ResourceBarrier(1, &ub);
    }
    if (fr_mode == 1) {
        barrier(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (before != read_state) { barrier(list, src, read_state, before); }

    static bool told[2] = { false, false };
    const int si = d32 ? 1 : 0;
    if (!told[si]) {
        told[si] = true;
        logf("fr: encode %s %ux%u -> %u tiles, slot %llu B\n",
             d32 ? "depth" : "mv", w, h, tiles,
             static_cast<unsigned long long>(dst->GetDesc().Width));
    }
    return true;
}

// Returns false when the block cannot exist (no Device3, mapping/heap/fence
// failure). The caller then simply stops publishing.
bool wait_fence(ID3D12Fence* f, uint64_t value, DWORD ms)
{
    if (!f || f->GetCompletedValue() >= value) { return true; }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { return false; }
    if (FAILED(f->SetEventOnCompletion(value, ev))) { CloseHandle(ev); return false; }
    const DWORD r = WaitForSingleObject(ev, ms);
    CloseHandle(ev);
    return r == WAIT_OBJECT_0;
}

// Opens this frame's async copy list. The previous frame's copies must have retired before
// the allocator is reset; they finish a few ms after the previous present, so this is
// effectively instant (bounded + logged if it ever is not).
void copy_begin_frame()
{
    if (!g_copy_enabled && !g_dma_enabled) { return; }
    const ULONGLONG w0 = GetTickCount64();
    if (g_copy_done && g_copy_done->GetCompletedValue() < g_copy_done_value) {
        if (!wait_fence(g_copy_done.Get(), g_copy_done_value, 50)) {
            static bool told = false;
            if (!told) { told = true; logf("copy: previous list did not retire in 50 ms\n"); }
        }
    }
    {
        const double dt = static_cast<double>(GetTickCount64() - w0);
        g_copy_wait_ema = (g_copy_wait_ema > 0.0) ? g_copy_wait_ema * 0.9 + dt * 0.1 : dt;
        if (dt > g_copy_wait_max) { g_copy_wait_max = dt; }
    }
    if (g_copy_list_open) { g_copy_list->Close(); g_copy_list_open = false; }
    if (g_dma_list_open) { g_dma_list->Close(); g_dma_list_open = false; }
    if (g_copy_enabled) {
        g_copy_alloc->Reset();
        g_copy_list->Reset(g_copy_alloc.Get(), nullptr);
        g_copy_list_open = true;
    }
    if (g_dma_enabled) {
        g_dma_alloc->Reset();
        g_dma_list->Reset(g_dma_alloc.Get(), nullptr);
        g_dma_list_open = true;
    }
    g_copy_open = false;
    g_copy_use_dma = false;
    g_copy_active_q = nullptr;
    g_copy_active_list = nullptr;
}

bool ensure_shared(ID3D12Device* device, uint32_t render_w, uint32_t render_h,
                   uint32_t mv_format, uint32_t colour_w, uint32_t colour_h,
                   uint32_t colour_format, uint32_t ui_w, uint32_t ui_h,
                   uint32_t ui_format)
{
    if (g_ready) { return true; }
    if (device == nullptr || render_w < 256 || render_h < 256) { return false; }
    if (colour_w < 256 || colour_h < 256 || colour_format == 0) {
        // lock_frame: ColourStream=0 means this is the normal state, not a fault.
        if (g_want_colour_session) {
            logf("producer: no usable colour resource in the record - colour stream disabled\n");
        }
        g_want_colour_session = false;      // no usable colour source: MV/depth only
    }

    ComPtr<ID3D12Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
        logf("shared: ID3D12Device3 unavailable (no OpenExistingHeapFromFileMapping)\n");
        return false;
    }

    g_mv_format = mv_format;
    // A depth-stencil format cannot live on a cross-adapter heap (measured:
    // D32_FLOAT_SZ24 -> E_INVALIDARG). R32_TYPELESS is the one the heap accepts and a
    // plain D32_FLOAT/R32_FLOAT source can be copied into (format_family).
    g_depth_format = DXGI_FORMAT_R32_TYPELESS;

    // v22: colour capacity is the OUTPUT size (a DLSS performance preset renders at
    // 720p/1080p but presents at 4K), capped to what the mapping can hold.
    if (g_want_colour_session) {
        g_colour_w = g_stream_w[mgpu::kStreamColour] = colour_w;
        g_colour_h = g_stream_h[mgpu::kStreamColour] = colour_h;
        g_colour_format = colour_format;
        g_colour_slot_w = cap_dim(colour_w);
        g_colour_slot_h = cap_dim(colour_h);
        if (g_colour_slot_w < colour_w || g_colour_slot_h < colour_h) {
            g_want_colour_session = false;
            logf("producer: colour %ux%u is above the %ux%u mapping capacity -"
                 " colour stream disabled\n", colour_w, colour_h,
                 mgpu::kStreamMaxW, mgpu::kStreamMaxH);
        }
    } else {
        g_colour_w = g_colour_h = g_colour_slot_w = g_colour_slot_h = 0;
        g_colour_format = 0;
    }

    // B mode 4: the UI stream is laid out once, at its downsampled size. If the title has
    // no UI resource this stays zero and the consumer never sees stream 3.
    if (g_want_ui_session && ui_w >= 64 && ui_h >= 64 && ui_format != 0) {
        compute_ui_target(ui_w, ui_h, static_cast<DXGI_FORMAT>(ui_format));
        g_ui_slot_w = cap_dim(g_stream_w[3]);
        g_ui_slot_h = cap_dim(g_stream_h[3]);
        if (g_ui_slot_w < g_stream_w[3] || g_ui_slot_h < g_stream_h[3]) {
            logf("producer: UI %ux%u is above the %ux%u mapping capacity - UI stream disabled\n",
                 ui_w, ui_h, mgpu::kStreamMaxW, mgpu::kStreamMaxH);
            g_ui_w = g_ui_h = 0;
            g_ui_slot_w = g_ui_slot_h = 0;
            g_ui_format = 0;
            g_stream_w[3] = g_stream_h[3] = 0;
        } else {
            logf("producer: UI %ux%u fmt %u -> %ux%u fmt %u (StreamPercentUi=%u)\n",
                 ui_w, ui_h, ui_format, g_stream_w[3], g_stream_h[3], g_ui_format,
                 g_percent[3]);
        }
    } else {
        g_ui_w = g_ui_h = 0;
        g_ui_slot_w = g_ui_slot_h = 0;
        g_ui_format = 0;
        g_stream_w[3] = g_stream_h[3] = 0;
    }

    // "auto" codec: decide BEFORE the layout, because the layout picks buffer vs texture
    // slots and the stream sizes (a codec stream runs at 100%).
    fr_resolve_codec(device);
    compute_target(render_w, render_h);
    // Capacity follows the actual stream size, not the render size. lock_frame fixes
    // StreamPercent (150), so there is no runtime request that can grow the streams; sizing
    // at render resolution instead reserved ~1.1 GB for 16 slots at 4K and the mapping/heap
    // creation stalled the device on PBZ. Use the largest stream we place.
    // v30: with MvCodec=1 the MV slot is a byte buffer sized from its own 1:1 content (see
    // layout_streams), so it must not drive the texture capacity. Letting it do so reserved
    // 3840x2304 x 32 slots = 2.3 GB and the transport creation then starved the WGC
    // capture-item creation at startup (measured: CreateForWindow FAILED, 2026-11-21).
    const bool codec_mv = (mv_codec() != 0 &&
                           g_mv_format == DXGI_FORMAT_R16G16_FLOAT);
    uint32_t capw = codec_mv ? 0u : g_stream_w[mgpu::kStreamMv];
    uint32_t caph = codec_mv ? 0u : g_stream_h[mgpu::kStreamMv];
    if (g_stream_w[mgpu::kStreamDepth] > capw) { capw = g_stream_w[mgpu::kStreamDepth]; }
    if (g_stream_h[mgpu::kStreamDepth] > caph) { caph = g_stream_h[mgpu::kStreamDepth]; }
    if (g_want_colour_session) {
        if (colour_w > capw) { capw = colour_w; }
        if (colour_h > caph) { caph = colour_h; }
    }
    if (capw < 256) { capw = render_w; }
    if (caph < 256) { caph = render_h; }
    g_slot_w   = cap_dim(capw);
    g_slot_h   = cap_dim(caph);
    if (g_scaled[1] || g_scaled[2]) {
        logf("producer: StreamPercent mv=%u depth=%u -> streams %ux%u / %ux%u"
             " (render %ux%u, capacity %ux%u)\n",
             g_percent[1], g_percent[2], g_stream_w[1], g_stream_h[1],
             g_stream_w[2], g_stream_h[2], g_render_w, g_render_h, g_slot_w, g_slot_h);
    }

    mgpu::SharedControl probe{};
    layout_streams(device, &probe, g_slot_w, g_slot_h,
                   g_want_colour_session ? g_colour_w : 0,
                   g_want_colour_session ? g_colour_h : 0,
                   g_want_colour_session ? g_colour_slot_w : 0,
                   g_want_colour_session ? g_colour_slot_h : 0,
                   g_want_colour_session ? g_colour_format : 0,
                   g_stream_w[3], g_stream_h[3], g_ui_slot_w, g_ui_slot_h,
                   g_ui_format);

    uint64_t reserve = mgpu::kDataStart;
    for (int i = 0; i < static_cast<int>(mgpu::kMaxStreams); ++i) {
        reserve += probe.streams[i].slot_size * mgpu::kMaxSlots;
    }

    logf("shared: begin mapping reserve=%.1f MB slots=%u capacity=%ux%u streams %ux%u/%ux%u\n",
         static_cast<double>(reserve) / 1048576.0, mgpu::kMaxSlots,
         g_slot_w, g_slot_h, g_stream_w[1], g_stream_h[1], g_stream_w[2], g_stream_h[2]);

    std::wstring name(mgpu::kMappingName, mgpu::kMappingName + strlen(mgpu::kMappingName));
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               static_cast<DWORD>(reserve >> 32),
                               static_cast<DWORD>(reserve & 0xFFFFFFFFu), name.c_str());
    if (g_map == nullptr) {
        logf("shared: CreateFileMapping FAILED err=%lu\n", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        logf("shared: mapping already existed - a previous producer or a running bridge owns it\n");
    }
    logf("shared: mapping ready (%llu bytes)\n", static_cast<unsigned long long>(reserve));
    g_view = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (g_view == nullptr) {
        logf("shared: MapViewOfFile FAILED err=%lu\n", GetLastError());
        return false;
    }
    logf("shared: view mapped\n");

    const HRESULT heap_hr = device3->OpenExistingHeapFromFileMapping(g_map, IID_PPV_ARGS(&g_heap));
    if (FAILED(heap_hr)) {
        logf("shared: OpenExistingHeapFromFileMapping FAILED hr=0x%08lX (%.1f MB)\n",
             static_cast<unsigned long>(heap_hr), static_cast<double>(reserve) / 1048576.0);
        return false;
    }
    logf("shared: heap opened\n");

    if ((g_want_colour_session &&
         !place_slots(device, mgpu::kStreamColour, g_colour_format,
                      g_colour_slot_w, g_colour_slot_h,
                      probe.streams[mgpu::kStreamColour].offset,
                      probe.streams[mgpu::kStreamColour].slot_size)) ||
        !place_slots(device, mgpu::kStreamMv, g_mv_format, g_slot_w, g_slot_h,
                     probe.streams[mgpu::kStreamMv].offset,
                     probe.streams[mgpu::kStreamMv].slot_size) ||
        !place_slots(device, mgpu::kStreamDepth, g_depth_format, g_slot_w, g_slot_h,
                     probe.streams[mgpu::kStreamDepth].offset,
                     probe.streams[mgpu::kStreamDepth].slot_size) ||
        (g_ui_format != 0 &&
         !place_slots(device, mgpu::kStreamUi, g_ui_format, g_ui_slot_w, g_ui_slot_h,
                      probe.streams[mgpu::kStreamUi].offset,
                      probe.streams[mgpu::kStreamUi].slot_size))) {
        return false;
    }
    logf("shared: placed textures ready\n");

    const D3D12_FENCE_FLAGS ff =
        (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER);
    for (uint32_t s = 0; s < mgpu::kMaxSlots; ++s) {
        if (FAILED(device->CreateFence(0, ff, IID_PPV_ARGS(&g_fwd[s]))) ||
            FAILED(device->CreateFence(0, ff, IID_PPV_ARGS(&g_back[s])))) {
            logf("shared: CreateFence FAILED\n");
            return false;
        }
        HANDLE h = nullptr;
        if (FAILED(device->CreateSharedHandle(g_fwd[s].Get(), nullptr, GENERIC_ALL, nullptr, &h))) {
            logf("shared: forward fence handle FAILED\n");
            return false;
        }
        probe.fence_forward[s] = reinterpret_cast<uint64_t>(h);
        h = nullptr;
        if (FAILED(device->CreateSharedHandle(g_back[s].Get(), nullptr, GENERIC_ALL, nullptr, &h))) {
            logf("shared: back fence handle FAILED\n");
            return false;
        }
        probe.fence_back[s] = reinterpret_cast<uint64_t>(h);
    }

    // Second DIRECT queue for the cross-adapter copy (see g_copy_q). If anything here
    // fails we keep the old inline path: correctness first, the cost is what we are here
    // to remove, not to make mandatory.
    {
        D3D12_COMMAND_QUEUE_DESC cq{};
        cq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        cq.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        HRESULT hr = device->CreateCommandQueue(&cq, IID_PPV_ARGS(&g_copy_q));
        if (SUCCEEDED(hr)) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g_copy_alloc));
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_copy_alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&g_copy_list));
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copy_done));
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_frame_fence));
        }
        if (SUCCEEDED(hr)) {
            g_copy_list->Close();
            g_copy_enabled = true;
            logf("producer: async copy queue ready (copies leave the game's queue)\n");
        } else {
            logf("producer: async copy queue FAILED hr=0x%08lX - copies stay on the game's"
                 " queue\n", static_cast<unsigned long>(hr));
            g_copy_q.Reset();
            g_copy_alloc.Reset();
            g_copy_list.Reset();
            g_copy_done.Reset();
            g_frame_fence.Reset();
            g_copy_enabled = false;
        }
    }

    // DMA queue: a COPY queue uses the copy engine instead of the 3D/compute engine, so
    // the render card's "GPU" utilization does not include the transfer. It cannot emit
    // barriers, so it is used only when every source is already in COPY_SOURCE (CP2077
    // declares state 0x800 for both streams); otherwise the DIRECT async queue is used.
    {
        D3D12_COMMAND_QUEUE_DESC dq{};
        dq.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        dq.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        HRESULT hr = device->CreateCommandQueue(&dq, IID_PPV_ARGS(&g_dma_q));
        if (SUCCEEDED(hr)) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                IID_PPV_ARGS(&g_dma_alloc));
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                           g_dma_alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&g_dma_list));
        }
        if (SUCCEEDED(hr)) {
            g_dma_list->Close();
            g_dma_enabled = true;
            logf("producer: DMA (COPY) queue ready - transfers leave the 3D engine\n");
        } else {
            logf("producer: DMA queue FAILED hr=0x%08lX - the DIRECT async queue is used\n",
                 static_cast<unsigned long>(hr));
            g_dma_q.Reset();
            g_dma_alloc.Reset();
            g_dma_list.Reset();
            g_dma_enabled = false;
        }
    }

    const LUID luid = device->GetAdapterLuid();
    probe.magic          = mgpu::kSharedMagic;   // v9: non-zero = the mapping is usable
    probe.version        = mgpu::kSharedVersion;
    probe.producer_pid   = GetCurrentProcessId();
    probe.slots          = mgpu::kMaxSlots;
    probe.stream_count   = 5;                 // colour + mv + depth + ui + scar (0..4)
    probe.mapping_size   = reserve;
    probe.generation     = 1;
    probe.slot_policy    = 0;                 // drop
    probe.producer.device_luid = mgpu::pack_luid(luid.HighPart, luid.LowPart);
    probe.producer.input_api   = mgpu::kInputApiFaker;
    probe.producer.render_w    = g_render_w ? g_render_w : g_stream_w[1];
    probe.producer.render_h    = g_render_h ? g_render_h : g_stream_h[1];
    probe.producer.output_w    = g_frame.output_w ? g_frame.output_w : probe.producer.render_w;
    probe.producer.output_h    = g_frame.output_h ? g_frame.output_h : probe.producer.render_h;
    probe.active_percent_mv    = g_percent[1];
    probe.active_percent_depth = g_percent[2];
    memset(probe.frames, 0, sizeof(probe.frames));

    // magic last: the consumer treats a zero magic as "still being created".
    memcpy(g_view, &probe, sizeof(probe));

    g_device = device;
    g_ready  = true;
    g_ready_ms = GetTickCount64();
    // What each stream actually does this session (the four combinations are all legal).
    const bool codec_plan_mv = (probe.streams[mgpu::kStreamMv].codec != 0);
    const bool codec_plan_dp = (probe.streams[mgpu::kStreamDepth].codec != 0);
    logf("fr: transport plan: mv=%s%ux%u depth=%s%ux%u colour=%s"
         " (MvCodec=%u DepthCodec=%u StreamPercent mv/depth=%u/%u)\n",
         codec_plan_mv ? "codec " : "texture ", g_stream_w[1], g_stream_h[1],
         codec_plan_dp ? "codec " : "texture ", g_stream_w[2], g_stream_h[2],
         g_want_colour_session ? "on" : "off",
         mv_codec(), depth_codec(), g_percent[1], g_percent[2]);
    logf("shared ready: name=%hs pid=%lu used=%.1f MB slots=%u mv %ux%u fmt %u"
         " depth %ux%u fmt %u\n",
         mgpu::kMappingName, GetCurrentProcessId(),
         static_cast<double>(reserve) / 1048576.0, mgpu::kMaxSlots,
         g_stream_w[1], g_stream_h[1], g_mv_format,
         g_stream_w[2], g_stream_h[2], g_depth_format);
    return true;
}

// PBZ: OpenExistingHeapFromFileMapping can block for seconds when it runs inside the
// DLSS-G Evaluate on the game's render thread. The transport is optional for that frame,
// so build it on a worker: the title keeps presenting, and the first publication waits
// until g_ready. (The heap itself is still device work; this only takes it off the
// title's critical path.)
struct SharedInitArgs {
    ID3D12Device* device = nullptr;
    uint32_t render_w = 0, render_h = 0;
    uint32_t mv_format = 0;
    uint32_t colour_w = 0, colour_h = 0, colour_format = 0;
    uint32_t ui_w = 0, ui_h = 0, ui_format = 0;
};

DWORD WINAPI shared_init_thread(void* p)
{
    SharedInitArgs* a = static_cast<SharedInitArgs*>(p);
    if (a == nullptr) { return 1; }
    const ULONGLONG t0 = GetTickCount64();
    const bool ok = ensure_shared(a->device, a->render_w, a->render_h, a->mv_format,
                                  a->colour_w, a->colour_h, a->colour_format,
                                  a->ui_w, a->ui_h, a->ui_format);
    logf("producer: shared init worker %s after %llu ms\n",
         ok ? "ready" : "FAILED",
         static_cast<unsigned long long>(GetTickCount64() - t0));
    if (!ok) { InterlockedExchange(&g_init_failed, 1); }
    if (a->device != nullptr) { a->device->Release(); }
    delete a;
    return 0;
}

void start_shared_init(ID3D12Device* device, uint32_t render_w, uint32_t render_h,
                       uint32_t mv_format, uint32_t colour_w, uint32_t colour_h,
                       uint32_t colour_format, uint32_t ui_w, uint32_t ui_h,
                       uint32_t ui_format)
{
    SharedInitArgs* a = new SharedInitArgs{};
    a->device = device;
    a->render_w = render_w;
    a->render_h = render_h;
    a->mv_format = mv_format;
    a->colour_w = colour_w;
    a->colour_h = colour_h;
    a->colour_format = colour_format;
    a->ui_w = ui_w;
    a->ui_h = ui_h;
    a->ui_format = ui_format;
    a->device->AddRef();
    HANDLE th = CreateThread(nullptr, 0, &shared_init_thread, a, 0, nullptr);
    if (th != nullptr) {
        CloseHandle(th);
        logf("producer: shared init moved to a worker (render %ux%u, mv fmt %u)\n",
             render_w, render_h, mv_format);
        return;
    }
    a->device->Release();
    delete a;
    InterlockedExchange(&g_init_failed, 1);
    logf("producer: CreateThread for shared init FAILED\n");
}

// ---------------------------------------------------------------- credit window
// A slot may only be reused once the consumer ACKed the value we last published
// there (`consumed[k]`). While the consumer is silent for 30 frames we fall back
// to round-robin - otherwise a dead bridge would stop the producer forever.
void update_credit()
{
    mgpu::SharedControl* c = ctl();
    if (c == nullptr || !g_ready) { g_credit_on = false; return; }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < mgpu::kMaxSlots; ++i) { sum += c->consumed[i]; }
    if (sum != g_ack_sum) { g_ack_sum = sum; g_ack_idle = 0; }
    else if (g_ack_idle < 1000000ull) { ++g_ack_idle; }
    const bool alive = (g_ack_idle < kAckGraceFrames);
    if (alive != g_credit_on) {
        g_credit_on = alive;
        logf("shared: credit window %s - consumer %s\n",
             alive ? "ON" : "OFF (round-robin)",
             alive ? "is ACKing" : "silent for 30 frames");
    }
}

bool slot_free(uint32_t slot)
{
    if (!g_credit_on) { return true; }
    mgpu::SharedControl* c = ctl();
    if (c == nullptr) { return true; }
    const uint64_t published = g_fwd_value[slot];
    if (published == 0) { return true; }
    // The colour watermark is the strongest credit: anything older than the last frame
    // the consumer actually paired can never be needed again. Without this, a consumer
    // that keeps matching the same slot ACKs only that slot, slots 1..N stay un-ACKed,
    // and the producer degrades to reusing slot 0 forever (Wuchang's 1-slot ring).
    if (c->colour_seq != 0) {
        const uint64_t fid = c->frames[slot].frame_id;
        if (fid != 0 && fid < c->colour_seq) { return true; }
    }
    return c->consumed[slot] >= published;
}

bool claim_slot()
{
    update_credit();
    for (uint32_t i = 0; i < mgpu::kMaxSlots; ++i) {
        const uint32_t s = (g_next_slot + i) % mgpu::kMaxSlots;
        if (!slot_free(s)) { continue; }
        g_cur_slot = s;
        // Round-robin: without advancing g_next_slot the producer reused slot 0 on every
        // frame (as soon as the consumer ACKed it), overwriting the publication the WGC
        // colour was still in flight to match. That is exactly "marker seq = newest-1,
        // hits=0" on Wuchang: only one slot ever held the newest frame.
        g_next_slot = (s + 1u) % mgpu::kMaxSlots;
        return true;
    }

    // No ACKed slot. Dropping this publication leaves a permanent hole in frames[] (the
    // marker seq can never be paired) and collapses the ring. Reclaim the OLDEST
    // publication instead: its colour is several frames behind and can no longer be
    // matched; a continuous 32-slot stream is worth more than preserving one stale slot.
    if (mgpu::SharedControl* c = ctl()) {
        int oldest = -1;
        uint64_t oldest_pub = UINT64_MAX;
        for (uint32_t i = 0; i < mgpu::kMaxSlots; ++i) {
            const uint64_t p = g_fwd_value[i];
            if (p == 0) { oldest = static_cast<int>(i); oldest_pub = 0; break; }
            if (p < oldest_pub) { oldest_pub = p; oldest = static_cast<int>(i); }
        }
        if (oldest >= 0) {
            g_cur_slot = static_cast<uint32_t>(oldest);
            g_next_slot = (g_cur_slot + 1u) % mgpu::kMaxSlots;
            ++c->producer_skipped;   // keep the diagnostic counter meaningful
            static uint32_t reclaimed = 0;
            if (++reclaimed <= 3 || (reclaimed % 300u) == 0u) {
                logf("producer: no ACKed slot - reclaimed oldest slot %d (pub=%llu, %u so far)\n",
                     oldest, static_cast<unsigned long long>(oldest_pub), reclaimed);
            }
            return true;
        }
    }

    // v23.1: no ACKed slot. The old behaviour overwrote the oldest slot, which could be
    // the exact slot the consumer had just picked: its colour/MV/depth then came from two
    // different frames (measured as "the picture zooms/shifts every few frames") and the
    // consumer's identity check failed, flapping the MV gate. Dropping this publication is
    // strictly better: the consumer repeats the previous frame for one input interval.
    if (mgpu::SharedControl* c = ctl()) { ++c->producer_skipped; ++c->producer_overrun; }
    static uint32_t skipped = 0;
    if (++skipped <= 3 || (skipped % 300u) == 0u) {
        logf("producer: no ACKed slot - publication skipped (%u so far)\n", skipped);
    }
    return false;
}

// ---------------------------------------------------------------- copy
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    list->ResourceBarrier(1, &b);
}

// One 1:1 copy, on the list the record was published on. The source state comes
// from the record (fakerFG declares it - it is the pass that consumes the data),
// never from a guess.
bool copy_stream(ID3D12GraphicsCommandList* list, ID3D12Resource* src, int stream,
                 uint32_t declared_state)
{
    if (list == nullptr || src == nullptr) { return false; }
    ID3D12Resource* dst = g_slot[stream][g_cur_slot].Get();
    if (dst == nullptr) { return false; }

    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    static const char* const kStreamName[4] = { "colour", "mv", "depth", "ui" };
    const char* sname = (stream >= 0 && stream <= 3) ? kStreamName[stream] : "stream";
    if (sd.Width == 0 || sd.Height == 0) { return false; }
    if (static_cast<uint32_t>(sd.Width) > static_cast<uint32_t>(dd.Width) ||
        sd.Height > dd.Height) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("[%s] %llux%llu is above the placed %llux%llu - stream skipped until the"
                 " next session\n", sname,
                 static_cast<unsigned long long>(sd.Width),
                 static_cast<unsigned long long>(sd.Height),
                 static_cast<unsigned long long>(dd.Width),
                 static_cast<unsigned long long>(dd.Height));
        }
        return false;
    }
    if (!copy_formats_ok(sd.Format, dd.Format)) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("[%s] format %u cannot be copied into the slot (%u) - skipped\n",
                 sname, static_cast<unsigned>(sd.Format),
                 static_cast<unsigned>(dd.Format));
        }
        return false;
    }

    uint32_t w = static_cast<uint32_t>(sd.Width < dd.Width ? sd.Width : dd.Width);
    uint32_t h = static_cast<uint32_t>(sd.Height < dd.Height ? sd.Height : dd.Height);
    if (g_stream_w[stream] > 0 && g_stream_w[stream] < w) { w = g_stream_w[stream]; }
    if (g_stream_h[stream] > 0 && g_stream_h[stream] < h) { h = g_stream_h[stream]; }

    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    const bool can_barrier = (type != D3D12_COMMAND_LIST_TYPE_COPY);
    const bool state_unknown = (declared_state == 0xFFFFFFFFu);
    // DLSSG parameter-block resources do not carry a declared state. The previous guesses
    // (COMMON / NON_PIXEL_SRV) still crashed in nvwgf2umx, so for this lane copy from the
    // state the producer itself relies on: the caller's resource is already usable as a
    // copy source (same assumption as the old 0x800 path), and no barrier is emitted.
    const D3D12_RESOURCE_STATES unknown_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const D3D12_RESOURCE_STATES before =
        static_cast<D3D12_RESOURCE_STATES>(declared_state != 0 && !state_unknown
                                                ? declared_state
                                                : (state_unknown
                                                       ? unknown_state
                                                       : (stream == 0
                                                              ? D3D12_RESOURCE_STATE_COMMON
                                                              : (stream == 1
                                                                     ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                                     : (stream == 3
                                                                            ? D3D12_RESOURCE_STATE_COMMON
                                                                            : D3D12_RESOURCE_STATE_DEPTH_READ)))));
    // v22/v29: colour and UI may need a transition out of the game's declared state. A
    // COPY list cannot express a barrier, so skip only when a barrier is actually needed.
    // The scaled UI/HUDless target is already COPY_SOURCE, and v29's CP2077 path is
    // exactly that: the source is g_ds_tex[3], not the title's tagged resource.
    if ((stream == 0 || stream == 3) && !can_barrier &&
        before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("[%s] COPY list has no barriers - stream skipped (declared 0x%X)\n",
                 sname, declared_state);
        }
        return false;
    }

    if (can_barrier && before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barrier(list, src, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{ 0, 0, 0, w, h, 1 };
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    if (can_barrier && before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, before);
    }

    static bool told[4] = {};
    if (stream >= 0 && stream <= 3 && !told[stream]) {
        told[stream] = true;
        logf("[%s] copy %ux%u fmt %u (declared state 0x%X, list type %d)\n",
             sname, w, h, static_cast<unsigned>(sd.Format),
             declared_state, static_cast<int>(type));
    }
    return true;
}

// Applies the consumer's per-stream request (protocol v15). 0 / out of range = **revert
// to the fakerFG.ini value**, not "keep the previous request": a new bridge must be able
// to undo what the previous one asked for. Called from update_content() once per frame,
// so the bridge's controller and its A/B switches take effect without a restart.
void apply_requests()
{
    mgpu::SharedControl* c = ctl();
    if (c == nullptr) { return; }
    const uint32_t req[3] = { 0u, c->requested_percent_mv, c->requested_percent_depth };
    for (int i = 1; i <= 2; ++i) {
        g_percent[i] = g_force_full_size
                           ? 100u
                           : ((req[i] >= 100u && req[i] <= 400u) ? req[i] : g_ini_percent[i]);
    }
}

// A render-size change (DLSS preset) or a consumer request only rewrites the live
// rectangles. The slots keep their capacity, so the consumer never has to re-place
// anything. `generation` tells the consumer to rebuild its local copies.
void update_content(uint32_t render_w, uint32_t render_h)
{
    if (render_w == 0 || render_h == 0) { return; }
    apply_requests();
    const uint32_t old_rw = g_render_w, old_rh = g_render_h;
    const uint32_t old_w[3] = { 0u, g_stream_w[1], g_stream_w[2] };
    const uint32_t old_h[3] = { 0u, g_stream_h[1], g_stream_h[2] };
    compute_target(render_w, render_h);

    bool changed = (g_render_w != old_rw || g_render_h != old_rh);
    for (int i = 1; i <= 2; ++i) {
        changed = changed || (g_stream_w[i] != old_w[i]) || (g_stream_h[i] != old_h[i]);
    }
    mgpu::SharedControl* c = ctl();
    if (!changed) {
        if (c != nullptr) {
            c->active_percent_mv    = g_percent[1];
            c->active_percent_depth = g_percent[2];
        }
        return;
    }
    for (int i = 1; i <= 2; ++i) {
        if (g_stream_w[i] > g_slot_w || g_stream_h[i] > g_slot_h) {
            static bool told = false;
            if (!told) {
                told = true;
                logf("shared: stream %d %ux%u is above the placed %ux%u - content size kept\n",
                     i, g_stream_w[i], g_stream_h[i], g_slot_w, g_slot_h);
            }
            g_render_w = old_rw;
            g_render_h = old_rh;
            for (int k = 1; k <= 2; ++k) {
                g_stream_w[k] = old_w[k];
                g_stream_h[k] = old_h[k];
            }
            return;
        }
    }
    for (int i = 1; i <= 2; ++i) { g_ds_w[i] = 0; g_ds_h[i] = 0; }  // targets follow size
    if (c != nullptr) {
        for (int i = 1; i <= 2; ++i) {
            c->streams[i].seq += 1;             // odd: being written
            MemoryBarrier();
            c->streams[i].width  = g_stream_w[i];
            c->streams[i].height = g_stream_h[i];
            MemoryBarrier();
            c->streams[i].seq += 1;             // even: stable
        }
        c->producer.render_w = g_render_w;
        c->producer.render_h = g_render_h;
        c->active_percent_mv    = g_percent[1];
        c->active_percent_depth = g_percent[2];
        MemoryBarrier();
        c->generation += 1;
        memset(c->frames, 0, sizeof(c->frames));
    }
    g_next_slot = 0;
    logf("shared: content is now mv %ux%u (%%%u), depth %ux%u (%%%u), render %ux%u, gen %u"
         " - slots stay %ux%u\n",
         g_stream_w[1], g_stream_h[1], g_percent[1],
         g_stream_w[2], g_stream_h[2], g_percent[2],
         g_render_w, g_render_h, c != nullptr ? c->generation : 0, g_slot_w, g_slot_h);
}

// ---------------------------------------------------------------- record
bool consumer_present()
{
    static ULONGLONG last_check = 0;
    static bool seen = false;
    if (seen) { return true; }
    const ULONGLONG now = GetTickCount64();
    if (last_check != 0 && now - last_check < 500) { return false; }
    last_check = now;
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, mgpu::kConsumerPresentEvent);
    if (h != nullptr) {
        CloseHandle(h);
        seen = true;
        logf("producer: consumer announced itself (%ls) - transport may start\n",
             mgpu::kConsumerPresentEvent);
    }
    return seen;
}

// A new consumer must not inherit the previous consumer's ACK/frame history.
void reset_consumer_session(mgpu::SharedControl* c, uint64_t session)
{
    if (c == nullptr) { return; }
    c->colour_seq   = 0;
    c->producer_skipped = 0;
    c->frame_published = 0;
    c->active_mask = 0;
    for (uint32_t i = 0; i < mgpu::kMaxSlots; ++i) {
        c->consumed[i] = 0;
        memset(&c->frames[i], 0, sizeof(c->frames[i]));
    }
    for (uint32_t i = 0; i < mgpu::kMaxStreams; ++i) { c->stream_bytes[i] = 0; }
    c->generation += 1;
    g_next_slot = 0;
    g_last_publish_frame_qpc = 0;
    g_last_published_seq = 0;
    g_last_publish_qpc = 0;
    MemoryBarrier();
    logf("producer: NEW consumer session 0x%llX - ACK/frame state reset (generation %u)\n",
         static_cast<unsigned long long>(session), c->generation);
}

uint32_t ini_int(const wchar_t* key, uint32_t def);

// 0 = off, 1 = on, 2 = auto (the default): the codec is used whenever this session can
// actually run it, and that stream silently keeps the copy path otherwise. This is what
// makes "codec by default, degrade when the hardware cannot" work without a rebuild.
int ini_codec_mode(const wchar_t* key)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *slash = L'\0'; }
    wcscat_s(path, MAX_PATH, L"\\fakerFG.ini");
    wchar_t buf[32]{};
    GetPrivateProfileStringW(L"fakerFG", key, L"auto", buf, 32, path);
    if (buf[0] == L'0') { return 0; }
    if (buf[0] == L'1') { return 1; }
    return 2;                                    // "auto", empty or anything else
}

int  g_codec_mode[3] = { 0, -1, -1 };            // [1] = mv, [2] = depth; -1 = unread
bool g_codec_resolved = false;


uint32_t depth_codec()
{
    if (g_codec_mode[2] < 0) { g_codec_mode[2] = ini_codec_mode(L"DepthCodec"); }
    return static_cast<uint32_t>(g_codec_mode[2] >= 2 ? 1 : g_codec_mode[2]);
}

// Called once, BEFORE the layout decides the slot types: resolves "auto" against what this
// session can actually do, so a machine that cannot run the codec ends up with the old
// copy path instead of a codec that does not work.
void fr_resolve_codec(ID3D12Device* device)
{
    if (g_codec_resolved) { return; }
    for (int i = 1; i <= 2; ++i) {
        if (g_codec_mode[i] < 0) {
            g_codec_mode[i] = ini_codec_mode(i == 1 ? L"MvCodec" : L"DepthCodec");
        }
    }
    const bool want_any = (g_codec_mode[1] != 0 || g_codec_mode[2] != 0);
    const bool objs = !want_any || ensure_fr_objects(device);
    const bool fmt_mv = (g_mv_format == DXGI_FORMAT_R16G16_FLOAT);
    const bool fmt_dp = (g_depth_format == DXGI_FORMAT_R32_TYPELESS ||
                         g_depth_format == DXGI_FORMAT_R32_FLOAT ||
                         wide_depth_format(static_cast<DXGI_FORMAT>(g_depth_format)));
    if (!objs) {
        g_codec_mode[1] = 0;
        g_codec_mode[2] = 0;
    } else {
        if (!fmt_mv) { g_codec_mode[1] = 0; }
        if (!fmt_dp) { g_codec_mode[2] = 0; }
    }
    g_codec_resolved = true;
    logf("fr: codec capability: pass=%s, mv fmt %u %s, depth fmt %u %s -> MV %s, depth %s\n",
         objs ? "ok" : "FAILED", g_mv_format, fmt_mv ? "ok" : "unsupported",
         g_depth_format, fmt_dp ? "ok" : "unsupported",
         g_codec_mode[1] ? "CODEC(1:1)" : "copy",
         g_codec_mode[2] ? "CODEC(1:1)" : "copy");
}

bool ensure_fr_objects(ID3D12Device* device);   // defined with the encode pass below

// 1 = MV, 2 = depth; everything else is not a codec stream.
uint32_t stream_codec(int stream)
{
    if (stream == 1) { return mv_codec(); }
    if (stream == 2) { return depth_codec(); }
    return 0;
}

uint32_t mv_codec()
{
    if (g_codec_mode[1] < 0) { g_codec_mode[1] = ini_codec_mode(L"MvCodec"); }
    // 2 = auto: optimistic here, fr_resolve_codec() turns it off before the layout when
    // this session cannot actually run the codec. 3 = the "no Dispatch" bisect mode.
    const int m = g_codec_mode[1];
    return static_cast<uint32_t>(m == 2 ? 1 : (m > 3 ? 3 : m));
}

uint32_t ini_int(const wchar_t* key, uint32_t def)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *slash = L'\0'; }
    wcscat_s(path, MAX_PATH, L"\\fakerFG.ini");
    const int v = GetPrivateProfileIntW(L"fakerFG", key, static_cast<int>(def), path);
    return v < 0 ? 0u : static_cast<uint32_t>(v);
}

void ini_string(const wchar_t* key, const wchar_t* def, wchar_t* out, DWORD count)
{
    if (out == nullptr || count == 0) { return; }
    out[0] = L'\0';
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) { *slash = L'\0'; }
    wcscat_s(path, MAX_PATH, L"\\fakerFG.ini");
    GetPrivateProfileStringW(L"fakerFG", key, def, out, count, path);
}

// v29: pick the stream-3 source once, when the transport is created. Auto prefers the
// title's own UI (exact alpha, best unblend), then HUDless (consumer synthesises UI).
// `hudless == backbuffer` means the title never separated UI; sending that would make the
// consumer synthesise a zero diff, so it is rejected here.
const MgpuFgResource* select_ui_source(const MgpuFgFrame& f, uint32_t* out_source)
{
    // UI_SYNTHESIS_GATE: CLOSED by default. `UiSynthesis=1` is the explicit local A/B
    // override (docs/UI_SYNTHESIS_GATE.md); without it Game/Hudless synthesis stays off even
    // if UiStream=1 is written by hand.
    if (!mgpu::kUiSynthesisGateOpen && !g_ui_synthesis_override) {
        if (out_source) { *out_source = mgpu::kUiSourceOff; }
        return nullptr;
    }
    const MgpuFgResource& ui  = f.stream[MGPU_FG_STREAM_UI_COLOR];
    const MgpuFgResource& hud = f.stream[MGPU_FG_STREAM_HUDLESS];
    const MgpuFgResource& bb  = f.stream[MGPU_FG_STREAM_BACKBUFFER];
    auto usable = [](const MgpuFgResource& r) {
        // width/height can be 0 on an older provider; the descriptor fallback below
        // fills them in. A non-null resource with a format is enough to select it.
        return r.resource != nullptr && r.format != 0 &&
               (r.width == 0 || r.width >= 64) &&
               (r.height == 0 || r.height >= 64);
    };
    auto hud_usable = [&]() {
        return usable(hud) && hud.resource != bb.resource;
    };

    // Once a type was selected, keep it: stream 3's slot format was laid out for it.
    const uint32_t mode = (g_ui_source_type == mgpu::kUiSourceGame) ? 1
                        : (g_ui_source_type == mgpu::kUiSourceHudless) ? 2
                        : g_ui_source_mode;
    if (mode == 3) {
        if (out_source) { *out_source = mgpu::kUiSourceOff; }
        return nullptr;
    }
    if (mode == 1) {
        if (usable(ui)) { if (out_source) { *out_source = mgpu::kUiSourceGame; } return &ui; }
        if (out_source) { *out_source = mgpu::kUiSourceOff; }
        return nullptr;
    }
    if (mode == 2) {
        if (hud_usable()) { if (out_source) { *out_source = mgpu::kUiSourceHudless; } return &hud; }
        if (out_source) { *out_source = mgpu::kUiSourceOff; }
        return nullptr;
    }
    // Auto.
    if (usable(ui)) { if (out_source) { *out_source = mgpu::kUiSourceGame; } return &ui; }
    if (hud_usable()) { if (out_source) { *out_source = mgpu::kUiSourceHudless; } return &hud; }
    if (out_source) { *out_source = mgpu::kUiSourceOff; }
    return nullptr;
}

// v22: which record resource is the colour we interpolate. The DLSS-G Backbuffer is the
// final image (what the window would show, UI included) and is therefore first choice;
// OutputReal is the FG backend's real output frame; HUDLess is the clean scene without UI
// and is only used when neither of the other two exists.
const MgpuFgResource& colour_source(const MgpuFgFrame& f)
{
    const MgpuFgResource& bb = f.stream[MGPU_FG_STREAM_BACKBUFFER];
    if (bb.resource != nullptr && bb.format != 0) { return bb; }
    const MgpuFgResource& out = f.stream[MGPU_FG_STREAM_OUTPUT_REAL];
    if (out.resource != nullptr && out.format != 0) { return out; }
    return f.stream[MGPU_FG_STREAM_HUDLESS];
}

// A ready transport that never gets a present tick cannot publish anything: the only
// publisher entry point is MgpuProducerPresent(), which fakerFG calls from the Present
// detour it installs after xefg_dyn::Load() succeeded (XeFG has to take the swap chain).
// When that fails - libxess_fg.dll without xefgSwapChainD3D12GetProperties, the cyberpunk
// 1.2.0 field case - the mapping exists but every publication is skipped, and the UI keeps
// saying "not the producer". Report the real reason once, from the record path (the only
// callback that still ticks in that state).
void present_watchdog()
{
    if (!g_ready || g_ready_ms == 0 || g_present_calls != 0) { return; }
    static bool told = false;
    if (told) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_ready_ms < 5000) { return; }
    told = true;
    logf("producer: transport ready for %llu ms but no present tick arrived - fakerFG's"
         " Present detour is NOT installed.\n"
         "producer:   check <exe>_fakerFG.log for \"xefg: libxess_fg not loadable\" /"
         " \"XeFG could not take the swap chain\"; the title's own libxess_fg.dll may be too"
         " old - deploy the packaged one to <game>\\fakerFG\\libxess_fg.dll\n",
         static_cast<unsigned long long>(now - g_ready_ms));
}

void on_record(const MgpuFgFrame* f, void* /*user*/)
{
    present_watchdog();
    ++g_record_in;
    if (g_t0 == 0) { g_t0 = GetTickCount64(); }
    static uint64_t s_last_hb = 0;
    if (g_record_in - s_last_hb >= 300) {
        const uint64_t di = g_record_in - s_last_hb;
        s_last_hb = g_record_in;
        logf("producer: t=%llums in=%llu (+%llu) present=%llu publish=%llu skip=%llu"
             " seqdedup=%llu replaced=%llu dropped=%llu copywait=%.1fms max=%.1fms mask=0x%X wantc=%d\n",
             static_cast<unsigned long long>(GetTickCount64() - g_t0),
             static_cast<unsigned long long>(g_record_in),
             static_cast<unsigned long long>(di),
             static_cast<unsigned long long>(g_present_calls),
             static_cast<unsigned long long>(g_publish_calls),
             static_cast<unsigned long long>(g_publish_skips),
             static_cast<unsigned long long>(g_publish_seq_dedup),
             static_cast<unsigned long long>(g_record_replaced),
             static_cast<unsigned long long>(g_record_dropped),
             g_copy_wait_ema, g_copy_wait_max, g_copied_mask,
             g_want_colour_session ? 1 : 0);
    }
    if (g_fg == nullptr || f == nullptr || f->mode == MGPU_FG_MODE_OFF) { return; }

    // A consumer restart writes a fresh session token. Clear the previous session's
    // ACK/frame state before this record is allowed to publish; otherwise a stale
    // colour_seq can skip every publication in the new session.
    {
        static uint64_t s_seen_session = 0;
        mgpu::SharedControl* c = ctl();
        if (c != nullptr && c->consumer_session != 0 &&
            c->consumer_session != s_seen_session) {
            s_seen_session = c->consumer_session;
            reset_consumer_session(c, s_seen_session);
        }
    }

    const MgpuFgResource& mv = f->stream[MGPU_FG_STREAM_MV];
    const MgpuFgResource& dp = f->stream[MGPU_FG_STREAM_DEPTH];
    const MgpuFgResource& col = colour_source(*f);
    const MgpuFgResource& ui = f->stream[MGPU_FG_STREAM_UI_COLOR];
    const MgpuFgResource& hud = f->stream[MGPU_FG_STREAM_HUDLESS];
    if (mv.resource == nullptr && dp.resource == nullptr && col.resource == nullptr) {
        return;
    }

    // No consumer -> no transport. Creating the mapping/heap/placed textures during the
    // game's black loading screen was the only part of startup we control, and it is pure
    // cost without a consumer. The consumer creates this event before it waits for the
    // mailbox, so the normal run is unaffected.
    if (!g_ready && !consumer_present()) {
        static bool told_no_consumer = false;
        if (!told_no_consumer) {
            told_no_consumer = true;
            logf("producer: no consumer present - transport idle, no mapping/heap created\n");
        }
        return;
    }

    // Startup publish gate: the first record arrives while the game is still loading its
    // menu. Creating the mapping/heap/placed textures there sits on the game's own startup
    // path (measured 78 ms at 4 slots, seconds at 8). For `PublishDelayMs` after this first
    // record we do nothing at all; the consumer simply waits for the mailbox. 0 disables.
    static ULONGLONG s_first_record_ms = 0;
    static uint32_t  s_gate_ms = 0xFFFFFFFFu;
    static bool      s_gate_logged = false;
    if (s_first_record_ms == 0) { s_first_record_ms = GetTickCount64(); }
    if (!g_ready) {
        if (s_gate_ms == 0xFFFFFFFFu) { s_gate_ms = ini_int(L"PublishDelayMs", 1200); }
        if (s_gate_ms > 0 &&
            (GetTickCount64() - s_first_record_ms) < s_gate_ms) {
            if (!s_gate_logged) {
                s_gate_logged = true;
                logf("producer: publish gate %u ms from the first record - transport is not"
                     " created during game startup (fakerFG.ini PublishDelayMs)\n", s_gate_ms);
            }
            return;
        }
        if (!s_gate_logged) {
            s_gate_logged = true;
            logf("producer: publish gate released after %llu ms\n",
                 static_cast<unsigned long long>(GetTickCount64() - s_first_record_ms));
        }
    }

    if (!g_ready) {
        if (f->device == nullptr) {
            static bool told_dev = false;
            if (!told_dev) { told_dev = true; logf("producer: record has no device - no transport\n"); }
            return;
        }
        // Stream 1 is the MV slot. When the title provides no MV we still lay it out with
        // the MV format/size (fixed layout), so a later frame that does carry vectors can
        // be copied without a protocol change. The depth format is separate (always R32).
        uint32_t fmt = mv.resource != nullptr
                           ? mv.format
                           : static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
        {
            static bool s_colour_ini = false;
            if (!s_colour_ini) {
                s_colour_ini = true;
                g_want_colour_session = ini_int(L"ColourStream", 0) != 0;
                // UI stream is an unverified experiment: default OFF. Set UiStream=1 by hand
                // for a local A/B. UiSource selects the stream-3 payload:
                //   Auto (default) = title UI tag, else downsampled HUDless
                //   Game           = title UI tag only
                //   Hudless        = downsampled HUDless only (consumer synthesises UI)
                //   Off            = never publish stream 3
                g_want_ui_session = ini_int(L"UiStream", 0) != 0;
                g_ui_premultiplied_cfg = ini_int(L"UiPremultiplied", 1) != 0;
                {
                    wchar_t src[32]{};
                    ini_string(L"UiSource", L"Auto", src, 32);
                    if (_wcsicmp(src, L"game") == 0)         { g_ui_source_mode = 1; }
                    else if (_wcsicmp(src, L"hudless") == 0) { g_ui_source_mode = 2; }
                    else if (_wcsicmp(src, L"off") == 0)     { g_ui_source_mode = 3; }
                    else                                     { g_ui_source_mode = 0; }
                }
                // The gate is closed by default. Two explicit local opt-ins open it for
                // this session: `UiSynthesis=1`, or `UiStream=1` together with an explicit
                // `UiSource=Game|Hudless` (Auto stays closed). Never a release default.
                g_ui_synthesis_override = ini_int(L"UiSynthesis", 0) != 0;
                if (!g_ui_synthesis_override && g_want_ui_session &&
                    (g_ui_source_mode == 1 || g_ui_source_mode == 2)) {
                    g_ui_synthesis_override = true;
                    logf("producer: UiStream=1 + UiSource=%s opens the synthesis gate for"
                         " this session (local A/B only; see docs/UI_SYNTHESIS_GATE.md)\n",
                         g_ui_source_mode == 1 ? "Game" : "Hudless");
                } else if (g_ui_synthesis_override) {
                    logf("producer: UiSynthesis=1 - UI synthesis gate overridden for this"
                         " session (local A/B only; see docs/UI_SYNTHESIS_GATE.md)\n");
                }
                if (g_ui_source_mode == 3) {
                    g_want_ui_session = false;
                    logf("producer: UiSource=Off - stream 3 is not published\n");
                }
                g_force_inline = ini_int(L"CopyInline", 0) != 0;
                if (!g_want_colour_session) {
                    logf("producer: ColourStream=0 - colour is not published"
                         " (MV/depth only; for cost measurement)\n");
                }
                if (g_force_inline) {
                    logf("producer: CopyInline=1 - copies use the title's command list\n");
                }
            }
        }
        uint32_t cw = col.width, ch = col.height, cf = col.format;
        if ((cw == 0 || ch == 0) && col.resource != nullptr) {
            const D3D12_RESOURCE_DESC cd =
                static_cast<ID3D12Resource*>(col.resource)->GetDesc();
            cw = static_cast<uint32_t>(cd.Width);
            ch = cd.Height;
            if (cf == 0) { cf = static_cast<uint32_t>(cd.Format); }
        }
        if (cw < 256 || ch < 256) { cw = f->output_w; ch = f->output_h; }
        // v29: choose what stream 3 carries before the layout is placed. If the source
        // is not available yet (the title creates its FG tag after the first record),
        // reserve stream 3 at output size / R10 and let the per-frame selection pick the
        // real source later. Disabling here would leave UI off for the whole session.
        const MgpuFgResource* ui_aux = g_want_ui_session
                                           ? select_ui_source(*f, &g_ui_source)
                                           : nullptr;
        if (!g_want_ui_session) { g_ui_source = mgpu::kUiSourceOff; }
        if (g_ui_source != mgpu::kUiSourceOff) { g_ui_source_type = g_ui_source; }
        g_ui_premultiplied = (g_ui_source == mgpu::kUiSourceGame) && g_ui_premultiplied_cfg;
        if (g_want_ui_session && ui_aux == nullptr) {
            static bool told_reserved = false;
            if (!told_reserved) {
                told_reserved = true;
                logf("producer: UiStream=1 but no UI/HUDless resource yet - reserving stream 3"
                     " at output size / R10; source is re-checked every record\n");
            }
        }
        {
            static uint32_t logged_source = 0xFFFFFFFFu;
            if (g_ui_source != logged_source) {
                logged_source = g_ui_source;
                if (g_ui_source == mgpu::kUiSourceGame) {
                    logf("producer: UI source = game UI tag\n");
                } else if (g_ui_source == mgpu::kUiSourceHudless) {
                    logf("producer: UI source = HUDless (consumer synthesises UI;"
                         " HUDless must not equal BACKBUFFER)\n");
                } else {
                    logf("producer: UI source = Off\n");
                }
            }
        }
        uint32_t uw = 0, uh = 0, uf = 0;
        if (ui_aux != nullptr) {
            uw = ui_aux->width;
            uh = ui_aux->height;
            uf = ui_aux->format;
            if ((uw == 0 || uh == 0) && ui_aux->resource != nullptr) {
                const D3D12_RESOURCE_DESC ud =
                    static_cast<ID3D12Resource*>(ui_aux->resource)->GetDesc();
                uw = static_cast<uint32_t>(ud.Width);
                uh = ud.Height;
                if (uf == 0) { uf = static_cast<uint32_t>(ud.Format); }
            }
            if (uw < 64 || uh < 64 || uf == 0) { uw = uh = uf = 0; }
        } else if (g_want_ui_session) {
            // Reserved placeholder: the real source is selected after the FG tag appears.
            uw = f->output_w ? f->output_w : f->render_w;
            uh = f->output_h ? f->output_h : f->render_h;
            uf = static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);
            if (uw < 64 || uh < 64) { uw = uh = 0; uf = 0; }
        }
        // A depth-only record (no motion vectors) still has to size the transport.
        // f.render_w/h used to come only from the MV stream; fakerFG now falls back to
        // depth there, and this second fallback keeps an older fakerFG working too.
        uint32_t eff_rw = f->render_w, eff_rh = f->render_h;
        if (eff_rw < 8 || eff_rh < 8) {
            eff_rw = dp.width; eff_rh = dp.height;
            if (eff_rw < 8 || eff_rh < 8) { eff_rw = f->output_w; eff_rh = f->output_h; }
        }
        ID3D12Device* dev = static_cast<ID3D12Device*>(f->device);
        if (InterlockedCompareExchange(&g_init_started, 1, 0) == 0) {
            start_shared_init(dev, eff_rw, eff_rh, fmt, cw, ch, cf, uw, uh, uf);
        }
        if (InterlockedCompareExchange(&g_init_failed, 0, 0) != 0) {
            static bool told_fail = false;
            if (!told_fail) {
                told_fail = true;
                logf("producer: shared init failed - transport stays off\n");
            }
        }
        // Never block the title on shared-heap creation. The worker sets g_ready; the
        // next record then proceeds through update_content/publish.
        return;
    }

    // Same fallback as the init path: a depth-only record has no MV stream to take the
    // render size from. Without this, update_content(0,0) returns immediately and the
    // stream rectangles never leave 0 - the producer publishes nothing.
    {
        uint32_t rec_rw = f->render_w, rec_rh = f->render_h;
        if (rec_rw < 8 || rec_rh < 8) { rec_rw = dp.width; rec_rh = dp.height; }
        if (rec_rw < 8 || rec_rh < 8) { rec_rw = f->output_w; rec_rh = f->output_h; }
        if (rec_rw >= 8 && rec_rh >= 8) { update_content(rec_rw, rec_rh); }
    }

    // Who wants what? The bridge writes request_mask every frame it processes; with no
    // bridge running it stays 0, and copying 63 MB/frame into cross-adapter memory for
    // nobody is the single largest avoidable cost on the render GPU. Falls back to
    // consumer_request (v7) for a consumer that only sets the older field.
    uint32_t want = 0;
    if (mgpu::SharedControl* c = ctl()) {
        want = c->request_mask | c->consumer_request;
    }
    // 锁帧版：颜色不走 producer，所以引导流必须是 MV+depth。consumer 在第一条
    // 发布出现之前无法写 request_mask，空掩码以前表示"只发颜色"；颜色关闭后那样
    // 会让两边互相等待。
    if (want == 0) { want = mgpu::kRequestMv | mgpu::kRequestDepth; }
    if (want & mgpu::kRequestRawMv)    { want |= mgpu::kRequestMv; }
    if (want & mgpu::kRequestRawDepth) { want |= mgpu::kRequestDepth; }
    const bool want_mv    = (want & mgpu::kRequestMv) != 0;
    const bool want_depth = (want & mgpu::kRequestDepth) != 0;
    const bool want_colour = g_want_colour_session &&
                             (want & mgpu::kRequestColor) != 0 &&
                             col.resource != nullptr;
    // v29: stream 3 follows the source chosen at init, but the source can appear after
    // the first record (FG tag creation is often late). Re-check every record; a source
    // that disappears simply idles the stream. The slot layout is never re-placed.
    const MgpuFgResource* ui_aux = nullptr;
    if (g_want_ui_session) {
        uint32_t new_source = mgpu::kUiSourceOff;
        const MgpuFgResource* sel = select_ui_source(*f, &new_source);
        // Once stream 3 has a type, do not switch UI->HUDless mid-session: the slot
        // format was chosen for the first type. A temporary disappearance goes Off and
        // recovers when the same type returns.
        if (sel != nullptr && g_ui_source != mgpu::kUiSourceOff &&
            new_source != g_ui_source) {
            sel = nullptr;
            new_source = mgpu::kUiSourceOff;
        }
        if (sel != nullptr) {
            if (new_source != g_ui_source) {
                logf("producer: UI source %u -> %u%s\n", g_ui_source, new_source,
                     new_source == mgpu::kUiSourceHudless
                         ? " (consumer synthesises UI)"
                         : (new_source == mgpu::kUiSourceGame ? " (game UI tag)" : ""));
            }
            g_ui_source = new_source;
            g_ui_source_type = new_source;
            ui_aux = sel;
        } else if (g_ui_source != mgpu::kUiSourceOff) {
            logf("producer: UI source %u unavailable this record - stream 3 idles\n",
                 g_ui_source);
            g_ui_source = mgpu::kUiSourceOff;
        }
    }
    g_ui_premultiplied = (g_ui_source == mgpu::kUiSourceGame) && g_ui_premultiplied_cfg;
    bool want_ui = g_want_ui_session &&
                   (want & mgpu::kRequestUi) != 0 &&
                   ui_aux != nullptr && g_ui_format != 0 &&
                   g_stream_w[3] >= 64 && g_stream_h[3] >= 64;
    if ((want & mgpu::kRequestUi) && ui_aux == nullptr && g_want_ui_session) {
        static bool told_no_ui = false;
        if (!told_no_ui) {
            told_no_ui = true;
            logf("producer: consumer requested UI but the selected source"
                 " (ui_source=%u) has no resource this record - stream 3 idles\n",
                 g_ui_source);
        }
    }
    if (!want_mv && !want_depth && !want_colour && !want_ui) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("producer: no consumer requested a stream - transport idle"
                 " (nothing copied)\n");
        }
        return;
    }

    // v22: the colour's live rectangle is the output size, which can change when the
    // upscaler preset changes (output stays 4K, render does not). Rewrite it only when
    // it actually changed; the slot capacity never moves.
    if (want_colour) {
        uint32_t cw = col.width, ch = col.height;
        if ((cw == 0 || ch == 0) && col.resource != nullptr) {
            const D3D12_RESOURCE_DESC cd =
                static_cast<ID3D12Resource*>(col.resource)->GetDesc();
            cw = static_cast<uint32_t>(cd.Width);
            ch = cd.Height;
        }
        if (cw < 256 || ch < 256) { cw = f->output_w; ch = f->output_h; }
        if (cw > g_colour_slot_w) { cw = g_colour_slot_w; }
        if (ch > g_colour_slot_h) { ch = g_colour_slot_h; }
        if (cw >= 256 && ch >= 256 && (cw != g_colour_w || ch != g_colour_h)) {
            g_colour_w = cw;
            g_colour_h = ch;
            if (mgpu::SharedControl* c = ctl()) {
                c->streams[mgpu::kStreamColour].seq += 1;      // odd: being written
                MemoryBarrier();
                c->streams[mgpu::kStreamColour].width  = cw;
                c->streams[mgpu::kStreamColour].height = ch;
                MemoryBarrier();
                c->streams[mgpu::kStreamColour].seq += 1;      // even: stable
            }
            logf("colour: live rectangle %ux%u (slot %ux%u, fmt %u)\n", cw, ch,
                 g_colour_slot_w, g_colour_slot_h, g_colour_format);
        }
    }

    // Colour watermark: never run more than N publications ahead of the last colour the
    // consumer verified. Without this the producer's seq advances at the game rate while
    // the colour path advances at the WGC rate; the difference is unbounded and no finite
    // ring can hold it. Dropping the extra MV frame is correct: that game frame's colour
    // will be dropped too, so it can never be mispaired.
    if (mgpu::SharedControl* c = ctl()) {
        const uint64_t ack = c->colour_seq;
        // PBZ measured colour lag up to 13 publications. A limit of kMaxSlots/2 (8) dropped
        // MV frames whose colour was still on its way, which the consumer could only count
        // as no_slot (74% hit). Keep two slots as safety: the window now covers the real
        // lag, and only a genuine overflow drops an MV frame.
        const uint64_t limit = mgpu::kMaxSlots - 2;
        // An ACK beyond the ring is not a usable watermark: the slot it refers to has
        // already been recycled, and a consumer restart can leave colour_seq far above
        // the new producer's frame ids (measured: ack=226 while seq was 8k+). Skipping
        // forever in that state starves the consumer, so treat it as stale and publish.
        const bool ack_in_window = (ack != 0 && f->seq >= ack &&
                                    (f->seq - ack) <= mgpu::kMaxSlots);
        const bool ack_stale = (ack != 0 && f->seq >= ack &&
                                (f->seq - ack) > mgpu::kMaxSlots);
        if (ack_in_window && f->seq > ack + limit) {
            ++g_publish_skips;
            static uint32_t rate_skips = 0;
            if (++rate_skips <= 3 || (rate_skips % 300u) == 0u) {
                logf("producer: colour watermark skip seq=%llu ack=%llu (ahead=%llu, limit %llu)"
                     " - MV dropped so the ring tracks the colour\n",
                     (unsigned long long)f->seq, (unsigned long long)ack,
                     (unsigned long long)(f->seq - ack), (unsigned long long)limit);
            }
            return;
        }
        if (ack_stale) {
            static uint32_t rate_stale = 0;
            if (++rate_stale <= 3 || (rate_stale % 300u) == 0u) {
                logf("producer: stale colour ACK seq=%llu ack=%llu (ahead=%llu > ring %u)"
                     " - ACK ignored, publishing so the consumer can re-anchor\n",
                     (unsigned long long)f->seq, (unsigned long long)ack,
                     (unsigned long long)(f->seq - ack), mgpu::kMaxSlots);
            }
        }
    }

    if (!g_slot_locked) {
        if (!claim_slot()) {
            static unsigned told_noslot = 0;
            if (f->seq != 0 && told_noslot++ < 300) {
                logf("record: no slot for seq=%llu (copies pending mask=0x%X)\n",
                     static_cast<unsigned long long>(f->seq), g_copied_mask);
            }
            return;
        }
        g_slot_locked = true;
        copy_begin_frame();     // one async list per frame (no-op when disabled)
    }
    if (g_copied_mask == 0) {
        g_frame = *f;   // the first record owns this frame
    } else if (f->seq != 0 && f->seq != g_frame.seq &&
               f->stream[mgpu::kStreamMv].resource == g_frame.stream[mgpu::kStreamMv].resource &&
               f->stream[mgpu::kStreamDepth].resource == g_frame.stream[mgpu::kStreamDepth].resource) {
        // A newer record for the SAME inputs arrived before the pending one was published.
        // Keep the copies already recorded (same resources) but adopt the new identity, so
        // the seq the marker is carrying is the one that reaches frames[].
        ++g_record_replaced;
        if (g_record_replaced <= 20) {
            logf("record: seq %llu replaces pending seq %llu (same MV/depth, copies kept)\n",
                 static_cast<unsigned long long>(f->seq),
                 static_cast<unsigned long long>(g_frame.seq));
        }
        g_frame = *f;
    } else if (f->seq != 0 && f->seq != g_frame.seq) {
        ++g_record_dropped;
        if (g_record_dropped <= 20) {
            logf("record: seq %llu arrived while seq %llu pending with DIFFERENT inputs -"
                 " dropped\n",
                 static_cast<unsigned long long>(f->seq),
                 static_cast<unsigned long long>(g_frame.seq));
        }
    }

    auto* list = static_cast<ID3D12GraphicsCommandList*>(f->cmd_list);
    // A COPY-only list (some UE5 tag lanes) cannot accept the downscale Dispatch. Fall
    // back to 1:1 for the rest of the session; update_content() rewrites the stream
    // rectangles and bumps `generation`, so the consumer rebuilds its local copies.
    if ((g_scaled[1] || g_scaled[2]) && list != nullptr && g_copied_mask == 0 &&
        f->render_w <= g_slot_w && f->render_h <= g_slot_h &&
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        static bool told = false;
        if (!told) {
            told = true;
            g_force_full_size = true;
            uint32_t crw = f->render_w, crh = f->render_h;
            if (crw < 8 || crh < 8) { crw = dp.width; crh = dp.height; }
            if (crw < 8 || crh < 8) { crw = f->output_w; crh = f->output_h; }
            if (crw >= 8 && crh >= 8) { update_content(crw, crh); }
            logf("downscale: list type %d cannot take a Dispatch - falling back to"
                 " StreamPercent=100 for this title\n", static_cast<int>(list->GetType()));
        }
    }
    // Copies go to a second queue; the scale pass (if any) MUST stay on the game's list
    // because the source texture is only valid there and Dispatch is illegal on COPY.
    bool use_dma = false;
    // v22: colour needs a barrier (the game's back buffer is not in COPY_SOURCE), and a
    // COPY list cannot express one. One list carries all three streams so a publication
    // is one fence.
    // Wide depth also needs the conversion Dispatch, which a COPY list cannot execute;
    // keep those frames on the DIRECT copy queue even when StreamPercent=100.
    const bool depth_convert = want_depth && dp.resource != nullptr &&
                               wide_depth_format(static_cast<DXGI_FORMAT>(dp.format));
    if (!g_force_inline && g_dma_enabled && !want_colour && !depth_convert) {
        const bool mv_ok = !want_mv || g_scaled[1] ||
            static_cast<D3D12_RESOURCE_STATES>(mv.state) == D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool dp_ok = !want_depth || g_scaled[2] ||
            static_cast<D3D12_RESOURCE_STATES>(dp.state) == D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool ui_ok = !want_ui || g_scaled[3] ||
            static_cast<D3D12_RESOURCE_STATES>(ui_aux->state) == D3D12_RESOURCE_STATE_COPY_SOURCE;
        use_dma = mv_ok && dp_ok && ui_ok;
    }
    auto* copy_list = g_force_inline
                          ? list
                          : (use_dma ? g_dma_list.Get()
                                     : (g_copy_enabled ? g_copy_list.Get() : list));
    g_copy_use_dma = use_dma;
    g_copy_active_q = g_force_inline
                          ? nullptr
                          : (use_dma ? g_dma_q.Get()
                                     : (g_copy_enabled ? g_copy_q.Get() : nullptr));
    g_copy_active_list = g_force_inline
                             ? nullptr
                             : (use_dma ? g_dma_list.Get()
                                        : (g_copy_enabled ? g_copy_list.Get() : nullptr));

    // StreamPercent > 100: scale MV+depth to the stream size with one compute pass,
    // then copy what the pass wrote. Both streams must end up at the SAME size (XeFG
    // rejects mismatched inputs), so a failed dispatch skips the whole frame instead of
    // sending one scaled and one full-size texture.
    // Wide depth additionally needs the pass at any percent, including 100: the source
    // is R32G8X24 (8 B/px) and the slot is R32 (4 B/px).
    bool scaled_mv = false, scaled_depth = false, scaled_ui = false;
    // Depth codec: the kernel reads an R32_FLOAT view, so a source it cannot read directly
    // (wide depth, D32_FLOAT, ...) is converted once by the existing scale pass.
    const bool depth_codec_on = (depth_codec() != 0 && dp.resource != nullptr);
    const bool depth_fmt_direct =
        dp.resource != nullptr &&
        (dp.format == DXGI_FORMAT_R32_TYPELESS || dp.format == DXGI_FORMAT_R32_FLOAT);
    const bool need_depth_convert = want_depth && dp.resource != nullptr &&
                                    (wide_depth_format(static_cast<DXGI_FORMAT>(dp.format)) ||
                                     (depth_codec_on && !depth_fmt_direct));
    // Only pass a stream through the downscale when its source actually exists. A
    // depth-only record (no MV tagged) must NOT fail just because StreamPercent>100
    // also covers the missing MV stream: that made scale_dispatch(null) return false,
    // skipped the whole frame and therefore published nothing at all - the consumer's
    // 100% no_slot in the no-MV case.
    const bool need_scaled_mv = want_mv && mv.resource != nullptr && g_scaled[1];
    const bool need_scaled_depth = want_depth && dp.resource != nullptr &&
                                   (g_scaled[2] || need_depth_convert);
    bool need_scaled_ui = want_ui && ui_aux != nullptr && g_scaled[3];
    if (need_scaled_mv || need_scaled_depth || need_scaled_ui) {
        const bool can_dispatch =
            list != nullptr && (list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                                list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        if (!can_dispatch) {
            static bool told = false;
            if (!told) {
                told = true;
                logf("downscale: list type %d cannot take a Dispatch - set StreamPercent=100\n",
                     list != nullptr ? static_cast<int>(list->GetType()) : -1);
            }
            // UI is optional: drop it for this frame instead of losing MV/depth too.
            if (need_scaled_ui) {
                static bool told_ui = false;
                if (!told_ui) {
                    told_ui = true;
                    logf("downscale: UI/HUDless needs a Dispatch but the list is COPY-only"
                         " - stream 3 idles (MV/depth continue at their own sizes)\n");
                }
                want_ui = false;
                need_scaled_ui = false;
            }
            if (need_scaled_mv || need_scaled_depth) { return; }
        }
        bool ok = true;
        if (ok && need_scaled_mv) {
            ok = scale_dispatch(list, static_cast<ID3D12Resource*>(mv.resource), mv.state, 1);
        }
        if (ok && need_scaled_depth) {
            ok = scale_dispatch(list, static_cast<ID3D12Resource*>(dp.resource), dp.state, 2);
        }
        if (ok && need_scaled_ui) {
            ok = scale_dispatch(list, static_cast<ID3D12Resource*>(ui_aux->resource),
                                ui_aux->state, 3);
        }
        if (!ok) {
            static bool told = false;
            if (!told) { told = true; logf("downscale: pass failed - frames skipped\n"); }
            return;
        }
        scaled_mv = need_scaled_mv;
        scaled_depth = need_scaled_depth;
        scaled_ui = need_scaled_ui;
        if (need_depth_convert) {
            static bool told_wide = false;
            if (!told_wide) {
                told_wide = true;
                logf("downscale: wide depth (fmt %u) -> R32 conversion pass enabled at StreamPercent=%u\n",
                     static_cast<unsigned>(dp.format), g_percent[2]);
            }
        }
    }

    if (want_mv && (g_copied_mask & mgpu::kRequestMv) == 0) {
        if (mv_codec() != 0 && !scaled_mv && mv.resource != nullptr) {
            // v30: lossless - encode straight from the title's MV into the slot buffer.
            // Recorded on the game's list because the MV texture is only valid there; the
            // copy queue waits on the frame fence, so the forward fence covers this write.
            if (fr_encode_dispatch(list, static_cast<ID3D12Resource*>(mv.resource), mv.state,
                                   1, g_cur_slot)) {
                g_copied_mask |= mgpu::kRequestMv;
            } else {
                static bool told = false;
                if (!told) {
                    told = true;
                    logf("fr: encode failed - MV dropped this frame (MvCodec=0 reverts)\n");
                }
            }
        } else {
            ID3D12Resource* src = scaled_mv ? g_ds_tex[1].Get()
                                            : static_cast<ID3D12Resource*>(mv.resource);
            const uint32_t state = scaled_mv
                ? static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE) : mv.state;
            if (copy_stream(copy_list, src, 1, state)) { g_copied_mask |= mgpu::kRequestMv; }
        }
    }
    if (want_depth && (g_copied_mask & mgpu::kRequestDepth) == 0) {
        ID3D12Resource* src = scaled_depth ? g_ds_tex[2].Get()
                                           : static_cast<ID3D12Resource*>(dp.resource);
        const uint32_t state = scaled_depth ? static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE)
                                            : dp.state;
        if (depth_codec_on) {
            // v30: lossless depth - encode on the render card, decode on the consumer.
            if (fr_encode_dispatch(list, src, state, 2, g_cur_slot)) {
                g_copied_mask |= mgpu::kRequestDepth;
            } else {
                static bool told_dep = false;
                if (!told_dep) {
                    told_dep = true;
                    logf("fr: depth encode failed - depth dropped this frame"
                         " (DepthCodec=0 reverts)\n");
                }
            }
        } else if (copy_stream(copy_list, src, 2, state)) {
            g_copied_mask |= mgpu::kRequestDepth;
        }
    }
    if (want_ui && (g_copied_mask & mgpu::kRequestUi) == 0) {
        ID3D12Resource* src = scaled_ui ? g_ds_tex[3].Get()
                                        : static_cast<ID3D12Resource*>(ui_aux->resource);
        const uint32_t state = scaled_ui ? static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE)
                                         : ui_aux->state;
        if (copy_stream(copy_list, src, 3, state)) { g_copied_mask |= mgpu::kRequestUi; }
    }
    // v22: colour, verbatim, into the same publication slot. Its declared state is used
    // for the barrier exactly like MV/depth.
    if (want_colour && (g_copied_mask & mgpu::kRequestColor) == 0) {
        if (copy_stream(copy_list, static_cast<ID3D12Resource*>(col.resource), 0, col.state)) {
            g_copied_mask |= mgpu::kRequestColor;
        }
    }
    if ((g_copy_enabled || g_dma_enabled) && g_copied_mask != 0) { g_copy_open = true; }
    if (g_copied_mask != 0) { g_frame_valid = true; }
}

// What the record's MV.Scale means, the same reading as in_ngx/in_faker:
// 1.0 = pixels, ~width = UV, ~width/2 = NDC; an explicit NDC flag wins.
uint32_t mv_unit_bits(const MgpuFgFrame& f)
{
    if (f.flags & MGPU_FG_FRAME_MV_NDC) { return 3; }
    const float nxs = f.mv_scale_x < 0.0f ? -f.mv_scale_x : f.mv_scale_x;
    const float nys = f.mv_scale_y < 0.0f ? -f.mv_scale_y : f.mv_scale_y;
    const float nw  = static_cast<float>(f.render_w);
    if (nxs > 0.0f && nxs <= 1.5f && nys <= 1.5f) { return 1; }        // pixels
    if (nw > 0.0f && nxs >= 0.7f * nw && nxs <= 4.0f * nw) { return 2; } // uv
    if (nw > 0.0f && nxs >= 0.35f * nw && nxs < 0.7f * nw) { return 3; } // ndc
    return 0;
}

// ---------------------------------------------------------------- publish
// Called from fakerFG's Present detour, i.e. after the title submitted the list
// the copies were recorded on. Metadata first, fence last (protocol v5).
struct PublishSpinGuard
{
    volatile LONG* busy = nullptr;
    explicit PublishSpinGuard(volatile LONG* b) : busy(b) {}
    ~PublishSpinGuard() { if (busy != nullptr) { InterlockedExchange(busy, 0); } }
};

void publish_frame(void* queue_ptr)
{
    fr_drain_msgs("publish");
    // Wuchang presents ~2x per rendered frame; if two presents ever land on different
    // threads, g_frame/g_copied_mask/g_cur_slot would be corrupted. Serialize with a
    // plain Win32 interlocked flag: std::mutex pulled MSVCP140 into the first Present and
    // crashed (0xC0000005 -> UE PresentInternal E_ABORT).
    static volatile LONG s_publish_busy = 0;
    static unsigned s_concurrent = 0;
    if (InterlockedCompareExchange(&s_publish_busy, 1, 0) != 0) {
        ++s_concurrent;
        if (s_concurrent <= 5) {
            logf("producer: concurrent publish_frame detected - serialized (%u so far)\n",
                 s_concurrent);
        }
        while (InterlockedCompareExchange(&s_publish_busy, 1, 0) != 0) {
            SwitchToThread();
        }
    }
    PublishSpinGuard publish_guard(&s_publish_busy);
    ++g_publish_calls;
    // One publication per source frame. fakerFG's Present path can call us more than
    // once for the same record (XeFG chain + title present). Dedup by SEQ, not by
    // frame_qpc_ms: the marker/record identity is seq, and two distinct seqs can share
    // the same millisecond qpc (or one lane can stamp a new qpc). Deduping by qpc left
    // those seqs unpublished while the marker had already advanced -> no_slot.
    if (g_frame.seq != 0 && g_frame.seq == g_last_published_seq) {
        ++g_publish_seq_dedup;
        return;
    }
    const uint64_t pub_fq = g_frame.frame_qpc_ms;

    // Frame interval, counted only when the source frame changes: fakerFG's Present detour
    // can publish the same record twice (XeFG chain + title present), so a per-call
    // interval reads ~13 ms instead of the 25 ms frame. `frame_qpc_ms` is set at Evaluate
    // and is identical for both calls of the same frame.
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        if (g_qpc_freq == 0) {
            LARGE_INTEGER fq{};
            QueryPerformanceFrequency(&fq);
            g_qpc_freq = static_cast<uint64_t>(fq.QuadPart);
        }
        const bool same_frame = (pub_fq != 0 && pub_fq == g_last_publish_frame_qpc);
        if (!same_frame && g_last_publish_qpc != 0 &&
            qpc.QuadPart > static_cast<LONGLONG>(g_last_publish_qpc)) {
            const double dt = static_cast<double>(qpc.QuadPart - g_last_publish_qpc) * 1000.0 /
                              static_cast<double>(g_qpc_freq);
            if (dt > 0.0 && dt < 1000.0) {
                g_publish_ms_ema = (g_publish_ms_ema > 0.0) ? g_publish_ms_ema * 0.9 + dt * 0.1 : dt;
            }
            g_last_publish_qpc = static_cast<uint64_t>(qpc.QuadPart);
            g_last_publish_frame_qpc = pub_fq;
        } else if (!same_frame || g_last_publish_qpc == 0) {
            g_last_publish_qpc = static_cast<uint64_t>(qpc.QuadPart);
            g_last_publish_frame_qpc = pub_fq;
        }
    }

    if (!g_ready || !g_frame_valid || g_copied_mask == 0) {
        ++g_publish_skips;
        static unsigned told_skip = 0;
        if (g_frame.seq != 0 && told_skip++ < 300) {
            logf("publish skip seq=%llu ready=%d valid=%d mask=0x%X\n",
                 static_cast<unsigned long long>(g_frame.seq),
                 g_ready ? 1 : 0, g_frame_valid ? 1 : 0, g_copied_mask);
        }
        g_copied_mask = 0;
        g_frame_valid = false;
        g_slot_locked = false;
        return;
    }
    auto* q = static_cast<ID3D12CommandQueue*>(queue_ptr);
    mgpu::SharedControl* c = ctl();
    const uint32_t slot = g_cur_slot;
    const uint64_t value = ++g_fwd_value[slot];
    const uint64_t pub_seq = ++g_pub_seq;
    const MgpuFgFrame& f = g_frame;
    const uint32_t unit = mv_unit_bits(f);

    if (c != nullptr) {
        mgpu::FrameShared& fr = c->frames[slot];
        fr.ready = 0;                       // invalid while the rest is rewritten
        MemoryBarrier();

        fr.frame_id     = f.seq;
        // marker_* fields are reserved/unused: the marker lives in the colour image
        // itself (fakerFG draws it, the consumer decodes it).
        fr.marker_valid  = 0u;
        fr.marker_format = 0u;
        fr.marker_x      = 0u;
        fr.marker_y      = 0u;
        memset(fr.marker_tile, 0, sizeof(fr.marker_tile));
        fr.jitter_x     = f.jitter_x;
        fr.jitter_y     = f.jitter_y;
        fr.render_w     = f.render_w;
        fr.render_h     = f.render_h;
        // No normalisation pass in this producer: the stream holds exactly what the
        // game wrote, so the declared MV.Scale is published unchanged.
        fr.mv_scale_x   = f.mv_scale_x != 0.0f ? f.mv_scale_x : 1.0f;
        fr.mv_scale_y   = f.mv_scale_y != 0.0f ? f.mv_scale_y : 1.0f;
        fr.near_plane   = f.camera_near;
        fr.far_plane    = f.camera_far;
        fr.vfov_rad     = f.camera_fov_y;
        fr.frame_time_ms = static_cast<float>(g_publish_ms_ema);
        fr.valid_w      = f.render_w;
        fr.valid_h      = f.render_h;

        uint32_t sem = (unit << mgpu::kSemMvUnitShift) |
                       (1u << mgpu::kSemMvSignShift);          // prev-minus-cur
        const uint32_t depth_code = (f.flags & MGPU_FG_FRAME_DEPTH_INVERTED) ? 1u : 2u;
        sem |= depth_code << mgpu::kSemDepthShift;
        fr.semantics = sem;

        if (f.flags & MGPU_FG_FRAME_HAS_MATRIX) {
            memcpy(fr.view_to_clip, f.view_to_clip, sizeof(fr.view_to_clip));
            fr.matrix_tick_ms = f.frame_qpc_ms;
        } else {
            fr.matrix_tick_ms = 0;
        }
        memcpy(fr.clip_to_prev_clip, f.clip_to_prev_clip, sizeof(fr.clip_to_prev_clip));
        memcpy(fr.prev_clip_to_clip, f.prev_clip_to_clip, sizeof(fr.prev_clip_to_clip));
        fr.frame_key    = f.present_id;
        fr.frame_qpc_ms = f.frame_qpc_ms;
        fr.present_id   = static_cast<uint32_t>(f.present_id);
        fr.fg_flags     = f.flags |
                          ((g_copied_mask & mgpu::kRequestColor) ? mgpu::kInputFlagColourActive : 0u);
        fr.mv_invalid_value = 0.0f;
        memset(fr.camera_pos, 0, sizeof(fr.camera_pos));
        memset(fr.camera_up, 0, sizeof(fr.camera_up));
        memset(fr.camera_right, 0, sizeof(fr.camera_right));
        memset(fr.camera_fwd, 0, sizeof(fr.camera_fwd));
        memset(fr.camera_pinhole, 0, sizeof(fr.camera_pinhole));
        fr.published = value;               // v10: what the consumer ACKs with
        fr.pub_seq   = pub_seq;             // v23: global order for the consumer loop

        uint32_t active = mgpu::kRequestMeta;
        if (g_copied_mask & mgpu::kRequestColor) { active |= mgpu::kRequestColor; }
        if (g_copied_mask & mgpu::kRequestMv)    { active |= mgpu::kRequestMv; }
        if (g_copied_mask & mgpu::kRequestDepth) { active |= mgpu::kRequestDepth; }
        if (g_copied_mask & mgpu::kRequestUi)    { active |= mgpu::kRequestUi; }
        c->active_mask    = active;
        c->stream_bytes[mgpu::kStreamColour] = (active & mgpu::kRequestColor)
                                 ? g_colour_w * g_colour_h *
                                   ((g_colour_format == DXGI_FORMAT_R10G10B10A2_UNORM) ? 4u : 8u)
                                 : 0u;
        c->stream_bytes[mgpu::kStreamMv] = (active & mgpu::kRequestMv)
                                 ? g_stream_w[1] * g_stream_h[1] * 4u : 0u;
        c->stream_bytes[mgpu::kStreamDepth] = (active & mgpu::kRequestDepth)
                                 ? g_stream_w[2] * g_stream_h[2] * 4u : 0u;
        c->stream_bytes[mgpu::kStreamUi] = (active & mgpu::kRequestUi)
                                 ? g_stream_w[3] * g_stream_h[3] *
                                   ((g_ui_format == DXGI_FORMAT_R10G10B10A2_UNORM) ? 4u : 4u)
                                 : 0u;
        c->frame_published = f.seq;
        g_last_published_seq = f.seq;
        static int trace_pub = -1;
        if (trace_pub < 0) { trace_pub = ini_int(L"PublishTrace", 0) != 0 ? 1 : 0; }
        if (trace_pub != 0) {
            logf("publish trace seq=%llu slot=%u frame_published=%llu\n",
                 static_cast<unsigned long long>(f.seq), slot,
                 static_cast<unsigned long long>(f.seq));
        }

        c->producer.input_api   = mgpu::kInputApiFaker;
        c->producer.input_flags = (f.flags & ~mgpu::kInputFlagMvActive) |
                                  ((g_copied_mask & mgpu::kRequestMv) ? mgpu::kInputFlagMvActive : 0u) |
                                  (unit == 3 ? mgpu::kInputFlagMvNdc : 0u);
        c->producer.render_w      = g_render_w ? g_render_w : g_stream_w[1];
        c->producer.render_h      = g_render_h ? g_render_h : g_stream_h[1];
        c->producer.output_w      = f.output_w ? f.output_w
                                               : (g_render_w ? g_render_w : g_stream_w[1]);
        c->producer.output_h      = f.output_h ? f.output_h
                                               : (g_render_h ? g_render_h : g_stream_h[1]);
        c->producer.declared_frames = f.declared_frames;
        c->producer.harvest_flags   = 0;
        c->ui_source = (g_ui_source != mgpu::kUiSourceOff && (g_copied_mask & mgpu::kRequestUi))
                           ? g_ui_source
                           : mgpu::kUiSourceOff;
        MemoryBarrier();
    }

    // Async copy: the game's queue signals that the frame was submitted; the copy queue
    // waits for that, copies into the shared slots, and only then signals fwd. The game's
    // present is never behind the transfer (the whole point of g_copy_q).
    bool signalled = false;
    if ((g_copy_enabled || g_dma_enabled) && g_copy_open &&
        g_copy_active_q && g_copy_active_list && q != nullptr) {
        g_copy_active_list->Close();
        if (g_copy_use_dma) { g_dma_list_open = false; } else { g_copy_list_open = false; }
        const uint64_t fv = ++g_frame_fence_value;
        bool ok = SUCCEEDED(q->Signal(g_frame_fence.Get(), fv));
        if (ok) {
            g_copy_active_q->Wait(g_frame_fence.Get(), fv);
            ID3D12CommandList* lists[] = { g_copy_active_list };
            g_copy_active_q->ExecuteCommandLists(1, lists);
            ok = SUCCEEDED(g_copy_active_q->Signal(g_copy_done.Get(), ++g_copy_done_value)) &&
                 SUCCEEDED(g_copy_active_q->Signal(g_fwd[slot].Get(), value));
        }
        signalled = ok;
        g_copy_open = false;
        g_copy_active_q = nullptr;
        g_copy_active_list = nullptr;
        if (!ok) {
            logf("publish: async copy submit FAILED (slot=%u, dma=%d) - falling back to"
                 " the game queue\n", slot, static_cast<int>(g_copy_use_dma));
        }
    }
    if (!signalled && q != nullptr) {
        signalled = SUCCEEDED(q->Signal(g_fwd[slot].Get(), value));
    }
    if (!signalled) {
        logf("publish: Signal FAILED (queue=%p slot=%u value=%llu)\n", queue_ptr, slot,
             static_cast<unsigned long long>(value));
    } else {
        g_next_slot = (slot + 1) % mgpu::kMaxSlots;
    }

    static uint32_t logged = 0;
    ++logged;
    if (logged <= 3 || (logged % 600) == 0) {
        logf("publish #%u t=%llums slot=%u id=%llu mask=0x%X render=%ux%u colour=%ux%u fmt=%u"
             " mv=%ux%u depth=%ux%u frame=%.1fms unit=%u | in=%llu present=%llu skip=%llu"
             " copywait=%.1f/%.1fms\n",
             logged,
             static_cast<unsigned long long>(g_t0 ? (GetTickCount64() - g_t0) : 0),
             slot, static_cast<unsigned long long>(f.seq), g_copied_mask,
             g_render_w, g_render_h, g_colour_w, g_colour_h, g_colour_format,
             g_stream_w[1], g_stream_h[1],
             g_stream_w[2], g_stream_h[2], g_publish_ms_ema, unit,
             static_cast<unsigned long long>(g_record_in),
             static_cast<unsigned long long>(g_present_calls),
             static_cast<unsigned long long>(g_publish_skips),
             g_copy_wait_ema, g_copy_wait_max);
    }

    g_copied_mask = 0;
    g_frame_valid = false;
    g_slot_locked = false;
}

} // namespace

// ---------------------------------------------------------------- exports
extern "C" __declspec(dllexport) int __cdecl MgpuProducerStart(const void* fg_api)
{
    const MgpuFgApi* api = static_cast<const MgpuFgApi*>(fg_api);
    if (api == nullptr || api->subscribe == nullptr || api->version < 2u) {
        logf("producer: no usable MgpuFgApi (need a v2 table with subscribe)\n");
        return 1;
    }
    fr_debug_enable();
    g_fg = api;
    if (api->subscribe(&on_record, nullptr) != 0) {
        logf("producer: subscribe FAILED\n");
        g_fg = nullptr;
        return 2;
    }
    logf("producer: subscribed to the fakerFG record (api v%u, record %u bytes)\n",
         api->version, api->size);
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl MgpuProducerPresent(void* queue)
{
    ++g_present_calls;
    publish_frame(queue);
}

// Legacy CPU path (v2). Kept as an export so an older fakerFG still links; the
// producer no longer stores CPU tiles - v3's record_scar() is the transport.
extern "C" __declspec(dllexport) void __cdecl MgpuProducerMarkerTile(
    uint64_t, const void*, uint32_t, uint32_t, uint32_t, uint32_t)
{
}

// MgpuProducerRecordScar() is retired: the marker no longer transports a tile. The
// export stays so an older fakerFG that still looks it up gets a clean "not supported"
// instead of a missing export (the api table publishes it as nullptr).
extern "C" __declspec(dllexport) int __cdecl MgpuProducerRecordScar(
    void*, void*, uint32_t, uint32_t, uint32_t, uint32_t)
{
    return 5;   // not supported: the marker carries only a 32-bit code
}

extern "C" __declspec(dllexport) void __cdecl MgpuProducerShutdown(void)
{
    if (g_fg != nullptr && g_fg->unsubscribe != nullptr) { g_fg->unsubscribe(); }
    g_fg = nullptr;
    // The mapping/placed resources are process-scoped; the process is going away.
    g_ready = false;
    logf("producer: shutdown\n");
}

extern "C" __declspec(dllexport) MgpuProducerApi* __cdecl MgpuProducerGetApi(
    uint32_t min_version, uint32_t max_version)
{
    static MgpuProducerApi api{};
    if (min_version > MGPU_PRODUCER_VERSION || max_version < 1u) { return nullptr; }
    api.size    = sizeof(MgpuProducerApi);
    api.version = MGPU_PRODUCER_VERSION;
    api.start   = &MgpuProducerStart;
    api.present = &MgpuProducerPresent;
    api.marker_tile = nullptr;          // v2 CPU path retired
    api.record_scar = nullptr;           // retired with the scar tile transport
    api.shutdown = &MgpuProducerShutdown;
    return &api;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        log_open();
        logf("DllMain: attached (pid=%lu)\n", GetCurrentProcessId());
    }
    return TRUE;
}
