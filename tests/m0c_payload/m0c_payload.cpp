// ============================================================================
// M0.3 - parameterised payload measurement (real Cyberpunk 2077 shapes)
//
// M0.2 fixed the transport mechanism (file-mapping cross-adapter heap) and gave
// a raw 9.19 GB/s for one 5K RGBA8 texture. M0.3 answers the only question left
// before writing the transport layer:
//
//   how long does the REAL payload of a 5K/40fps Cyberpunk frame take to cross?
//
// Real shape: colour at OUTPUT resolution (5K, HDR = R10G10B10A2), motion
// vectors and depth at RENDER resolution (DLSS quality -> 3413x1920), because
// XeSS-FG requires depth to match the motion vector buffer size.
//
// Two measurements per run:
//   A) sequential - producer write ms + consumer read ms (upper bound of latency)
//   B) pipelined  - slot ring with shared fences, producer writes slot i while
//                   the consumer reads slot i-1; reports achievable frames/sec.
//      This is the number that decides whether 80 fps is reachable.
//
// Usage:
//   m0c_payload.exe --mode texture --pipeline 2 --iters 60
//                   --stream color 5120 2880 r10g10b10a2
//                   --stream mv    3413 1920 r16g16_float
//                   --stream depth 3413 1920 r32_float
//
// Build: ./scripts/build_m0.sh m0c_payload
// ============================================================================

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

static FILE* g_log = nullptr;

static void logf_(const char* fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    vprintf(fmt, a);
    va_end(a);
    if (g_log) { vfprintf(g_log, fmt, b); fflush(g_log); }
    va_end(b);
    fflush(stdout);
}

static const UINT64 kAlign = 64 * 1024;

struct Gpu
{
    const char* role = "";
    unsigned    vendor = 0;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    HANDLE                            event = nullptr;
    UINT64                            value = 0;
    bool                              ok = false;
};

static bool gpu_init(Gpu& g, IDXGIAdapter1* adapter, const char* role)
{
    g.role = role;
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    g.vendor = d.VendorId;

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device)))) {
        logf_("  [%s] D3D12CreateDevice FAILED\n", role);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
        FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                IID_PPV_ARGS(&g.alloc))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g.alloc.Get(),
                                           nullptr, IID_PPV_ARGS(&g.list))) ||
        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        logf_("  [%s] queue/allocator/list/fence FAILED\n", role);
        return false;
    }
    g.list->Close();
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.ok = true;
    logf_("  [%s] %ls (vendor 0x%04X)\n", role, d.Description, d.VendorId);
    return true;
}

static void gpu_wait_idle(Gpu& g)
{
    if (!g.ok) { return; }
    ++g.value;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.value))) { return; }
    g.fence->SetEventOnCompletion(g.value, g.event);
    WaitForSingleObject(g.event, 20000);
}

// ------------------------------------------------------------------ streams

struct Stream
{
    const char* name = "";
    UINT        w = 0;
    UINT        h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT64      alloc_size = 0;      // driver-required size of one slot
    UINT64      offset = 0;          // offset of slot 0 inside the mailbox

    ComPtr<ID3D12Resource> localP;   // producer VRAM (the real game texture)
    ComPtr<ID3D12Resource> localC;   // consumer VRAM (what XeSS-FG will tag)

    std::vector<ComPtr<ID3D12Resource>> slotsP;   // shared heap views, producer side
    std::vector<ComPtr<ID3D12Resource>> slotsC;   // shared heap views, consumer side
};

static UINT bytes_per_pixel(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return 4;
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return 4;
    case DXGI_FORMAT_R16G16_FLOAT:          return 4;
    case DXGI_FORMAT_R32_FLOAT:             return 4;
    case DXGI_FORMAT_R16_UNORM:             return 2;
    case DXGI_FORMAT_R16_FLOAT:             return 2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 8;
    default:                                return 4;
    }
}

static DXGI_FORMAT parse_format(const char* s)
{
    if (strcmp(s, "r8g8b8a8") == 0)            { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    if (strcmp(s, "r10g10b10a2") == 0)         { return DXGI_FORMAT_R10G10B10A2_UNORM; }
    if (strcmp(s, "r16g16_float") == 0)        { return DXGI_FORMAT_R16G16_FLOAT; }
    if (strcmp(s, "r32_float") == 0)           { return DXGI_FORMAT_R32_FLOAT; }
    if (strcmp(s, "r16_unorm") == 0)           { return DXGI_FORMAT_R16_UNORM; }
    if (strcmp(s, "r16_float") == 0)           { return DXGI_FORMAT_R16_FLOAT; }
    if (strcmp(s, "r16g16b16a16_float") == 0)  { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
    return DXGI_FORMAT_UNKNOWN;
}

static const char* format_name(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT:       return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:          return "R32_FLOAT";
    case DXGI_FORMAT_R16_UNORM:          return "R16_UNORM";
    case DXGI_FORMAT_R16_FLOAT:          return "R16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default:                             return "?";
    }
}

static D3D12_RESOURCE_DESC stream_desc(const Stream& s)
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = s.w;
    rd.Height           = s.h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = s.fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    return rd;
}

static HRESULT make_local_texture(Gpu& g, const Stream& s, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd = stream_desc(s);
    rd.Flags  = D3D12_RESOURCE_FLAG_NONE;      // local VRAM copy, not cross-adapter
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    return g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&out));
}

static HRESULT make_placed(Gpu& g, ID3D12Heap* heap, const Stream& s,
                           UINT64 offset, ComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC rd = stream_desc(s);
    rd.Alignment = 0;                          // 0 == use the driver default
    return g.device->CreatePlacedResource(heap, offset, &rd,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&out));
}

// -------------------------------------------------------------- shared fence

struct SharedFence
{
    ComPtr<ID3D12Fence> onP;     // created on the producer, D3D12_FENCE_FLAG_SHARED
    ComPtr<ID3D12Fence> onC;     // opened on the consumer
};

static bool make_shared_fence(Gpu& producer, Gpu& consumer, SharedFence& sf)
{
    // A plain SHARED fence is only shareable within one adapter; crossing
    // vendors needs SHARED_CROSS_ADAPTER (E_INVALIDARG without it).
    const D3D12_FENCE_FLAGS flags =
        (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER);

    if (FAILED(producer.device->CreateFence(0, flags, IID_PPV_ARGS(&sf.onP)))) {
        logf_("  CreateFence(SHARED_CROSS_ADAPTER) FAILED\n");
        return false;
    }
    HANDLE h = nullptr;
    if (FAILED(producer.device->CreateSharedHandle(sf.onP.Get(), nullptr, GENERIC_ALL,
                                                   nullptr, &h))) {
        logf_("  CreateSharedHandle(fence) FAILED\n");
        return false;
    }
    const HRESULT hr = consumer.device->OpenSharedHandle(h, IID_PPV_ARGS(&sf.onC));
    CloseHandle(h);
    if (FAILED(hr)) {
        logf_("  OpenSharedHandle(fence) FAILED hr=0x%08lX\n", (unsigned long)hr);
        return false;
    }
    return true;
}

// ------------------------------------------------------------- mailbox setup

struct Mailbox
{
    HANDLE  h_map = nullptr;
    UINT64  size = 0;
    std::vector<Stream>      streams;
    std::vector<SharedFence> forward;   // producer -> consumer, one per slot
    std::vector<SharedFence> back;      // consumer -> producer, one per slot
};

// Allocates one file mapping covering `slots` copies of every stream and wraps
// it as a D3D12 heap on both devices.
static bool mailbox_create(Gpu& producer, Gpu& consumer, Mailbox& mb,
                           UINT slots, bool verbose)
{
    ComPtr<ID3D12Device3> devP3, devC3;
    if (FAILED(producer.device.As(&devP3)) || FAILED(consumer.device.As(&devC3))) {
        logf_("  ID3D12Device3 unavailable\n");
        return false;
    }

    UINT64 cursor = 0;
    for (auto& s : mb.streams) {
        D3D12_RESOURCE_DESC rd = stream_desc(s);
        const D3D12_RESOURCE_ALLOCATION_INFO ai =
            producer.device->GetResourceAllocationInfo(0, 1, &rd);
        s.alloc_size = (ai.SizeInBytes + kAlign - 1) / kAlign * kAlign;
        s.offset = cursor;
        cursor += s.alloc_size * slots;
        if (verbose) {
            logf_("  %-6s %ux%u %-20s alloc %6.2f MB x%u\n",
                  s.name, s.w, s.h, format_name(s.fmt),
                  ai.SizeInBytes / (1024.0 * 1024.0), slots);
        }
    }
    mb.size = cursor;

    mb.h_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  (DWORD)(mb.size >> 32),
                                  (DWORD)(mb.size & 0xFFFFFFFFu), nullptr);
    if (!mb.h_map) {
        logf_("  CreateFileMapping FAILED err=%lu\n", GetLastError());
        return false;
    }
    if (verbose) { logf_("  file mapping: %.2f MB\n", mb.size / (1024.0 * 1024.0)); }

    ComPtr<ID3D12Heap> heapP, heapC;
    if (FAILED(devP3->OpenExistingHeapFromFileMapping(mb.h_map, IID_PPV_ARGS(&heapP))) ||
        FAILED(devC3->OpenExistingHeapFromFileMapping(mb.h_map, IID_PPV_ARGS(&heapC)))) {
        logf_("  OpenExistingHeapFromFileMapping FAILED\n");
        return false;
    }

    for (auto& s : mb.streams) {
        if (FAILED(make_local_texture(producer, s, s.localP)) ||
            FAILED(make_local_texture(consumer, s, s.localC))) {
            logf_("  local texture FAILED (%s)\n", s.name);
            return false;
        }
        s.slotsP.resize(slots);
        s.slotsC.resize(slots);
        for (UINT i = 0; i < slots; ++i) {
            const UINT64 off = s.offset + s.alloc_size * i;
            if (FAILED(make_placed(producer, heapP.Get(), s, off, s.slotsP[i])) ||
                FAILED(make_placed(consumer, heapC.Get(), s, off, s.slotsC[i]))) {
                logf_("  placed slot FAILED (%s slot %u)\n", s.name, i);
                return false;
            }
        }
    }

    mb.forward.resize(slots);
    mb.back.resize(slots);
    for (UINT i = 0; i < slots; ++i) {
        if (!make_shared_fence(producer, consumer, mb.forward[i]) ||
            !make_shared_fence(producer, consumer, mb.back[i])) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------- submits

struct SubmitResult
{
    bool   ok = false;
    UINT64 signal_value = 0;
};

// Records one CopyResource and executes it. When `wait` is false the call
// returns immediately (pipelining) and the caller syncs on the shared fence.
static SubmitResult submit_copy(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src,
                                bool wait, double* elapsed_ms)
{
    SubmitResult r;
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) {
        return r;
    }
    g.list->CopyResource(dst, src);
    if (FAILED(g.list->Close())) { return r; }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    ID3D12CommandList* lists[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, lists);

    ++g.value;
    g.queue->Signal(g.fence.Get(), g.value);
    r.ok = true;
    r.signal_value = g.value;

    if (wait) {
        g.fence->SetEventOnCompletion(g.value, g.event);
        WaitForSingleObject(g.event, 20000);
        QueryPerformanceCounter(&t1);
        if (elapsed_ms) {
            *elapsed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        }
    }
    return r;
}

// --------------------------------------------------------------- measurement

static double now_ms()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// A) every stream sequentially, both directions, blocking. This is the upper
//    bound of a single-frame handoff latency.
static void measure_sequential(Gpu& producer, Gpu& consumer, Mailbox& mb, UINT iters)
{
    logf_("\n== A) sequential (blocking, per-frame latency upper bound) ==\n");
    double total = 0.0;

    for (auto& s : mb.streams) {
        double pw = 0.0, cr = 0.0;
        for (UINT i = 0; i < iters; ++i) {
            double ms = 0.0;
            submit_copy(producer, s.slotsP[0].Get(), s.localP.Get(), true, &ms);
            pw += ms;
            submit_copy(consumer, s.localC.Get(), s.slotsC[0].Get(), true, &ms);
            cr += ms;
        }
        pw /= (double)iters;
        cr /= (double)iters;
        logf_("  %-6s write %6.2f ms   read %6.2f ms   sum %6.2f ms\n",
              s.name, pw, cr, pw + cr);
        total += pw + cr;
    }
    logf_("  total per frame: %.2f ms  -> against a 12.5 ms budget: %s\n",
          total, total < 12.5 ? "fits ONLY if pipelined" : "OVER BUDGET even pipelined");
}

// B) ring of `slots` with cross-device fences: the producer writes the next slot
//    while the consumer reads the previous one. This is the number that decides
//    whether 40 fps render / 80 fps present is reachable.
static void measure_pipelined(Gpu& producer, Gpu& consumer, Mailbox& mb,
                              UINT iters, UINT slots, double target_render_fps)
{
    logf_("\n== B) pipelined (%u slots, shared fences) ==\n", slots);

    UINT64 fwd = 0, back_signal = 0;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    for (UINT i = 0; i < iters; ++i) {
        const UINT slot = i % slots;

        if (i >= slots) {
            const UINT64 need = (i - slots) / slots + 1;
            producer.queue->Wait(mb.back[slot].onP.Get(), need);
        }

        for (auto& s : mb.streams) {
            submit_copy(producer, s.slotsP[slot].Get(), s.localP.Get(), false, nullptr);
        }
        const UINT64 fwd_now = ++fwd;
        producer.queue->Signal(mb.forward[slot].onP.Get(), fwd_now);

        consumer.queue->Wait(mb.forward[slot].onC.Get(), fwd_now);
        for (auto& s : mb.streams) {
            submit_copy(consumer, s.localC.Get(), s.slotsC[slot].Get(), false, nullptr);
        }
        consumer.queue->Signal(mb.back[slot].onC.Get(), ++back_signal);
    }

    gpu_wait_idle(producer);
    gpu_wait_idle(consumer);
    QueryPerformanceCounter(&t1);

    const double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    const double per_frame = sec * 1000.0 / (double)iters;
    const double fps = 1000.0 / per_frame;

    double payload_mb = 0.0;
    for (const auto& s : mb.streams) {
        payload_mb += (double)s.w * s.h * bytes_per_pixel(s.fmt) / (1024.0 * 1024.0);
    }

    logf_("  per frame : %.2f ms (both directions)\n", per_frame);
    logf_("  payload   : %.1f MB/frame -> %.2f GB/s effective\n",
          payload_mb, payload_mb * 2.0 / per_frame);
    logf_("  ceiling   : %.1f render fps / %.1f present fps at 2x\n", fps, fps * 2.0);
    logf_("  verdict   : %s (target render %.0f fps)\n",
          fps >= target_render_fps ? "REACHABLE" : "TOO SLOW - shrink the payload",
          target_render_fps);
}

// C) paced: reproduces the real operating point. The flat-out loop above
//    saturates the bus, which overstates contention with the game. Here each
//    frame gets `1000/fps` ms and we measure how long the handoff really takes.
static void measure_paced(Gpu& producer, Gpu& consumer, Mailbox& mb,
                          UINT iters, UINT slots, double fps)
{
    const double interval = 1000.0 / fps;
    logf_("\n== C) paced at %.0f fps (%.2f ms per frame) ==\n", fps, interval);

    UINT64 fwd = 0, back_signal = 0;
    double sum = 0.0, worst = 0.0;
    const double t_start = now_ms();

    for (UINT i = 0; i < iters; ++i) {
        const double frame_begin = now_ms();
        const UINT slot = i % slots;

        if (i >= slots) {
            const UINT64 need = (i - slots) / slots + 1;
            producer.queue->Wait(mb.back[slot].onP.Get(), need);
        }
        for (auto& s : mb.streams) {
            submit_copy(producer, s.slotsP[slot].Get(), s.localP.Get(), false, nullptr);
        }
        const UINT64 fwd_now = ++fwd;
        producer.queue->Signal(mb.forward[slot].onP.Get(), fwd_now);

        consumer.queue->Wait(mb.forward[slot].onC.Get(), fwd_now);
        for (auto& s : mb.streams) {
            submit_copy(consumer, s.localC.Get(), s.slotsC[slot].Get(), false, nullptr);
        }
        consumer.queue->Signal(mb.back[slot].onC.Get(), ++back_signal);

        // block until the consumer has really finished reading this frame
        gpu_wait_idle(consumer);

        const double used = now_ms() - frame_begin;
        sum += used;
        if (used > worst) { worst = used; }

        const double next = t_start + (double)(i + 1) * interval;
        const double t = now_ms();
        if (t < next) { Sleep((DWORD)(next - t)); }

        if ((i + 1) % 20 == 0) {
            logf_("   frame %3u/%u   handoff %.2f ms\n", i + 1, iters, used);
        }
    }

    const double avg = sum / (double)iters;
    logf_("  handoff avg %.2f ms   worst %.2f ms   budget %.2f ms\n", avg, worst, interval);
    logf_("  budget used: %.1f%%   verdict: %s\n",
          avg / interval * 100.0,
          avg < interval * 0.8 ? "HEALTHY" : (avg < interval ? "TIGHT" : "OVER BUDGET"));
}


// ----------------------------------------------------------------------- main

static void usage()
{
    logf_("usage: m0c_payload [--stream <name> <w> <h> <fmt>]... [options]\n");
    logf_("  fmt: r8g8b8a8 | r10g10b10a2 | r16g16_float | r32_float | r16g16b16a16_float\n");
    logf_("  --pipeline <n>  ring depth (default 2)\n");
    logf_("  --iters <n>     iterations (default 60)\n");
    logf_("  --target <fps>  render fps the run must sustain (default 40)\n");
    logf_("  --paced <fps>   also run a frame-paced test at that fps (realistic)\n");
    logf_("\n  default streams = Cyberpunk 2077 at 5K with DLSS Quality:\n");
    logf_("    color 5120x2880 r10g10b10a2  (HDR backbuffer)\n");
    logf_("    mv    3413x1920 r16g16_float (DLSS render resolution)\n");
    logf_("    depth 3413x1920 r32_float    (must match mv size)\n");
}

int main(int argc, char** argv)
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* dot = strrchr(exePath, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - exePath), ".log");
    }
    fopen_s(&g_log, exePath, "w");

    UINT   slots = 2;
    UINT   iters = 60;
    double target = 40.0;
    double paced = 0.0;
    Mailbox mb;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stream") == 0 && i + 4 < argc) {
            Stream s;
            s.name = argv[i + 1];
            s.w = (UINT)atoi(argv[i + 2]);
            s.h = (UINT)atoi(argv[i + 3]);
            s.fmt = parse_format(argv[i + 4]);
            if (s.fmt == DXGI_FORMAT_UNKNOWN) {
                logf_("unknown format: %s\n", argv[i + 4]);
                return 2;
            }
            mb.streams.push_back(s);
            i += 4;
        } else if (strcmp(argv[i], "--pipeline") == 0 && i + 1 < argc) {
            slots = (UINT)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = (UINT)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = atof(argv[++i]);
        } else if (strcmp(argv[i], "--paced") == 0 && i + 1 < argc) {
            paced = atof(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            if (g_log) { fclose(g_log); }
            return 0;
        } else {
            logf_("unknown argument: %s\n", argv[i]);
            usage();
            if (g_log) { fclose(g_log); }
            return 2;
        }
    }

    if (mb.streams.empty()) {
        mb.streams.push_back({ "color", 5120, 2880, DXGI_FORMAT_R10G10B10A2_UNORM });
        mb.streams.push_back({ "mv",    3413, 1920, DXGI_FORMAT_R16G16_FLOAT });
        mb.streams.push_back({ "depth", 3413, 1920, DXGI_FORMAT_R32_FLOAT });
    }

    logf_("============================================================================\n");
    logf_("  M0.3  payload measurement   (NVIDIA producer -> AMD consumer)\n");
    logf_("  ring depth %u, %u iterations, target render %.0f fps\n", slots, iters, target);
    logf_("============================================================================\n");

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        logf_("  CreateDXGIFactory2 FAILED\n");
        if (g_log) { fclose(g_log); }
        return 3;
    }

    ComPtr<IDXGIAdapter1> nvAdapter, amdAdapter;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { continue; }
        if (d.VendorId == 0x10DE && !nvAdapter)  { nvAdapter = a; }
        if (d.VendorId == 0x1002 && !amdAdapter) { amdAdapter = a; }
    }
    if (!nvAdapter || !amdAdapter) {
        logf_("  need one NVIDIA and one AMD adapter\n");
        if (g_log) { fclose(g_log); }
        return 4;
    }

    Gpu producer, consumer;
    if (!gpu_init(producer, nvAdapter.Get(), "producer/nvidia") ||
        !gpu_init(consumer, amdAdapter.Get(), "consumer/amd")) {
        if (g_log) { fclose(g_log); }
        return 5;
    }

    logf_("\n== mailbox ==\n");
    if (!mailbox_create(producer, consumer, mb, slots, true)) {
        logf_("  mailbox_create FAILED\n");
        if (g_log) { fclose(g_log); }
        return 6;
    }
    logf_("  ready\n");

    double payload_mb = 0.0;
    for (const auto& s : mb.streams) {
        payload_mb += (double)s.w * s.h * bytes_per_pixel(s.fmt) / (1024.0 * 1024.0);
    }
    logf_("  payload per frame: %.1f MB (%.1f MB with both directions)\n",
          payload_mb, payload_mb * 2.0);

    measure_sequential(producer, consumer, mb, iters);
    measure_pipelined(producer, consumer, mb, iters, slots, target);
    if (paced > 0.0) {
        measure_paced(producer, consumer, mb, iters, slots, paced);
    }

    logf_("\n============================================================================\n");
    logf_(" What to do with this number\n");
    logf_("   * pipelined render fps comfortably above %.0f -> proceed to M1.\n", target);
    logf_("   * barely above -> try --pipeline 3, and lower the aux resolution\n");
    logf_("     (DLSS Performance renders MV/depth at 2560x1440 instead of 3413x1920).\n");
    logf_("   * below -> colour must not cross at 5K: capture at a lower internal\n");
    logf_("     resolution and let the consumer scale, or fall back to 5K 2x @30 fps.\n");
    logf_("============================================================================\n");

    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}



