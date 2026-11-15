// ============================================================================
//  host_smoke.cpp - 我们自己的宿主（mgpu_host.dll）的独立验证程序
//
//  为什么要它：把 mgpu_host.dll 放进游戏里调试，每次都要启动一次游戏、看一次崩溃
//  对话框，而且分不清是"我们的钩子坏了"还是"游戏自己有问题"。这个程序在桌面上
//  复现同样的调用序列，几秒钟就能跑完：
//
//      LoadLibrary(mgpu_host.dll)        <- 让它的四个钩子先装好
//      D3D12CreateDevice                 <- ① device
//      CreateCommandQueue(DIRECT)        <- ② queue
//      CreateDXGIFactory2 -> CreateSwapChainForHwnd   <- ③ swapchain
//      CreateCommandList / ResourceBarrier / ExecuteCommandLists
//      Present x N                       <- ④ 节拍
//
//  每一步都打印 HRESULT；任何一步失败就能立刻指到具体的钩子。
//  用法：host_smoke.exe [frames]             （mgpu_host.dll 放同目录）
// ============================================================================
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>   // strcmp (adapter selection)

// A crash here is the normal way to learn which hook is wrong, so name the module the
// faulting address belongs to instead of letting Windows show a dialog.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &mod);
    if (mod != nullptr) { GetModuleFileNameW(mod, path, MAX_PATH); }
    printf("CRASH code=0x%08lX at %p module=%ls\n", static_cast<unsigned long>(code),
           addr, mod != nullptr ? path : L"<unknown>");
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void hr(const char* what, HRESULT r)
{
    printf("  %-40s 0x%08lX %s\n", what, static_cast<unsigned long>(r),
           SUCCEEDED(r) ? "ok" : "<-- FAILED");
}

// ---------------------------------------------------------------- cross-adapter probe
// Which formats can actually live on a cross-adapter heap on THIS machine? The capture
// core's slot layout depends on it (it already found R32_TYPELESS to be the only workable
// depth format), and "rawdepth" needs 8-byte depth (R32G8X24_TYPELESS) to travel as a plain
// 1:1 copy. Shared heaps must be opened from an existing mapping, not created directly.
static void probe_cross_adapter(ID3D12Device* device)
{
    ID3D12Device3* dev3 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev3))) || dev3 == nullptr) {
        printf("cross-adapter: no ID3D12Device3 (needs Windows 10 1803+)\n");
        return;
    }

    const UINT64 size = 256ull * 1024 * 1024;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(size >> 32),
                                        static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr);
    if (mapping == nullptr) {
        printf("cross-adapter: CreateFileMapping FAILED %lu\n", GetLastError());
        dev3->Release();
        return;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view == nullptr) {
        printf("cross-adapter: MapViewOfFile FAILED\n");
        CloseHandle(mapping);
        dev3->Release();
        return;
    }

    ID3D12Heap* heap = nullptr;
    const HRESULT hhr = dev3->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap));
    if (FAILED(hhr) || heap == nullptr) {
        printf("cross-adapter: OpenExistingHeapFromAddress FAILED hr=0x%08lX\n",
               static_cast<unsigned long>(hhr));
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        dev3->Release();
        return;
    }

    struct Probe { const char* name; DXGI_FORMAT format; UINT w, h; };
    const Probe list[] = {
        { "MV  RG16F  4K",      DXGI_FORMAT_R16G16_FLOAT,         3840, 2160 },
        { "MV  RGBA16F 4K",     DXGI_FORMAT_R16G16B16A16_FLOAT,    3840, 2160 },
        { "CLR RGBA8  4K",      DXGI_FORMAT_R8G8B8A8_UNORM,        3840, 2160 },
        { "CLR R10G10B10A2 4K", DXGI_FORMAT_R10G10B10A2_UNORM,     3840, 2160 },
        { "DEP R32    1440p",   DXGI_FORMAT_R32_TYPELESS,          2560, 1440 },
        { "DEP R32G8X24 1440p", DXGI_FORMAT_R32G8X24_TYPELESS,     2560, 1440 },
        { "DEP D32S8  1440p",   DXGI_FORMAT_D32_FLOAT_S8X24_UINT,  2560, 1440 },
        { "DEP R32    4K",      DXGI_FORMAT_R32_TYPELESS,          3840, 2160 },
        { "DEP R32G8X24 4K",    DXGI_FORMAT_R32G8X24_TYPELESS,     3840, 2160 },
    };

    printf("\n-- cross-adapter placement (%u MB heap) --\n", static_cast<unsigned>(size >> 20));
    printf("  %-20s %-10s %-10s %s\n", "format", "size", "bytes", "result");
    // Each texture must get its own slice: placing them all at offset 0 is undefined and
    // removed the device on the first run, which made every later line meaningless.
    UINT64 cursor = 0;
    for (const Probe& p : list) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = p.w;
        d.Height = p.h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = p.format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        const D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &d);
        ID3D12Resource* res = nullptr;
        const UINT64 at = (cursor + 65535ull) & ~65535ull;
        cursor = at + ai.SizeInBytes;
        const HRESULT r = (cursor <= size)
                              ? device->CreatePlacedResource(heap, at, &d,
                                                             D3D12_RESOURCE_STATE_COMMON,
                                                             nullptr, IID_PPV_ARGS(&res))
                              : E_OUTOFMEMORY;
        char sz[32];
        snprintf(sz, sizeof(sz), "%ux%u", p.w, p.h);
        char by[32];
        snprintf(by, sizeof(by), "%.1f MB",
                 static_cast<double>(ai.SizeInBytes) / (1024.0 * 1024.0));
        printf("  %-20s %-10s %-10s %s (hr=0x%08lX)\n", p.name, sz, by,
               SUCCEEDED(r) ? "OK" : "REJECTED", static_cast<unsigned long>(r));
        if (res != nullptr) { res->Release(); }
    }
    printf("\n");

    heap->Release();
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    dev3->Release();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(&crash_filter);
    const UINT frames = (argc > 1) ? static_cast<UINT>(atoi(argv[1])) : 60;
    const UINT w = 1280, h = 720;

    // The host module must be loaded BEFORE any D3D12 call: its hooks are install-time,
    // not event-driven (that is the whole point of loading it from version.dll).
    HMODULE host = LoadLibraryA("mgpu_host.dll");
    printf("LoadLibrary(mgpu_host.dll) = %p\n", static_cast<void*>(host));
    if (host == nullptr) { printf("  GetLastError=%lu\n", GetLastError()); return 2; }
    Sleep(1500);   // its worker thread waits for d3d12/dxgi, then hooks

    WNDCLASSW wc{};
    wc.lpfnWndProc = &wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mgpu_host_smoke";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"mgpu host smoke", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd == nullptr) { printf("CreateWindow failed %lu\n", GetLastError()); return 2; }
    ShowWindow(hwnd, SW_SHOW);

    // Pick the adapter explicitly. This machine has an AMD iGPU next to the NVIDIA card, and
    // creating the device on the default adapter put the earlier run on AMD - its shader
    // compiler then crashed inside the capture core's first-frame work, which says nothing
    // about the game (it runs on NVIDIA).  --adapter N  or  --nvidia
    IDXGIFactory1* enum_factory = nullptr;
    IDXGIAdapter1* chosen = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&enum_factory)))) {
        int wanted = -1;
        bool want_nvidia = false;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--nvidia") == 0) { want_nvidia = true; }
            else if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) { wanted = atoi(argv[++i]); }
        }
        int index = 0;
        IDXGIAdapter1* a = nullptr;
        while (enum_factory->EnumAdapters1(index, &a) == S_OK) {
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            const bool is_nvidia = wcsstr(desc.Description, L"NVIDIA") != nullptr;
            const bool take = want_nvidia ? is_nvidia : (wanted < 0 ? index == 0 : index == wanted);
            printf("  adapter %d: %ls%s\n", index, desc.Description,
                   take ? "  <- using" : "");
            if (take && chosen == nullptr) { chosen = a; } else { a->Release(); }
            ++index;
        }
    }

    ID3D12Device* device = nullptr;
    hr("D3D12CreateDevice", D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_11_0,
                                              IID_PPV_ARGS(&device)));
    if (enum_factory != nullptr) { enum_factory->Release(); }
    if (device == nullptr) { return 1; }
    // Opt-in: the probe deliberately exhausts its heap and can leave the device removed on
    // some drivers, which would turn the normal 120-frame regression run into a FAIL for a
    // reason that has nothing to do with the hooks.
    bool want_probe = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cross-adapter") == 0) { want_probe = true; }
    }
    if (want_probe) { probe_cross_adapter(device); }
    printf("  device = %p\n", static_cast<void*>(device));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    hr("CreateCommandQueue(DIRECT)", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

    IDXGIFactory4* factory = nullptr;
    hr("CreateDXGIFactory2", CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w; sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferCount = 2;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* swap1 = nullptr;
    hr("CreateSwapChainForHwnd", factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr,
                                                                 nullptr, &swap1));
    IDXGISwapChain3* swap = nullptr;
    if (swap1 != nullptr) {
        hr("QueryInterface(IDXGISwapChain3)", swap1->QueryInterface(IID_PPV_ARGS(&swap)));
    }

    ID3D12CommandAllocator* alloc = nullptr;
    hr("CreateCommandAllocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList* list = nullptr;
    hr("CreateCommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                                      nullptr, IID_PPV_ARGS(&list)));
    if (swap == nullptr || queue == nullptr || list == nullptr) { printf("setup failed\n"); return 1; }

    ID3D12Resource* bb = nullptr;
    hr("GetBuffer(0)", swap->GetBuffer(0, IID_PPV_ARGS(&bb)));

    printf("\npresenting %u frames\n", frames);
    const DWORD t0 = GetTickCount();
    UINT presented = 0;
    for (UINT i = 0; i < frames; ++i) {
        if (SUCCEEDED(list->Reset(alloc, nullptr))) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = bb;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &b);
            list->Close();
            ID3D12CommandList* lists[] = { list };
            queue->ExecuteCommandLists(1, lists);
        }
        const HRESULT r = swap->Present(1, 0);
        if (i < 3 || (i + 1) == frames) {
            printf("  present #%u -> 0x%08lX %s\n", i + 1, static_cast<unsigned long>(r),
                   SUCCEEDED(r) ? "ok" : "<-- FAILED");
        }
        if (FAILED(r)) { break; }
        ++presented;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    const DWORD dt = GetTickCount() - t0;
    printf("\nRESULT: %u/%u presents ok in %lu ms (%.1f fps)\n", presented, frames, dt,
           dt != 0 ? (presented * 1000.0 / dt) : 0.0);

    if (bb != nullptr) { bb->Release(); }
    if (list != nullptr) { list->Release(); }
    if (alloc != nullptr) { alloc->Release(); }
    if (swap != nullptr) { swap->Release(); }
    if (swap1 != nullptr) { swap1->Release(); }
    if (factory != nullptr) { factory->Release(); }
    if (queue != nullptr) { queue->Release(); }
    if (device != nullptr) { device->Release(); }
    DestroyWindow(hwnd);
    printf("%s\n", presented == frames ? "PASS" : "FAIL");
    return presented == frames ? 0 : 1;

}
