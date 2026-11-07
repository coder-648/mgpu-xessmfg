// ============================================================================
// M1 harness - exercises src/cross_adapter_mailbox.h with the real payload shape
//
//   colour 5120x2880 R10G10B10A2  (DLSS Performance already upscaled to 5K)
//   mv     2560x1440 R16G16_FLOAT (native render resolution, no resample needed)
//   depth  2560x1440 R16_UNORM    (must match mv size, 16-bit to save 7 MB)
//
// Phase 1 (validate): 12 frames, each colour frame carries its own id in the
//   first pixel. Verifies that data really crosses, that ring slots do not
//   alias, and that the shared fences order correctly.
//
// Phase 2 (paced): 120 frames published/consumed without blocking the CPU at
//   40 fps, reporting the CPU submit cost and whether any frame failed.
//
// Build: ./scripts/build_m0.sh m1_transport
// ============================================================================

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cross_adapter_mailbox.h"

using Microsoft::WRL::ComPtr;
using mgpu::CrossAdapterMailbox;
using mgpu::StreamSpec;

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

// ---------------------------------------------------------------- small gpu

struct Gpu
{
    const char*               role = "";
    ComPtr<ID3D12Device>      device;
    ComPtr<ID3D12CommandQueue>        queue;      // harness-side filling only
    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>       fence;
    HANDLE                    event = nullptr;
    UINT64                    value = 0;
};

static bool gpu_init(Gpu& g, IDXGIAdapter1* adapter, const char* role)
{
    g.role = role;
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);

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
        logf_("  [%s] queue creation FAILED\n", role);
        return false;
    }
    g.list->Close();
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    logf_("  [%s] %ls\n", role, d.Description);
    return true;
}

static void gpu_wait_idle(Gpu& g)
{
    ++g.value;
    if (FAILED(g.queue->Signal(g.fence.Get(), g.value))) { return; }
    g.fence->SetEventOnCompletion(g.value, g.event);
    WaitForSingleObject(g.event, 20000);
}

// -------------------------------------------------------------- upload path

// One UPLOAD texture per stream, so the harness can stamp a frame id into the
// producer source and later read the consumer target back.
struct Uploader
{
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    BYTE*  mapped = nullptr;
};

static bool uploader_init(Gpu& g, const StreamSpec& spec, Uploader& u)
{
    // A texture cannot live in an UPLOAD heap for every format, so the standard
    // path is used instead: an UPLOAD *buffer* plus a placed footprint.
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width            = spec.width;
    tex.Height           = spec.height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels        = 1;
    tex.Format           = spec.format;
    tex.SampleDesc.Count = 1;
    tex.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    UINT   rows = 0;
    UINT64 row_size = 0;
    g.device->GetCopyableFootprints(&tex, 0, 1, 0, &u.fp, &rows, &row_size, &u.total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width            = u.total;
    buf.Height           = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels        = 1;
    buf.Format           = DXGI_FORMAT_UNKNOWN;
    buf.SampleDesc.Count = 1;
    buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf,
                                                         D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr,
                                                         IID_PPV_ARGS(&u.upload));
    if (FAILED(hr)) {
        logf_("    uploader: buffer create FAILED hr=0x%08lX\n", (unsigned long)hr);
        return false;
    }

    D3D12_RANGE read_range{ 0, 0 };
    const HRESULT m = u.upload->Map(0, &read_range, reinterpret_cast<void**>(&u.mapped));
    if (FAILED(m)) {
        logf_("    uploader: Map FAILED hr=0x%08lX\n", (unsigned long)m);
        return false;
    }
    return true;
}

// Copies the upload texture's first row into dst, then submits and waits.
static bool upload_row_into(Gpu& g, Uploader& u, ID3D12Resource* dst)
{
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = u.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = u.fp;

    D3D12_TEXTURE_COPY_LOCATION dloc{};
    dloc.pResource = dst;
    dloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dloc.SubresourceIndex = 0;

    D3D12_BOX box{ 0, 0, 0, 1, 1, 1 };   // first pixel only
    g.list->CopyTextureRegion(&dloc, 0, 0, 0, &src, &box);
    if (FAILED(g.list->Close())) { return false; }

    ID3D12CommandList* lists[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, lists);
    gpu_wait_idle(g);
    return true;
}

// ------------------------------------------------------------- readback path

struct Reader
{
    ComPtr<ID3D12Resource>          buffer;     // READBACK heap, footprint sink
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64                          total = 0;
};

static bool reader_init(Gpu& g, const StreamSpec& spec, Reader& r)
{
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width            = spec.width;
    tex.Height           = spec.height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels        = 1;
    tex.Format           = spec.format;
    tex.SampleDesc.Count = 1;
    tex.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    UINT   rows = 0;
    UINT64 row_size = 0;
    g.device->GetCopyableFootprints(&tex, 0, 1, 0, &r.fp, &rows, &row_size, &r.total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width            = r.total;
    buf.Height           = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels        = 1;
    buf.Format           = DXGI_FORMAT_UNKNOWN;
    buf.SampleDesc.Count = 1;
    buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf,
                                                         D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr, IID_PPV_ARGS(&r.buffer));
    if (FAILED(hr)) {
        logf_("    reader: buffer create FAILED hr=0x%08lX\n", (unsigned long)hr);
        return false;
    }
    return true;
}

static bool reader_grab(Gpu& g, Reader& r, ID3D12Resource* src, UINT32* out)
{
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) {
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = r.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = r.fp;

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;

    const D3D12_BOX one_pixel{ 0, 0, 0, 1, 1, 1 };
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &s, &one_pixel);
    if (FAILED(g.list->Close())) { return false; }

    ID3D12CommandList* lists[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, lists);
    gpu_wait_idle(g);

    BYTE* p = nullptr;
    if (FAILED(r.buffer->Map(0, nullptr, reinterpret_cast<void**>(&p)))) { return false; }
    memcpy(out, p + r.fp.Offset, 4);
    r.buffer->Unmap(0, nullptr);
    return true;
}

// Colour frame id packed into R10G10B10A2 so a mismatch is unambiguous.
static UINT32 pack_frame_id(UINT32 frame)
{
    const UINT32 r = (frame * 7u) & 1023u;
    const UINT32 g = (frame * 13u) & 1023u;
    const UINT32 b = (frame * 29u) & 1023u;
    return r | (g << 10) | (b << 20) | (3u << 30);
}

// ---------------------------------------------------------------------- main

int main()
{
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    if (char* dot = strrchr(exe_path, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - exe_path), ".log");
    }
    fopen_s(&g_log, exe_path, "w");

    logf_("============================================================================\n");
    logf_("  M1  cross-adapter mailbox harness\n");
    logf_("  colour 5120x2880 R10G10B10A2 | mv 2560x1440 RG16F | depth 2560x1440 R16\n");
    logf_("============================================================================\n");

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        logf_("  CreateDXGIFactory2 FAILED\n");
        return 2;
    }

    ComPtr<IDXGIAdapter1> nv, amd;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { continue; }
        if (d.VendorId == 0x10DE && !nv)  { nv = a; }
        if (d.VendorId == 0x1002 && !amd) { amd = a; }
    }
    if (!nv || !amd) {
        logf_("  need one NVIDIA and one AMD adapter\n");
        return 3;
    }

    Gpu producer, consumer;
    logf_("\n== devices ==\n");
    if (!gpu_init(producer, nv.Get(), "producer/nvidia") ||
        !gpu_init(consumer, amd.Get(), "consumer/amd")) {
        return 4;
    }

    std::vector<StreamSpec> streams = {
        { "color", 5120, 2880, DXGI_FORMAT_R10G10B10A2_UNORM },
        { "mv",    2560, 1440, DXGI_FORMAT_R16G16_FLOAT },
        { "depth", 2560, 1440, DXGI_FORMAT_R16_UNORM },
    };

    CrossAdapterMailbox mailbox;
    logf_("\n== mailbox ==\n");
    std::string err;
    if (!mailbox.create(producer.device.Get(), consumer.device.Get(), streams, 2, &err)) {
        logf_("  create FAILED: %s\n", err.c_str());
        if (g_log) { fclose(g_log); }
        return 5;
    }

    UINT64 total_bytes = 0;
    for (size_t i = 0; i < mailbox.stream_count(); ++i) {
        const UINT64 b = mailbox.stream_bytes(i);
        total_bytes += b;
        logf_("  %-6s %10llu bytes (%6.2f MB)\n",
              streams[i].name, (unsigned long long)b, b / (1024.0 * 1024.0));
    }
    logf_("  total %.2f MB/frame, %u slots, backed by a %.2f MB mapping\n",
          total_bytes / (1024.0 * 1024.0), mailbox.slots(),
          (double)total_bytes * mailbox.slots() / (1024.0 * 1024.0));

    // ---- phase 1: does the data really cross, and do the slots rotate? ----
    logf_("\n== phase 1: content verification (12 frames) ==\n");

    Uploader up_color;
    Reader   rd_color;
    if (!uploader_init(producer, streams[0], up_color) ||
        !reader_init(consumer, streams[0], rd_color)) {
        logf_("  staging resources FAILED\n");
        mailbox.shutdown();
        return 6;
    }

    int mismatches = 0;
    const UINT frames_validate = 12;
    for (UINT f = 0; f < frames_validate; ++f) {
        const UINT32 expected = pack_frame_id(f);
        memcpy(up_color.mapped + up_color.fp.Offset, &expected, 4);

        if (!upload_row_into(producer, up_color, mailbox.producer_source(0)) ||
            !mailbox.publish(f) || !mailbox.consume(f)) {
            logf_("  frame %u: submit FAILED (%s)\n", f, mailbox.last_error().c_str());
            ++mismatches;
            continue;
        }
        mailbox.wait_producer_idle();
        mailbox.wait_consumer_idle();

        UINT32 got = 0;
        if (!reader_grab(consumer, rd_color, mailbox.consumer_target(0), &got)) {
            logf_("  frame %u: readback FAILED\n", f);
            ++mismatches;
            continue;
        }
        const bool ok = (got == expected);
        if (!ok) { ++mismatches; }
        logf_("  frame %2u  slot %u  expected %08X  got %08X  %s\n",
              f, f % mailbox.slots(), expected, got, ok ? "ok" : "MISMATCH");
    }
    logf_("  -> %s (%d mismatch)\n", mismatches == 0 ? "PASS" : "FAIL", mismatches);

    // ---- phase 2: paced throughput, exactly what the addon will do ----
    if (mismatches == 0) {
        logf_("\n== phase 2: paced at 40 fps (120 frames, non-blocking) ==\n");
        const UINT   frames_paced = 120;
        const double interval = 25.0;
        double worst_cpu = 0.0, sum_cpu = 0.0;
        int    failures = 0;

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        auto now_ms = [&freq]() {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
        };

        const double t0 = now_ms();
        for (UINT f = 0; f < frames_paced; ++f) {
            const double a = now_ms();
            const bool ok = mailbox.publish(f) && mailbox.consume(f);
            const double cpu = now_ms() - a;
            if (!ok) { ++failures; }
            sum_cpu += cpu;
            if (cpu > worst_cpu) { worst_cpu = cpu; }

            const double next = t0 + (double)(f + 1) * interval;
            const double t = now_ms();
            if (t < next) { Sleep((DWORD)(next - t)); }
        }
        mailbox.wait_producer_idle();
        const double wall = now_ms() - t0;

        logf_("  wall %.1f ms for %u frames (%.1f fps achieved)\n",
              wall, frames_paced, frames_paced * 1000.0 / wall);
        logf_("  CPU submit per frame: avg %.3f ms, worst %.3f ms\n",
              sum_cpu / frames_paced, worst_cpu);
        logf_("  failures: %d\n", failures);
        logf_("  -> %s\n", failures == 0 ? "PASS" : "FAIL (see last_error)");
    }

    mailbox.shutdown();
    if (g_log) { fclose(g_log); g_log = nullptr; }
    return mismatches == 0 ? 0 : 1;
}



