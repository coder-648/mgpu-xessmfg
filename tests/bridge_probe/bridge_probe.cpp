// ============================================================================
// bridge_probe - M4 consumer, stage 1: prove the cross-process handshake works
//
// Runs in its OWN process, which is the whole point: ReShade's D3D12 hooks are
// process-global, so the AMD device must never be created inside the game.
//
// It does not run XeSS-FG yet. It:
//   1. opens the named mapping the add-on publishes
//   2. wraps it as a D3D12 heap on the AMD device and places the same textures
//   3. duplicates the producer's shared fences into this process
//   4. waits for frames and reports the rate, so we know data is arriving
//
// Build: ./scripts/build_m0.sh bridge_probe     (needs the add-on running first)
// ============================================================================

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "mgpu_shared.h"

using Microsoft::WRL::ComPtr;

static void fail(const char* what, HRESULT hr = S_OK)
{
    if (hr != S_OK) {
        printf("[bridge] %s FAILED hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
    } else {
        printf("[bridge] %s FAILED err=%lu\n", what, GetLastError());
    }
}

static ComPtr<ID3D12Resource> g_local[mgpu::kMaxStreams][mgpu::kMaxSlots];

int main(int argc, char** argv)
{
    const int seconds = (argc > 1) ? atoi(argv[1]) : 30;

    printf("=== bridge_probe: waiting for '%s' ===\n", mgpu::kMappingName);

    // ---- 1. wait for the producer's mapping ----
    HANDLE map = nullptr;
    mgpu::SharedControl ctl{};
    bool have_view = false;
    void* view = nullptr;

    for (int i = 0; i < 600 && !have_view; ++i) {          // up to ~60 s
        map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mgpu::kMappingName);
        if (map) {
            view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (view) {
                memcpy(&ctl, view, sizeof(ctl));
                if (ctl.magic == mgpu::kSharedMagic && ctl.version == mgpu::kSharedVersion) {
                    have_view = true;
                    break;
                }
            }
            UnmapViewOfFile(view);
            view = nullptr;
            CloseHandle(map);
            map = nullptr;
        }
        Sleep(100);
    }

    if (!have_view) {
        printf("[bridge] producer mapping never appeared. Start the game (add-on installed) first.\n");
        return 1;
    }

    printf("[bridge] control block: pid=%u slots=%u streams=%u size=%.1f MB\n",
           ctl.producer_pid, ctl.slots, ctl.stream_count,
           ctl.mapping_size / (1024.0 * 1024.0));
    for (uint32_t i = 0; i < ctl.stream_count && i < mgpu::kMaxStreams; ++i) {
        printf("  stream %u: %ux%u fmt=%u offset=%llu slot=%.2f MB\n",
               i, ctl.streams[i].width, ctl.streams[i].height, ctl.streams[i].format,
               (unsigned long long)ctl.streams[i].offset,
               ctl.streams[i].slot_size / (1024.0 * 1024.0));
    }

    // ---- 2. AMD device ----
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { fail("CreateDXGIFactory2"); return 2; }

    ComPtr<IDXGIAdapter1> amd;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { continue; }
        if (d.VendorId == 0x1002) { amd = a; break; }
    }
    if (!amd) { printf("[bridge] no AMD adapter\n"); return 3; }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(amd.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        fail("D3D12CreateDevice"); return 4;
    }
    printf("[bridge] AMD device ready\n");

    // ---- 3. wrap the mapping as a heap, place the same textures ----
    ComPtr<ID3D12Device3> device3;
    if (FAILED(device.As(&device3))) { fail("ID3D12Device3"); return 5; }

    ComPtr<ID3D12Heap> heap;
    HRESULT hr = device3->OpenExistingHeapFromFileMapping(map, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) { fail("OpenExistingHeapFromFileMapping", hr); return 6; }
    printf("[bridge] mapping wrapped as a heap\n");

    for (uint32_t i = 0; i < ctl.stream_count && i < mgpu::kMaxStreams; ++i) {
        for (UINT s = 0; s < ctl.slots && s < mgpu::kMaxSlots; ++s) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = ctl.streams[i].width;
            rd.Height           = ctl.streams[i].height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = static_cast<DXGI_FORMAT>(ctl.streams[i].format);
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            const UINT64 off = ctl.streams[i].offset + ctl.streams[i].slot_size * s;
            if (FAILED(device->CreatePlacedResource(heap.Get(), off, &rd,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&g_local[i][s])))) {
                printf("[bridge] placed resource FAILED (stream %u slot %u)\n", i, s);
                return 7;
            }
        }
    }
    printf("[bridge] placed %u x %u textures on the shared heap\n", ctl.stream_count, ctl.slots);

    // ---- 4. duplicate the producer's shared fences ----
    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ctl.producer_pid);
    if (!producer) { fail("OpenProcess(PROCESS_DUP_HANDLE)"); return 8; }

    ComPtr<ID3D12Fence> fwd[mgpu::kMaxSlots];
    for (UINT s = 0; s < ctl.slots && s < mgpu::kMaxSlots; ++s) {
        HANDLE local_handle = nullptr;
        if (!DuplicateHandle(producer, reinterpret_cast<HANDLE>(ctl.fence_forward[s]),
                             GetCurrentProcess(), &local_handle,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            fail("DuplicateHandle(forward fence)");
            return 9;
        }
        hr = device->OpenSharedHandle(local_handle, IID_PPV_ARGS(&fwd[s]));
        CloseHandle(local_handle);
        if (FAILED(hr)) { fail("OpenSharedHandle(forward fence)", hr); return 10; }
    }
    CloseHandle(producer);
    printf("[bridge] fences duplicated\n");

    static const char* kStreamName[4] = { "color", "mv", "depth", "extra" };

    // ---- 4b. content verification: copy the MV slot back and sanity check it ----
    // Fences ticking only proves the producer signalled. This proves the shared
    // memory really holds the game's motion vectors.
    ComPtr<ID3D12CommandQueue> copy_queue;
    ComPtr<ID3D12CommandAllocator> copy_alloc;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    ComPtr<ID3D12Fence> copy_fence;
    HANDLE copy_event = nullptr;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rb_fp[mgpu::kMaxStreams]{};
    UINT64 rb_total[mgpu::kMaxStreams]{};
    UINT64 rb_max = 0;
    const uint32_t verify_stream = (ctl.stream_count > 1) ? 1u : 0u;

    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy_queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_alloc.Get(), nullptr,
                                  IID_PPV_ARGS(&copy_list));
        copy_list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence));
        copy_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        for (uint32_t i = 0; i < ctl.stream_count && i < mgpu::kMaxStreams; ++i) {
            D3D12_RESOURCE_DESC tex{};
            tex.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tex.Width            = ctl.streams[i].width;
            tex.Height           = ctl.streams[i].height;
            tex.DepthOrArraySize = 1;
            tex.MipLevels        = 1;
            tex.Format           = static_cast<DXGI_FORMAT>(ctl.streams[i].format);
            tex.SampleDesc.Count = 1;
            device->GetCopyableFootprints(&tex, 0, 1, 0, &rb_fp[i], nullptr, nullptr, &rb_total[i]);
            if (rb_total[i] > rb_max) { rb_max = rb_total[i]; }
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC buf{};
        buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width            = rb_max;
        buf.Height           = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels        = 1;
        buf.Format           = DXGI_FORMAT_UNKNOWN;
        buf.SampleDesc.Count = 1;
        buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&readback)))) {
            printf("[bridge] content check armed for stream %u (%ux%u)\n",
                   verify_stream, ctl.streams[verify_stream].width,
                   ctl.streams[verify_stream].height);
        }
    }

    auto verify_one = [&](uint32_t stream, UINT slot) {
        if (!readback || stream >= ctl.stream_count) { return; }
        ID3D12Resource* src = g_local[stream][slot].Get();
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = rb_fp[stream];

        copy_alloc->Reset();
        copy_list->Reset(copy_alloc.Get(), nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copy_list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;

        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.SubresourceIndex = 0;

        copy_list->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        copy_list->ResourceBarrier(1, &b);

        copy_list->Close();
        ID3D12CommandList* lists[] = { copy_list.Get() };
        copy_queue->ExecuteCommandLists(1, lists);
        copy_queue->Signal(copy_fence.Get(), 1);
        copy_fence->SetEventOnCompletion(1, copy_event);
        if (WaitForSingleObject(copy_event, 2000) != WAIT_OBJECT_0) { return; }

        BYTE* p = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) { return; }

        // FNV-1a over the first 64 KB, plus a count of zero words
        uint64_t hash = 1469598103934665603ull;
        uint32_t zero_words = 0;
        const uint32_t words = 16384;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(p + fp.Offset);
        for (uint32_t i = 0; i < words; ++i) {
            hash = (hash ^ w[i]) * 1099511628211ull;
            if (w[i] == 0) { ++zero_words; }
        }
        const uint32_t first_word = w[0];
        readback->Unmap(0, nullptr);

        printf("  [verify] %-6s slot %u  hash=%016llX  zero-words=%u/%u  first=%08X\n",
               kStreamName[stream], slot, (unsigned long long)hash, zero_words, words,
               first_word);
    };

    // ---- 5. receive frames ----
    printf("\n[bridge] receiving for %d s ...\n", seconds);

    UINT64 last_value[mgpu::kMaxSlots] = {};
    UINT64 received = 0;
    UINT   last_slot = 0;
    DWORD  start = GetTickCount();

    while ((GetTickCount() - start) < static_cast<DWORD>(seconds) * 1000) {
        const UINT slot = last_slot;
        // wait for the producer to publish this slot
        if (fwd[slot]->GetCompletedValue() <= last_value[slot]) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fwd[slot]->SetEventOnCompletion(last_value[slot] + 1, ev);
            const DWORD w = WaitForSingleObject(ev, 500);
            CloseHandle(ev);
            if (w != WAIT_OBJECT_0) { last_slot = (slot + 1) % ctl.slots; continue; }
        }
        last_value[slot] = fwd[slot]->GetCompletedValue();
        ++received;

        if ((received % 40) == 1) {
            const double secs = (GetTickCount() - start) / 1000.0;
            printf("  frame %6llu  slot %u  fence=%llu  (%.1f fps)\n",
                   (unsigned long long)received, slot,
                   (unsigned long long)last_value[slot], received / (secs > 0 ? secs : 1.0));
        }
        if ((received % 200) == 1) { verify_one(0, slot); verify_one(verify_stream, slot); }
        last_slot = (slot + 1) % ctl.slots;
    }

    printf("\n[bridge] done: %llu frames received in %d s\n",
           (unsigned long long)received, seconds);
    for (uint32_t i = 0; i < ctl.stream_count && i < mgpu::kMaxStreams; ++i) {
        printf("  %-6s %ux%u fmt=%u  (placed on AMD device)\n",
               kStreamName[i], ctl.streams[i].width, ctl.streams[i].height,
               ctl.streams[i].format);
    }
    return 0;
}

