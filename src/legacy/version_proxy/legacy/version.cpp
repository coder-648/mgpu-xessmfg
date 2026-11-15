// ============================================================================
//  version.dll proxy - arms the one thing that has to happen before slInit.
//
//  Why this exists (Wuchang / Streamline 2.x, 2026-11-10):
//  Streamline decides inside slInit whether the frame-generation plugin (sl.dlss_g)
//  is usable on this platform. sl.common compares every adapter's
//  NvAPI_GPU_GetArchInfo architecture against sl.dlss_g's minimum (AD100 = 0x190) and
//  evicts the plugin ("Ignoring plugin 'sl.dlss_g' ...") - before the D3D device
//  exists and long before any runtime loads. An add-on (ReShade .addon64) is loaded
//  after D3D init, so hooking NVAPI from there is measurably too late: our hook landed
//  7 s after sl.interposer had already initialised.
//
//  This DLL is that earlier injection point. It does exactly one thing:
//
//      nvapi64!nvapi_QueryInterface(NvAPI_GPU_GetArchInfo)
//          -> report 0x190 (Ada) instead of 0x170 (Ampere) to non-NVIDIA callers
//
//  It is NOT an enabler: it generates no frames and patches no NVIDIA code paths.
//  The capability answers and the stubbed feature live in the add-on (FgProvide=1).
//
//  Caller exclusion is load-bearing, not defensive coding: NVIDIA's own components must
//  keep seeing the real architecture. dlssg_sm86 learned this the hard way (issues
//  #535, #538, #540, #542): once the *super-resolution* snippet was spoofed it took an
//  arch>=0x180 kernel path and the GPU died with DXGI_ERROR_DEVICE_HUNG before frame
//  generation was even created. So _nvngx / nvngx_* / nvapi* / DriverStore / the NGX
//  model library (except models\sl_*, which *is* Streamline's own OTA plugin) all see
//  the truth.
// ============================================================================
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
#include <string.h>
#include <array>
#include <utility>

#include "MinHook.h"

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static HMODULE g_self = nullptr;
static bool g_flag_arch = true;
static bool g_flag_ngx  = true;
static bool g_flag_dxgi = true;
static bool g_flag_ver  = true;
static bool g_flag_verlog = false;

static bool proxy_flag(const wchar_t* key, bool def)
{
    wchar_t ini[MAX_PATH]{};
    if (g_self == nullptr || GetModuleFileNameW(g_self, ini, MAX_PATH) == 0) { return def; }
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (slash != nullptr) { *(slash + 1) = L'\0'; } else { ini[0] = L'\0'; }
    wcscat_s(ini, L"mgpu_proxy.ini");
    return GetPrivateProfileIntW(L"proxy", key, def ? 1 : 0, ini) != 0;
}

static CRITICAL_SECTION g_log_cs;
static volatile LONG g_log_cs_ready = 0;

static void plog(const char* fmt, ...)
{
    if (g_log == nullptr) { return; }
    if (InterlockedCompareExchange(&g_log_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_log_cs);
    }
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static void open_log(HMODULE self)
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) { return; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash != nullptr) { *slash = L'\0'; }
    wcscat_s(path, L"\\mgpu_proxy.log");
    char narrow[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    g_log = _fsopen(narrow, "a", _SH_DENYNO);   // readable while the game runs
}

// ------------------------------------------------------------ NVAPI hooking
typedef void* (WINAPI* pfn_query_interface)(unsigned int id);
typedef int   (WINAPI* pfn_get_arch_info)(void* gpu_handle, void* info);

static pfn_query_interface g_real_query_interface = nullptr;
static pfn_get_arch_info   g_real_get_arch_info   = nullptr;

static constexpr unsigned kIdGpuGetArchInfo = 0x2D3A1A3A;
static constexpr unsigned kArchInfoVersion  = 0x20010;   // what Streamline passes
static constexpr unsigned char kArchSpoofed = 0x190;     // AD10x (Ampere 0x170 -> 0x190)
// The *physical* arch, captured before spoofing. sm86 needs it for the runtime-side
// patch ("coreArch"): everything the NGX core compares against the real GPU has to
// answer with this, never with the spoofed value.
static unsigned g_phys_arch = 0;

static bool contains_ci(const wchar_t* hay, const wchar_t* needle)
{
    if (hay == nullptr || needle == nullptr || *needle == L'\0') { return false; }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, n) == 0) { return true; }
    }
    return false;
}

// True when the immediate caller is one of NVIDIA's own components, which must keep
// seeing the real architecture (see the file header).
static bool caller_is_nvidia()
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &mod) ||
        mod == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(mod, path, MAX_PATH) == 0) { return false; }

    if (contains_ci(path, L"\\nvngx")) { return true; }
    if (contains_ci(path, L"\\_nvngx")) { return true; }
    if (contains_ci(path, L"\\nvapi")) { return true; }
    if (contains_ci(path, L"drivestore")) { return true; }
    // The NGX model library: SR / ray-reconstruction snippets break if they believe the
    // card is newer. Streamline's own OTA plugin (models\sl_*) is the one exception -
    // that is the component we are trying to convince.
    if (contains_ci(path, L"\\nvidia\\ngx\\models\\") && !contains_ci(path, L"\\models\\sl_")) {
        return true;
    }
    return false;
}

static int WINAPI hook_get_arch_info(void* gpu, void* info)
{
    const int r = g_real_get_arch_info ? g_real_get_arch_info(gpu, info) : -1;
    if (!g_flag_arch) { return r; }
    if (r != 0 || info == nullptr) { return r; }

    auto* b = static_cast<unsigned char*>(info);
    unsigned ver = 0;
    memcpy(&ver, b, sizeof(ver));
    if (ver != kArchInfoVersion) { return r; }      // unknown layout: do not touch it

    if (g_phys_arch == 0) {
        g_phys_arch = static_cast<unsigned>(b[4]) | (static_cast<unsigned>(b[5]) << 8);
    }

    if (caller_is_nvidia()) {
        static bool logged_nv = false;
        if (!logged_nv) {
            logged_nv = true;
            plog("arch: real 0x%X kept for an NVIDIA component", b[4]);
        }
        return r;
    }
    if (b[4] < kArchSpoofed) {
        const unsigned was = b[4];
        b[4] = static_cast<unsigned char>(kArchSpoofed);
        static bool logged = false;
        if (!logged) {
            logged = true;
            plog("arch: reported 0x%X instead of 0x%X to the game/Streamline"
                 " (struct version 0x%X) - this is what keeps sl.dlss_g alive",
                 kArchSpoofed, was, ver);
        }
    }
    return r;
}

// ---- generic 4-arg wrapper for the NVAPI ids Streamline actually asks for ----------
// We do not need to know which id is "GetArchInfo": the NV_GPU_ARCH_INFO struct arrives
// as an argument and its first dword is the version (0x20010). Whoever fills it gets the
// spoofed architecture, and the log names the id - so the mapping is discovered instead
// of assumed (the published id 0x2D3A1A3A is NOT among the ids this Streamline build
// asks for; measured 2026-11-10 by sweeping nvapi_QueryInterface with callers).
typedef void* (WINAPI* pfn_generic4)(void*, void*, void*, void*);

struct WrappedId { unsigned id; pfn_generic4 real; };
static constexpr int kWrapSlots = 32;
static WrappedId g_wrapped[kWrapSlots]{};
static bool g_arch_logged = false;
static int g_wrapped_n = 0;

static bool try_patch_arch(void* arg)
{
    if (arg == nullptr) { return false; }
    __try {
        auto* b = static_cast<unsigned char*>(arg);
        unsigned ver = 0;
        memcpy(&ver, b, sizeof(ver));
        if (ver != kArchInfoVersion) { return false; }
        if (b[4] < kArchSpoofed) { b[4] = static_cast<unsigned char>(kArchSpoofed); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <int I>
static void* WINAPI wrap_generic(void* a, void* b, void* c, void* d)
{
    void* r = g_wrapped[I].real ? g_wrapped[I].real(a, b, c, d) : nullptr;
    const bool a_ok = try_patch_arch(a);
    const bool b_ok = try_patch_arch(b);
    if (a_ok || b_ok) {
        if (!g_arch_logged) {
            g_arch_logged = true;
            plog("arch: spoofed through NVAPI id 0x%08X (struct was arg%d) - this is the"
                 " architecture answer Streamline uses to drop sl.dlss_g",
                 g_wrapped[I].id, a_ok ? 1 : 2);
        }
    }
    return r;
}

template <std::size_t... Is>
static constexpr std::array<pfn_generic4, sizeof...(Is)> make_wrap4(std::index_sequence<Is...>)
{
    return { &wrap_generic<static_cast<int>(Is)>... };
}
static constexpr auto g_wrap_fns =
    make_wrap4(std::make_index_sequence<kWrapSlots>{});

// Second forwarder shape: six pointers. Every NVAPI entry point we have seen takes at
// most four arguments, but ``the game hands the id to us and we hand back a pointer''
// means the wrapper has to accept whatever the caller passes; six covers the rest of the
// GPU-query family without guessing per function.
typedef void* (WINAPI* pfn_generic6)(void*, void*, void*, void*, void*, void*);
static pfn_generic6 g_wrapped6[kWrapSlots]{};

template <int I>
static void* WINAPI wrap_generic6(void* a, void* b, void* c, void* d, void* e, void* f)
{
    void* r = g_wrapped6[I] ? g_wrapped6[I](a, b, c, d, e, f) : nullptr;
    const bool a_ok = try_patch_arch(a);
    const bool b_ok = try_patch_arch(b);
    if ((a_ok || b_ok) && !g_arch_logged) {
        g_arch_logged = true;
        plog("arch: spoofed through NVAPI id 0x%08X (struct in arg%d)",
             g_wrapped[I].id, a_ok ? 1 : 2);
    }
    return r;
}

template <std::size_t... Is>
static constexpr std::array<pfn_generic6, sizeof...(Is)> make_wrap6(std::index_sequence<Is...>)
{
    return { &wrap_generic6<static_cast<int>(Is)>... };
}
static constexpr auto g_wrap6_fns =
    make_wrap6(std::make_index_sequence<kWrapSlots>{});

// The ids Streamline asks for (observed), plus the published arch id in case another
// title uses it. All of them take at most four arguments, so a 4-slot forwarder is safe.
static bool id_is_wrappable(unsigned id)
{
    // The only entry point we have actually verified: the one whose second argument is an
    // NV_GPU_ARCH_INFO (version 0x20010). Wrapping anything else means guessing a
    // signature, and a wrong guess corrupts the caller's registers/stack.
    return id == kIdGpuGetArchInfo || id == 0xD8265D24u;   // published + this NVAPI's real id
}

static void* WINAPI hook_query_interface(unsigned int id)
{
    // Diagnostic sweep: which ids does this title ask for at all? Cheap (bounded) and it
    // is the only way to find the arch query when the published id is not the one used.
    {
        static unsigned seen[64]{};
        static int seen_n = 0;
        bool known = false;
        for (int i = 0; i < seen_n; ++i) { if (seen[i] == id) { known = true; break; } }
        if (!known && seen_n < 64) {
            seen[seen_n++] = id;
            // Which module asks matters as much as which id: Streamline's own log string
            // names NvAPI_GPU_GetArchInfo, but this build does not embed the published id,
            // so the caller is how we find it.
            char who[MAX_PATH]{0};
            HMODULE mod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(_ReturnAddress()), &mod) &&
                mod != nullptr) {
                wchar_t wp[MAX_PATH]{};
                if (GetModuleFileNameW(mod, wp, MAX_PATH)) {
                    wchar_t* slash = wcsrchr(wp, L'\\');
                    WideCharToMultiByte(CP_ACP, 0, slash ? slash + 1 : wp, -1, who,
                                        sizeof(who) - 1, nullptr, nullptr);
                }
            }
            plog("qid: 0x%08X  from %s", id, who[0] ? who : "?");
        }
    }
    void* fn = g_real_query_interface ? g_real_query_interface(id) : nullptr;
    // Wrap the ids Streamline asks for, so the architecture struct is patched wherever it
    // is filled - the id is discovered from the log instead of assumed.
    // MGPU_PROXY_WRAP_STREAMLINE=1 wraps *every* id Streamline asks for. Measured
    // 2026-11-11: with it on, the title exits within seconds at startup - the 6-arg
    // forwarder is not safe for every entry point - so it is off by default.
    // ("wrap everything Streamline asks for" was tried and removed: it wrapped ids whose
    // signatures we had not verified, and a wrong forward corrupts the caller - CP2077
    // died reading 0xFFFFFFFF right after asking for 0x33C7358C.)
    const bool wrap_this = g_flag_arch && id_is_wrappable(id);
    if (fn != nullptr && wrap_this) {
        for (int i = 0; i < g_wrapped_n; ++i) {
            if (g_wrapped[i].id == id) {
                if (id_is_wrappable(id)) { return reinterpret_cast<void*>(g_wrap_fns[i]); }
                return reinterpret_cast<void*>(g_wrap6_fns[i]);
            }
        }
        if (g_wrapped_n < kWrapSlots) {
            const int i = g_wrapped_n++;
            g_wrapped[i].id = id;
            g_wrapped[i].real = reinterpret_cast<pfn_generic4>(fn);
            g_wrapped6[i] = reinterpret_cast<pfn_generic6>(fn);
            plog("wrap: id 0x%08X wrapped (%s)", id,
                 id_is_wrappable(id) ? "known 4-arg" : "caller=Streamline, 6-arg");
            if (id_is_wrappable(id)) { return reinterpret_cast<void*>(g_wrap_fns[i]); }
            return reinterpret_cast<void*>(g_wrap6_fns[i]);
        }
    }
    if (id == kIdGpuGetArchInfo && fn != nullptr) {
        if (g_real_get_arch_info == nullptr) {
            g_real_get_arch_info = reinterpret_cast<pfn_get_arch_info>(fn);
            plog("arch: NvAPI_GPU_GetArchInfo resolved @ %p", fn);
        }
        return reinterpret_cast<void*>(&hook_get_arch_info);
    }
    return fn;
}

static void hook_nvapi(HMODULE m)
{
    FARPROC qi = GetProcAddress(m, "nvapi_QueryInterface");
    if (qi == nullptr) {
        plog("arch: nvapi_QueryInterface not exported - nothing hooked");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(qi),
                      reinterpret_cast<LPVOID>(&hook_query_interface),
                      reinterpret_cast<LPVOID*>(&g_real_query_interface)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<LPVOID>(qi)) != MH_OK) {
        plog("arch: MinHook could not patch nvapi_QueryInterface");
        return;
    }
    plog("arch: nvapi_QueryInterface hooked @ %p (nvapi64 = %p)", qi, m);
}

// ---- NGX capability answers, armed as early as the architecture spoof --------------
// The last gate is timing, exactly like the architecture one: UE's DLSS-G plugin decides
// "hardware not supported" during its own init and caches it, and the add-on's hooks are
// installed too late to be asked (measured 2026-11-11: nine Streamline hooks live, not one
// capability/requirement call reached them). So the same answers are given from here, as
// soon as the NGX modules appear:
//
//   NVSDK_NGX_D3D12_GetCapabilityParameters -> FrameGeneration.Available=1, MFG max=1
//   NVSDK_NGX_D3D12_GetFeatureRequirements   -> supported, min arch 0x170
//
// The *feature itself* is still stubbed by the add-on (FgProvide=1): creation happens
// after init, when the add-on is already live.
typedef int (WINAPI* pfn_ngx_cap)(void** out_params);
typedef int (WINAPI* pfn_ngx_req)(void* adapter, const void* info, void* out_requirement);
typedef void (WINAPI* pfn_ngx_set_i)(void* params, const char* name, int value);

static pfn_ngx_cap g_real_ngx_cap = nullptr;
static pfn_ngx_req g_real_ngx_req = nullptr;
static pfn_ngx_set_i g_ngx_set_i = nullptr;
static bool g_ngx_logged_cap = false;
static bool g_ngx_logged_req = false;
static bool g_ngx_logged_req_detail = false;
static bool g_ngx_plugin_hooked = false;   // nvngx_dlssg.dll (the module UE asks)

static int WINAPI hook_ngx_cap(void** out_params)
{
    const int r = g_real_ngx_cap ? g_real_ngx_cap(out_params) : 0;
    if (!g_flag_ngx) { return r; }
    if (out_params != nullptr && *out_params != nullptr && g_ngx_set_i != nullptr) {
        if (!g_ngx_logged_cap) {
            g_ngx_logged_cap = true;
            plog("ngx: GetCapabilityParameters seen - answering FrameGeneration.Available=1,"
                 " MultiFrameCountMax=1");
        }
        g_ngx_set_i(*out_params, "FrameGeneration.Available", 1);
        g_ngx_set_i(*out_params, "FrameGeneration.NeedsUpdatedDriver", 0);
        g_ngx_set_i(*out_params, "DLSSG.MultiFrameCountMax", 1);
        // The one we were missing: the core sets FeatureInitResult to 0xbad0000b
        // (FAIL_OutOfDate) because the runtime declares a minimum architecture
        // (hardcoded 0x190 in nvngx_dlssg) that is newer than the physical GPU it read
        // itself. Available=1 alone is not enough - a host that reads this field still
        // concludes "hardware not supported" (dlssg_sm86 ARCHITECTURE.md 633/637: the
        // comparison happens during the core's own init, so no NVAPI spoof reaches it).
        g_ngx_set_i(*out_params, "FrameGeneration.FeatureInitResult", 1);
        g_ngx_set_i(*out_params, "FrameInterpolation.Available", 1);
        g_ngx_set_i(*out_params, "FrameInterpolation.FeatureInitResult", 1);
    }
    return r;
}

static int WINAPI hook_ngx_req(void* adapter, const void* info, void* out_requirement)
{
    const int r = g_real_ngx_req ? g_real_ngx_req(adapter, info, out_requirement) : 0;
    if (!g_flag_ngx) { return r; }
    if (info != nullptr && out_requirement != nullptr) {
        __try {
            const int feature = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(info) + 4);
            if (feature == 11) {           // NVSDK_NGX_Feature_FrameGeneration
                auto* words = static_cast<unsigned*>(out_requirement);
                const unsigned was_supported = words[0];
                const unsigned was_arch      = words[1];
                words[0] = 0;              // FeatureSupported: 0 = supported
                words[1] = 0x170;          // MinHWArchitecture: sm_80 (what we claim)
                if (!g_ngx_logged_req_detail) {
                    g_ngx_logged_req_detail = true;
                    plog("ngx: requirements were supported=%u arch=0x%X -> 0, 0x170",
                         was_supported, was_arch);
                }
                if (!g_ngx_logged_req) {
                    g_ngx_logged_req = true;
                    plog("ngx: GetFeatureRequirements(feature=11) seen - answering SUPPORTED");
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

typedef int (WINAPI* pfn_ngx_init)(int app_id, const wchar_t* data_path, const void* common,
                                   unsigned sdk_version);
// NVSDK_NGX_D3D12_Init_with_ProjectID(projectId, engineType, engineVersion, dataPath,
//                                     featureCommonInfo, sdkVersion, appId)
// The extra arguments are forwarded untouched; only the result is inspected/forced.
typedef int (WINAPI* pfn_ngx_init_pid)(const char*, int, const char*, const wchar_t*,
                                       const void*, unsigned, int);
static pfn_ngx_init g_real_ngx_init = nullptr;
static pfn_ngx_init_pid g_real_ngx_init_pid = nullptr;
static int WINAPI hook_ngx_init_pid(const char* project_id, int engine_type,
                                    const char* engine_version, const wchar_t* data_path,
                                    const void* common, unsigned sdk_version, int app_id)
{
    const int r = g_real_ngx_init_pid
                      ? g_real_ngx_init_pid(project_id, engine_type, engine_version,
                                            data_path, common, sdk_version, app_id)
                      : 0;
    static int logged = -999;
    if (logged != r) {
        logged = r;
        plog("ngx: Init_with_ProjectID(%.32s, appid 0x%X) = 0x%X%s", project_id ? project_id : "?",
             app_id, r, (r != 1) ? "  -> forcing Success" : "");
    }
    return 1;
}


static int WINAPI hook_ngx_init(int app_id, const wchar_t* data_path, const void* common,
                                unsigned sdk_version)
{
    const int r = g_real_ngx_init ? g_real_ngx_init(app_id, data_path, common, sdk_version) : 0;
    static int logged = -999;
    if (logged != r) {
        logged = r;
        plog("ngx: Init(0x%X) = 0x%X%s", app_id, r,
             (r != 1) ? "  -> forcing Success (the plugin caches this as 'FG unsupported')" : "");
    }
    // 0x1 = NVSDK_NGX_Result_Success. A failure here is exactly what makes the DLSS-G
    // plugin report "hardware not supported" and never create the feature.
    return 1;
}

static void hook_ngx_module(HMODULE m)
{
    if (!g_flag_ngx) { return; }
    FARPROC cap = GetProcAddress(m, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    FARPROC req = GetProcAddress(m, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    if (cap == nullptr && req == nullptr) { return; }   // nothing of ours here
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(m, path, MAX_PATH);
        const bool is_plugin = contains_ci(path, L"nvngx_dlssg");
        if (is_plugin) { g_ngx_plugin_hooked = true; }
        static const wchar_t* last_path = nullptr;
        (void)last_path;
        plog("ngx: arming %ls%s", wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path,
             is_plugin ? "  (the DLSS-G plugin module - what UE asks)" : "");
    }
    // The setter lives next to the getter in the same NGX module.
    if (g_ngx_set_i == nullptr) {
        g_ngx_set_i = reinterpret_cast<pfn_ngx_set_i>(
            GetProcAddress(m, "NVSDK_NGX_Parameter_SetI"));
    }
    {
        FARPROC fn = GetProcAddress(m, "NVSDK_NGX_D3D12_Init");
        if (fn != nullptr && g_real_ngx_init == nullptr &&
            MH_CreateHook(reinterpret_cast<LPVOID>(fn),
                          reinterpret_cast<LPVOID>(&hook_ngx_init),
                          reinterpret_cast<LPVOID*>(&g_real_ngx_init)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(fn)) == MH_OK) {
            plog("ngx: NVSDK_NGX_D3D12_Init hooked on %p", fn);
        }
        FARPROC fp = GetProcAddress(m, "NVSDK_NGX_D3D12_Init_with_ProjectID");
        if (fp != nullptr && g_real_ngx_init_pid == nullptr &&
            MH_CreateHook(reinterpret_cast<LPVOID>(fp),
                          reinterpret_cast<LPVOID>(&hook_ngx_init_pid),
                          reinterpret_cast<LPVOID*>(&g_real_ngx_init_pid)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(fp)) == MH_OK) {
            plog("ngx: NVSDK_NGX_D3D12_Init_with_ProjectID hooked on %p", fp);
        }
    }
    if (cap != nullptr && g_real_ngx_cap == nullptr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(cap),
                          reinterpret_cast<LPVOID>(&hook_ngx_cap),
                          reinterpret_cast<LPVOID*>(&g_real_ngx_cap)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(cap)) == MH_OK) {
            plog("ngx: capability hook armed on %p", cap);
        }
    }
    if (req != nullptr && g_real_ngx_req == nullptr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(req),
                          reinterpret_cast<LPVOID>(&hook_ngx_req),
                          reinterpret_cast<LPVOID*>(&g_real_ngx_req)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(req)) == MH_OK) {
            plog("ngx: requirements hook armed on %p", req);
        }
    }
}

// The whole point of this DLL is timing: slInit runs before any add-on exists, so the

// arming thread starts at DLL_PROCESS_ATTACH and waits for nvapi64 itself.
// Watches sl.dlss_g without touching the loader. Hooking LoadLibraryW from the arming
// thread killed this title within seconds (measured), so presence is polled instead: the
// transition itself is the answer to "did Streamline try and give up, or never try".
static DWORD WINAPI watch_thread_unused(LPVOID)
{
    bool seen = false;
    for (int i = 0; i < 1800; ++i) {            // ~3 min
        const bool now = GetModuleHandleW(L"sl.dlss_g.dll") != nullptr;
        if (now != seen) {
            seen = now;
            plog("watch: sl.dlss_g.dll %s", now ? "LOADED" : "absent");
        }
        Sleep(100);
    }
    return 0;
}

static void hook_loader();   // defined with the module-load trace at the end

// ---- DXGI adapter description -------------------------------------------------------
// The last place a host can learn "which GPU is this" without calling an API we already
// answer: DXGI_ADAPTER_DESC. Wuchang refuses DLSS-G without asking NGX, Streamline or
// NVAPI (all three traced with callers), so the verdict must come from the adapter the
// host sees. Adapters are adopted wherever they appear, because the game may create its
// factory before this DLL is even loaded.
static HRESULT WINAPI hook_adapter_get_desc(void* self, DXGI_ADAPTER_DESC* desc);
static HRESULT WINAPI hook_adapter_get_desc1(void* self, DXGI_ADAPTER_DESC1* desc);

static const int kIdxGetDesc  = 8;
static const int kIdxGetDesc1 = 10;
static const int kVtSlots     = 24;

static void** g_adapter_vt = nullptr;
static void*  g_adapter_orig[kVtSlots]{};
static bool   g_dxgi_logged = false;

static bool patch_vtable_slot(void** vt, int slot, void* replacement, void** saved)
{
    if (vt == nullptr || slot < 0 || slot > 64 || replacement == nullptr) { return false; }
    if (*saved != nullptr) { return true; }              // already patched
    DWORD old_protect = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
    }
    *saved = vt[slot];
    vt[slot] = replacement;
    VirtualProtect(&vt[slot], sizeof(void*), old_protect, &old_protect);
    return true;
}

static void adopt_adapter(void* adapter)
{
    if (adapter == nullptr || g_adapter_vt != nullptr) { return; }
    void** vt = *reinterpret_cast<void***>(adapter);
    if (vt == nullptr) { return; }
    g_adapter_vt = vt;
    const bool a = patch_vtable_slot(vt, kIdxGetDesc,  reinterpret_cast<void*>(&hook_adapter_get_desc),
                                     &g_adapter_orig[kIdxGetDesc]);
    const bool b = patch_vtable_slot(vt, kIdxGetDesc1, reinterpret_cast<void*>(&hook_adapter_get_desc1),
                                     &g_adapter_orig[kIdxGetDesc1]);
    plog("dxgi: adapter vtable patched in place (GetDesc=%s, GetDesc1=%s)",
         a ? "ok" : "not yet", b ? "ok" : "not yet");
}

static HRESULT WINAPI hook_adapter_get_desc(void* self, DXGI_ADAPTER_DESC* desc)
{
    typedef HRESULT (WINAPI* fn)(void*, DXGI_ADAPTER_DESC*);
    auto real = reinterpret_cast<fn>(g_adapter_orig[kIdxGetDesc]);
    const HRESULT hr = real ? real(self, desc) : E_FAIL;
    if (SUCCEEDED(hr) && desc != nullptr) {
        if (!g_dxgi_logged) {
            g_dxgi_logged = true;
            char narrow[160]{};
            WideCharToMultiByte(CP_ACP, 0, desc->Description, -1, narrow, 159, nullptr, nullptr);
            plog("dxgi: adapter desc: \"%s\" vendor=0x%04X device=0x%04X subsys=0x%08X rev=%u"
                 " vram=%llu MB -> spoofing name + device id",
                 narrow, desc->VendorId, desc->DeviceId, desc->SubSysId, desc->Revision,
                 static_cast<unsigned long long>(desc->DedicatedVideoMemory >> 20));
        }
        wcscpy_s(desc->Description, L"NVIDIA GeForce RTX 5090");
        desc->DeviceId = 0x2B85;        // GB202 (RTX 5090)
        desc->SubSysId = 0;
        desc->Revision = 0;
    }
    return hr;
}

static HRESULT WINAPI hook_adapter_get_desc1(void* self, DXGI_ADAPTER_DESC1* desc)
{
    typedef HRESULT (WINAPI* fn)(void*, DXGI_ADAPTER_DESC1*);
    auto real = reinterpret_cast<fn>(g_adapter_orig[kIdxGetDesc1]);
    const HRESULT hr = real ? real(self, desc) : E_FAIL;
    if (SUCCEEDED(hr) && desc != nullptr) {
        if (!g_dxgi_logged) {
            g_dxgi_logged = true;
            char narrow[160]{};
            WideCharToMultiByte(CP_ACP, 0, desc->Description, -1, narrow, 159, nullptr, nullptr);
            plog("dxgi: adapter desc1: \"%s\" vendor=0x%04X device=0x%04X flags=0x%X"
                 " vram=%llu MB -> spoofing name + device id",
                 narrow, desc->VendorId, desc->DeviceId, desc->Flags,
                 static_cast<unsigned long long>(desc->DedicatedVideoMemory >> 20));
        }
        wcscpy_s(desc->Description, L"NVIDIA GeForce RTX 5090");
        desc->DeviceId = 0x2B85;        // GB202 (RTX 5090)
        desc->SubSysId = 0;
        desc->Revision = 0;
    }
    return hr;
}

// Factory side: every adapter we are handed gets adopted, so a factory created before this
// DLL loaded is still covered as soon as anything enumerates it.
static HRESULT WINAPI hook_factory_enum_adapters(void* self, UINT index, void** out);
static HRESULT WINAPI hook_factory_enum_adapters1(void* self, UINT index, void** out);
static const int kIdxEnumAdapters  = 7;
static const int kIdxEnumAdapters1 = 12;
static void** g_factory_vt = nullptr;
static void*  g_factory_orig[kVtSlots]{};

static void adopt_factory(void* factory)
{
    if (factory == nullptr || g_factory_vt != nullptr) { return; }
    void** vt = *reinterpret_cast<void***>(factory);
    if (vt == nullptr) { return; }
    g_factory_vt = vt;
    const bool a = patch_vtable_slot(vt, kIdxEnumAdapters,
                                     reinterpret_cast<void*>(&hook_factory_enum_adapters),
                                     &g_factory_orig[kIdxEnumAdapters]);
    const bool b = patch_vtable_slot(vt, kIdxEnumAdapters1,
                                     reinterpret_cast<void*>(&hook_factory_enum_adapters1),
                                     &g_factory_orig[kIdxEnumAdapters1]);
    plog("dxgi: factory vtable patched in place (EnumAdapters=%s, EnumAdapters1=%s)",
         a ? "ok" : "not yet", b ? "ok" : "not yet");
}

static HRESULT WINAPI hook_factory_enum_adapters(void* self, UINT index, void** out)
{
    typedef HRESULT (WINAPI* fn)(void*, UINT, void**);
    auto real = reinterpret_cast<fn>(g_factory_orig[kIdxEnumAdapters]);
    const HRESULT hr = real ? real(self, index, out) : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr) { adopt_adapter(*out); }
    return hr;
}

static HRESULT WINAPI hook_factory_enum_adapters1(void* self, UINT index, void** out)
{
    typedef HRESULT (WINAPI* fn)(void*, UINT, void**);
    auto real = reinterpret_cast<fn>(g_factory_orig[kIdxEnumAdapters1]);
    const HRESULT hr = real ? real(self, index, out) : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr) { adopt_adapter(*out); }
    return hr;
}

typedef HRESULT (WINAPI* pfn_cf_t)(REFIID, void**);
typedef HRESULT (WINAPI* pfn_cf2_t)(UINT, REFIID, void**);
static pfn_cf_t  g_real_cf  = nullptr;
static pfn_cf_t  g_real_cf1 = nullptr;
static pfn_cf2_t g_real_cf2 = nullptr;

static HRESULT WINAPI hook_cf(REFIID iid, void** factory)
{
    const HRESULT hr = g_real_cf ? g_real_cf(iid, factory) : E_FAIL;
    if (SUCCEEDED(hr) && factory != nullptr) { adopt_factory(*factory); }
    return hr;
}

static HRESULT WINAPI hook_cf1(REFIID iid, void** factory)
{
    const HRESULT hr = g_real_cf1 ? g_real_cf1(iid, factory) : E_FAIL;
    if (SUCCEEDED(hr) && factory != nullptr) { adopt_factory(*factory); }
    return hr;
}

static HRESULT WINAPI hook_cf2(UINT flags, REFIID iid, void** factory)
{
    const HRESULT hr = g_real_cf2 ? g_real_cf2(flags, iid, factory) : E_FAIL;
    if (SUCCEEDED(hr) && factory != nullptr) { adopt_factory(*factory); }
    return hr;
}

static void install_dxgi_hooks()
{
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr) { dxgi = LoadLibraryW(L"dxgi.dll"); }
    if (dxgi == nullptr) { plog("dxgi: not loadable - adapter description left alone"); return; }
    struct T { const char* name; void* hook; void** orig; };
    const T targets[] = {
        { "CreateDXGIFactory",  reinterpret_cast<void*>(&hook_cf),
          reinterpret_cast<void**>(&g_real_cf) },
        { "CreateDXGIFactory1", reinterpret_cast<void*>(&hook_cf1),
          reinterpret_cast<void**>(&g_real_cf1) },
        { "CreateDXGIFactory2", reinterpret_cast<void*>(&hook_cf2),
          reinterpret_cast<void**>(&g_real_cf2) },
    };
    for (const auto& t : targets) {
        FARPROC fn = GetProcAddress(dxgi, t.name);
        if (fn == nullptr || *t.orig != nullptr) { continue; }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), t.hook,
                          reinterpret_cast<LPVOID*>(t.orig)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(fn)) == MH_OK) {
            plog("dxgi: %s hooked on %p", t.name, fn);
        }
    }
}


// ---- A. answer the GetProcAddress(sl.common makes for nvapi_QueryInterface) ----------
// sm86 redirects nvapi64.dll's export table from its loader hook, i.e. before
// sl.common's LoadLibrary("nvapi64.dll") returns. Polling for the module cannot win that
// window (measured: our hook landed after slInit had already asked). Answering the very
// GetProcAddress call is the same moment, needs no loader hook and touches no NVIDIA code.
typedef FARPROC (WINAPI* pfn_get_proc)(HMODULE, LPCSTR);
static pfn_get_proc g_real_get_proc = nullptr;

static FARPROC WINAPI hook_get_proc(HMODULE mod, LPCSTR name)
{
    if (g_flag_arch && mod != nullptr && name != nullptr &&
        strcmp(name, "nvapi_QueryInterface") == 0) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(mod, path, MAX_PATH) != 0 && contains_ci(path, L"nvapi")) {
            if (g_real_query_interface == nullptr && g_real_get_proc != nullptr) {
                g_real_query_interface = reinterpret_cast<pfn_query_interface>(
                    g_real_get_proc(mod, name));
                plog("arch: nvapi_QueryInterface resolved through GetProcAddress -"
                     " answering with our wrapper (no load race)");
            }
            return reinterpret_cast<FARPROC>(&hook_query_interface);
        }
    }
    return g_real_get_proc ? g_real_get_proc(mod, name) : nullptr;
}

static void install_get_proc_hook()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) { return; }
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(k32, "GetProcAddress"));
    if (addr == nullptr || g_real_get_proc != nullptr) { return; }
    if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_get_proc),
                      reinterpret_cast<LPVOID*>(&g_real_get_proc)) == MH_OK &&
        MH_EnableHook(addr) == MH_OK) {
        plog("arch: GetProcAddress hooked @ %p (nvapi lookups answered by us)", addr);
    }
}


// ---- B. the runtime's hardcoded minimum architecture -------------------------------
// dlssg_sm86's ARCHITECTURE.md (633/637): nvngx_dlssg declares a minimum architecture as
// a constant - `mov eax,0x190; ret` (310.9.1: RVA 0x13eb0). The NGX core compares that
// value with the physical GPU it reads itself, during slInit, before any external hook can
// answer: 0x190 vs Ampere's 0x170 -> FeatureInitResult=0xbad0000b -> Available=0, and
// Streamline never calls CreateFeature. Answering capability params is too late for that
// comparison, so - exactly like sm86 - the constant itself is lowered to the physical
// architecture. One 5-byte immediate, no code injection, reversible by restarting.
static int patch_runtime_min_arch(HMODULE mod)
{
    if (!g_flag_arch || mod == nullptr || g_phys_arch == 0) { return 0; }

    auto* base = reinterpret_cast<unsigned char*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return 0; }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return 0; }

    const unsigned char want_lo = static_cast<unsigned char>(g_phys_arch & 0xFF);
    const unsigned char want_hi = static_cast<unsigned char>((g_phys_arch >> 8) & 0xFF);
    static const unsigned char kCandidates[][2] = { { 0x90, 0x01 }, { 0xB0, 0x01 }, { 0xC0, 0x01 } };
    int patched = 0;

    for (int sec = 0; sec < nt->FileHeader.NumberOfSections; ++sec) {
        auto* sh = IMAGE_FIRST_SECTION(nt) + sec;
        if ((sh->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) { continue; }
        auto* start = base + sh->VirtualAddress;
        const size_t size = sh->Misc.VirtualSize;
        for (size_t i = 0; i + 6 <= size; ++i) {
            if (start[i] != 0xB8 || start[i + 5] != 0xC3) { continue; }   // mov eax,imm32; ret
            bool candidate = false;
            for (const auto& c : kCandidates) {
                if (start[i + 1] == c[0] && start[i + 2] == c[1] && start[i + 3] == 0 &&
                    start[i + 4] == 0) {
                    candidate = true;
                }
            }
            if (!candidate) { continue; }
            const unsigned was_lo = start[i + 1];
            const unsigned was_hi = start[i + 2];
            DWORD old_protect = 0;
            if (!VirtualProtect(start + i, 6, PAGE_EXECUTE_READWRITE, &old_protect)) { continue; }
            start[i + 1] = want_lo;
            start[i + 2] = want_hi;
            VirtualProtect(start + i, 6, old_protect, &old_protect);
            ++patched;
            plog("arch: runtime minimum-arch lowered to 0x%X at %p (was 0x%X)",
                 g_phys_arch, start + i, was_lo | (was_hi << 8));
        }
    }
    return patched;
}

static DWORD WINAPI arm_thread(LPVOID)
{
    plog("proxy loaded - arming for nvapi64");
    if (MH_Initialize() != MH_OK) { plog("MinHook init failed"); return 0; }
    g_flag_arch = proxy_flag(L"ArchSpoof", true);
    g_flag_ngx  = proxy_flag(L"NgxSpoof",  true);
    g_flag_dxgi = proxy_flag(L"DxgiSpoof", true);
    g_flag_ver  = proxy_flag(L"VerSpoof",  true);
    g_flag_verlog = proxy_flag(L"VerLog", false);
    plog("switches: ArchSpoof=%d NgxSpoof=%d DxgiSpoof=%d VerSpoof=%d (mgpu_proxy.ini)",
         g_flag_arch, g_flag_ngx, g_flag_dxgi, g_flag_ver);

    // DXGI first: the adapter description is the one input a host can read without asking
    // NGX, Streamline or NVAPI (Wuchang's whole DLSS-G verdict comes from it).
    if (proxy_flag(L"GpaHook", false)) { install_get_proc_hook(); }
    if (g_flag_dxgi) { install_dxgi_hooks(); } else { plog("dxgi: spoof disabled by DxgiSpoof=0"); }

    // Pre-load the NGX core ourselves and hook it *before* anyone can ask it anything.
    // Polling for a module that the game loads and immediately queries loses that race
    // (measured on Wuchang: the "hardware not supported" verdict is cached at init and no
    // later call ever reaches our hooks). Deliberately no loader hook: hooking
    // LoadLibraryW from this thread killed the title within seconds on 2026-11-11.
    if (!g_flag_ngx) {
        plog("ngx: spoof disabled by NgxSpoof=0 (no pre-load, no hooks)");
    } else {
        const bool already = GetModuleHandleW(L"_nvngx.dll") != nullptr;
        HMODULE core = LoadLibraryW(L"_nvngx.dll");
        if (core != nullptr) {
            hook_ngx_module(core);
            plog("ngx: _nvngx.dll pre-loaded and hooked (%s)",
                 already ? "was already loaded" : "loaded by us, before the game asked");
        } else {
            plog("ngx: _nvngx.dll not loadable here (no driver NGX?)");
        }
    }
    // (LoadLibraryW tracing removed: hooking the loader this early crashed the title.)
    // nvapi64.dll is a thin shell: nvapi_QueryInterface is a forwarder there, and a
    // patched forwarder is never executed because the loader resolves the import straight
    // into nvapi64_impl.dll. So hook the implementation, and the shell too if it has a
    // real body (older layouts do).
    const wchar_t* names[] = { L"nvapi64_impl.dll", L"nvapi64.dll", L"nvapi.dll" };
    bool hooked_impl = false, hooked_shell = false;
    for (int i = 0; i < 9000; ++i) {                // up to ~90 s at 10 ms
        for (int k = 0; k < 3; ++k) {
            HMODULE m = GetModuleHandleW(names[k]);
            if (m == nullptr) { continue; }
            if (k == 0 && hooked_impl) { continue; }
            if (k != 0 && hooked_shell) { continue; }
            hook_nvapi(m);
            if (k == 0) { hooked_impl = true; } else { hooked_shell = true; }
        }
        // NGX modules appear around the same time: hook their capability answers the
        // moment they exist (this is the "hardware not supported" gate).
        for (const wchar_t* ngx : { L"_nvngx.dll", L"nvngx_dlssg.dll" }) {
            HMODULE g = GetModuleHandleW(ngx);
            if (g != nullptr) {
                hook_ngx_module(g);
                static bool scanned_dlssg = false;
                if (!scanned_dlssg && wcsstr(ngx, L"dlssg") != nullptr && g_phys_arch != 0) {
                    scanned_dlssg = true;          // one scan, whatever it finds
                    const int n = patch_runtime_min_arch(g);
                    plog("arch: runtime min-arch scan: %d site(s) patched", n);
                }
            }
        }
        if (hooked_impl && (g_ngx_plugin_hooked || !g_flag_ngx)) { return 0; }
        Sleep(10);
    }
    plog("arch: nvapi64_impl.dll never appeared (shell hooked: %d)", hooked_shell ? 1 : 0);
    return 0;
}

// ---- module load trace -------------------------------------------------------------
// Did Streamline even try to load sl.dlss_g? The hook makes the answer explicit instead
// of inferring it from the module list, and it also catches a load that is immediately
// undone by the plugin's own capability check.
typedef HMODULE (WINAPI* pfn_load_lib_w)(LPCWSTR);
static pfn_load_lib_w g_real_load_lib_w = nullptr;

static HMODULE WINAPI hook_load_lib_w(LPCWSTR name)
{
    const HMODULE m = g_real_load_lib_w ? g_real_load_lib_w(name) : nullptr;
    if (name != nullptr) {
        const wchar_t* base = wcsrchr(name, L'\\');
        base = base ? base + 1 : name;
        if (contains_ci(base, L"sl.") || contains_ci(base, L"nvngx")) {
            plog("load: %ls -> %p", base, m);
        }
    }
    return m;
}

static void hook_loader()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) { return; }
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(k32, "LoadLibraryW"));
    if (addr == nullptr) { return; }
    if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_load_lib_w),
                      reinterpret_cast<LPVOID*>(&g_real_load_lib_w)) == MH_OK &&
        MH_EnableHook(addr) == MH_OK) {
        plog("load: tracing LoadLibraryW for sl.*/nvngx*");
    }
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        open_log(self);
        HANDLE h = CreateThread(nullptr, 0, arm_thread, nullptr, 0, nullptr);
        if (h != nullptr) { CloseHandle(h); }
        // (the module-polling thread is NOT started: creating a second worker inside
        // DllMain made the title exit during startup - measured 2026-11-11.)
    }
    return TRUE;
}

// ============================================================================
//  version.dll exports, forwarded to the system copy
// ============================================================================
static HMODULE real_version()
{
    static HMODULE m = nullptr;
    if (m == nullptr) {
        wchar_t sys[MAX_PATH]{};
        if (GetSystemDirectoryW(sys, MAX_PATH)) {
            wcscat_s(sys, L"\\version.dll");
            m = LoadLibraryW(sys);
        }
    }
    return m;
}

// The system headers already declare these with C linkage, so the definitions get their
// own names and version.def maps the export table onto them (avoids "redefinition with a
// different linkage" and keeps the forwarder table explicit).
#define FWD(ret, name, params, args)                                             \
    extern "C" ret WINAPI my_##name params                                       \
    {                                                                            \
        typedef ret (WINAPI * pfn) params;                                       \
        static pfn p = nullptr;                                                  \
        if (p == nullptr) {                                                      \
            p = reinterpret_cast<pfn>(GetProcAddress(real_version(), #name));     \
        }                                                                        \
        return p ? p args : ret{};                                               \
    }

static void log_version_query(const wchar_t* name)
{
    if (!g_flag_verlog || name == nullptr) { return; }
    static wchar_t seen[16][96]{};
    static int n = 0;
    for (int i = 0; i < n; ++i) { if (_wcsicmp(seen[i], name) == 0) { return; } }
    if (n >= 16) { return; }
    wcsncpy_s(seen[n++], name, 95);
    const wchar_t* base = wcsrchr(name, L'\\');
    plog("ver: GetFileVersionInfo(%ls)", base ? base + 1 : name);
}

// The DLSS-G runtime version is a supported-feature gate of its own: the host reads
// nvngx_dlssg.dll's file version (measured 2026-11-11 - two GetFileVersionInfo calls for
// exactly that file, right before it decided "hardware not supported"), and the engine
// ships 310.2.1 here while the enabler that DID work shipped 310.9.16. So the version is
// rewritten to 310.9.16.0 in the VS_FIXEDFILEINFO the caller receives.
static void spoof_plugin_version(LPCWSTR file, LPVOID data)
{
    if (!g_flag_ver) { return; }
    if (data == nullptr || file == nullptr) { return; }
    const wchar_t* base = wcsrchr(file, L'\\');
    if (!contains_ci(base ? base + 1 : file, L"nvngx_dlssg")) { return; }
    // VS_FIXEDFILEINFO is NOT at the start of the version blob: it sits behind a small
    // header, so it has to be located by its signature (0xFEEF04BD) - assuming offset 0
    // was why the first attempt logged the read but never rewrote it.
    // The blob starts with VS_VERSIONINFO.wLength (a WORD). Scanning a fixed 1 KB past a
    // short blob found a coincidental 0xFEEF04BD in adjacent heap and wrote 16 bytes there
    // - CP2077 crashed reading 0x400 right after (the fault address was my scan bound).
    // Clamp to the declared length and refuse anything implausible.
    const unsigned short vt_len = *reinterpret_cast<const unsigned short*>(data);
    if (vt_len < 32) { return; }
    int max_dwords = static_cast<int>(vt_len / 4u);
    if (max_dwords > 512) { max_dwords = 512; }
    auto* dwords = static_cast<unsigned*>(data);
    unsigned* fixed = nullptr;
    for (int i = 0; i + 5 < max_dwords; ++i) {
        if (dwords[i] == 0xFEEF04BDu) { fixed = dwords + i; break; }
    }
    if (fixed == nullptr) { return; }
    const unsigned ms = (310u << 16) | 9u;      // 310.9
    const unsigned ls = (16u << 16) | 0u;       // .16.0
    fixed[2] = ms;                              // dwFileVersionMS
    fixed[3] = ls;                              // dwFileVersionLS
    fixed[4] = ms;                              // dwProductVersionMS
    fixed[5] = ls;                              // dwProductVersionLS
    static bool logged = false;
    if (!logged) {
        logged = true;
        plog("ver: nvngx_dlssg.dll reported as 310.9.16.0 instead of the engine's 310.2.1.0");
    }
}

extern "C" BOOL WINAPI my_GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{
    log_version_query(file);
    typedef BOOL (WINAPI * pfn)(LPCWSTR, DWORD, DWORD, LPVOID);
    static pfn p = nullptr;
    if (p == nullptr) { p = reinterpret_cast<pfn>(GetProcAddress(real_version(), "GetFileVersionInfoW")); }
    const BOOL ok = p ? p(file, handle, len, data) : FALSE;
    if (ok) { spoof_plugin_version(file, data); }
    return ok;
}

extern "C" BOOL WINAPI my_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, LPVOID data)
{
    if (file != nullptr) {
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH - 1);
        log_version_query(wide);
    }
    typedef BOOL (WINAPI * pfn)(LPCSTR, DWORD, DWORD, LPVOID);
    static pfn p = nullptr;
    if (p == nullptr) { p = reinterpret_cast<pfn>(GetProcAddress(real_version(), "GetFileVersionInfoA")); }
    const BOOL ok = p ? p(file, handle, len, data) : FALSE;
    if (ok && file != nullptr) {
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH - 1);
        spoof_plugin_version(wide, data);
    }
    return ok;
}
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
extern "C" BOOL WINAPI my_GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle,
                                                  DWORD len, LPVOID data)
{
    log_version_query(file);
    typedef BOOL (WINAPI * pfn)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    static pfn p = nullptr;
    if (p == nullptr) {
        p = reinterpret_cast<pfn>(GetProcAddress(real_version(), "GetFileVersionInfoExW"));
    }
    const BOOL ok = p ? p(flags, file, handle, len, data) : FALSE;
    if (ok) { spoof_plugin_version(file, data); }
    return ok;
}

extern "C" BOOL WINAPI my_GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD handle,
                                                  DWORD len, LPVOID data)
{
    if (file != nullptr) {
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH - 1);
        log_version_query(wide);
    }
    typedef BOOL (WINAPI * pfn)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    static pfn p = nullptr;
    if (p == nullptr) {
        p = reinterpret_cast<pfn>(GetProcAddress(real_version(), "GetFileVersionInfoExA"));
    }
    const BOOL ok = p ? p(flags, file, handle, len, data) : FALSE;
    if (ok && file != nullptr) {
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH - 1);
        spoof_plugin_version(wide, data);
    }
    return ok;
}

FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD a, LPCSTR b, LPDWORD c), (a, b, c))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD a, LPCWSTR b, LPDWORD c), (a, b, c))
FWD(DWORD, VerFindFileA,
    (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerFindFileW,
    (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileA,
    (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, LPSTR f, PUINT g),
    (a, b, c, d, e, f, g))
FWD(DWORD, VerInstallFileW,
    (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, LPWSTR f, PUINT g),
    (a, b, c, d, e, f, g))
FWD(DWORD, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a, b, c))
FWD(DWORD, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a, b, c))
FWD(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID* c, PUINT d), (a, b, c, d))
FWD(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d), (a, b, c, d))
