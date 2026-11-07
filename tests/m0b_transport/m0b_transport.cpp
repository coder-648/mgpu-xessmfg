// ============================================================================
// M0.2 - cross-adapter transport probe (NVIDIA producer -> AMD consumer)
//
// M0.1 showed CrossAdapterRowMajorTextureSupported == FALSE on both adapters,
// so the official "cross-adapter heap" route is out. This program measures
// every remaining route instead of guessing which one WDDM accepts:
//
//   V1 shared texture, DEFAULT heap (VRAM),           no sim-access flag
//   V2 shared texture, DEFAULT heap (VRAM),           ALLOW_SIMULTANEOUS_ACCESS
//   V3 shared texture, CUSTOM heap / pool L0 (system), no sim-access flag
//   V4 shared texture, CUSTOM heap / pool L0 (system), ALLOW_SIMULTANEOUS_ACCESS
//   V5 CPU staged: producer readback heap -> memcpy -> consumer upload heap
//
// Every variant reports: create ok, open-on-other-adapter ok, copy accepted,
// and a measured MB/s for a 5120x2880 RGBA8 payload (59.0 MB per frame).
//
// A debug layer (if the "Graphics Tools" feature is installed) is enabled so
// rejected copies are reported instead of silently ignored.
//
// Build: same pattern as m0_probe (see scripts/build_m0.sh, MODULE=m0b).
// ============================================================================

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
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

// ----------------------------------------------------------------- payload

static const UINT  kWidth  = 5120;
static const UINT  kHeight = 2880;
static const DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static const UINT  kIterations = 20;

static double frame_mb()
{
    return (double)kWidth * kHeight * 4.0 / (1024.0 * 1024.0);
}

// ------------------------------------------------------------------- gpu box

struct Gpu
{
    const char*          role = "";
    unsigned             vendor = 0;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device>  device;
    ComPtr<ID3D12CommandQueue>      queue;
    ComPtr<ID3D12CommandAllocator>  alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>  fence;
    HANDLE               event = nullptr;
    UINT64               value = 0;
    bool                 ok = false;
};

static bool gpu_init(Gpu& g, IDXGIAdapter1* adapter, const char* role)
{
    g.role = role;
    g.adapter = adapter;

    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    g.vendor = d.VendorId;

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device)))) {
        logf_("  [%s] D3D12CreateDevice FAILED\n", role);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) {
        logf_("  [%s] CreateCommandQueue(COPY) FAILED\n", role);
        return false;
    }
    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                IID_PPV_ARGS(&g.alloc)))) {
        logf_("  [%s] CreateCommandAllocator FAILED\n", role);
        return false;
    }
    if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g.alloc.Get(),
                                           nullptr, IID_PPV_ARGS(&g.list)))) {
        logf_("  [%s] CreateCommandList FAILED\n", role);
        return false;
    }
    g.list->Close();

    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        logf_("  [%s] CreateFence FAILED\n", role);
        return false;
    }
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    g.ok = true;
    logf_("  [%s] device ready: %ls (vendor 0x%04X)\n", role, d.Description, d.VendorId);
    return true;
}

static void gpu_wait_idle(Gpu& g)
{
    if (!g.ok || !g.queue) { return; }
    ++g.value;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.value))) { return; }
    g.fence->SetEventOnCompletion(g.value, g.event);
    WaitForSingleObject(g.event, 20000);
}

static void gpu_release(Gpu& g)
{
    if (g.event) { CloseHandle(g.event); g.event = nullptr; }
}

// --------------------------------------------------------------- resources

// Creates a 5120x2880 RGBA8 texture either in producer VRAM (system_mem=false)
// or in system memory via a custom heap with memory pool L0 (system_mem=true).
// `shared` adds D3D12_HEAP_FLAG_SHARED so a NT handle can be produced.
static HRESULT make_texture(Gpu& g, bool system_mem, bool sim_access,
                            bool shared, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    if (system_mem) {
        hp.Type                 = D3D12_HEAP_TYPE_CUSTOM;
        hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;   // memory nearest the CPU
    } else {
        hp.Type                 = D3D12_HEAP_TYPE_DEFAULT;
        hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = kWidth;
    rd.Height           = kHeight;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = kFormat;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = sim_access ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                                     : D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_FLAGS hf = shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE;

    return g.device->CreateCommittedResource(&hp, hf, &rd,
                                             D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&out));
}

// D3D12 resources are shared through ID3D12Device::CreateSharedHandle.
// (IDXGIResource1::CreateSharedHandle is the D3D11/DXGI path and returns
//  E_NOINTERFACE on a D3D12 committed resource.)
static HRESULT make_shared_handle(ID3D12Device* device, ID3D12Resource* res, HANDLE* out)
{
    return device->CreateSharedHandle(res, nullptr, GENERIC_ALL, nullptr, out);
}

// Submits one CopyResource on a copy queue and blocks until it retires.
// Returns the wall time in milliseconds, or a negative value on failure.
static double copy_once(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src)
{
    if (FAILED(g.alloc->Reset())) { return -1.0; }
    if (FAILED(g.list->Reset(g.alloc.Get(), nullptr))) { return -1.0; }
    g.list->CopyResource(dst, src);
    if (FAILED(g.list->Close())) { return -1.0; }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    ID3D12CommandList* lists[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, lists);
    gpu_wait_idle(g);

    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

struct Timing
{
    double total_ms = 0.0;
    double avg_ms   = 0.0;
    double mb_per_s = 0.0;
};

static Timing measure_copies(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src)
{
    Timing t;
    const double warm = copy_once(g, dst, src);      // warm-up, discarded
    if (warm < 0.0) { t.avg_ms = -1.0; return t; }

    for (UINT i = 0; i < kIterations; ++i) {
        const double ms = copy_once(g, dst, src);
        if (ms < 0.0) { t.avg_ms = -1.0; return t; }
        t.total_ms += ms;
    }
    t.avg_ms   = t.total_ms / (double)kIterations;
    t.mb_per_s = frame_mb() / (t.avg_ms / 1000.0);
    return t;
}

// -------------------------------------------------- content verification

// Creates a texture in the given heap type (UPLOAD for CPU-writable source,
// READBACK for CPU-readable sink). Same desc as the payload texture.
static HRESULT make_staging_texture(Gpu& g, D3D12_HEAP_TYPE type,
                                    ComPtr<ID3D12Resource>& out, UINT* row_pitch)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = kWidth;
    rd.Height           = kHeight;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = kFormat;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { return hr; }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (row_pitch) { *row_pitch = fp.Footprint.RowPitch; }
    return S_OK;
}

// Fills `tex` (an UPLOAD texture) with a solid RGBA pattern on the CPU side.
static bool fill_upload_texture(ID3D12Device* device, ID3D12Resource* tex, BYTE pattern)
{
    BYTE* p = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(tex->Map(0, &readRange, reinterpret_cast<void**>(&p)))) { return false; }

    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
    for (UINT y = 0; y < fp.Footprint.Height; ++y) {
        BYTE* row = p + (SIZE_T)y * fp.Footprint.RowPitch;
        for (UINT x = 0; x < kWidth * 4; ++x) {
            row[x] = pattern;
        }
    }
    tex->Unmap(0, nullptr);
    return true;
}

// Full round trip: producer upload texture -> shared texture ->
// consumer readback texture -> CPU. Proves the bytes really crossed adapters,
// which a silently ignored CopyResource would not.
static int verify_roundtrip(Gpu& producer, Gpu& consumer,
                            ID3D12Resource* sharedP, ID3D12Resource* sharedC,
                            BYTE pattern)
{
    ComPtr<ID3D12Resource> uploadT, readbackT;
    UINT up_pitch = 0, rb_pitch = 0;
    if (FAILED(make_staging_texture(producer, D3D12_HEAP_TYPE_UPLOAD, uploadT, &up_pitch))) {
        logf_("     verify: create UPLOAD texture FAILED\n");
        return -1;
    }
    if (!fill_upload_texture(producer.device.Get(), uploadT.Get(), pattern)) {
        logf_("     verify: fill UPLOAD texture FAILED\n");
        return -1;
    }
    if (FAILED(copy_once(producer, sharedP, uploadT.Get()))) {
        logf_("     verify: producer upload->shared FAILED\n");
        return -1;
    }

    if (FAILED(make_staging_texture(consumer, D3D12_HEAP_TYPE_READBACK, readbackT, &rb_pitch))) {
        logf_("     verify: create READBACK texture FAILED\n");
        return -1;
    }
    if (FAILED(copy_once(consumer, readbackT.Get(), sharedC))) {
        logf_("     verify: consumer shared->readback FAILED\n");
        return -1;
    }

    std::vector<BYTE> pixel(4, 0);
    const D3D12_BOX onePixel{ 0, 0, 0, 1, 1, 1 };
    if (FAILED(readbackT->ReadFromSubresource(pixel.data(), 4, 4, 0, &onePixel))) {
        logf_("     verify: ReadFromSubresource FAILED\n");
        return -1;
    }

    const bool match = (pixel[0] == pattern && pixel[1] == pattern &&
                        pixel[2] == pattern && pixel[3] == pattern);
    logf_("     verify: first pixel = %02X %02X %02X %02X (expected %02X x4) -> %s\n",
          pixel[0], pixel[1], pixel[2], pixel[3], pattern,
          match ? "DATA CROSSED ADAPTERS" : "DATA DID NOT CROSS");
    return match ? 1 : 0;
}

// ------------------------------------------------------------ variant runner

static void run_shared_variant(Gpu& producer, Gpu& consumer,
                               const char* name, bool system_mem, bool sim_access)
{
    logf_("\n-- %s --\n", name);
    logf_("   heap        : %s\n", system_mem ? "CUSTOM / pool L0 (system memory)"
                                              : "DEFAULT (producer VRAM)");
    logf_("   sim-access  : %s\n", sim_access ? "ALLOW_SIMULTANEOUS_ACCESS" : "none");

    ComPtr<ID3D12Resource> texP;
    HRESULT hr = make_texture(producer, system_mem, sim_access, true, texP);
    if (FAILED(hr)) {
        logf_("   create on producer : FAILED hr=0x%08lX\n", (unsigned long)hr);
        return;
    }
    logf_("   create on producer : OK\n");

    HANDLE h = nullptr;
    hr = make_shared_handle(producer.device.Get(), texP.Get(), &h);
    if (FAILED(hr)) {
        logf_("   CreateSharedHandle : FAILED hr=0x%08lX\n", (unsigned long)hr);
        return;
    }
    logf_("   CreateSharedHandle : OK\n");

    ComPtr<ID3D12Resource> texC;
    hr = consumer.device->OpenSharedHandle(h, IID_PPV_ARGS(&texC));
    CloseHandle(h);
    if (FAILED(hr) || !texC) {
        logf_("   OpenSharedHandle on consumer : FAILED hr=0x%08lX\n", (unsigned long)hr);
        return;
    }
    logf_("   OpenSharedHandle on consumer : OK\n");

    const int crossed = verify_roundtrip(producer, consumer, texP.Get(), texC.Get(), 0x37);
    if (crossed <= 0) {
        logf_("   VERDICT: unusable - the copy was accepted but the data never arrived\n");
        return;
    }

    ComPtr<ID3D12Resource> dst;
    hr = make_texture(consumer, false, false, false, dst);
    if (FAILED(hr)) {
        logf_("   consumer local dst create FAILED hr=0x%08lX\n", (unsigned long)hr);
        return;
    }

    const Timing t = measure_copies(consumer, dst.Get(), texC.Get());
    if (t.avg_ms < 0.0) {
        logf_("   COPY shared -> local : FAILED\n");
        return;
    }
    logf_("   COPY shared -> local : %.2f ms/frame   %.2f MB/s   (%.1f fps budget)\n",
          t.avg_ms, t.mb_per_s, 1000.0 / t.avg_ms);
}

// --------------------------------------------------- debug layer diagnostics

// Copies rejected by validation are not HRESULT failures, they are reported
// through the info queue. Dumping it is the only way to know *why* a variant
// failed, so it runs after every variant.
static void dump_info_queue(ID3D12Device* dev, const char* where)
{
    ComPtr<ID3D12InfoQueue> iq;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq)))) { return; }

    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(iq->GetMessage(i, nullptr, &len)) || len == 0) { continue; }
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(iq->GetMessage(i, msg, &len))) { continue; }
        logf_("     [layer/%s/%d] %.*s\n", where, (int)msg->Severity,
              (int)msg->DescriptionByteLength, msg->pDescription);
    }
    if (n > 0) { logf_("     (%llu debug message(s))\n", (unsigned long long)n); }
    iq->ClearStoredMessages();
}

// ------------------------------------------------- CPU staged ceiling (V5)

static HRESULT make_buffer(Gpu& g, D3D12_HEAP_TYPE type, UINT64 size,
                           ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type                 = type;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&out));
}

static bool copy_buffer_once(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src, UINT64 size)
{
    if (FAILED(g.alloc->Reset())) { return false; }
    if (FAILED(g.list->Reset(g.alloc.Get(), nullptr))) { return false; }
    g.list->CopyBufferRegion(dst, 0, src, 0, size);
    if (FAILED(g.list->Close())) { return false; }
    ID3D12CommandList* lists[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, lists);
    gpu_wait_idle(g);
    return true;
}

// The guaranteed-to-work fallback: VRAM -> readback -> CPU copy -> upload -> VRAM.
// It moves the same bytes twice over PCIe, so whatever it measures is the floor
// that any GPU-side transport must beat.
static void run_staged_ceiling(Gpu& producer, Gpu& consumer)
{
    logf_("\n-- V5 CPU staged ceiling (guaranteed fallback) --\n");
    const UINT64 bytes = (UINT64)kWidth * kHeight * 4;

    ComPtr<ID3D12Resource> vramP, readbackP, uploadC, vramC;
    if (FAILED(make_buffer(producer, D3D12_HEAP_TYPE_DEFAULT,  bytes, vramP)) ||
        FAILED(make_buffer(producer, D3D12_HEAP_TYPE_READBACK, bytes, readbackP)) ||
        FAILED(make_buffer(consumer, D3D12_HEAP_TYPE_UPLOAD,   bytes, uploadC)) ||
        FAILED(make_buffer(consumer, D3D12_HEAP_TYPE_DEFAULT,  bytes, vramC))) {
        logf_("   buffer creation FAILED\n");
        return;
    }

    std::vector<BYTE> stage(static_cast<size_t>(bytes));

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    for (UINT i = 0; i < kIterations; ++i) {
        if (!copy_buffer_once(producer, readbackP.Get(), vramP.Get(), bytes)) {
            logf_("   producer readback copy FAILED\n");
            return;
        }
        void* rp = nullptr;
        if (FAILED(readbackP->Map(0, nullptr, &rp))) { logf_("   readback Map FAILED\n"); return; }
        memcpy(stage.data(), rp, static_cast<size_t>(bytes));
        readbackP->Unmap(0, nullptr);

        void* up = nullptr;
        if (FAILED(uploadC->Map(0, nullptr, &up))) { logf_("   upload Map FAILED\n"); return; }
        memcpy(up, stage.data(), static_cast<size_t>(bytes));
        uploadC->Unmap(0, nullptr);

        if (!copy_buffer_once(consumer, vramC.Get(), uploadC.Get(), bytes)) {
            logf_("   consumer upload copy FAILED\n");
            return;
        }
    }

    QueryPerformanceCounter(&t1);
    const double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    const double avg_ms = sec * 1000.0 / (double)kIterations;
    const double moved_mb = 2.0 * frame_mb();     // down to CPU, then up to VRAM
    logf_("   round trip per frame: %.2f ms   (%.2f MB moved)   effective %.2f MB/s\n",
          avg_ms, moved_mb, moved_mb / (avg_ms / 1000.0));
    logf_("   -> at %.1f fps (2x from 40) the budget is 12.5 ms/frame\n", 80.0);
}

// ------------------------------------------------- file-mapping mailbox (V6)

// The debug layer said: "some shared CPU accessible heaps can be accomplished
// through OpenExistingHeap". That is the last high-value transport candidate:
// one file mapping (system RAM) wrapped as a D3D12 heap on BOTH devices, so each
// GPU reads/writes the SAME bytes with no CPU memcpy and no double staging.
static void run_filemap_variant(Gpu& producer, Gpu& consumer)
{
    logf_("\n-- V6 file-mapping heap shared by both devices (zero-copy mailbox) --\n");

    const UINT64 kAlign = 64 * 1024;
    const UINT64 bytes  = ((UINT64)kWidth * kHeight * 4 + kAlign - 1) / kAlign * kAlign;

    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFFu),
                                     nullptr);
    if (!hMap) {
        logf_("   CreateFileMapping FAILED err=%lu\n", GetLastError());
        return;
    }
    logf_("   file mapping : OK (%.1f MB)\n", bytes / (1024.0 * 1024.0));

    ComPtr<ID3D12Device3> devP3, devC3;
    if (FAILED(producer.device.As(&devP3)) || FAILED(consumer.device.As(&devC3))) {
        logf_("   ID3D12Device3 unavailable (OpenExistingHeapFromFileMapping needs it)\n");
        CloseHandle(hMap);
        return;
    }

    ComPtr<ID3D12Heap> heapP, heapC;
    HRESULT hr = devP3->OpenExistingHeapFromFileMapping(hMap, IID_PPV_ARGS(&heapP));
    if (FAILED(hr)) {
        logf_("   OpenExistingHeapFromFileMapping(producer) FAILED hr=0x%08lX\n",
              (unsigned long)hr);
        CloseHandle(hMap);
        return;
    }
    hr = devC3->OpenExistingHeapFromFileMapping(hMap, IID_PPV_ARGS(&heapC));
    if (FAILED(hr)) {
        logf_("   OpenExistingHeapFromFileMapping(consumer) FAILED hr=0x%08lX\n",
              (unsigned long)hr);
        CloseHandle(hMap);
        return;
    }
    logf_("   heap on both devices : OK\n");

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = bytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // The file-mapping heap is reported as a cross-adapter heap, and validation
    // requires this flag to be set iff the heap carries SHARED_CROSS_ADAPTER.
    bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    ComPtr<ID3D12Resource> mailboxP, mailboxC;
    hr = producer.device->CreatePlacedResource(heapP.Get(), 0, &bd,
                                               D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&mailboxP));
    if (FAILED(hr)) {
        logf_("   CreatePlacedResource(producer) FAILED hr=0x%08lX\n", (unsigned long)hr);
        CloseHandle(hMap);
        return;
    }
    hr = consumer.device->CreatePlacedResource(heapC.Get(), 0, &bd,
                                               D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&mailboxC));
    if (FAILED(hr)) {
        logf_("   CreatePlacedResource(consumer) FAILED hr=0x%08lX\n", (unsigned long)hr);
        CloseHandle(hMap);
        return;
    }
    logf_("   placed buffer on both devices : OK (same physical memory)\n");

    // ---- content check: producer writes, consumer reads ----
    ComPtr<ID3D12Resource> uploadP, readbackC;
    if (FAILED(make_buffer(producer, D3D12_HEAP_TYPE_UPLOAD,   bytes, uploadP)) ||
        FAILED(make_buffer(consumer, D3D12_HEAP_TYPE_READBACK, bytes, readbackC))) {
        logf_("   staging buffers FAILED\n");
        CloseHandle(hMap);
        return;
    }

    {
        void* p = nullptr;
        if (FAILED(uploadP->Map(0, nullptr, &p))) { CloseHandle(hMap); return; }
        memset(p, 0x5A, static_cast<size_t>(bytes));
        uploadP->Unmap(0, nullptr);
    }

    if (!copy_buffer_once(producer, mailboxP.Get(), uploadP.Get(), bytes)) {
        logf_("   producer write FAILED\n");
        CloseHandle(hMap);
        return;
    }
    if (!copy_buffer_once(consumer, readbackC.Get(), mailboxC.Get(), bytes)) {
        logf_("   consumer read FAILED\n");
        CloseHandle(hMap);
        return;
    }

    {
        BYTE* rp = nullptr;
        if (SUCCEEDED(readbackC->Map(0, nullptr, reinterpret_cast<void**>(&rp)))) {
            const bool match = (rp[0] == 0x5A && rp[bytes - 1] == 0x5A);
            logf_("   verify: first/last byte = %02X/%02X (expected 5A/5A) -> %s\n",
                  rp[0], rp[bytes - 1],
                  match ? "ZERO-COPY MAILBOX WORKS" : "DATA DID NOT ARRIVE");
            readbackC->Unmap(0, nullptr);
        }
    }

    // ---- timing: write side then read side, CPU fill excluded ----
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (UINT i = 0; i < kIterations; ++i) {
        copy_buffer_once(producer, mailboxP.Get(), uploadP.Get(), bytes);
        copy_buffer_once(consumer, readbackC.Get(), mailboxC.Get(), bytes);
    }
    QueryPerformanceCounter(&t1);
    const double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    const double avg_ms = sec * 1000.0 / (double)kIterations;
    const double moved_mb = 2.0 * (double)bytes / (1024.0 * 1024.0);
    logf_("   per frame: %.2f ms (%.1f MB over PCIe, no CPU memcpy) -> %.2f MB/s\n",
          avg_ms, moved_mb, moved_mb / (avg_ms / 1000.0));

    CloseHandle(hMap);
}


// ------------------------------------------- cross-adapter texture (V7, probe)

// V6 proved a cross-adapter heap built from a file mapping works for BUFFERS.
// The remaining question is whether a ROW_MAJOR *texture* can live there too,
// because CrossAdapterRowMajorTextureSupported reported FALSE in M0.1. If it
// can, the transport skips the footprint/linearisation dance entirely.
static void run_cross_adapter_texture(Gpu& producer, Gpu& consumer)
{
    logf_("\n-- V7 ROW_MAJOR texture inside a cross-adapter heap (probe) --\n");

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = kWidth;
    rd.Height           = kHeight;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = kFormat;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    const D3D12_RESOURCE_ALLOCATION_INFO ai =
        producer.device->GetResourceAllocationInfo(0, 1, &rd);
    logf_("   required size: %.1f MB, alignment: %llu\n",
          ai.SizeInBytes / (1024.0 * 1024.0), (unsigned long long)ai.Alignment);

    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     (DWORD)(ai.SizeInBytes >> 32),
                                     (DWORD)(ai.SizeInBytes & 0xFFFFFFFFu), nullptr);
    if (!hMap) { logf_("   CreateFileMapping FAILED err=%lu\n", GetLastError()); return; }

    ComPtr<ID3D12Device3> devP3, devC3;
    ComPtr<ID3D12Heap> heapP, heapC;
    if (FAILED(producer.device.As(&devP3)) || FAILED(consumer.device.As(&devC3)) ||
        FAILED(devP3->OpenExistingHeapFromFileMapping(hMap, IID_PPV_ARGS(&heapP))) ||
        FAILED(devC3->OpenExistingHeapFromFileMapping(hMap, IID_PPV_ARGS(&heapC)))) {
        logf_("   heap creation FAILED\n");
        CloseHandle(hMap);
        return;
    }

    D3D12_RESOURCE_DESC placed = rd;
    placed.Alignment = ai.Alignment;

    ComPtr<ID3D12Resource> texP, texC;
    HRESULT hr = producer.device->CreatePlacedResource(heapP.Get(), 0, &placed,
                                                       D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&texP));
    logf_("   placed texture on producer : %s (hr=0x%08lX)\n",
          SUCCEEDED(hr) ? "OK" : "FAILED", (unsigned long)hr);

    hr = consumer.device->CreatePlacedResource(heapC.Get(), 0, &placed,
                                               D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&texC));
    logf_("   placed texture on consumer : %s (hr=0x%08lX)\n",
          SUCCEEDED(hr) ? "OK" : "FAILED", (unsigned long)hr);

    if (texP && texC) {
        logf_("   -> textures work in the mailbox: both GPUs share the pixels\n");
    } else {
        logf_("   -> textures are rejected (matches CrossAdapterRowMajorTextureSupported\n");
        logf_("      = FALSE); the mailbox must carry a linear BUFFER and each side\n");
        logf_("      does a footprint copy texture<->buffer.\n");
    }

    CloseHandle(hMap);
}

// ----------------------------------------------------------------------- main

int main()
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* dot = strrchr(exePath, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - exePath), ".log");
    }
    fopen_s(&g_log, exePath, "w");

    logf_("============================================================================\n");
    logf_("  M0.2  cross-adapter transport probe   (NVIDIA -> AMD)\n");
    logf_("  payload: %ux%u RGBA8 = %.1f MB/frame, %u iterations per variant\n",
          kWidth, kHeight, frame_mb(), kIterations);
    logf_("  target : 5K@40 render, 2x present -> 80 fps -> 12.5 ms/frame budget\n");
    logf_("============================================================================\n");

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        logf_("  debug layer : ENABLED\n");
    } else {
        logf_("  debug layer : unavailable (Windows optional feature 'Graphics Tools')\n");
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        logf_("  CreateDXGIFactory2 FAILED\n");
        if (g_log) { fclose(g_log); }
        return 2;
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
        logf_("  need one NVIDIA (0x10DE) and one AMD (0x1002) adapter, aborting\n");
        if (g_log) { fclose(g_log); }
        return 3;
    }

    Gpu producer, consumer;
    if (!gpu_init(producer, nvAdapter.Get(), "producer/nvidia") ||
        !gpu_init(consumer, amdAdapter.Get(), "consumer/amd")) {
        if (g_log) { fclose(g_log); }
        return 4;
    }

    run_shared_variant(producer, consumer,
                       "V1 shared texture, producer VRAM, no sim-access", false, false);
    dump_info_queue(consumer.device.Get(), "V1-consumer");
    dump_info_queue(producer.device.Get(), "V1-producer");

    run_shared_variant(producer, consumer,
                       "V2 shared texture, producer VRAM, ALLOW_SIMULTANEOUS_ACCESS", false, true);
    dump_info_queue(consumer.device.Get(), "V2-consumer");

    run_shared_variant(producer, consumer,
                       "V3 shared texture, system memory (custom pool L0), no sim-access", true, false);
    dump_info_queue(consumer.device.Get(), "V3-consumer");

    run_shared_variant(producer, consumer,
                       "V4 shared texture, system memory (custom pool L0), ALLOW_SIMULTANEOUS_ACCESS",
                       true, true);
    dump_info_queue(consumer.device.Get(), "V4-consumer");

    run_staged_ceiling(producer, consumer);
    dump_info_queue(consumer.device.Get(), "V5-consumer");
    dump_info_queue(producer.device.Get(), "V5-producer");

    run_filemap_variant(producer, consumer);
    dump_info_queue(consumer.device.Get(), "V6-consumer");
    dump_info_queue(producer.device.Get(), "V6-producer");

    run_cross_adapter_texture(producer, consumer);
    dump_info_queue(consumer.device.Get(), "V7-consumer");
    dump_info_queue(producer.device.Get(), "V7-producer");

    logf_("\n============================================================================\n");
    logf_(" Decision rule for M1\n");
    logf_("   * Any V1..V4 that reports 'DATA CROSSED ADAPTERS' plus a ms/frame\n");
    logf_("     under ~6 ms  -> use it: GPU-side copy, no CPU involvement.\n");
    logf_("   * Only V5 works, or every V* exceeds the budget\n");
    logf_("                 -> transport must shrink: send MV+depth only at render\n");
    logf_("                    resolution and keep colour on the producer, or drop to\n");
    logf_("                    2560x1440 output.\n");
    logf_("============================================================================\n");

    gpu_release(producer);
    gpu_release(consumer);
    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}





