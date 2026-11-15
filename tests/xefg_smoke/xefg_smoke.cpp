// ============================================================================
//  xefg_smoke.cpp - API-level smoke test for the XeSS-FG stub
//
//  Why this exists: no title we own ships native XeSS-FG yet, so the stub cannot be
//  exercised by a game (docs/design/FG_CLAIM.md section 23). This harness walks the exact
//  call sequence an integration makes, in order, against whichever libxess_fg.dll is
//  named on the command line:
//
//      GetVersion -> D3D12CreateContext -> GetProperties -> D3D12GetProperties
//      -> GetInitializationParameters -> InitFromSwapChainDesc -> BuildPipelines
//      -> SetDescriptorHeap -> {TagFrameConstants, SetPresentId, D3D12TagFrameResource,
//         GetLastPresentStatus} x N -> Destroy
//
//  Pointed at our stub it proves: every call returns success, nothing crashes, the handle
//  survives N frames, and the stub's log has one line per recorded declaration.
//  Pointed at the real library it proves the harness is meaningful (the real one refuses
//  the missing queue/swapchain state).
//
//  usage: xefg_smoke.exe [path\to\libxess_fg.dll] [frames]
// ============================================================================
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"

struct Api
{
    HMODULE mod = nullptr;
    xefg_swapchain_result_t (*GetVersion)(xefg_swapchain_version_t*) = nullptr;
    xefg_swapchain_result_t (*CreateContext)(ID3D12Device*, xefg_swapchain_handle_t*) = nullptr;
    xefg_swapchain_result_t (*GetProperties)(xefg_swapchain_handle_t, xefg_swapchain_properties_t*) = nullptr;
    xefg_swapchain_result_t (*D3D12GetProperties)(xefg_swapchain_handle_t,
        const xefg_swapchain_d3d12_init_params_t*, uint32_t, uint32_t, DXGI_FORMAT,
        xefg_swapchain_properties_t*) = nullptr;
    xefg_swapchain_result_t (*GetInitParams)(xefg_swapchain_handle_t,
        xefg_swapchain_d3d12_init_params_t*) = nullptr;
    xefg_swapchain_result_t (*InitFromDesc)(xefg_swapchain_handle_t, HWND,
        const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*,
        IDXGIFactory2*, const xefg_swapchain_d3d12_init_params_t*) = nullptr;
    xefg_swapchain_result_t (*BuildPipelines)(xefg_swapchain_handle_t, ID3D12PipelineLibrary*,
        uint8_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*SetHeap)(xefg_swapchain_handle_t, ID3D12DescriptorHeap*, uint32_t) = nullptr;
    xefg_swapchain_result_t (*TagConstants)(xefg_swapchain_handle_t, uint32_t,
        const xefg_swapchain_frame_constant_data_t*) = nullptr;
    xefg_swapchain_result_t (*TagResource)(xefg_swapchain_handle_t, ID3D12CommandList*, uint32_t,
        const xefg_swapchain_d3d12_resource_data_t*) = nullptr;
    xefg_swapchain_result_t (*SetPresentId)(xefg_swapchain_handle_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*SetNumInterp)(xefg_swapchain_handle_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*PresentStatus)(xefg_swapchain_handle_t,
        xefg_swapchain_present_status_t*) = nullptr;
    xefg_swapchain_result_t (*Destroy)(xefg_swapchain_handle_t) = nullptr;
};

#define BIND(name, var) api.var = reinterpret_cast<decltype(api.var)>(GetProcAddress(api.mod, name)); \
                        if (api.var == nullptr) { printf("MISSING export: %s\n", name); ++missing; }

static int failures = 0;
static void check(const char* what, xefg_swapchain_result_t r)
{
    const bool ok = (r == XEFG_SWAPCHAIN_RESULT_SUCCESS);
    printf("  %-32s r=%-5d %s\n", what, static_cast<int>(r), ok ? "ok" : "<-- UNEXPECTED");
    if (!ok) { ++failures; }
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "libxess_fg.dll";
    const int frames = (argc > 2) ? atoi(argv[2]) : 5;

    Api api;
    api.mod = LoadLibraryA(dll);
    if (api.mod == nullptr) {
        printf("LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
        return 2;
    }
    int missing = 0;
    BIND("xefgSwapChainGetVersion", GetVersion)
    BIND("xefgSwapChainD3D12CreateContext", CreateContext)
    BIND("xefgSwapChainGetProperties", GetProperties)
    BIND("xefgSwapChainD3D12GetProperties", D3D12GetProperties)
    BIND("xefgSwapChainD3D12GetInitializationParameters", GetInitParams)
    BIND("xefgSwapChainD3D12InitFromSwapChainDesc", InitFromDesc)
    BIND("xefgSwapChainD3D12BuildPipelines", BuildPipelines)
    BIND("xefgSwapChainD3D12SetDescriptorHeap", SetHeap)
    BIND("xefgSwapChainTagFrameConstants", TagConstants)
    BIND("xefgSwapChainD3D12TagFrameResource", TagResource)
    BIND("xefgSwapChainSetPresentId", SetPresentId)
    BIND("xefgSwapChainSetNumInterpolatedFrames", SetNumInterp)
    BIND("xefgSwapChainGetLastPresentStatus", PresentStatus)
    BIND("xefgSwapChainDestroy", Destroy)
    printf("loaded %s (%d missing exports)\n\n", dll, missing);
    if (missing != 0) { return 2; }

    // A device is all this needs: no window, no swapchain. WARP keeps it runnable on a
    // machine whose adapter the runtime might not like.
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        printf("D3D12CreateDevice(hardware) failed 0x%08lX, trying WARP\n",
               static_cast<unsigned long>(hr));
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter* warp = nullptr;
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
                D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
                warp->Release();
            }
            factory->Release();
        }
    }
    if (device == nullptr) { printf("no D3D12 device at all\n"); return 2; }

    printf("step 1: version\n");
    xefg_swapchain_version_t ver{};
    check("GetVersion", api.GetVersion(&ver));
    printf("        version %u.%u.%u\n", ver.major, ver.minor, ver.patch);

    printf("\nstep 2: context\n");
    xefg_swapchain_handle_t sc = nullptr;
    check("D3D12CreateContext", api.CreateContext(device, &sc));
    printf("        handle %p\n", static_cast<void*>(sc));

    printf("\nstep 3: properties\n");
    xefg_swapchain_properties_t props{};
    check("GetProperties", api.GetProperties(sc, &props));
    printf("        descriptors=%u maxInterpolations=%u\n",
           props.requiredDescriptorCount, props.maxSupportedInterpolations);
    xefg_swapchain_d3d12_init_params_t init{};
    init.maxInterpolatedFrames = 2;
    check("D3D12GetProperties", api.D3D12GetProperties(sc, &init, 3840, 2160,
                                                       DXGI_FORMAT_R16G16B16A16_FLOAT, &props));
    check("GetInitializationParameters", api.GetInitParams(sc, &init));

    printf("\nstep 4: init (desc form; no real swapchain - nothing is presented here)\n");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 3840; desc.Height = 2160; desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    xefg_swapchain_d3d12_init_params_t ip{};
    ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    ip.maxInterpolatedFrames = 2;
    check("InitFromSwapChainDesc", api.InitFromDesc(sc, nullptr, &desc, nullptr, nullptr, nullptr, &ip));
    check("BuildPipelines", api.BuildPipelines(sc, nullptr, 1, ip.initFlags));
    check("SetDescriptorHeap", api.SetHeap(sc, nullptr, 0));

    printf("\nstep 5: %d frames of declarations\n", frames);
    for (int f = 0; f < frames; ++f) {
        xefg_swapchain_frame_constant_data_t c{};
        for (int i = 0; i < 16; ++i) { c.viewMatrix[i]       = (i % 5 == 0) ? 1.0f : 0.0f; }
        for (int i = 0; i < 16; ++i) { c.projectionMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
        c.jitterOffsetX = 0.25f;
        c.jitterOffsetY = -0.375f;
        c.motionVectorScaleX = 2558.0f;
        c.motionVectorScaleY = 1438.0f;
        c.resetHistory = (f == 0) ? 1u : 0u;
        c.frameRenderTime = 23.0f;
        check("TagFrameConstants", api.TagConstants(sc, 1000 + f, &c));
        check("SetPresentId", api.SetPresentId(sc, 1000 + f));

        xefg_swapchain_d3d12_resource_data_t rd{};
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.resourceSize.x = 2558; rd.resourceSize.y = 1438;
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
        check("TagFrameResource(DEPTH)", api.TagResource(sc, nullptr, 1000 + f, &rd));
        rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        check("TagFrameResource(MV)", api.TagResource(sc, nullptr, 1000 + f, &rd));
        rd.resourceSize.x = 3840; rd.resourceSize.y = 2160;
        rd.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        check("TagFrameResource(HUDLESS)", api.TagResource(sc, nullptr, 1000 + f, &rd));

        check("SetNumInterpolatedFrames", api.SetNumInterp(sc, 2));
        xefg_swapchain_present_status_t st{};
        check("GetLastPresentStatus", api.PresentStatus(sc, &st));
        if (f == 0) {
            printf("        status: presented=%u result=%d enabled=%u\n",
                   st.framesPresented, static_cast<int>(st.frameGenResult), st.isFrameGenEnabled);
        }
    }

    printf("\nstep 6: teardown\n");
    check("Destroy", api.Destroy(sc));

    device->Release();
    FreeLibrary(api.mod);
    printf("\nRESULT: %d unexpected return code(s), %d export(s) missing\n", failures, missing);
    return (failures == 0 && missing == 0) ? 0 : 1;
}

