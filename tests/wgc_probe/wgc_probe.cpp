// ============================================================================
// wgc_probe - M4.2a: capture the game window with Windows.Graphics.Capture
//
// Why WGC for the colour stream (see docs/M4_WGC_PLAN.md):
// the game renders on the NVIDIA card but its window lives on the 5K display
// attached to the AMD card, so DWM already copies every frame across the bus to
// get it on screen. Capturing the composed window reuses that transfer instead
// of adding another 56 MB/frame of PCIe writes from the game's own queue - which
// is what pushed the game from ~80% to ~95% GPU.
//
// This probe answers the questions that decide the design:
//   * can we capture at all (exclusive fullscreen vs borderless)
//   * what size and format do we actually get (HDR!)
//   * does the frame pool work on the AMD adapter
//   * at what rate do frames arrive compared to the game's 40 fps
//
// It does not touch the game process and does not run XeSS-FG yet. Pure polling
// (TryGetNextFrame) instead of the WinRT delegate, to keep it dependency free.
//
// Build: ./scripts/build_m0.sh wgc_probe
// ============================================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <roapi.h>

#include <winrt/windows.graphics.capture.h>
#include <winrt/windows.graphics.directx.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;

static ComPtr<ID3D11Device> g_d3d11;

// ------------------------------------------------------------ window lookup

struct FindWindowCtx
{
    DWORD pid = 0;
    HWND  best = nullptr;
    LONG  best_area = 0;
};

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) { return TRUE; }
    if (!IsWindowVisible(hwnd)) { return TRUE; }

    RECT r{};
    if (!GetClientRect(hwnd, &r)) { return TRUE; }
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

static HWND find_game_window(const wchar_t* process_name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return nullptr; }

    DWORD pid = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, process_name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (pid == 0) { return nullptr; }

    FindWindowCtx ctx{};
    ctx.pid = pid;
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// ------------------------------------------------------------ D3D11 on AMD

static ComPtr<ID3D11Device> create_d3d11_on_amd()
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { return nullptr; }

    ComPtr<IDXGIAdapter1> amd;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) { break; }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { continue; }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { continue; }
        if (d.VendorId == 0x1002) {
            amd = a;
            printf("[wgc] capture device adapter: %ls\n", d.Description);
            break;
        }
    }
    if (!amd) { return nullptr; }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT hr = D3D11CreateDevice(amd.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, 2, D3D11_SDK_VERSION,
                                         &device, nullptr, &context);
    if (FAILED(hr)) {
        printf("[wgc] D3D11CreateDevice FAILED hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return nullptr;
    }
    return device;
}

// IEEE half -> float, for reading back scRGB samples.
static float half_to_float(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {
            int e = -1;
            do { ++e; mant <<= 1; } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            bits = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// --------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    const int seconds = (argc > 1) ? atoi(argv[1]) : 20;
    // The hash readback copies the full 5K frame to the CPU every frame (~56 MB
    // per frame, 2.2 GB/s at 39 fps) and IS the GPU cost this probe shows.
    // The production path never reads back - the frame stays on the GPU.
    const bool do_hash = !(argc > 2 && strcmp(argv[2], "nohash") == 0);
    // sdr = B8G8R8A8 (DWM tone-mapped it, HDR is already lost)
    // hdr = R16G16B16A16_FLOAT scRGB, i.e. the composed HDR surface untouched
    const bool want_hdr = (argc > 3 && strcmp(argv[3], "hdr") == 0);
    printf("[wgc] readback hash: %s\n",
           do_hash ? "on (costs GPU: full 5K copy to CPU every frame)"
                   : "off (production path, no GPU readback)");
    printf("[wgc] pixel format : %s\n",
           want_hdr ? "R16G16B16A16_FLOAT (scRGB, no HDR->SDR conversion)"
                    : "B8G8R8A8_UNORM (DWM already tone-mapped)");

    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) {
        printf("[wgc] RoInitialize FAILED\n");
        return 1;
    }

    const HWND hwnd = find_game_window(L"Cyberpunk2077.exe");
    if (!hwnd) {
        printf("[wgc] game window not found - is Cyberpunk 2077 running?\n");
        return 2;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    printf("[wgc] window %p client %ldx%ld\n", (void*)hwnd,
           rc.right - rc.left, rc.bottom - rc.top);

    ComPtr<IGraphicsCaptureItemInterop> interop;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HSTRING class_name = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &class_name);
        const HRESULT h = RoGetActivationFactory(class_name, IID_PPV_ARGS(&interop));
        WindowsDeleteString(class_name);
        if (FAILED(h)) {
            printf("[wgc] GraphicsCaptureItem activation FAILED hr=0x%08lX\n",
                   static_cast<unsigned long>(h));
            return 3;
        }
    }

    ComPtr<IGraphicsCaptureItem> item;
    HRESULT hr = interop->CreateForWindow(hwnd, __uuidof(IGraphicsCaptureItem),
                                          reinterpret_cast<void**>(item.GetAddressOf()));
    if (FAILED(hr)) {
        printf("[wgc] CreateForWindow FAILED hr=0x%08lX (exclusive fullscreen?)\n",
               static_cast<unsigned long>(hr));
        return 4;
    }

    SizeInt32 item_size{};
    item->get_Size(&item_size);
    printf("[wgc] capture item size: %dx%d\n", item_size.Width, item_size.Height);

    g_d3d11 = create_d3d11_on_amd();
    if (!g_d3d11) { printf("[wgc] no D3D11 device on AMD\n"); return 5; }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(g_d3d11.As(&dxgi_device))) { return 5; }

    ComPtr<IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable);
    if (FAILED(hr)) { printf("[wgc] CreateDirect3D11DeviceFromDXGIDevice FAILED\n"); return 5; }

    ComPtr<IDirect3DDevice> rt_device;
    if (FAILED(inspectable.As(&rt_device))) { return 5; }

    ComPtr<IDirect3D11CaptureFramePoolStatics2> pool_statics;
    {
        const wchar_t* kClass = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
        HSTRING class_name = nullptr;
        WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &class_name);
        hr = RoGetActivationFactory(class_name, IID_PPV_ARGS(&pool_statics));
        WindowsDeleteString(class_name);
        if (FAILED(hr)) {
            printf("[wgc] frame pool activation FAILED hr=0x%08lX\n",
                   static_cast<unsigned long>(hr));
            return 6;
        }
    }

    ComPtr<IDirect3D11CaptureFramePool> pool;
    const ABI::Windows::Graphics::DirectX::DirectXPixelFormat pool_format =
        want_hdr ? ABI::Windows::Graphics::DirectX::DirectXPixelFormat_R16G16B16A16Float
                 : ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;
    hr = pool_statics->CreateFreeThreaded(rt_device.Get(), pool_format, 2,
                                          item_size, &pool);
    if (FAILED(hr)) {
        printf("[wgc] CreateFreeThreaded FAILED hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 6;
    }

    ComPtr<IGraphicsCaptureSession> session;
    hr = pool->CreateCaptureSession(item.Get(), &session);
    if (FAILED(hr)) { printf("[wgc] CreateCaptureSession FAILED\n"); return 7; }

    ComPtr<IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(session.As(&session2))) { session2->put_IsCursorCaptureEnabled(FALSE); }
    hr = session->StartCapture();
    if (FAILED(hr)) {
        printf("[wgc] StartCapture FAILED hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 8;
    }

    printf("[wgc] capturing for %d s ...\n\n", seconds);

    // ---- poll frames ----
    UINT64 frames = 0;
    UINT64 last_hash = 0;
    UINT   hash_changes = 0;
    const DWORD start = GetTickCount();
    DXGI_FORMAT seen_format = DXGI_FORMAT_UNKNOWN;
    UINT seen_w = 0, seen_h = 0;
    bool hdr_sampled = false;

    ComPtr<ID3D11DeviceContext> ctx;
    g_d3d11->GetImmediateContext(&ctx);

    while ((GetTickCount() - start) < static_cast<DWORD>(seconds) * 1000) {
        ComPtr<IDirect3D11CaptureFrame> frame;
        hr = pool->TryGetNextFrame(&frame);
        if (FAILED(hr) || !frame) { Sleep(1); continue; }

        ComPtr<IDirect3DSurface> surface;
        frame->get_Surface(&surface);

        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(surface.As(&access))) {
            access->GetInterface(IID_PPV_ARGS(&texture));
        }

        if (texture) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (seen_format == DXGI_FORMAT_UNKNOWN) {
                seen_format = desc.Format;
                seen_w = desc.Width;
                seen_h = desc.Height;
                printf("[wgc] first frame: %ux%u fmt=%u mips=%u\n",
                       desc.Width, desc.Height, desc.Format, desc.MipLevels);
            }

            if (want_hdr && !hdr_sampled) {
                hdr_sampled = true;
                // One middle row only: ~40 KB, so this costs nothing and proves
                // whether the range is really HDR (values above 1.0) or just
                // SDR values wrapped in a float format.
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width  = desc.Width;
                sd.Height = 1;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = desc.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                ComPtr<ID3D11Texture2D> row;
                if (SUCCEEDED(g_d3d11->CreateTexture2D(&sd, nullptr, &row))) {
                    const UINT y = desc.Height / 2;
                    const D3D11_BOX box{ 0, y, 0, desc.Width, y + 1, 1 };
                    ctx->CopySubresourceRegion(row.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);

                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(ctx->Map(row.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                        const uint16_t* hp = reinterpret_cast<const uint16_t*>(m.pData);
                        float max_v = 0.0f;
                        UINT over_one = 0, total = 0;
                        for (UINT i = 0; i + 3 < m.RowPitch / 2; i += 4, ++total) {
                            const float r = half_to_float(hp[i]);
                            const float g2 = half_to_float(hp[i + 1]);
                            const float b = half_to_float(hp[i + 2]);
                            float v = r > g2 ? r : g2;
                            v = v > b ? v : b;
                            if (v > max_v) { max_v = v; }
                            if (v > 1.0f) { ++over_one; }
                        }
                        printf("[wgc] HDR sample: max RGB = %.3f   pixels above 1.0 = %u/%u\n",
                               max_v, over_one, total);
                        printf("[wgc] HDR verdict: %s\n",
                               (max_v > 1.05f) ? "real HDR range (scRGB above 1.0)"
                                               : "SDR range only - display HDR may be off");
                        ctx->Unmap(row.Get(), 0);
                    }
                }
            }

            if (do_hash) {
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> staging;
            if (SUCCEEDED(g_d3d11->CreateTexture2D(&sd, nullptr, &staging))) {
                ctx->CopyResource(staging.Get(), texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const uint32_t* px = reinterpret_cast<const uint32_t*>(mapped.pData);
                    uint64_t hash = 1469598103934665603ull;
                    const UINT count = (mapped.RowPitch / 4) * 8;   // first 8 rows
                    for (UINT i = 0; i < count; ++i) {
                        hash = (hash ^ px[i]) * 1099511628211ull;
                    }
                    if (hash != last_hash) { ++hash_changes; last_hash = hash; }
                    ctx->Unmap(staging.Get(), 0);
                }
            }
            }   // do_hash
        }

        ++frames;
        if ((frames % 40) == 1) {
            const double secs = (GetTickCount() - start) / 1000.0;
            printf("  frame %6llu  %ux%u  hash-changes=%u  (%.1f fps)\n",
                   (unsigned long long)frames, seen_w, seen_h, hash_changes,
                   frames / (secs > 0 ? secs : 1.0));
        }
    }

    ComPtr<ABI::Windows::Foundation::IClosable> closer;
    if (SUCCEEDED(session.As(&closer))) { closer->Close(); }
    closer.Reset();
    if (SUCCEEDED(pool.As(&closer))) { closer->Close(); }

    printf("\n[wgc] done: %llu frames, %u distinct hashes, %ux%u fmt=%u\n",
           (unsigned long long)frames, hash_changes, seen_w, seen_h, seen_format);
    return 0;
}
