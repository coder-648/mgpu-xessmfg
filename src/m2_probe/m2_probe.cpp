// ============================================================================
// M2 probe - ReShade add-on that intercepts the DLSS (NGX) evaluation call and
// dumps what the game actually hands to DLSS.
//
// Why: XeSS-FG needs motion vectors, depth, jitter and camera matrices 5K/40fps
// per frame. Before writing a transport or an FG pipeline we need the real
// answers, not assumptions:
//   * which module actually drives DLSS in this game (nvngx_dlss vs Streamline)
//   * the exact MV / depth formats and sizes
//   * whether jitter and camera matrices are reachable at all
//     (they are NOT part of the DLSS-SR parameter set - only DLSSG has them -
//      so this probe also queries the DLSSG key names)
//
// Nothing is drawn, nothing is uploaded, no frame is touched. It only logs.
//
// Deploy: copy m2_probe.addon64 next to dxgi.dll (ReShade) in bin/x64.
// Log:    <game>/bin/x64/m2_probe.log
// ============================================================================

#include <windows.h>
#include <dxgi.h>
#include <d3d12.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "reshade.hpp"
#include "MinHook.h"

// Only the pieces of the DLSS SDK we need: the parameter object (a C++ class
// with virtual Get overloads) and the parameter key names. nvsdk_ngx.h itself
// is avoided on purpose - it pulls in the loader header and Vulkan paths.
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"

// ---------------------------------------------------------------- logging

static FILE* g_log = nullptr;
static HMODULE g_self = nullptr;

static void logf_(const char* fmt, ...)
{
    if (!g_log) { return; }
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

static void open_log()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) { strcpy_s(dot, MAX_PATH - (dot - path), ".log"); }

    fopen_s(&g_log, path, "w");
    if (g_log) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        logf_("=== m2_probe  %04d-%02d-%02d %02d:%02d:%02d ===\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
}

// ------------------------------------------------------- NGX function types

typedef NVSDK_NGX_Result (NVSDK_CONV *pfn_evaluate_t)(
    ID3D12GraphicsCommandList* cmd_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* parameters,
    void* callback);

typedef NVSDK_NGX_Result (NVSDK_CONV *pfn_create_t)(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** out_handle);

static pfn_evaluate_t g_real_evaluate = nullptr;
static pfn_create_t   g_real_create = nullptr;

// GetFeatureRequirements is called while the game probes for DLSS support, which
// happens earlier than any feature creation. Hooking it proves "NGX was really
// consulted" even when no feature is ever created (3 fixed arguments - safe).
typedef NVSDK_NGX_Result (NVSDK_CONV *pfn_getreq_t)(
    IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* info,
    NVSDK_NGX_FeatureRequirement* out_supported);

static pfn_getreq_t g_real_getreq = nullptr;

static volatile LONG g_eval_calls = 0;
static volatile LONG g_create_calls = 0;

// ------------------------------------------------------------- param helper

// Reads a value through the NGX parameter object. The object is a C++ class
// with a fixed virtual table, so the templated overloads resolve to a specific
// vtable slot - that is exactly what the game's own code does.
static float read_float(const NVSDK_NGX_Parameter* p, const char* name, bool* ok)
{
    float v = 0.0f;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) {
        if (ok) { *ok = true; }
        return v;
    }
    if (ok) { *ok = false; }
    return 0.0f;
}

static unsigned read_uint(const NVSDK_NGX_Parameter* p, const char* name, bool* ok)
{
    unsigned v = 0;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) {
        if (ok) { *ok = true; }
        return v;
    }
    if (ok) { *ok = false; }
    return 0u;
}

static ID3D12Resource* read_resource(const NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    if (p && p->Get(name, &r) == NVSDK_NGX_Result_Success) { return r; }
    return nullptr;
}

static void* read_voidp(const NVSDK_NGX_Parameter* p, const char* name)
{
    void* v = nullptr;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success) { return v; }
    return nullptr;
}

static void log_resource(const char* label, ID3D12Resource* res)
{
    if (!res) {
        logf_("    %-14s : <none>\n", label);
        return;
    }
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    logf_("    %-14s : %p  %llux%u  fmt=%d  dim=%d  mips=%u  samples=%u\n",
          label, (void*)res,
          (unsigned long long)d.Width, d.Height,
          (int)d.Format, (int)d.Dimension, d.MipLevels, d.SampleDesc.Count);
}

// Names the module that called into NGX. Needed because Streamline, the game
// and the driver shim can all be the caller; knowing which one drives DLSS
// decides where the production hook belongs.
static void log_caller_module(const void* return_address, const char* tag)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(return_address), &mod)) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) { return; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    logf_("    %-10s caller: %ls + 0x%llX\n", tag, base,
          (unsigned long long)(reinterpret_cast<const BYTE*>(return_address) -
                               reinterpret_cast<const BYTE*>(mod)));
}

// ------------------------------------------------------------------- hooks

static NVSDK_NGX_Result NVSDK_CONV hook_evaluate(
    ID3D12GraphicsCommandList* cmd_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* parameters,
    void* callback)
{
    const LONG n = InterlockedIncrement(&g_eval_calls);
    if (n <= 20 || (n % 120) == 0) {
        bool ok_w = false, ok_h = false, ok_jx = false, ok_jy = false;

        const unsigned w = read_uint(parameters, NVSDK_NGX_Parameter_Width, &ok_w);
        const unsigned h = read_uint(parameters, NVSDK_NGX_Parameter_Height, &ok_h);
        const float jx = read_float(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, &ok_jx);
        const float jy = read_float(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, &ok_jy);

        logf_("\n-- EvaluateFeature #%ld  (feature handle %p)\n", n, (void*)feature_handle);
        if (n <= 3) { log_caller_module(_ReturnAddress(), "evaluate"); }
        logf_("    render size   : %u x %u\n", ok_w ? w : 0u, ok_h ? h : 0u);
        logf_("    jitter        : %s%.6f , %s%.6f\n",
              ok_jx ? "" : "<none> ", jx, ok_jy ? "" : "<none> ", jy);

        log_resource("MotionVectors", read_resource(parameters, NVSDK_NGX_Parameter_MotionVectors));
        log_resource("Depth",         read_resource(parameters, NVSDK_NGX_Parameter_Depth));
        log_resource("ColorIn",       read_resource(parameters, NVSDK_NGX_Parameter_Color));
        log_resource("Output",        read_resource(parameters, NVSDK_NGX_Parameter_Output));

        // Camera matrices are not part of the DLSS-SR parameter set: only the
        // frame-generation (DLSSG) side receives them. Probe the DLSSG names so
        // we learn whether they are reachable in this title at all.
        void* m0 = read_voidp(parameters, "DLSSG.CameraViewToClip");
        void* m1 = read_voidp(parameters, "DLSSG.ClipToCameraView");
        void* m2 = read_voidp(parameters, "DLSSG.ClipToPrevClip");
        logf_("    matrices      : CamViewToClip=%p  ClipToCamView=%p  ClipToPrevClip=%p\n",
              m0, m1, m2);
    }

    return g_real_evaluate(cmd_list, feature_handle, parameters, callback);
}

static NVSDK_NGX_Result NVSDK_CONV hook_create(
    ID3D12GraphicsCommandList* cmd_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** out_handle)
{
    const LONG n = InterlockedIncrement(&g_create_calls);
    if (n <= 10) {
        bool ok_w = false, ok_h = false, ok_rw = false, ok_rh = false;
        const unsigned w  = read_uint(parameters, NVSDK_NGX_Parameter_Width, &ok_w);
        const unsigned h  = read_uint(parameters, NVSDK_NGX_Parameter_Height, &ok_h);
        const unsigned rw = read_uint(parameters,
                                      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &ok_rw);
        const unsigned rh = read_uint(parameters,
                                      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &ok_rh);

        logf_("\n== CreateFeature #%ld  feature=%d\n", n, (int)feature_id);
        logf_("    render size          : %u x %u\n", ok_w ? w : 0u, ok_h ? h : 0u);
        logf_("    render subrect       : %u x %u\n", ok_rw ? rw : 0u, ok_rh ? rh : 0u);
    }
    return g_real_create(cmd_list, feature_id, parameters, out_handle);
}

static NVSDK_NGX_Result NVSDK_CONV hook_getreq(
    IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* info,
    NVSDK_NGX_FeatureRequirement* out_supported)
{
    static LONG n = 0;
    const LONG i = InterlockedIncrement(&n);
    if (i <= 10) {
        logf_("  [probe] GetFeatureRequirements #%ld  feature=%d  adapter=%p\n",
              i, info ? (int)info->FeatureID : -1, (void*)adapter);
    }
    return g_real_getreq(adapter, info, out_supported);
}

// -------------------------------------------------------- module discovery
// Logs every NVSDK_NGX_* export of the module, so we can see which entry points
// this build actually offers (the "_C" variants matter for hooking).
static void dump_exports(HMODULE mod, const char* dll_name)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return; }

    const IMAGE_DATA_DIRECTORY& dir_ = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir_.VirtualAddress == 0) { return; }

    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir_.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);

    int count = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (strncmp(name, "NVSDK_NGX_D3D12", 15) != 0) { continue; }
        const DWORD rva = functions[ordinals[i]];
        void* addr = const_cast<BYTE*>(base + rva);
        logf_("    export %-52s @ %p\n", name, addr);
        ++count;
    }
    logf_("  %s : %d NVSDK_NGX_D3D12* exports\n", dll_name, count);
}

// ------------------------------------------------------------ hook installer

static bool g_mh_ready = false;
static bool g_hooked = false;
static HMODULE g_hooked_module = nullptr;

static bool install_hooks(HMODULE ngx)
{
    if (!g_mh_ready) {
        if (MH_Initialize() != MH_OK) {
            logf_("  MH_Initialize FAILED\n");
            return false;
        }
        g_mh_ready = true;
    }

    // The plain name is the C++ overload (default argument), the _C suffix is
    // the C variant the game may resolve instead; try both, hook whatever exists.
    struct Target { const char* name; LPVOID detour; LPVOID* original; bool* done; };
    Target targets[] = {
        { "NVSDK_NGX_D3D12_EvaluateFeature",   reinterpret_cast<LPVOID>(&hook_evaluate), reinterpret_cast<LPVOID*>(&g_real_evaluate), &g_hooked },
        { "NVSDK_NGX_D3D12_EvaluateFeature_C", reinterpret_cast<LPVOID>(&hook_evaluate), reinterpret_cast<LPVOID*>(&g_real_evaluate), &g_hooked },
        { "NVSDK_NGX_D3D12_CreateFeature",     reinterpret_cast<LPVOID>(&hook_create),   reinterpret_cast<LPVOID*>(&g_real_create),   nullptr },
        { "NVSDK_NGX_D3D12_GetFeatureRequirements", reinterpret_cast<LPVOID>(&hook_getreq), reinterpret_cast<LPVOID*>(&g_real_getreq), nullptr },
    };

    bool any = false;
    for (auto& t : targets) {
        if (t.done && *t.done) { continue; }
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(ngx, t.name));
        if (!addr) { continue; }

        if (MH_CreateHook(addr, t.detour, t.original) != MH_OK) {
            logf_("  hook %s FAILED\n", t.name);
            continue;
        }
        if (MH_EnableHook(addr) != MH_OK) {
            logf_("  enable %s FAILED\n", t.name);
            continue;
        }
        logf_("  hooked %s @ %p\n", t.name, addr);
        if (t.done) { *t.done = true; }
        any = true;
    }
    return any;
}

// ------------------------------------------------------- picking the module

// Cyberpunk keeps its DLSS runtime in a subfolder
// (bin/x64/dlssg_sm86/sr/nvngx_dlss.dll), NOT next to the exe. Windows lets two
// DLLs with the same base name coexist, so GetModuleHandle("nvngx_dlss.dll")
// can hand back a completely different copy than the one the game uses.
// Run 2 proved that: hooks installed fine on the exe-dir copy and never fired.
//
// So instead of guessing a path, we hook the loader and instrument whichever
// copy the game actually loads, before control returns to the game.
static void hook_module_if_ngx(HMODULE m)
{
    if (!m || g_hooked_module == m) { return; }

    wchar_t full[MAX_PATH]{};
    if (!GetModuleFileNameW(m, full, MAX_PATH)) { return; }

    const bool is_driver_shim = (wcsstr(full, L"_nvngx.dll") != nullptr);
    const bool is_dlss_copy   = (wcsstr(full, L"nvngx_dlss.dll") != nullptr);
    if (!is_driver_shim && !is_dlss_copy) { return; }

    logf_("\n== candidate NGX provider: %ls @ %p\n", full, (void*)m);

    if (g_hooked_module) {
        logf_("  (already instrumenting %p - logging only)\n", (void*)g_hooked_module);
        return;
    }

    // Run 4 showed the driver-store shim (_nvngx.dll) is the only NGX provider
    // still resident while the game renders, and the nvngx_dlss.dll copies are
    // loaded, queried and unloaded again. So the shim is the one to hook; the
    // copies are logged but left alone.
    if (!is_driver_shim) {
        logf_("  -> NOT hooking (waiting for the driver shim _nvngx.dll)\n");
        return;
    }

    g_hooked_module = m;
    dump_exports(m, "_nvngx.dll");
    if (install_hooks(m)) {
        logf_("  hooks active on THIS copy - waiting for DLSS calls\n");
    } else {
        logf_("  NO HOOK INSTALLED - dump only\n");
    }
}

static HMODULE (WINAPI* g_real_load_library_ex_w)(LPCWSTR, HANDLE, DWORD) = nullptr;
static HMODULE (WINAPI* g_real_load_library_w)(LPCWSTR) = nullptr;

static HMODULE WINAPI hook_load_library_ex_w(LPCWSTR name, HANDLE file, DWORD flags)
{
    const HMODULE m = g_real_load_library_ex_w(name, file, flags);
    if (name) { hook_module_if_ngx(m); }
    return m;
}

static HMODULE WINAPI hook_load_library_w(LPCWSTR name)
{
    const HMODULE m = g_real_load_library_w(name);
    if (name) { hook_module_if_ngx(m); }
    return m;
}

// ANSI variants: some engines call these instead. hook_module_if_ngx filters by
// full path, so passing every result through costs nothing.
static HMODULE (WINAPI* g_real_load_library_a)(LPCSTR) = nullptr;
static HMODULE (WINAPI* g_real_load_library_ex_a)(LPCSTR, HANDLE, DWORD) = nullptr;

static HMODULE WINAPI hook_load_library_a(LPCSTR name)
{
    const HMODULE m = g_real_load_library_a(name);
    hook_module_if_ngx(m);
    return m;
}

static HMODULE WINAPI hook_load_library_ex_a(LPCSTR name, HANDLE file, DWORD flags)
{
    const HMODULE m = g_real_load_library_ex_a(name, file, flags);
    hook_module_if_ngx(m);
    return m;
}

// Which entry points does the game actually resolve, and from which module?
// This is what separates "NGX is used but through another copy/variant" from
// "NGX is never called at all".
static FARPROC (WINAPI* g_real_get_proc_address)(HMODULE, LPCSTR) = nullptr;

static bool is_interesting_symbol(const char* name)
{
    if (!name) { return false; }
    if (strstr(name, "NVSDK_NGX")) { return true; }
    if (strncmp(name, "sl", 2) == 0 && strstr(name, "Feature")) { return true; }
    return false;
}

static FARPROC WINAPI hook_get_proc_address(HMODULE mod, LPCSTR name)
{
    const FARPROC p = g_real_get_proc_address(mod, name);
    if (is_interesting_symbol(name)) {
        logf_("  [GetProcAddress] %p  %-46s -> %p\n", (void*)mod, name, (void*)p);
    }
    return p;
}

// Modules loaded before the add-on (or through a path our loader hooks miss)
// are picked up here.
static void dump_all_modules(const char* why)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { return; }

    logf_("\n== full module list (%s) ==\n", why);
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            logf_("  %p  %ls\n", (void*)me.hModule, me.szExePath);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static void hook_any_loaded_ngx()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { return; }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (wcsstr(me.szModule, L"nvngx")) {   // _nvngx.dll or nvngx_dlss.dll
                hook_module_if_ngx(me.hModule);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static const char* kModules[] = {
    "nvngx_dlss.dll", "sl.interposer.dll", "sl.dlss.dll", "sl.dlss_g.dll",
    "nvngx_dlssg.dll", "libxess.dll", "libxess_fg.dll", "dxgi.dll",
};
static const int kModuleCount = static_cast<int>(sizeof(kModules) / sizeof(kModules[0]));

// Logs transitions only, so we can see exactly when a module appears.
static void log_module_states(bool force)
{
    static bool last[16] = {};
    for (int i = 0; i < kModuleCount && i < 16; ++i) {
        const bool now = GetModuleHandleA(kModules[i]) != nullptr;
        if (force || now != last[i]) {
            logf_("  %-20s %s\n", kModules[i], now ? "LOADED" : "-");
            last[i] = now;
        }
    }
}

static DWORD WINAPI watch_thread(LPVOID)
{
    // Dump the whole module list a couple of times, late enough that the game
    // is in-world with the upscaler active. This is what tells us which DLSS
    // stack Cyberpunk really runs (the subfolder copy, Streamline, DirectSR...).
    const DWORD t0 = GetTickCount();
    bool dumped_20 = false, dumped_45 = false;

    for (;;) {
        log_module_states(false);
        hook_any_loaded_ngx();

        const DWORD elapsed = GetTickCount() - t0;
        if (!dumped_20 && elapsed > 20000) { dumped_20 = true; dump_all_modules("t+20s"); }
        if (!dumped_45 && elapsed > 45000) { dumped_45 = true; dump_all_modules("t+45s"); }

        Sleep(100);
    }
    return 0;
}

// ------------------------------------------------------------------ add-on

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    g_self = addon_module;
    open_log();
    logf_("AddonInit (addon=%p reshade=%p)\n", (void*)addon_module, (void*)reshade_module);

    if (reshade::register_addon(addon_module, reshade_module)) {
        logf_("registered with ReShade\n");
    } else {
        logf_("reshade::register_addon FAILED\n");
    }

    logf_("\n== modules at add-on start ==\n");
    log_module_states(true);

    // Hook the loader FIRST, so any DLSS copy the game loads from now on gets
    // instrumented before the game can resolve its exports.
    // (Run 2 preloaded a fixed path and hooked the wrong copy: the game uses
    //  bin/x64/dlssg_sm86/sr/nvngx_dlss.dll, not the one next to the exe.)
    if (!g_mh_ready) {
        if (MH_Initialize() == MH_OK) {
            g_mh_ready = true;
            logf_("\nMinHook initialised\n");
        } else {
            logf_("\nMH_Initialize FAILED\n");
        }
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    struct LoaderHook { const char* name; LPVOID detour; LPVOID* original; };
    LoaderHook loader_hooks[] = {
        { "LoadLibraryExW", reinterpret_cast<LPVOID>(&hook_load_library_ex_w),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_ex_w) },
        { "LoadLibraryW",   reinterpret_cast<LPVOID>(&hook_load_library_w),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_w) },
        { "LoadLibraryExA", reinterpret_cast<LPVOID>(&hook_load_library_ex_a),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_ex_a) },
        { "LoadLibraryA",   reinterpret_cast<LPVOID>(&hook_load_library_a),
                            reinterpret_cast<LPVOID*>(&g_real_load_library_a) },
        { "GetProcAddress", reinterpret_cast<LPVOID>(&hook_get_proc_address),
                            reinterpret_cast<LPVOID*>(&g_real_get_proc_address) },
    };
    for (auto& lh : loader_hooks) {
        LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(kernel32, lh.name));
        if (!addr) {
            logf_("  loader hook %s : symbol not found\n", lh.name);
            continue;
        }
        if (MH_CreateHook(addr, lh.detour, lh.original) != MH_OK ||
            MH_EnableHook(addr) != MH_OK) {
            logf_("  loader hook %s : FAILED\n", lh.name);
            continue;
        }
        logf_("  loader hook %s : OK @ %p\n", lh.name, addr);
    }

    // Catch copies that were already resident before we hooked the loader.
    hook_any_loaded_ngx();

    HANDLE th = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (th) { CloseHandle(th); }
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    logf_("AddonUninit (eval calls: %ld, create calls: %ld)\n", g_eval_calls, g_create_calls);

    if (g_mh_ready) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    reshade::unregister_addon(addon_module, reshade_module);

    if (g_log) { fclose(g_log); g_log = nullptr; }
}

