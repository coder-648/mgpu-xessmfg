// ============================================================================
// M0.1 - XeSS-FG / XeLL capability probe (minimum viable module)
//
// Answers exactly ONE question before any further code is written:
//   can libxess_fg.dll create an XeSS-FG context on the AMD RX 6750 GRE?
//
// Also records per adapter: DXGI identity, highest shader model,
// CrossAdapterRowMajorTextureSupported (needed by the cross-adapter transport
// in M0.2/M1), XeLL context result, XeSS-FG context result,
// SetLatencyReduction handshake, and maxSupportedInterpolations.
//
// No window, no swap chain, no ReShade. Deliberately tiny.
// Build: tests/m0_probe/build.bat (driven by scripts/build_m0.sh)
// Output: stdout + m0_probe.log next to the exe.
// ============================================================================

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "xell/xell.h"
#include "xell/xell_d3d12.h"
#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xess_fg/xefg_swapchain_debug.h"

using Microsoft::WRL::ComPtr;

static FILE* g_log = nullptr;

static void logf_(const char* fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    vprintf(fmt, a);
    va_end(a);
    if (g_log) {
        vfprintf(g_log, fmt, b);
        fflush(g_log);
    }
    va_end(b);
    fflush(stdout);
}

// ---------------------------------------------------------------- result names

static const char* xefg_result_name(xefg_swapchain_result_t r)
{
    switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS:                            return "SUCCESS";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER:                 return "WARNING_OLD_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES:             return "WARNING_TOO_FEW_FRAMES";
    case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH:         return "WARNING_FRAMES_ID_MISMATCH";
    case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS:     return "WARNING_MISSING_PRESENT_STATUS";
    case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH:    return "WARNING_RESOURCE_SIZES_MISMATCH";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE:           return "ERROR_UNSUPPORTED_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER:           return "ERROR_UNSUPPORTED_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED:                return "ERROR_UNINITIALIZED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT:             return "ERROR_INVALID_ARGUMENT";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:         return "ERROR_DEVICE_OUT_OF_MEMORY";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE:                       return "ERROR_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED:              return "ERROR_NOT_IMPLEMENTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT:              return "ERROR_INVALID_CONTEXT";
    case XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS:        return "ERROR_OPERATION_IN_PROGRESS";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED:                  return "ERROR_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY:            return "ERROR_CANT_LOAD_LIBRARY";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES:     return "ERROR_MISMATCH_INPUT_RESOURCES";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES:   return "ERROR_INCORRECT_OUTPUT_RESOURCES";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES:    return "ERROR_INCORRECT_INPUT_RESOURCES";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED:
        return "ERROR_LATENCY_REDUCTION_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING:
        return "ERROR_LATENCY_REDUCTION_FUNCTION_MISSING";
    case XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE:              return "ERROR_HRESULT_FAILURE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL:            return "ERROR_DXGI_INVALID_CALL";
    case XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE:         return "ERROR_POINTER_STILL_IN_USE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP:      return "ERROR_INVALID_DESCRIPTOR_HEAP";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER:             return "ERROR_WRONG_CALL_ORDER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN:                      return "ERROR_UNKNOWN";
    default:                                                       return "???";
    }
}

static const char* xell_result_name(xell_result_t r)
{
    switch (r) {
    case XELL_RESULT_SUCCESS:                    return "SUCCESS";
    case XELL_RESULT_ERROR_UNSUPPORTED_DEVICE:   return "ERROR_UNSUPPORTED_DEVICE";
    case XELL_RESULT_ERROR_UNSUPPORTED_DRIVER:   return "ERROR_UNSUPPORTED_DRIVER";
    case XELL_RESULT_ERROR_UNINITIALIZED:        return "ERROR_UNINITIALIZED";
    case XELL_RESULT_ERROR_INVALID_ARGUMENT:     return "ERROR_INVALID_ARGUMENT";
    case XELL_RESULT_ERROR_DEVICE:               return "ERROR_DEVICE";
    case XELL_RESULT_ERROR_NOT_IMPLEMENTED:      return "ERROR_NOT_IMPLEMENTED";
    case XELL_RESULT_ERROR_INVALID_CONTEXT:      return "ERROR_INVALID_CONTEXT";
    case XELL_RESULT_ERROR_UNSUPPORTED:          return "ERROR_UNSUPPORTED";
    case XELL_RESULT_ERROR_UNKNOWN:              return "ERROR_UNKNOWN";
    default:                                     return "???";
    }
}

// -------------------------------------------------------------- log callbacks

static void xefg_log_cb(const char* msg, xefg_swapchain_logging_level_t lvl, void* /*userData*/)
{
    const char* tag = "?";
    switch (lvl) {
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG:   tag = "DBG"; break;
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO:    tag = "INF"; break;
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING: tag = "WRN"; break;
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR:   tag = "ERR"; break;
    default: break;
    }
    logf_("      [xefg %s] %s\n", tag, msg ? msg : "(null)");
}

static void xell_log_cb(const char* msg, xell_logging_level_t lvl)
{
    const char* tag = "?";
    switch (lvl) {
    case XELL_LOGGING_LEVEL_DEBUG:   tag = "DBG"; break;
    case XELL_LOGGING_LEVEL_INFO:    tag = "INF"; break;
    case XELL_LOGGING_LEVEL_WARNING: tag = "WRN"; break;
    case XELL_LOGGING_LEVEL_ERROR:   tag = "ERR"; break;
    default: break;
    }
    logf_("      [xell %s] %s\n", tag, msg ? msg : "(null)");
}

// --------------------------------------------------------------- per adapter

static void probe_adapter(IDXGIAdapter1* adapter, unsigned index)
{
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
        logf_("---- adapter #%u: GetDesc1 failed\n", index);
        return;
    }

    const bool is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    logf_("\n---- adapter #%u -------------------------------------------\n", index);
    logf_("  name      : %ls\n", desc.Description);
    logf_("  vendor    : 0x%04X   device: 0x%04X   subsys: 0x%08X\n",
          desc.VendorId, desc.DeviceId, desc.SubSysId);
    logf_("  LUID      : %08lX:%08lX\n",
          (unsigned long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart);
    logf_("  dedicated : %.1f MB   shared: %.1f MB\n",
          desc.DedicatedVideoMemory / (1024.0 * 1024.0),
          desc.SharedSystemMemory / (1024.0 * 1024.0));
    logf_("  flags     : 0x%X%s\n", desc.Flags, is_software ? " (SOFTWARE)" : "");

    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
            logf_("  vram      : usage %.1f MB / budget %.1f MB\n",
                  mi.CurrentUsage / (1024.0 * 1024.0), mi.Budget / (1024.0 * 1024.0));
        }
    }

    if (is_software) {
        logf_("  -> skipping software adapter\n");
        return;
    }

    // ---- D3D12 device ----
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        logf_("  D3D12CreateDevice : FAILED hr=0x%08lX\n", (unsigned long)hr);
        return;
    }
    logf_("  D3D12CreateDevice : OK\n");

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        logf_("  CrossAdapterRowMajorTextureSupported : %s   <-- M1 transport needs TRUE\n",
              opts.CrossAdapterRowMajorTextureSupported ? "TRUE" : "FALSE");
        logf_("  ResourceBindingTier : %d   TiledResourcesTier : %d\n",
              (int)opts.ResourceBindingTier, (int)opts.TiledResourcesTier);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_7 };
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        logf_("  highest shader model : 6.%d\n",
              (int)sm.HighestShaderModel - (int)D3D_SHADER_MODEL_6_0);
    }

    // ---- XeLL ----
    logf_("  ---- XeLL ----\n");
    xell_context_handle_t xellCtx = nullptr;
    xell_result_t xr = xellD3D12CreateContext(device.Get(), &xellCtx);
    logf_("  xellD3D12CreateContext : %s (%d)\n", xell_result_name(xr), (int)xr);
    if (xr == XELL_RESULT_SUCCESS && xellCtx) {
        xellSetLoggingCallback(xellCtx, XELL_LOGGING_LEVEL_WARNING, xell_log_cb);
    }

    // ---- XeSS-FG ----
    logf_("  ---- XeSS-FG ----\n");
    xefg_swapchain_handle_t fgCtx = nullptr;
    xefg_swapchain_result_t fr = xefgSwapChainD3D12CreateContext(device.Get(), &fgCtx);
    logf_("  xefgSwapChainD3D12CreateContext : %s (%d)\n", xefg_result_name(fr), (int)fr);

    if (fr == XEFG_SWAPCHAIN_RESULT_SUCCESS && fgCtx) {
        xefgSwapChainSetLoggingCallback(fgCtx, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,
                                        xefg_log_cb, nullptr);

        xefg_swapchain_properties_t props{};
        xefg_swapchain_result_t pr = xefgSwapChainGetProperties(fgCtx, &props);
        if (pr == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            logf_("  maxSupportedInterpolations : %u   requiredDescriptorCount : %u\n",
                  props.maxSupportedInterpolations, props.requiredDescriptorCount);
            logf_("  tempBufferHeap=%llu B  tempTextureHeap=%llu B  cbuffer=%llu B\n",
                  (unsigned long long)props.tempBufferHeapSize,
                  (unsigned long long)props.tempTextureHeapSize,
                  (unsigned long long)props.constantBufferSize);
            logf_("  (target is 3x -> 5K40 becomes 120. If this number is >= 3 the SDK\n");
            logf_("   already allows it on this GPU; if it is 1, a community unlock is\n");
            logf_("   genuinely required.)\n");
        } else {
            logf_("  xefgSwapChainGetProperties : %s (%d)\n", xefg_result_name(pr), (int)pr);
        }

        if (xellCtx) {
            xefg_swapchain_result_t lr =
                xefgSwapChainSetLatencyReduction(fgCtx, reinterpret_cast<void*>(xellCtx));
            logf_("  xefgSwapChainSetLatencyReduction : %s (%d)\n",
                  xefg_result_name(lr), (int)lr);
        } else {
            logf_("  xefgSwapChainSetLatencyReduction : SKIPPED (no XeLL context)\n");
        }

        xefgSwapChainDestroy(fgCtx);
        logf_("  xefg context destroyed\n");
    }

    if (xellCtx) {
        xellDestroyContext(xellCtx);
    }
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
    logf_("  M0.1  XeSS-FG / XeLL capability probe\n");
    logf_("============================================================================\n");

    logf_("\n== libraries ==\n");
    xefg_swapchain_version_t xv{};
    xefg_swapchain_result_t vr = xefgSwapChainGetVersion(&xv);
    if (vr == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf_("  libxess_fg : %u.%u.%u\n", xv.major, xv.minor, xv.patch);
    } else {
        logf_("  xefgSwapChainGetVersion : %s (%d)\n", xefg_result_name(vr), (int)vr);
    }

    xell_version_t lv{};
    xell_result_t lvr = xellGetVersion(&lv);
    if (lvr == XELL_RESULT_SUCCESS) {
        logf_("  libxell    : %u.%u.%u\n", lv.major, lv.minor, lv.patch);
    } else {
        logf_("  xellGetVersion : %s (%d)\n", xell_result_name(lvr), (int)lvr);
    }

    logf_("\n== DXGI adapters ==\n");
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        logf_("  CreateDXGIFactory2 FAILED hr=0x%08lX\n", (unsigned long)hr);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return 2;
    }

    unsigned hardware_count = 0;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) &&
            !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            ++hardware_count;
        }
        probe_adapter(adapter.Get(), i);
    }

    logf_("\n============================================================================\n");
    logf_(" How to read this result\n");
    logf_("   PASS requires, on the AMD adapter (vendor 0x1002):\n");
    logf_("     1) xellD3D12CreateContext              -> SUCCESS\n");
    logf_("     2) xefgSwapChainD3D12CreateContext     -> SUCCESS\n");
    logf_("     3) xefgSwapChainSetLatencyReduction    -> SUCCESS\n");
    logf_("   And on BOTH adapters:\n");
    logf_("     4) CrossAdapterRowMajorTextureSupported -> TRUE\n");
    logf_("   hardwares enumerated: %u\n", hardware_count);
    logf_("============================================================================\n");

    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}


