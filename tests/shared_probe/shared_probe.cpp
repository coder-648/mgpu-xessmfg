// ============================================================================
//  shared_probe.cpp - does this machine allow a cross-adapter heap from a file
//  mapping, and does it allow depth formats there? Runs in two seconds, no game.
//
//  Born from Wuchang (UE5): "shared: OpenExistingHeapFromFileMapping FAILED"
//  at ~180 MB, while the same code path had worked at ~265 MB before.
// ============================================================================
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static const char* hx(HRESULT hr, char* buf, size_t n)
{
    sprintf_s(buf, n, "0x%08lX", (unsigned long)hr);
    return buf;
}

// Which formats can be placed on a cross-adapter heap from a file mapping?
static void probe_placed(ID3D12Device* dev, ID3D12Heap* heap)
{
    struct Probe { const char* name; DXGI_FORMAT fmt; };
    const Probe probes[] = {
        { "RG16F (4 B/px)   ", DXGI_FORMAT_R16G16_FLOAT },
        { "RGBA16F (8 B/px) ", DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "R32_TYPELESS     ", DXGI_FORMAT_R32_TYPELESS },
        { "D32_FLOAT_S8X24  ", DXGI_FORMAT_D32_FLOAT_S8X24_UINT },
    };
    for (const Probe& p : probes) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = 2560;
        rd.Height           = 1536;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = p.fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        const D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &rd);
        ComPtr<ID3D12Resource> res;
        const HRESULT hr = dev->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&res));
        char b[16];
        printf("          placed %s alloc=%llu KB  %s %s\n", p.name,
               static_cast<unsigned long long>(ai.SizeInBytes >> 10),
               hx(hr, b, sizeof(b)), SUCCEEDED(hr) ? "ok" : "FAILED");
    }
}

// The other transport route: committed cross-adapter resources + shared handles.
static void committed_route(ID3D12Device* dev)
{
    struct Probe { const char* name; DXGI_FORMAT fmt; };
    const Probe probes[] = {
        { "RGBA16F", DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "D32_FLOAT_S8X24", DXGI_FORMAT_D32_FLOAT_S8X24_UINT },
    };
    printf("\ncommitted route (ALLOW_CROSS_ADAPTER + HEAP_FLAG_SHARED):\n");
    for (const Probe& p : probes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = 2560;
        rd.Height           = 1536;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = p.fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        ComPtr<ID3D12Resource> res;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&res));
        char b[16];
        printf("   %-16s create %s %s\n", p.name, hx(hr, b, sizeof(b)),
               SUCCEEDED(hr) ? "ok" : "FAILED");
        if (SUCCEEDED(hr) && res) {
            HANDLE sh = nullptr;
            hr = dev->CreateSharedHandle(res.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
            printf("   %-16s shared handle %s %s\n", p.name, hx(hr, b, sizeof(b)),
                   SUCCEEDED(hr) ? "ok" : "FAILED");
            if (sh) { CloseHandle(sh); }
        }
    }
}

int main()
{
    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    printf("D3D12CreateDevice: 0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "ok" : "FAILED");
    if (FAILED(hr)) { return 1; }

    ComPtr<ID3D12Device3> dev3;
    hr = dev.As(&dev3);
    printf("ID3D12Device3: 0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "ok" : "FAILED");

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
    printf("CrossAdapterRowMajorTextureSupported: %d\n",
           opts.CrossAdapterRowMajorTextureSupported ? 1 : 0);

    const uint64_t sizes[] = { 189ull << 20, 253ull << 20, 265ull << 20, 380ull << 20 };
    for (uint64_t sz : sizes) {
        wchar_t name[64];
        swprintf_s(name, L"mgpu_probe_%llu", static_cast<unsigned long long>(sz >> 20));
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(sz >> 32),
                                        static_cast<DWORD>(sz & 0xFFFFFFFFu), name);
        printf("\n[%llu MB] CreateFileMapping: %p err=%lu\n",
               static_cast<unsigned long long>(sz >> 20), (void*)map, GetLastError());
        if (!map) { continue; }

        void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        printf("          MapViewOfFile: %p err=%lu\n", view, GetLastError());

        if (dev3) {
            ComPtr<ID3D12Heap> heap;
            hr = dev3->OpenExistingHeapFromFileMapping(map, IID_PPV_ARGS(&heap));
            char b[16];
            printf("          OpenExistingHeapFromFileMapping: %s %s\n",
                   hx(hr, b, sizeof(b)), SUCCEEDED(hr) ? "ok" : "FAILED");
            if (SUCCEEDED(hr) && heap) {
                D3D12_HEAP_DESC hd = heap->GetDesc();
                printf("          heap size=%llu MB flags=0x%X\n",
                       static_cast<unsigned long long>(hd.SizeInBytes >> 20),
                       static_cast<unsigned>(hd.Flags));
                probe_placed(dev.Get(), heap.Get());
            }
        }
        if (view) { UnmapViewOfFile(view); }
        CloseHandle(map);
    }

    committed_route(dev.Get());
    printf("\ndone\n");
    return 0;
}
