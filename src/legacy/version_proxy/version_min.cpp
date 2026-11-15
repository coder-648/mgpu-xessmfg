// ============================================================================
//  version.dll (minimal) - only what dlssg_sm86 actually relies on.
//
//  The previous proxy grew four subsystems (NVAPI arch, NGX capability/requirements/init,
//  DLSS-G file version, DXGI adapter). Three of them were guesses and they broke titles:
//  CP2077 crashed on a mis-signed NVAPI forward, on an out-of-bounds version write, and
//  hung when GetProcAddress was hooked. This file starts over with the two mechanisms
//  dlssg_sm86 documents as load-bearing (docs/ARCHITECTURE.md 633/635/637):
//
//    1. NvAPI_GPU_GetArchInfo must answer *the game and Streamline* with a newer
//       architecture than the physical one (Ampere 0x170 -> Ada 0x190), otherwise
//       sl.common evicts sl.dlss_g inside slInit ("Feature 'DLSSG' is not supported on
//       this platform") long before any runtime exists.
//
//    2. The DLSS-G runtime declares a *hardcoded* minimum architecture
//       (`mov eax,0x190; ret`). The NGX core compares it with the physical GPU it read
//       itself - during slInit - so no external answer can fix it: FeatureInitResult
//       ends up 0xbad0000b and Streamline never calls CreateFeature. The constant is
//       the thing that has to change (sm86 calls the physical value "coreArch").
//
//  Nothing else. No NGX hooks, no version rewriting, no DXGI, no GetProcAddress hook.
//  Everything is per-feature switchable in mgpu_proxy.ini next to this DLL, default ON
//  for the two mechanisms above and nothing else.
// ============================================================================
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
#include <string.h>

#include "MinHook.h"

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_log_cs;
static volatile LONG g_log_cs_ready = 0;
static HMODULE g_self = nullptr;

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
    g_log = _fsopen(narrow, "a", _SH_DENYNO);
}

static bool proxy_flag(const wchar_t* key, bool def)
{
    wchar_t ini[MAX_PATH]{};
    if (g_self == nullptr || GetModuleFileNameW(g_self, ini, MAX_PATH) == 0) { return def; }
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (slash != nullptr) { *(slash + 1) = L'\0'; } else { ini[0] = L'\0'; }
    wcscat_s(ini, L"mgpu_proxy.ini");
    return GetPrivateProfileIntW(L"proxy", key, def ? 1 : 0, ini) != 0;
}

static bool contains_ci(const wchar_t* hay, const wchar_t* needle)
{
    if (hay == nullptr || needle == nullptr || *needle == L'\0') { return false; }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, n) == 0) { return true; }
    }
    return false;
}


// ------------------------------------------------------- 1. architecture answer
typedef void* (WINAPI* pfn_query_interface)(unsigned int id);
typedef int   (WINAPI* pfn_get_arch_info)(void* gpu, void* info);

static pfn_query_interface g_real_qi   = nullptr;
static pfn_get_arch_info   g_real_arch = nullptr;
static bool g_flag_arch    = true;
static bool g_flag_minarch = true;

static constexpr unsigned kArchInfoVersion = 0x20010;   // NV_GPU_ARCH_INFO
// 16-bit on purpose: the arch field in NV_GPU_ARCH_INFO is 16 bits (little endian at
// b[4..5]). Declaring this as unsigned char silently truncated 0x190 to 0x90, and the
// answer only came out right because Ampere's high byte is 0x01 (0x90 | 0x0100 = 0x190).
// That luck does not extend to a 0x170/0x168 machine with a different high byte, and the
// log printed the truncated constant, which made the line look like a typo for weeks.
static constexpr unsigned kArchSpoofed = 0x190;         // Ada: what the host must see
static unsigned g_phys_arch = 0;                        // 0x170 here (Ampere)
static volatile LONG g_minarch_done = 0;                // one scan per process

// Already verified at run time: the second argument is NV_GPU_ARCH_INFO. Nothing else is
// ever wrapped - a wrong-arity forward is what killed CP2077 once (0xFFFFFFFF).
static int WINAPI hook_arch_info(void* gpu, void* info)
{
    const int r = g_real_arch ? g_real_arch(gpu, info) : -1;
    if (r != 0 || info == nullptr) { return r; }

    auto* b = static_cast<unsigned char*>(info);
    unsigned ver = 0;
    memcpy(&ver, b, sizeof(ver));
    if (ver != kArchInfoVersion) { return r; }
    if (g_phys_arch == 0) {
        g_phys_arch = static_cast<unsigned>(b[4]) | (static_cast<unsigned>(b[5]) << 8);
        plog("arch: physical architecture is 0x%X", g_phys_arch);
    }
    const unsigned now = static_cast<unsigned>(b[4]) | (static_cast<unsigned>(b[5]) << 8);
    if (now < kArchSpoofed) {
        const unsigned was = now;
        b[4] = static_cast<unsigned char>(kArchSpoofed & 0xFFu);
        b[5] = static_cast<unsigned char>((kArchSpoofed >> 8) & 0xFFu);
        static bool logged = false;
        if (!logged) {
            logged = true;
            plog("arch: answering 0x%X instead of 0x%X (the value sl.common compares"
                 " in slInit)", kArchSpoofed, was);
        }
    }
    return r;
}

static void* WINAPI hook_qi(unsigned int id)
{
    void* fn = g_real_qi ? g_real_qi(id) : nullptr;
    // 0xD8265D24 is the id this NVAPI build uses for NvAPI_GPU_GetArchInfo (found by
    // tracing callers; the published 0x2D3A1A3A never appears on this machine).
    if ((id == 0xD8265D24u || id == 0x2D3A1A3Au) && fn != nullptr) {
        if (g_real_arch == nullptr) {
            g_real_arch = reinterpret_cast<pfn_get_arch_info>(fn);
            plog("arch: NvAPI_GPU_GetArchInfo resolved @ %p", fn);
        }
        return reinterpret_cast<void*>(&hook_arch_info);
    }
    return fn;
}

static void hook_nvapi(HMODULE m)
{
    if (g_real_qi != nullptr || m == nullptr) { return; }
    FARPROC qi = GetProcAddress(m, "nvapi_QueryInterface");
    if (qi == nullptr) { return; }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(qi), reinterpret_cast<LPVOID>(&hook_qi),
                      reinterpret_cast<LPVOID*>(&g_real_qi)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(qi)) == MH_OK) {
        plog("arch: nvapi_QueryInterface hooked @ %p", qi);
    }
}

// ------------------------------------------------- 2. the runtime's hardcoded minimum
// `mov eax,0x190; ret` in an executable section of the DLSS-G runtime. Rewriting the
// immediate to the physical architecture is what stops the NGX core's own comparison
// (physical vs declared minimum, done during slInit) from failing with 0xbad0000b.
static int patch_min_arch(HMODULE mod)
{
    if (!g_flag_minarch || mod == nullptr || g_phys_arch == 0) { return 0; }
    auto* base = reinterpret_cast<unsigned char*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return 0; }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return 0; }

    const unsigned char want_lo = static_cast<unsigned char>(g_phys_arch & 0xFF);
    const unsigned char want_hi = static_cast<unsigned char>((g_phys_arch >> 8) & 0xFF);
    int patched = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        auto* sh = IMAGE_FIRST_SECTION(nt) + s;
        if ((sh->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) { continue; }
        auto* start = base + sh->VirtualAddress;
        const size_t size = sh->Misc.VirtualSize;
        for (size_t i = 0; i + 6 <= size; ++i) {
            if (start[i] != 0xB8 || start[i + 5] != 0xC3) { continue; }
            if (start[i + 3] != 0 || start[i + 4] != 0) { continue; }
            const unsigned imm = static_cast<unsigned>(start[i + 1]) |
                                 (static_cast<unsigned>(start[i + 2]) << 8);
            // Only plausible architectures, never the value we are installing.
            if (imm < 0x150 || imm > 0x1FF || imm == g_phys_arch) { continue; }
            DWORD old_protect = 0;
            if (!VirtualProtect(start + i, 6, PAGE_EXECUTE_READWRITE, &old_protect)) { continue; }
            start[i + 1] = want_lo;
            start[i + 2] = want_hi;
            VirtualProtect(start + i, 6, old_protect, &old_protect);
            ++patched;
            plog("arch: runtime min-arch 0x%X -> 0x%X at %p", imm, g_phys_arch, start + i);
        }
    }
    return patched;
}

// ------------------------------------------------------------------ arming
// Two triggers, both cheap: nvapi64.dll is pre-loaded (it is a plain system DLL) so the
// arch answer is ours before sl.common ever asks, and nvngx_dlssg.dll is patched the
// moment it maps. No LoadLibrary/GetProcAddress hooks: both hung CP2077.
static DWORD WINAPI arm_thread(LPVOID)
{
    plog("proxy loaded (minimal: arch + runtime min-arch only)");
    g_flag_arch    = proxy_flag(L"ArchSpoof", true);
    g_flag_minarch = proxy_flag(L"MinArchPatch", true);
    plog("switches: ArchSpoof=%d MinArchPatch=%d", g_flag_arch, g_flag_minarch);

    // Host=1: load our own D3D12 host (src/host/own_host_probe.cpp) next to this DLL.
    // It is a *separate module* on purpose - this proxy must stay as small and as safe as
    // it is (it is the thing that runs before slInit), and the host installs hooks that
    // touch d3d12/dxgi, which may only happen off the loader lock (we are on our own
    // thread here, which is exactly why the load lives in this function).
    if (proxy_flag(L"Host", false)) {
        wchar_t path[MAX_PATH]{};
        if (g_self != nullptr && GetModuleFileNameW(g_self, path, MAX_PATH) != 0) {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash != nullptr) { *(slash + 1) = L'\0'; }
            // mgpu_capture.dll is the real thing (capture core + own host glue).
            // mgpu_host.dll was the stage-1 probe and is only a fallback for debugging.
            HMODULE host = nullptr;
            const wchar_t* names[] = { L"mgpu_capture.dll", L"mgpu_host.dll" };
            for (const wchar_t* name : names) {
                wchar_t try_path[MAX_PATH]{};
                wcsncpy_s(try_path, path, _TRUNCATE);
                wcscat_s(try_path, name);
                host = LoadLibraryW(try_path);
                plog("host: %ls %s (%p)", name, host != nullptr ? "loaded" : "not present/refused",
                     static_cast<void*>(host));
                if (host != nullptr) { break; }
            }
        }
    }

    if (MH_Initialize() != MH_OK) { plog("MinHook init failed"); return 0; }

    if (g_flag_arch) {
        HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
        if (nv == nullptr) { nv = LoadLibraryW(L"nvapi64.dll"); }
        if (nv != nullptr) { hook_nvapi(nv); }
        else { plog("arch: nvapi64.dll not available"); }
    }

    for (int i = 0; i < 9000; ++i) {          // up to ~90 s at 10 ms
        if (g_flag_arch && g_real_qi == nullptr) {
            HMODULE nv = GetModuleHandleW(L"nvapi64_impl.dll");
            if (nv != nullptr) { hook_nvapi(nv); }
        }
        if (g_flag_minarch) {
            static bool scanned = false;
            HMODULE fg = GetModuleHandleW(L"nvngx_dlssg.dll");
            if (!scanned && fg != nullptr && g_phys_arch != 0) {
                scanned = true;
                const int n = patch_min_arch(fg);
                InterlockedExchange(&g_minarch_done, 1);
                plog("arch: min-arch scan: %d site(s) patched", n);
            }
        }
        // Only stop once *both* jobs are done: the min-arch scan needs g_phys_arch, which
        // is only learned on the first arch query, so breaking on that alone skipped the
        // scan entirely.
        if (g_real_qi != nullptr && g_phys_arch != 0 &&
            (!g_flag_minarch || GetModuleHandleW(L"nvngx_dlssg.dll") == nullptr ||
             g_minarch_done)) {
            break;
        }
        Sleep(10);
    }
    return 0;
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        open_log(self);
        HANDLE h = CreateThread(nullptr, 0, arm_thread, nullptr, 0, nullptr);
        if (h != nullptr) { CloseHandle(h); }
    }
    return TRUE;
}

// ============================================================================
//  version.dll exports, forwarded to the system copy (this DLL is only a landing spot)
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

#define FWD(ret, name, params, args)                                          \
    extern "C" ret WINAPI my_##name params                                   \
    {                                                                        \
        typedef ret (WINAPI * pfn) params;                                   \
        static pfn p = nullptr;                                              \
        if (p == nullptr) {                                                  \
            p = reinterpret_cast<pfn>(GetProcAddress(real_version(), #name)); \
        }                                                                    \
        return p ? p args : ret{};                                           \
    }

FWD(BOOL, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
FWD(BOOL, GetFileVersionInfoExA, (DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
FWD(BOOL, GetFileVersionInfoExW, (DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD a, LPCSTR b, LPDWORD c), (a, b, c))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD a, LPCWSTR b, LPDWORD c), (a, b, c))
FWD(DWORD, VerFindFileA,
    (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerFindFileW,
    (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileA, (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, LPSTR f, PUINT g),
    (a, b, c, d, e, f, g))
FWD(DWORD, VerInstallFileW, (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, LPWSTR f, PUINT g),
    (a, b, c, d, e, f, g))
FWD(DWORD, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a, b, c))
FWD(DWORD, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a, b, c))
FWD(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID* c, PUINT d), (a, b, c, d))
FWD(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d), (a, b, c, d))
