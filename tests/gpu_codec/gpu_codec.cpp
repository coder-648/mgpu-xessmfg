// gpu_codec.cpp - run the same HLSL codec on every adapter this machine has, so the
// vendor matrix (NV encode / AMD decode and the three other pairings) is measured
// instead of assumed.
//
//   build:  scripts/build_gpu_codec.sh          (fxc + cl, staged into C:\temp)
//   run  :  %TEMP%\gpu_codec\gpu_codec.exe --list
//           gpu_codec.exe --all --w 3840 --h 2160 --nw 2
//           gpu_codec.exe --all --raw E:\mgpu_dataset\...\f00_mv.raw
//
// What it answers, and why that is enough for all four pairings:
//   1. decode(encode(x)) == x on every adapter          -> the kernel is correct there
//   2. the encoded bytes are identical on every adapter -> NV-encoded can be AMD-decoded
//      (and vice versa), so all four combinations follow from these two facts
//   3. encode/decode ms per adapter                     -> A card as render/consumer card
//      pays its own numbers, not the 3080 Ti's
//   It also compares the GPU bytes against the CPU reference (fr_codec.h), so a
//   shader that is self-consistently wrong cannot pass.
#define NOMINMAX    // <windows.h> otherwise defines min()/max() and breaks std::min
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <string>
#include <wrl/client.h>

#include "fr_codec.h"

using Microsoft::WRL::ComPtr;

static const uint32_t kTileW = 32;   // must match codec.hlsl kTileW
static const uint32_t kTileH = 8;    // must match codec.hlsl kTileH
static const uint32_t kSlotWords = 520;     // >= (4 + 256*2*4 + 4) / 4

static void die(const char* what, HRESULT hr) {
    printf("FATAL %s hr=0x%08lX\n", what, (unsigned long)hr);
    exit(1);
}
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) die(#x, hr_); } while (0)

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> psoEnc, psoDec;
};

static ComPtr<ID3D12Resource> buf_default(ID3D12Device* d, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;                 // buffers: must be 1, 0 is E_INVALIDARG
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    CK(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static ComPtr<ID3D12Resource> buf_heap(ID3D12Device* d, uint64_t bytes, D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;                 // buffers: must be 1, 0 is E_INVALIDARG
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    // UPLOAD starts GENERIC_READ, READBACK must start COPY_DEST
    const D3D12_RESOURCE_STATES st = (t == D3D12_HEAP_TYPE_READBACK)
        ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;
    CK(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static void gpu_sync(Ctx& c) {
    c.fv++;
    CK(c.q->Signal(c.fence.Get(), c.fv));
    if (c.fence->GetCompletedValue() < c.fv) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CK(c.fence->SetEventOnCompletion(c.fv, ev));
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
}

static ComPtr<ID3D12PipelineState> make_pso(ID3D12Device* d, ID3D12RootSignature* rs,
                                           const char* csoPath) {
    FILE* f = fopen(csoPath, "rb");
    if (!f) { printf("FATAL cannot open %s\n", csoPath); exit(1); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> code((size_t)n);
    if (fread(code.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); exit(1); }
    fclose(f);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS = { code.data(), (SIZE_T)n };
    ComPtr<ID3D12PipelineState> pso;
    CK(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    return pso;
}

struct Adapter {
    ComPtr<IDXGIAdapter1> ad;
    std::string name;
    bool isAmd = false, isNv = false, isSw = false;
};

static std::vector<Adapter> enum_adapters() {
    IDXGIFactory4* fac = nullptr;
    CK(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    std::vector<Adapter> out;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        Adapter e;
        e.ad.Attach(a);
        char nm[512];
        WideCharToMultiByte(CP_ACP, 0, d.Description, -1, nm, sizeof(nm), nullptr, nullptr);
        e.name = nm;
        e.isSw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        e.isAmd = (d.VendorId == 0x1002);
        e.isNv = (d.VendorId == 0x10DE);
        out.push_back(e);
    }
    fac->Release();
    return out;
}


static void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
                       D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    l->ResourceBarrier(1, &bar);
}

struct Result {
    std::vector<uint8_t> encoded;
    std::vector<uint32_t> decoded;
    std::vector<uint32_t> tileLen;
    std::vector<uint32_t> cpuTileLen;
    double encMs = 0, decMs = 0;
    bool cpuMatch = false, rtMatch = false;
    size_t usedBytes = 0;
};

// CPU side of the same slot layout (fr::encode_tile), for the memcmp.
static void cpu_encode_slots(const std::vector<uint32_t>& px, uint32_t W, uint32_t H, uint32_t nw,
                             uint32_t tilesX, std::vector<uint8_t>& out,
                             std::vector<uint32_t>& lens) {
    const uint32_t tilesY = (H + kTileH - 1) / kTileH;
    out.assign((size_t)tilesX * tilesY * kSlotWords * 4, 0);
    lens.assign((size_t)tilesX * tilesY, 0);
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            const uint32_t t = ty * tilesX + tx;
            std::vector<uint8_t> one;
            fr::encode_tile(px.data(), W, H, nw, (size_t)tx * kTileW, (size_t)ty * kTileH, one);
            memcpy(out.data() + (size_t)t * kSlotWords * 4, one.data(), one.size());
            lens[t] = (uint32_t)one.size();
        }
    }
}

static bool run_one(const Adapter& a, uint32_t W, uint32_t H, uint32_t nw,
                    const std::vector<uint32_t>& pixels, int iters,
                    const char* dir, Result& res) {
    Ctx c;
    CK(D3D12CreateDevice(a.ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.dev)));

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CK(c.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.q)));
    CK(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc)));
    CK(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr,
                                IID_PPV_ARGS(&c.list)));
    CK(c.list->Close());
    CK(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)));

    // root signature: b0 constants + t0 SRV + u0/u1 UAV (root descriptors, no heap)
    D3D12_ROOT_PARAMETER rp[4]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 8;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 0;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].Descriptor.ShaderRegister = 1;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4; rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    CK(c.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  IID_PPV_ARGS(&c.rs)));

    char pathEnc[512], pathDec[512];
    sprintf_s(pathEnc, "%s\\encode.cso", dir);
    sprintf_s(pathDec, "%s\\decode.cso", dir);
    c.psoEnc = make_pso(c.dev.Get(), c.rs.Get(), pathEnc);
    c.psoDec = make_pso(c.dev.Get(), c.rs.Get(), pathDec);

    const uint32_t tilesX = (W + kTileW - 1) / kTileW;
    const uint32_t tilesY = (H + kTileH - 1) / kTileH;
    const uint32_t tiles = tilesX * tilesY;
    const uint64_t pixBytes = (uint64_t)W * H * nw * 4;
    const uint64_t slotBytes = (uint64_t)tiles * kSlotWords * 4;

    ComPtr<ID3D12Resource> srcPix = buf_default(c.dev.Get(), pixBytes);
    ComPtr<ID3D12Resource> upPix = buf_heap(c.dev.Get(), pixBytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> slots = buf_default(c.dev.Get(), slotBytes);
    ComPtr<ID3D12Resource> upSlots = buf_heap(c.dev.Get(), slotBytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> downSlots = buf_heap(c.dev.Get(), slotBytes, D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> tileLen = buf_default(c.dev.Get(), (uint64_t)tiles * 4);
    ComPtr<ID3D12Resource> downLen = buf_heap(c.dev.Get(), (uint64_t)tiles * 4, D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> decPix = buf_default(c.dev.Get(), pixBytes);
    ComPtr<ID3D12Resource> downPix = buf_heap(c.dev.Get(), pixBytes, D3D12_HEAP_TYPE_READBACK);

    // upload the pixels
    void* mp = nullptr;
    CK(upPix->Map(0, nullptr, &mp));
    memcpy(mp, pixels.data(), (size_t)pixBytes);
    upPix->Unmap(0, nullptr);
    CK(c.alloc->Reset());
    CK(c.list->Reset(c.alloc.Get(), nullptr));
    transition(c.list.Get(), srcPix.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
    c.list->CopyBufferRegion(srcPix.Get(), 0, upPix.Get(), 0, pixBytes);
    transition(c.list.Get(), srcPix.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CK(c.list->Close());
    ID3D12CommandList* lists[] = { c.list.Get() };
    c.q->ExecuteCommandLists(1, lists);
    gpu_sync(c);
    // ---------------------------------------------------------------- encode --
    const UINT gxEnc = tiles;               // encode: one block per tile (parallel)
    const UINT gxDec = tiles;               // decode: 1 block (256) per tile
    auto set_state = [&](ID3D12PipelineState* pso, ID3D12Resource* in, ID3D12Resource* out) {
        c.list->SetPipelineState(pso);
        c.list->SetComputeRootSignature(c.rs.Get());
        const UINT v[8] = { W, H, nw, tilesX, tilesY, kSlotWords, 0, 0 };
        c.list->SetComputeRoot32BitConstants(0, 8, v, 0);
        c.list->SetComputeRootShaderResourceView(1, in->GetGPUVirtualAddress());
        c.list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
        c.list->SetComputeRootUnorderedAccessView(3, tileLen->GetGPUVirtualAddress());
    };
    auto uav_barrier = [&]() {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = slots.Get();
        c.list->ResourceBarrier(1, &b);
    };

    for (int pass = 0; pass < 2; ++pass) {           // pass 0 = warmup, pass 1 = timed
        CK(c.alloc->Reset());
        CK(c.list->Reset(c.alloc.Get(), nullptr));
        transition(c.list.Get(), slots.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        set_state(c.psoEnc.Get(), srcPix.Get(), slots.Get());
        const int n = (pass == 0) ? 1 : iters;
        for (int i = 0; i < n; ++i) {
            c.list->Dispatch(gxEnc, 1, 1);
            if (i + 1 < n) uav_barrier();
        }
        transition(c.list.Get(), slots.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.list->CopyBufferRegion(downSlots.Get(), 0, slots.Get(), 0, slotBytes);
        c.list->CopyBufferRegion(downLen.Get(), 0, tileLen.Get(), 0, (UINT64)tiles * 4);
        transition(c.list.Get(), slots.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        CK(c.list->Close());
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        c.q->ExecuteCommandLists(1, lists);
        gpu_sync(c);
        QueryPerformanceCounter(&t1);
        if (pass == 1) res.encMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / iters;
    }

    // readback + compare with the CPU reference
    res.encoded.assign((size_t)slotBytes, 0);
    res.tileLen.assign((size_t)tiles, 0);
    {
        void* m = nullptr;
        CK(downSlots->Map(0, nullptr, &m));
        memcpy(res.encoded.data(), m, (size_t)slotBytes);
        downSlots->Unmap(0, nullptr);
        CK(downLen->Map(0, nullptr, &m));
        memcpy(res.tileLen.data(), m, (size_t)tiles * 4);
        downLen->Unmap(0, nullptr);
    }
    std::vector<uint8_t> cpuSlots;
    cpu_encode_slots(pixels, W, H, nw, tilesX, cpuSlots, res.cpuTileLen);
    res.cpuMatch = (cpuSlots == res.encoded);
    if (!res.cpuMatch) {
        for (size_t i = 0; i < res.tileLen.size() && i < res.cpuTileLen.size(); ++i) {
            const uint32_t g = *(const uint32_t*)(res.encoded.data() + i * (size_t)kSlotWords * 4);
            fr::TilePlan pl;
            fr::plan_tile(pixels.data(), W, H, nw, (i % tilesX) * kTileW, (i / tilesX) * kTileH, pl);
            printf("    tile %zu: gpu mode=%u w=%u | cpu mode=%d w=%d\n", i,
                   (g >> 6) & 3u, g & 0x3Fu, pl.mode, pl.w);
            if (res.tileLen[i] != res.cpuTileLen[i]) {
                printf("    tile %zu length: gpu=%u cpu=%u\n", i, res.tileLen[i], res.cpuTileLen[i]);
                break;
            }
        }
        size_t d = 0;
        while (d < cpuSlots.size() && d < res.encoded.size() && cpuSlots[d] == res.encoded[d]) ++d;
        const size_t slotBytes = (size_t)kSlotWords * 4;
        printf("    first diff vs CPU: byte %zu (tile %zu, offset %zu) gpu=%02x cpu=%02x\n",
               d, d / slotBytes, d % slotBytes, res.encoded[d], cpuSlots[d]);
    }
    size_t used = 0;
    for (uint32_t v : res.tileLen) used += v;
    res.usedBytes = used;

    // ---------------------------------------------------------------- decode --
    // upload the *encoded* bytes so decode reads exactly what was written
    {
        void* m = nullptr;
        CK(upSlots->Map(0, nullptr, &m));
        memcpy(m, res.encoded.data(), (size_t)slotBytes);
        upSlots->Unmap(0, nullptr);
    }
    CK(c.alloc->Reset());
    CK(c.list->Reset(c.alloc.Get(), nullptr));
    c.list->CopyBufferRegion(slots.Get(), 0, upSlots.Get(), 0, slotBytes);
    transition(c.list.Get(), slots.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CK(c.list->Close());
    c.q->ExecuteCommandLists(1, lists);
    gpu_sync(c);

    auto dec_barrier = [&]() {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = decPix.Get();
        c.list->ResourceBarrier(1, &b);
    };
    for (int pass = 0; pass < 2; ++pass) {
        CK(c.alloc->Reset());
        CK(c.list->Reset(c.alloc.Get(), nullptr));
        transition(c.list.Get(), decPix.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        set_state(c.psoDec.Get(), slots.Get(), decPix.Get());
        const int n = (pass == 0) ? 1 : iters;
        for (int i = 0; i < n; ++i) {
            c.list->Dispatch(gxDec, 1, 1);
            if (i + 1 < n) dec_barrier();
        }
        transition(c.list.Get(), decPix.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.list->CopyBufferRegion(downPix.Get(), 0, decPix.Get(), 0, pixBytes);
        transition(c.list.Get(), decPix.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        CK(c.list->Close());
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        c.q->ExecuteCommandLists(1, lists);
        gpu_sync(c);
        QueryPerformanceCounter(&t1);
        if (pass == 1) res.decMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / iters;
    }
    res.decoded.assign((size_t)W * H * nw, 0);
    {
        void* m = nullptr;
        CK(downPix->Map(0, nullptr, &m));
        memcpy(res.decoded.data(), m, (size_t)pixBytes);
        downPix->Unmap(0, nullptr);
    }
    res.rtMatch = (memcmp(res.decoded.data(), pixels.data(), (size_t)pixBytes) == 0);
    if (!res.rtMatch) {
        size_t d = 0;
        const size_t n = (size_t)W * H * nw;
        while (d < n && res.decoded[d] == pixels[d]) ++d;
        printf("    first diff: word %zu of %zu (y=%zu x=%zu) got=%08x want=%08x\n",
               d, n, d / ((size_t)W * nw), (d / nw) % W, res.decoded[d], pixels[d]);
    }
    return true;
}


// ------------------------------------------------------------------ main -----
static void make_pattern(std::vector<uint32_t>& px, uint32_t W, uint32_t H, uint32_t nw, int kind) {
    px.resize((size_t)W * H * nw);
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            for (uint32_t l = 0; l < nw; ++l) {
                uint32_t v;
                if (kind == 1) { v = rnd(); }                       // pure noise
                else if (kind == 2) { v = 0; }                      // zeros
                else {                                              // MV-ish
                    const uint32_t base = 0x3F000000u + (uint32_t)((x / 7 + y / 5) % 64) * 37u;
                    const bool noisy = (x > W / 3 && x < W / 2 && y > H / 3 && y < H / 2);
                    v = noisy ? (base + (rnd() % 4096u)) : (base + (rnd() % 8u));
                }
                px[((size_t)y * W + x) * nw + l] = v;
            }
        }
    }
}

static bool load_raw(const char* path, std::vector<uint32_t>& px, uint32_t& W, uint32_t& H,
                     uint32_t& nw) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return false; }
    uint32_t hdr[4];
    if (fread(hdr, 4, 4, f) != 4) { fclose(f); return false; }
    W = hdr[0]; H = hdr[1];
    const uint32_t fmt = hdr[2], pitch = hdr[3];
    const uint32_t bpp = (fmt == 10) ? 8 : 4;
    nw = bpp / 4;
    px.assign((size_t)W * H * nw, 0);
    std::vector<uint8_t> tmp(pitch);
    for (uint32_t y = 0; y < H; ++y) {
        if (fread(tmp.data(), 1, pitch, f) != pitch) break;
        memcpy((uint8_t*)px.data() + (size_t)y * W * bpp, tmp.data(), (size_t)W * bpp);
    }
    fclose(f);
    printf("loaded %s: %ux%u fmt=%u (nw=%u)\n", path, W, H, fmt, nw);
    return true;
}


int main(int argc, char** argv) {
    uint32_t W = 1920, H = 1080, nw = 1;
    int iters = 5, pattern = 0, only = -1;
    bool all = false, list = false;
    const char* raw = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) { list = true; }
        else if (!strcmp(argv[i], "--all")) { all = true; }
        else if (!strcmp(argv[i], "--w") && i + 1 < argc) { W = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--h") && i + 1 < argc) { H = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--nw") && i + 1 < argc) { nw = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) { iters = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--pattern") && i + 1 < argc) { pattern = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--adapter") && i + 1 < argc) { only = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) { raw = argv[++i]; }
        else { printf("unknown arg %s\n", argv[i]); return 2; }
    }

    const std::vector<Adapter> ads = enum_adapters();
    printf("adapters:\n");
    for (size_t i = 0; i < ads.size(); ++i) {
        printf("  [%zu] %-42s %s%s\n", i, ads[i].name.c_str(),
               ads[i].isNv ? "NVIDIA" : (ads[i].isAmd ? "AMD" : "other"),
               ads[i].isSw ? " (software)" : " (hardware)");
    }
    if (list) { return 0; }

    std::vector<uint32_t> pixels;
    if (raw) { if (!load_raw(raw, pixels, W, H, nw)) { return 1; } }
    else { make_pattern(pixels, W, H, nw, pattern); }

    char dir[512];
    GetModuleFileNameA(nullptr, dir, sizeof(dir));
    char* slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; }

    printf("\ninput %ux%u nw=%u  (%.1f MB)  iters=%d\n\n",
           W, H, nw, (double)W * H * nw * 4 / 1e6, iters);

    std::vector<Result> res(ads.size());
    std::vector<bool> ran(ads.size(), false);
    for (size_t i = 0; i < ads.size(); ++i) {
        if (ads[i].isSw) { continue; }
        if (only >= 0 && (int)i != only) { continue; }
        if (!all && only < 0) { continue; }
        printf("[%zu] %s\n", i, ads[i].name.c_str());
        if (!run_one(ads[i], W, H, nw, pixels, iters, dir, res[i])) { continue; }
        ran[i] = true;
        const double rawMB = (double)W * H * nw * 4 / 1e6;
    if (nw != 1) {
        printf("    (the GPU encoder handles nw=1 only; encode timings/ratios below are invalid)\n");
    }
        printf("    encode %7.3f ms  (%6.1f GB/s in)   %6.2f MB -> %6.2f MB  %.2fx\n",
               res[i].encMs, rawMB / res[i].encMs, rawMB, res[i].usedBytes / 1e6,
               rawMB / (res[i].usedBytes / 1e6));
        printf("    decode %7.3f ms\n", res[i].decMs);
        printf("    roundtrip %s   vs CPU reference %s\n",
               res[i].rtMatch ? "OK" : "MISMATCH", res[i].cpuMatch ? "OK" : "MISMATCH");
    }

    // cross-vendor: identical bytes + working roundtrip on both sides is exactly what
    // the four pairings need (N->N, N->A, A->N, A->A).
    int first = -1;
    bool identical = true;
    for (size_t i = 0; i < ads.size(); ++i) {
        if (!ran[i]) continue;
        if (first < 0) { first = (int)i; continue; }
        if (res[i].encoded != res[(size_t)first].encoded) { identical = false; }
    }
    if (first >= 0) {
        printf("\nencoded bytes identical across adapters: %s\n", identical ? "YES" : "NO");
        if (!identical) {
            const std::vector<uint8_t>& a = res[(size_t)first].encoded;
            for (size_t i = 0; i < ads.size(); ++i) {
                if (!ran[i] || (int)i == first) continue;
                size_t d = 0;
                while (d < a.size() && a[d] == res[i].encoded[d]) ++d;
                printf("  first diff vs [%zu]: byte %zu\n", i, d);
            }
        }
        printf("=> N/N, N/A, A/N, A/A all hold when: per-adapter roundtrip OK\n"
               "   AND bytes identical AND vs-CPU OK.\n");
    }
    return 0;
}

