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
#include "xess_fg/xefg_swapchain_debug.h"

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
typedef decltype(&xefgSwapChainSetUiCompositionState)      PFN_SetUiCompositionState;
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
    PFN_SetUiCompositionState      SetUiCompositionState      = nullptr;
    PFN_Destroy                    Destroy                    = nullptr;
    PFN_EnableDebugFeature         EnableDebugFeature         = nullptr;

    bool has_mfg() const { return SetNumInterpolatedFrames != nullptr; }
    bool has_ui_composition() const { return SetUiCompositionState != nullptr; }
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

// The error string ends up in a byte log (fprintf "%s"), so match the ANSI code page the
// rest of the log uses for "%ls" paths.
inline std::string narrow(const std::wstring& s)
{
    if (s.empty()) { return std::string(); }
    const int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return std::string(); }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n,
                        nullptr, nullptr);
    return out;
}

// Required entry points. A DLL that cannot provide all of them is not a runtime we can
// drive, and the reason (which names are missing) is what makes a field log answerable.
inline void resolve_required(HMODULE m, Api& api, std::string* missing)
{
    resolve(m, "xefgSwapChainGetVersion",                 api.GetVersion,                 missing);
    resolve(m, "xefgSwapChainGetProperties",              api.GetProperties,              missing);
    resolve(m, "xefgSwapChainSetLoggingCallback",         api.SetLoggingCallback,         missing);
    resolve(m, "xefgSwapChainSetLatencyReduction",        api.SetLatencyReduction,        missing);
    resolve(m, "xefgSwapChainSetEnabled",                 api.SetEnabled,                 missing);
    resolve(m, "xefgSwapChainSetPresentId",               api.SetPresentId,               missing);
    resolve(m, "xefgSwapChainGetLastPresentStatus",       api.GetLastPresentStatus,       missing);
    resolve(m, "xefgSwapChainTagFrameConstants",          api.TagFrameConstants,          missing);
    resolve(m, "xefgSwapChainD3D12CreateContext",         api.D3D12CreateContext,         missing);
    resolve(m, "xefgSwapChainD3D12GetProperties",         api.D3D12GetProperties,         missing);
    resolve(m, "xefgSwapChainD3D12InitFromSwapChain",     api.D3D12InitFromSwapChain,     missing);
    resolve(m, "xefgSwapChainD3D12InitFromSwapChainDesc", api.D3D12InitFromSwapChainDesc, missing);
    resolve(m, "xefgSwapChainD3D12GetSwapChainPtr",       api.D3D12GetSwapChainPtr,       missing);
    resolve(m, "xefgSwapChainD3D12TagFrameResource",      api.D3D12TagFrameResource,      missing);
}

// Optional: a missing one stays NULL and is simply never called.
inline void resolve_optional(HMODULE m, Api& api)
{
    std::string unused;
    resolve(m, "xefgSwapChainSetNumInterpolatedFrames",   api.SetNumInterpolatedFrames,   &unused);
    resolve(m, "xefgSwapChainD3D12GetInitializationParameters",
                                                          api.D3D12GetInitParams,         &unused);
    resolve(m, "xefgSwapChainSetSceneChangeThreshold",    api.SetSceneChangeThreshold,    &unused);
    resolve(m, "xefgSwapChainSetUiCompositionState",      api.SetUiCompositionState,      &unused);
    resolve(m, "xefgSwapChainEnableDebugFeature",         api.EnableDebugFeature,         &unused);
    resolve(m, "xefgSwapChainDestroy",                    api.Destroy,                    &unused);
}

} // namespace detail

// Loads the DLL and resolves the table.
//
//   override_path - from --fg-lib; when set it is the only candidate.
//   err           - human readable reason on failure (may be nullptr). On failure it
//                   names EVERY candidate that was tried and why it was rejected.
//
// Every candidate is tried, not just the first one that loads. A title can ship its own
// libxess_fg.dll built against a different XeSS SDK: LoadLibrary succeeds, but the
// required entry points are not all there (Cyberpunk 2077, 2026-11-20:
// "missing exports: xefgSwapChainD3D12GetProperties"). Stopping at that candidate hid
// the good copy our own "fakerFG\" folder was carrying, and the failure then cascaded
// into "producer never started" because producer_present() lives on XeFG's present
// detour.
//
// Returns true only when every required entry point was found.
inline bool Load(Api& api, const std::wstring& override_path, std::string* err)
{
    api = Api{};

    // Vendor DLLs live in our own folder first, exactly like OptiScaler's `streamline\`
    // (proxies/Streamline_Proxy.h:88-105): <exe>\fakerFG\libxess_fg.dll before the
    // title's copy, so an install does not depend on what the game ships where.
    std::wstring tries[3];
    int n = 0;
    if (!override_path.empty()) {
        tries[n++] = override_path;
    } else {
        tries[n++] = detail::exe_dir() + L"fakerFG\\libxess_fg.dll";
        tries[n++] = detail::exe_dir() + L"libxess_fg.dll";
        tries[n++] = L"libxess_fg.dll";
    }

    std::string problems;
    DWORD last_error = 0;
    auto note = [&problems](const std::string& s) {
        if (!problems.empty()) { problems += "; "; }
        problems += s;
    };

    for (int i = 0; i < n; ++i) {
        if (tries[i].empty()) { continue; }

        // LOAD_WITH_ALTERED_SEARCH_PATH so a DLL picked from another folder finds
        // the libxell.dll next to *it*, not the one next to us.
        HMODULE dll = LoadLibraryExW(tries[i].c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (dll == nullptr) {
            last_error = GetLastError();
            note(detail::narrow(tries[i]) + " -> LoadLibrary err=" + std::to_string(last_error));
            continue;
        }

        Api candidate{};
        std::string missing;
        detail::resolve_required(dll, candidate, &missing);
        if (!missing.empty()) {
            // Keep looking: our own folder may hold a runtime this title's build is
            // missing (that is the whole reason fakerFG\ comes first).
            note(detail::narrow(tries[i]) + " -> missing exports: " + missing);
            FreeLibrary(dll);
            continue;
        }

        detail::resolve_optional(dll, candidate);
        candidate.dll  = dll;
        candidate.path = tries[i];
        api = candidate;
        return true;
    }

    if (err) {
        *err = problems.empty()
                   ? ("LoadLibrary failed, GetLastError=" + std::to_string(last_error))
                   : problems;
    }
    return false;
}

} // namespace xefg_dyn
