// ============================================================================
// xefg_dyn.h  -  runtime loader for libxess_fg.dll
//
// Why we no longer link the import library:
//
//   XeSS-FG's multi frame generation (MFG) is decided *inside the DLL*. The
//   build shipped with the official SDK (libxess_fg 1.3.1.78 here) has all the
//   MFG code, but on a non-Intel adapter four one-byte checks in this DLL plus
//   one in libxell keep it at one interpolated frame (2x):
//
//       "Expected maxInterpolatedFrames to be 1, got %u"
//       "Network model does not support requested multi frame generation"
//       "XeLL version {}.{}.{} is too old to support multi-frame generation,
//        falling back to 2x"
//
//   Those checks are what src/xefg_unlock.h lifts for --unlock-mfg. Measured on
//   the 6750 GRE: stock reports maxSupportedInterpolations = 1; after the patch
//   it reports 3 and builds 4x. So a different DLL is not required after all -
//   but the file still has to be selectable and reportable, which is why it is
//   loaded here instead of being linked in:
//     * pick the file (--fg-lib <path>), so another build can be tried without
//       overwriting ours;
//     * print the DLL path, its version and maxSupportedInterpolations before
//       anything is initialised, so "why is it still 2x?" is answerable;
//     * keep running (with the warp interpolator) when the DLL is absent.
//
//   OptiScaler solves the same problem the same way (XeFG_Proxy + XeFGPath, and
//   its [XeFG] UnlockMFG is the same byte patch).
//
// Everything here is a plain pointer table: no behaviour of its own.
// ============================================================================
#pragma once

#include <windows.h>

#include <string>

#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"

namespace xefg_dyn {

typedef decltype(&xefgSwapChainGetVersion)                 PFN_GetVersion;
typedef decltype(&xefgSwapChainGetProperties)              PFN_GetProperties;
typedef decltype(&xefgSwapChainSetLoggingCallback)         PFN_SetLoggingCallback;
typedef decltype(&xefgSwapChainSetLatencyReduction)        PFN_SetLatencyReduction;
typedef decltype(&xefgSwapChainSetEnabled)                 PFN_SetEnabled;
typedef decltype(&xefgSwapChainSetPresentId)               PFN_SetPresentId;
typedef decltype(&xefgSwapChainGetLastPresentStatus)       PFN_GetLastPresentStatus;
typedef decltype(&xefgSwapChainTagFrameConstants)          PFN_TagFrameConstants;
typedef decltype(&xefgSwapChainSetNumInterpolatedFrames)   PFN_SetNumInterpolatedFrames;
typedef decltype(&xefgSwapChainSetSceneChangeThreshold)    PFN_SetSceneChangeThreshold;
typedef decltype(&xefgSwapChainDestroy)                    PFN_Destroy;

typedef decltype(&xefgSwapChainD3D12CreateContext)         PFN_D3D12CreateContext;
typedef decltype(&xefgSwapChainD3D12GetProperties)         PFN_D3D12GetProperties;
typedef decltype(&xefgSwapChainD3D12InitFromSwapChain)     PFN_D3D12InitFromSwapChain;
typedef decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) PFN_D3D12InitFromSwapChainDesc;
typedef decltype(&xefgSwapChainD3D12GetSwapChainPtr)       PFN_D3D12GetSwapChainPtr;
typedef decltype(&xefgSwapChainD3D12TagFrameResource)      PFN_D3D12TagFrameResource;
typedef decltype(&xefgSwapChainD3D12GetInitializationParameters)
                                                          PFN_D3D12GetInitParams;
// Optional debug features (present-only-interpolation, corner tags on generated frames).
// OptiScaler uses both to make artifacts visible; see docs/findings/HANDOFF.md 8.6.
typedef decltype(&xefgSwapChainEnableDebugFeature)         PFN_EnableDebugFeature;

struct Api
{
    HMODULE      dll  = nullptr;
    std::wstring path;

    // Required: the pipeline cannot run without these.
    PFN_GetVersion                 GetVersion                 = nullptr;
    PFN_GetProperties              GetProperties              = nullptr;
    PFN_SetLoggingCallback         SetLoggingCallback         = nullptr;
    PFN_SetLatencyReduction        SetLatencyReduction        = nullptr;
    PFN_SetEnabled                 SetEnabled                 = nullptr;
    PFN_SetPresentId               SetPresentId               = nullptr;
    PFN_GetLastPresentStatus       GetLastPresentStatus       = nullptr;
    PFN_TagFrameConstants          TagFrameConstants          = nullptr;
    PFN_D3D12CreateContext         D3D12CreateContext         = nullptr;
    PFN_D3D12GetProperties         D3D12GetProperties         = nullptr;
    PFN_D3D12InitFromSwapChain     D3D12InitFromSwapChain     = nullptr;
    PFN_D3D12InitFromSwapChainDesc D3D12InitFromSwapChainDesc = nullptr;
    PFN_D3D12GetSwapChainPtr       D3D12GetSwapChainPtr       = nullptr;
    PFN_D3D12TagFrameResource      D3D12TagFrameResource      = nullptr;

    // Optional. The first one decides MFG: without it the count can only be set
    // through the init parameter (and never changed at runtime).
    PFN_SetNumInterpolatedFrames   SetNumInterpolatedFrames   = nullptr;
    PFN_D3D12GetInitParams         D3D12GetInitParams         = nullptr;
    PFN_SetSceneChangeThreshold    SetSceneChangeThreshold    = nullptr;
    PFN_Destroy                    Destroy                    = nullptr;
    PFN_EnableDebugFeature         EnableDebugFeature         = nullptr;

    bool has_mfg() const { return SetNumInterpolatedFrames != nullptr; }
    bool ok() const { return dll != nullptr; }
};


namespace detail {

template <typename T>
inline void resolve(HMODULE m, const char* name, T& out, std::string* missing)
{
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!out && missing) {
        if (!missing->empty()) { *missing += ", "; }
        *missing += name;
    }
}

inline std::wstring exe_dir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, (n < MAX_PATH) ? n : MAX_PATH);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : s.substr(0, slash + 1);
}

} // namespace detail

// Loads the DLL and resolves the table.
//
//   override_path - from --fg-lib; when set it is the only candidate.
//   err           - human readable reason on failure (may be nullptr).
//
// Returns true only when every required entry point was found.
inline bool Load(Api& api, const std::wstring& override_path, std::string* err)
{
    api = Api{};

    std::wstring tries[2];
    int n = 0;
    if (!override_path.empty()) {
        tries[n++] = override_path;
    } else {
        tries[n++] = detail::exe_dir() + L"libxess_fg.dll";
        tries[n++] = L"libxess_fg.dll";
    }

    DWORD last_error = 0;
    for (int i = 0; i < n && api.dll == nullptr; ++i) {
        if (tries[i].empty()) { continue; }
        // LOAD_WITH_ALTERED_SEARCH_PATH so a DLL picked from another folder finds
        // the libxell.dll next to *it*, not the one next to us.
        api.dll = LoadLibraryExW(tries[i].c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (api.dll) {
            api.path = tries[i];
        } else {
            last_error = GetLastError();
        }
    }

    if (!api.dll) {
        if (err) { *err = "LoadLibrary failed, GetLastError=" + std::to_string(last_error); }
        return false;
    }

    std::string missing;
    detail::resolve(api.dll, "xefgSwapChainGetVersion",                 api.GetVersion,                 &missing);
    detail::resolve(api.dll, "xefgSwapChainGetProperties",              api.GetProperties,              &missing);
    detail::resolve(api.dll, "xefgSwapChainSetLoggingCallback",         api.SetLoggingCallback,         &missing);
    detail::resolve(api.dll, "xefgSwapChainSetLatencyReduction",        api.SetLatencyReduction,        &missing);
    detail::resolve(api.dll, "xefgSwapChainSetEnabled",                 api.SetEnabled,                 &missing);
    detail::resolve(api.dll, "xefgSwapChainSetPresentId",               api.SetPresentId,               &missing);
    detail::resolve(api.dll, "xefgSwapChainGetLastPresentStatus",       api.GetLastPresentStatus,       &missing);
    detail::resolve(api.dll, "xefgSwapChainTagFrameConstants",          api.TagFrameConstants,          &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12CreateContext",         api.D3D12CreateContext,         &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12GetProperties",         api.D3D12GetProperties,         &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12InitFromSwapChain",     api.D3D12InitFromSwapChain,     &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12InitFromSwapChainDesc", api.D3D12InitFromSwapChainDesc, &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12GetSwapChainPtr",       api.D3D12GetSwapChainPtr,       &missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12TagFrameResource",      api.D3D12TagFrameResource,      &missing);

    // Optional: a missing one stays NULL and is simply never called.
    std::string optional_missing;
    detail::resolve(api.dll, "xefgSwapChainSetNumInterpolatedFrames",   api.SetNumInterpolatedFrames,   &optional_missing);
    detail::resolve(api.dll, "xefgSwapChainD3D12GetInitializationParameters",
                                                                       api.D3D12GetInitParams,         &optional_missing);
    detail::resolve(api.dll, "xefgSwapChainSetSceneChangeThreshold",    api.SetSceneChangeThreshold,    &optional_missing);
    detail::resolve(api.dll, "xefgSwapChainEnableDebugFeature",         api.EnableDebugFeature,         &optional_missing);
    detail::resolve(api.dll, "xefgSwapChainDestroy",                    api.Destroy,                    &optional_missing);

    if (!missing.empty()) {
        if (err) { *err = "missing exports: " + missing; }
        FreeLibrary(api.dll);
        api = Api{};
        return false;
    }
    return true;
}

} // namespace xefg_dyn
