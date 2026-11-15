// ============================================================================
//  nvapi_arch.cpp - the architecture answer Streamline asks before slInit.
//
//  Streamline decides whether DLSS-G exists on this adapter by asking NvAPI for
//  the GPU architecture (`NvAPI_GPU_GetArchInfo`), and that happens before any
//  device exists. On an RTX 30-series the honest answer is Ampere, which means
//  "DLSS-G unsupported" - this hook is what makes the title offer frame
//  generation at all (dlssg_sm86 does the same thing from a version.dll proxy;
//  we are already loaded earlier than that).
//
//  Restriction (measured, and the reason the add-on kept this off by default in
//  `[capture] FgArchSpoof`): spoofing Ada for every caller breaks DLSS-SR / DLSS-D
//  on the same adapter. So the answer is rewritten only when the caller is the
//  Streamline side - the one gate we need - and the GPU's own runtime keeps
//  seeing the truth.
//
//  Provenance: same technique as OptiScaler's nvapi spoofing and our own
//  producer's in_fg.cpp (GPL-3.0).
// ============================================================================
#include "fg_internal.h"

#include <MinHook.h>

namespace fakerfg {
namespace {

typedef void* (NVSDK_CONV* pfn_nvapi_query)(unsigned int);
typedef int   (NVSDK_CONV* pfn_nvapi_arch)(void* gpu, void* info);

constexpr unsigned kNvApiGpuGetArchInfo = 0x2D3A1A3A;   // published id
constexpr unsigned kIdAlt               = 0xD8265D24;   // the id this NVAPI build resolves
constexpr unsigned char kNvArchReported = 0x90;         // AD10x: what Streamline accepts

pfn_nvapi_query g_real_query = nullptr;
pfn_nvapi_arch  g_real_arch  = nullptr;
bool            g_installed  = false;
bool            g_tried      = false;

bool contains_ci(const wchar_t* s, const wchar_t* needle)
{
    if (s == nullptr || needle == nullptr) { return false; }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = s; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, n) == 0) { return true; }
    }
    return false;
}

// Who gets the raised architecture: Streamline's own modules, and only those. The spoof
// exists for one gate - SL's feature support / SystemCaps check - which is what makes
// DLSS-G appear. Everything else gets the truth: NVIDIA's upscaler modules (spoofing Ada
// there breaks DLSS/DLSSD, OptiScaler: ";Can't spoof ada for DLSSG here as that breaks
// DLSS/DLSSD") and, importantly, the game itself. Answering CP2077 made it believe it ran
// on an AD10x part while the GPU is Ampere (0x170), after which it asked NGX for models
// the driver has no entry for and dropped the DLSS option from its menu (measured).
bool caller_is_streamline_module(const wchar_t* path)
{
    if (path == nullptr) { return false; }
    return contains_ci(path, L"\\sl.interposer.dll") ||
           contains_ci(path, L"\\sl.common.dll") ||
           contains_ci(path, L"\\sl.dlss") ||
           contains_ci(path, L"\\sl.reflex") ||
           contains_ci(path, L"\\sl.pcl") ||
           contains_ci(path, L"\\sl_common_override") ||
           contains_ci(path, L"\\sl_dlss_override") ||
           contains_ci(path, L"\\sl_dlss_g_override") ||
           contains_ci(path, L"\\sl_dlss_d_override") ||
           contains_ci(path, L"\\sl_reflex_override") ||
           contains_ci(path, L"\\sl_pcl_override");
}

// `caller` must come from _ReturnAddress() inside the hooked function itself -
// taken in a helper it always resolves to our own module (measured: the first live
// run reported "question from ...\dxgi.dll", i.e. us, and answered everybody).
bool caller_is_streamline(void* caller)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(caller), &self) ||
        self == nullptr) {
        return false;            // cannot tell -> honest, a wrong arch is worse than a hidden toggle
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) { return false; }

    const bool sl = caller_is_streamline_module(path);
    static wchar_t seen[8][MAX_PATH]{};
    static int seen_count = 0;
    bool logged = false;
    for (int i = 0; i < seen_count; ++i) {
        if (wcscmp(seen[i], path) == 0) { logged = true; break; }
    }
    if (!logged && seen_count < 8) {
        wcscpy_s(seen[seen_count++], path);
        logf("arch: question from %ls -> %s\n", path,
             sl ? "answered (Streamline module)" : "honest (everything else)");
    }
    return sl;
}

int NVSDK_CONV hook_arch(void* gpu, void* info)
{
    // Must be the first statement: no inlining, no helper, or it names the wrong frame.
    void* caller = _ReturnAddress();
    const int r = (g_real_arch != nullptr) ? g_real_arch(gpu, info) : -1;
    if (r == 0 && info != nullptr && cfg().arch_spoof != 0 && caller_is_streamline(caller)) {
        // NvAPI_GPU_GetArchInfo: { version; architecture; implementation; revision } -
        // all four are 32-bit, so the architecture field is bytes[4..7]. The first version
        // wrote only bytes[4], which happened to give 0x190 on an Ampere part only because
        // bytes[5] already held 0x01.
        auto* fields = static_cast<uint32_t*>(info);
        if (fields[1] < kNvArchReported) {
            static bool told = false;
            if (!told) {
                told = true;
                logf("arch: answering 0x%X (was 0x%X) so Streamline offers DLSS-G\n",
                     kNvArchReported, fields[1]);
            }
            fields[1] = kNvArchReported;
        }
    }
    return r;
}

void* NVSDK_CONV hook_query(unsigned int id)
{
    void* fn = (g_real_query != nullptr) ? g_real_query(id) : nullptr;
    if ((id == kNvApiGpuGetArchInfo || id == kIdAlt) && fn != nullptr &&
        cfg().arch_spoof != 0) {
        if (g_real_arch == nullptr) {
            g_real_arch = reinterpret_cast<pfn_nvapi_arch>(fn);
            logf("arch: NvAPI_GPU_GetArchInfo @ %p\n", fn);
        }
        return reinterpret_cast<void*>(&hook_arch);
    }
    return fn;
}

} // namespace

// Idempotent and cheap: called from the loader thread and from every early entry
// point, because nvapi64.dll may not be in the process yet the first time.
void arch_install_if_possible()
{
    if (g_installed || cfg().arch_spoof == 0) { return; }

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr) { nvapi = GetModuleHandleW(L"nvapi.dll"); }
    if (nvapi == nullptr) {
        // Normal early on: the game loads nvapi together with its NGX stack. The
        // callers retry (loader thread, first factory, first NGX init), so this is
        // logged once and not treated as a failure.
        static bool told = false;
        if (!told) { told = true; logf("arch: nvapi not loaded yet - will retry\n"); }
        return;
    }
    if (g_tried) { return; }
    g_tried = true;

    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (addr == nullptr) {
        logf("arch: nvapi_QueryInterface not exported\n");
        return;
    }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        logf("arch: MinHook init failed\n");
        return;
    }
    if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&hook_query),
                      reinterpret_cast<LPVOID*>(&g_real_query)) != MH_OK ||
        MH_EnableHook(addr) != MH_OK) {
        logf("arch: could not hook nvapi_QueryInterface\n");
        return;
    }
    g_installed = true;
    logf("arch: nvapi_QueryInterface hooked @ %p (nvapi64=%p)\n", addr, (void*)nvapi);
}

} // namespace fakerfg
