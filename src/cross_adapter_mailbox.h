// ============================================================================
// cross_adapter_mailbox.h  -  M1: cross-adapter transport for the dual-GPU FG
//
// Moves the per-frame payload (5K colour + 2560x1440 MV + 2560x1440 depth) from
// the producer adapter (NVIDIA, renders the game) to the consumer adapter
// (AMD, runs XeSS-FG) through one file-mapping heap that both devices map.
//
// Why this mechanism (measured in M0.2/M0.3, see docs/findings/milestones/M0_RESULTS.md):
//   * plain shared VRAM textures  -> OpenSharedHandle across vendors fails
//   * cross-adapter heaps via CreateHeap -> unsupported on this machine
//     (CrossAdapterRowMajorTextureSupported = FALSE)
//   * CreateFileMappingW + ID3D12Device3::OpenExistingHeapFromFileMapping
//     -> works, and accepts ROW_MAJOR textures on both devices
//     -> 9-19 GB/s, no CPU memcpy
//
// Synchronisation: per-slot ID3D12Fence created with
//   D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER
// (SHARED alone returns E_INVALIDARG across vendors).
//
// Usage per frame N (slot = N % slots):
//   mailbox.publish(N);      // producer: throttle, copy source -> slot, signal
//   mailbox.consume(N);      // consumer: wait, copy slot -> target, signal back
// Both calls only *record and submit*; nothing blocks the CPU.
// ============================================================================
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

namespace mgpu {

using Microsoft::WRL::ComPtr;

struct StreamSpec
{
    const char* name = "";
    UINT        width = 0;
    UINT        height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// A copy queue plus the objects needed to record onto it.
// One allocator/list per slot: a slot's allocator may only be reset once the
// GPU has finished the frame that used it, which the per-slot fence guarantees.
struct CopyLane
{
    ComPtr<ID3D12Device>       device;
    ComPtr<ID3D12CommandQueue> queue;
    std::vector<ComPtr<ID3D12CommandAllocator>>    allocs;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    ComPtr<ID3D12Fence>        local_fence;   // this device only
    HANDLE                     local_event = nullptr;
    UINT64                     local_value = 0;

    bool init(ID3D12Device* dev, UINT count);
    void shutdown();
    void wait_idle(UINT32 timeout_ms = 20000);
};

class CrossAdapterMailbox
{
public:
    // `slots` = ring depth. 2 is what M0.3 measured best under real game load.
    bool create(ID3D12Device* producer_device,
                ID3D12Device* consumer_device,
                const std::vector<StreamSpec>& streams,
                UINT slots,
                std::string* error);

    void shutdown();

    UINT slots() const { return slots_; }
    size_t stream_count() const { return streams_.size(); }

    // Producer-authored frames: the game (or our capture path) writes here.
    ID3D12Resource* producer_source(size_t stream) const;

    // Consumer-side frames: XeSS-FG reads these directly.
    ID3D12Resource* consumer_target(size_t stream) const;

    // Colour of one stream slot, for diagnostics.
    UINT64 stream_bytes(size_t stream) const;

    // Producer: wait until the consumer released this slot, record the copies
    // source -> shared slot, submit, and signal the forward fence.
    bool publish(UINT64 frame);

    // Same, but first waits on an external fence (typically "the game queue has
    // finished the command list our copies were recorded on").
    bool publish_after(UINT64 frame, ID3D12Fence* wait_fence, UINT64 wait_value);

    // Consumer: wait on the forward fence, record the copies shared slot ->
    // target, submit, and signal the back fence.
    bool consume(UINT64 frame);

    // Blocking helpers (diagnostics / shutdown only - not on the frame path).
    void wait_consumer_idle();
    void wait_producer_idle();

    // Last error text, for logging.
    const std::string& last_error() const { return error_; }

private:
    struct Stream
    {
        StreamSpec spec;
        UINT64     alloc_size = 0;
        UINT64     offset = 0;

        ComPtr<ID3D12Resource> source;             // producer VRAM
        ComPtr<ID3D12Resource> target;             // consumer VRAM
        std::vector<ComPtr<ID3D12Resource>> slotP; // shared heap, producer view
        std::vector<ComPtr<ID3D12Resource>> slotC; // shared heap, consumer view
    };

    struct SlotFence
    {
        ComPtr<ID3D12Fence> onP;
        ComPtr<ID3D12Fence> onC;
    };

    bool fail(const char* fmt, ...);
    bool make_local_texture(ID3D12Device* dev, const StreamSpec& spec,
                            ComPtr<ID3D12Resource>& out);
    bool make_placed_texture(ID3D12Device* dev, ID3D12Heap* heap,
                             const StreamSpec& spec, UINT64 offset,
                             ComPtr<ID3D12Resource>& out);
    bool make_shared_fence(ID3D12Device* prod, ID3D12Device* cons, SlotFence& f);

    std::vector<Stream>      streams_;
    std::vector<SlotFence>   forward_;
    std::vector<SlotFence>   back_;
    HANDLE                   map_ = nullptr;
    UINT64                   map_size_ = 0;
    UINT                     slots_ = 0;

    CopyLane producer_;
    CopyLane consumer_;
    std::string error_;
};

// ------------------------------------------------------------- implementation

inline bool CopyLane::init(ID3D12Device* dev, UINT count)
{
    device = dev;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { return false; }

    allocs.resize(count);
    lists.resize(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                               IID_PPV_ARGS(&allocs[i])))) {
            return false;
        }
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocs[i].Get(),
                                          nullptr, IID_PPV_ARGS(&lists[i])))) {
            return false;
        }
        lists[i]->Close();
    }

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&local_fence)))) {
        return false;
    }
    local_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return local_event != nullptr;
}

inline void CopyLane::shutdown()
{
    if (local_event) { CloseHandle(local_event); local_event = nullptr; }
    lists.clear(); allocs.clear();
    queue.Reset(); local_fence.Reset(); device.Reset();
}

inline void CopyLane::wait_idle(UINT32 timeout_ms)
{
    if (!queue) { return; }
    ++local_value;
    if (FAILED(queue->Signal(local_fence.Get(), local_value))) { return; }
    local_fence->SetEventOnCompletion(local_value, local_event);
    WaitForSingleObject(local_event, timeout_ms);
}

inline bool CrossAdapterMailbox::fail(const char* fmt, ...)
{
    char buf[512]{};
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    error_ = buf;
    return false;
}

inline bool CrossAdapterMailbox::make_local_texture(ID3D12Device* dev,
                                                    const StreamSpec& spec,
                                                    ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = spec.width;
    rd.Height           = spec.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = spec.format;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, IID_PPV_ARGS(&out)));
}

inline bool CrossAdapterMailbox::make_placed_texture(ID3D12Device* dev, ID3D12Heap* heap,
                                                     const StreamSpec& spec, UINT64 offset,
                                                     ComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = spec.width;
    rd.Height           = spec.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = spec.format;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // required because the file-mapping heap carries SHARED_CROSS_ADAPTER
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    return SUCCEEDED(dev->CreatePlacedResource(heap, offset, &rd,
                                               D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&out)));
}

inline bool CrossAdapterMailbox::make_shared_fence(ID3D12Device* prod, ID3D12Device* cons,
                                                   SlotFence& f)
{
    const D3D12_FENCE_FLAGS flags =
        (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER);
    if (FAILED(prod->CreateFence(0, flags, IID_PPV_ARGS(&f.onP)))) { return false; }

    HANDLE h = nullptr;
    if (FAILED(prod->CreateSharedHandle(f.onP.Get(), nullptr, GENERIC_ALL, nullptr, &h))) {
        return false;
    }
    const HRESULT hr = cons->OpenSharedHandle(h, IID_PPV_ARGS(&f.onC));
    CloseHandle(h);
    return SUCCEEDED(hr);
}

inline bool CrossAdapterMailbox::create(ID3D12Device* prod, ID3D12Device* cons,
                                        const std::vector<StreamSpec>& streams,
                                        UINT slots, std::string* error)
{
    bool ok = false;
    slots_ = (slots < 1) ? 1 : slots;
    streams_.clear();
    forward_.clear();
    back_.clear();
    error_.clear();

    do {
        if (streams.empty()) { fail("no streams"); break; }
        if (!producer_.init(prod, slots_)) { fail("producer copy lane"); break; }
        if (!consumer_.init(cons, slots_)) { fail("consumer copy lane"); break; }

        ComPtr<ID3D12Device3> dp3, dc3;
        if (FAILED(prod->QueryInterface(IID_PPV_ARGS(&dp3))) ||
            FAILED(cons->QueryInterface(IID_PPV_ARGS(&dc3)))) {
            fail("ID3D12Device3 unavailable (needs OpenExistingHeapFromFileMapping)");
            break;
        }

        UINT64 cursor = 0;
        for (const auto& spec : streams) {
            Stream s;
            s.spec = spec;

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = spec.width;
            rd.Height           = spec.height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = spec.format;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            const D3D12_RESOURCE_ALLOCATION_INFO ai = prod->GetResourceAllocationInfo(0, 1, &rd);
            const UINT64 aligned = (ai.SizeInBytes + 65535ULL) / 65536ULL * 65536ULL;
            s.alloc_size = aligned;
            s.offset = cursor;
            cursor += aligned * slots_;
            streams_.push_back(std::move(s));
        }
        map_size_ = cursor;

        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  (DWORD)(map_size_ >> 32),
                                  (DWORD)(map_size_ & 0xFFFFFFFFu), nullptr);
        if (!map_) { fail("CreateFileMapping err=%lu", GetLastError()); break; }

        ComPtr<ID3D12Heap> heapP, heapC;
        if (FAILED(dp3->OpenExistingHeapFromFileMapping(map_, IID_PPV_ARGS(&heapP)))) {
            fail("OpenExistingHeapFromFileMapping(producer)");
            break;
        }
        if (FAILED(dc3->OpenExistingHeapFromFileMapping(map_, IID_PPV_ARGS(&heapC)))) {
            fail("OpenExistingHeapFromFileMapping(consumer)");
            break;
        }

        bool streams_ok = true;
        for (auto& s : streams_) {
            if (!make_local_texture(prod, s.spec, s.source) ||
                !make_local_texture(cons, s.spec, s.target)) {
                fail("local texture for '%s'", s.spec.name);
                streams_ok = false;
                break;
            }
            s.slotP.resize(slots_);
            s.slotC.resize(slots_);
            for (UINT i = 0; i < slots_; ++i) {
                const UINT64 off = s.offset + s.alloc_size * i;
                if (!make_placed_texture(prod, heapP.Get(), s.spec, off, s.slotP[i]) ||
                    !make_placed_texture(cons, heapC.Get(), s.spec, off, s.slotC[i])) {
                    fail("placed slot %u for '%s'", i, s.spec.name);
                    streams_ok = false;
                    break;
                }
            }
            if (!streams_ok) { break; }
        }
        if (!streams_ok) { break; }

        forward_.resize(slots_);
        back_.resize(slots_);
        bool fences_ok = true;
        for (UINT i = 0; i < slots_; ++i) {
            if (!make_shared_fence(prod, cons, forward_[i]) ||
                !make_shared_fence(prod, cons, back_[i])) {
                fail("shared fence for slot %u", i);
                fences_ok = false;
                break;
            }
        }
        if (!fences_ok) { break; }

        ok = true;
    } while (0);

    if (!ok) {
        if (error) { *error = error_; }
        shutdown();
        return false;
    }
    return true;
}

inline void CrossAdapterMailbox::shutdown()
{
    forward_.clear();
    back_.clear();
    for (auto& s : streams_) {
        s.slotP.clear();
        s.slotC.clear();
        s.source.Reset();
        s.target.Reset();
    }
    streams_.clear();
    producer_.shutdown();
    consumer_.shutdown();
    if (map_) { CloseHandle(map_); map_ = nullptr; }
    map_size_ = 0;
}

inline ID3D12Resource* CrossAdapterMailbox::producer_source(size_t stream) const
{
    return (stream < streams_.size()) ? streams_[stream].source.Get() : nullptr;
}

inline ID3D12Resource* CrossAdapterMailbox::consumer_target(size_t stream) const
{
    return (stream < streams_.size()) ? streams_[stream].target.Get() : nullptr;
}

inline UINT64 CrossAdapterMailbox::stream_bytes(size_t stream) const
{
    if (stream >= streams_.size()) { return 0; }
    const StreamSpec& s = streams_[stream].spec;
    UINT bpp;
    switch (s.format) {
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:          bpp = 2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: bpp = 8; break;
    default:                             bpp = 4; break;
    }
    return (UINT64)s.width * s.height * bpp;
}

// Producer side. Non-blocking: enqueues the throttle wait, the copies and the
// forward signal, then returns.
inline bool CrossAdapterMailbox::publish(UINT64 frame)
{
    if (forward_.empty() || !producer_.queue) { return false; }

    const UINT   slot  = (UINT)(frame % slots_);
    const UINT64 value = frame / slots_ + 1;

    // do not overwrite a slot the consumer has not released yet
    if (frame >= slots_) {
        producer_.queue->Wait(back_[slot].onP.Get(), frame / slots_);
    }

    if (FAILED(producer_.allocs[slot]->Reset())) { return fail("producer alloc reset"); }
    if (FAILED(producer_.lists[slot]->Reset(producer_.allocs[slot].Get(), nullptr))) {
        return fail("producer list reset");
    }
    for (auto& s : streams_) {
        producer_.lists[slot]->CopyResource(s.slotP[slot].Get(), s.source.Get());
    }
    if (FAILED(producer_.lists[slot]->Close())) { return fail("producer list close"); }

    ID3D12CommandList* lists[] = { producer_.lists[slot].Get() };
    producer_.queue->ExecuteCommandLists(1, lists);
    return SUCCEEDED(producer_.queue->Signal(forward_[slot].onP.Get(), value));
}

// Consumer side. Non-blocking: the forward wait happens inside the GPU queue.
inline bool CrossAdapterMailbox::consume(UINT64 frame)
{
    if (forward_.empty() || !consumer_.queue) { return false; }

    const UINT   slot  = (UINT)(frame % slots_);
    const UINT64 value = frame / slots_ + 1;

    consumer_.queue->Wait(forward_[slot].onC.Get(), value);

    if (FAILED(consumer_.allocs[slot]->Reset())) { return fail("consumer alloc reset"); }
    if (FAILED(consumer_.lists[slot]->Reset(consumer_.allocs[slot].Get(), nullptr))) {
        return fail("consumer list reset");
    }
    for (auto& s : streams_) {
        consumer_.lists[slot]->CopyResource(s.target.Get(), s.slotC[slot].Get());
    }
    if (FAILED(consumer_.lists[slot]->Close())) { return fail("consumer list close"); }

    ID3D12CommandList* lists[] = { consumer_.lists[slot].Get() };
    consumer_.queue->ExecuteCommandLists(1, lists);
    return SUCCEEDED(consumer_.queue->Signal(back_[slot].onC.Get(), value));
}

// Producer side, ordering the copies after an external fence (e.g. "the game
// queue finished the list our source copies were recorded on").
inline bool CrossAdapterMailbox::publish_after(UINT64 frame, ID3D12Fence* wait_fence,
                                              UINT64 wait_value)
{
    if (forward_.empty() || !producer_.queue) { return false; }

    const UINT   slot  = (UINT)(frame % slots_);
    const UINT64 value = frame / slots_ + 1;

    if (frame >= slots_) {
        producer_.queue->Wait(back_[slot].onP.Get(), frame / slots_);
    }
    if (wait_fence != nullptr && wait_value != 0) {
        producer_.queue->Wait(wait_fence, wait_value);
    }

    if (FAILED(producer_.allocs[slot]->Reset())) { return fail("producer alloc reset"); }
    if (FAILED(producer_.lists[slot]->Reset(producer_.allocs[slot].Get(), nullptr))) {
        return fail("producer list reset");
    }
    for (auto& s : streams_) {
        producer_.lists[slot]->CopyResource(s.slotP[slot].Get(), s.source.Get());
    }
    if (FAILED(producer_.lists[slot]->Close())) { return fail("producer list close"); }

    ID3D12CommandList* lists[] = { producer_.lists[slot].Get() };
    producer_.queue->ExecuteCommandLists(1, lists);
    return SUCCEEDED(producer_.queue->Signal(forward_[slot].onP.Get(), value));
}

inline void CrossAdapterMailbox::wait_producer_idle() { producer_.wait_idle(); }
inline void CrossAdapterMailbox::wait_consumer_idle() { consumer_.wait_idle(); }

} // namespace mgpu

