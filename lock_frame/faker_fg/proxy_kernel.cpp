// ============================================================================
//  proxy_kernel.cpp - the module-load layer, ported from OptiScaler.
//
//  OptiScaler never hooks the NGX entry points. It intercepts module loading and lookup
//  and answers the NGX-core request with its own module:
//
//      hooks/LibraryLoad_Hooks.cpp:60
//      if (EnableDlssInputs && CheckDllNameW(&libName, &nvngxNamesW) && ...)
//      {
//          LOG_INFO("nvngx call: {0}, returning this dll!", libNameA);
//          return dllModule;                          // <- its own module
//      }
//
//  The same answer comes from every entry that resolves a module (all of them funnel into
//  CheckLoad() -> LibraryLoadHooks::LoadLibraryCheckW):
//      hooks/Kernel_Hooks.cpp:403   kernel32!LoadLibraryExW
//      hooks/Kernel_Hooks.cpp:186   kernel32!GetModuleHandleA/W, GetModuleHandleExA/W
//      hooks/Ntdll_Hooks.h:44       ntdll!LdrLoadDll   (Streamline loads plugins with it)
//  and the real core is loaded separately, only as the forwarding target
//  (proxies/NVNGX_Proxy.h:549 resolves the real entry points from it).
//
//  SL plugin modules are hooked the same way OptiScaler does it
//  (hooks/Streamline_Hooks.cpp:2557 hookPlugin / :2407 slotDetour): every plugin's
//  `slGetPluginFunction` is replaced per module instance, and `slOnPluginLoad` is answered
//  by us - that is where the DLSS-G architecture gate lives (:1043, spoofArch at :925).
// ============================================================================
#include "fg_internal.h"

#include <winternl.h>
#include <MinHook.h>
#include "nlohmann/json.hpp"

#include <string>

#include "sl.param/parameters.h"

namespace fakerfg {

// Defined in proxy_streamline.cpp / proxy_nvngx.cpp, called from the module-load hook in
// this file (OptiScaler hooks both at load time, inside LibraryLoad_Hooks).
void sl_note_interposer_loaded();
void nvngx_module_loaded(HMODULE m);

namespace {

constexpr int kMaxPluginSlots = 8;

typedef HMODULE(WINAPI* pfn_llex)(LPCWSTR, HMODULE, DWORD);
typedef HMODULE(WINAPI* pfn_llw)(LPCWSTR);
typedef HMODULE(WINAPI* pfn_gmh_w)(LPCWSTR);
typedef HMODULE(WINAPI* pfn_gmh_a)(LPCSTR);
typedef BOOL(WINAPI* pfn_gmh_ex_w)(DWORD, LPCWSTR, HMODULE*);
typedef BOOL(WINAPI* pfn_gmh_ex_a)(DWORD, LPCSTR, HMODULE*);
typedef NTSTATUS(NTAPI* pfn_ldr)(PWSTR, PULONG, PUNICODE_STRING, PHANDLE);
typedef void* (*pfn_sl_get_plugin_function)(const char* functionName);
typedef bool (*pfn_sl_on_plugin_load)(sl::param::IParameters*, const char*, const char**);

// Two patch sites per API, exactly like OptiScaler's hk_K32_ / hk_KB_ pairs: kernel32
// exports are jump thunks into kernelbase, so a caller that imports kernelbase directly
// only ever touches the second site. Slot 0 = kernel32 (and ntdll for LdrLoadDll),
// slot 1 = kernelbase. Each slot keeps its own trampoline, so forwarding never loops.
pfn_llex     g_real_llex[2]{};
pfn_llw      g_real_llw[2]{};
pfn_gmh_w    g_real_gmh_w[2]{};
pfn_gmh_a    g_real_gmh_a[2]{};
pfn_gmh_ex_w g_real_gmh_ex_w[2]{};
pfn_gmh_ex_a g_real_gmh_ex_a[2]{};
pfn_ldr      g_real_ldr[2]{};

HMODULE g_real_core = nullptr;     // the driver's own nvngx.dll / _nvngx.dll
HMODULE g_self = nullptr;          // our module, handed out for the core's name
bool    g_kernel_installed = false;

// Loading the real core must not re-enter our own hooks (LoadLibrary ends in LdrLoadDll).
thread_local bool t_bypass = false;

struct Bypass
{
    Bypass() { t_bypass = true; }
    ~Bypass() { t_bypass = false; }
};

bool core_name_w(const wchar_t* path)
{
    if (path == nullptr) { return false; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    return _wcsicmp(base, L"nvngx.dll") == 0 || _wcsicmp(base, L"_nvngx.dll") == 0;
}

bool core_name_a(const char* path)
{
    if (path == nullptr) { return false; }
    const char* base = strrchr(path, '\\');
    base = (base != nullptr) ? base + 1 : path;
    return _stricmp(base, "nvngx.dll") == 0 || _stricmp(base, "_nvngx.dll") == 0;
}
// The real core: use the path the caller handed over (Streamline passes the DriverStore
// copy). Guarded against re-entering our own hooks.
void ensure_real_core(const wchar_t* name)
{
    if (g_real_core != nullptr || name == nullptr) { return; }

    Bypass bypass;
    HMODULE m = nullptr;
    if (g_real_llex[0] != nullptr) {
        m = g_real_llex[0](name, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    } else if (g_real_llw[0] != nullptr) {
        m = g_real_llw[0](name);
    }
    if (m == nullptr || m == g_self) {
        logf("kernel: could not load the real NGX core (%ls), GetLastError=%lu\n", name,
             GetLastError());
        return;
    }
    g_real_core = m;
    logf("kernel: real NGX core = %p (%ls)\n", (void*)m, name);
}

bool is_sl_plugin(const wchar_t* path);
void hook_sl_plugin(HMODULE m);

// Every module that loads is offered to hook_sl_plugin(): plugin copies carry hashed names
// (170_E658700.dll) and the requested name is often just the base name, so the module's
// own path is the only reliable classifier - and it self-filters on `slGetPluginFunction`.
//
// Answering the NGX CORE name with our own module is a separate decision and off by
// default (`NgxCoreProxy`): OptiScaler never impersonates nvngx.dll/_nvngx.dll - it only
// exists as `nvngx_dlssg.dll`, the DLSS-G *feature* module - and leaving DLSS-SR on the
// driver's real core is what keeps the game's DLSS option alive.
template <int SET>
HMODULE WINAPI hook_llex(LPCWSTR name, HMODULE file, DWORD flags)
{
    if (!t_bypass && cfg().ngx_core_proxy != 0 && core_name_w(name)) {
        ensure_real_core(name);
        static bool told = false;
        if (!told) { told = true; logf("kernel: LoadLibraryExW(%ls) -> our module %p\n", name, (void*)g_self); }
        return g_self;
    }
    HMODULE m = (g_real_llex[SET] != nullptr) ? g_real_llex[SET](name, file, flags) : nullptr;
    if (!t_bypass && m != nullptr && m != g_self) { hook_sl_plugin(m); ffx_module_loaded(m); }
    return m;
}

template <int SET>
HMODULE WINAPI hook_llw(LPCWSTR name) { return hook_llex<SET>(name, nullptr, 0); }

template <int SET>
HMODULE WINAPI hook_gmh_w(LPCWSTR name)
{
    if (!t_bypass && cfg().ngx_core_proxy != 0 && core_name_w(name)) {
        static bool told = false;
        if (!told) { told = true; logf("kernel: GetModuleHandleW(%ls) -> our module %p\n", name, (void*)g_self); }
        return g_self;
    }
    return (g_real_gmh_w[SET] != nullptr) ? g_real_gmh_w[SET](name) : nullptr;
}

template <int SET>
HMODULE WINAPI hook_gmh_a(LPCSTR name)
{
    if (!t_bypass && cfg().ngx_core_proxy != 0 && core_name_a(name)) { return g_self; }
    return (g_real_gmh_a[SET] != nullptr) ? g_real_gmh_a[SET](name) : nullptr;
}

template <int SET>
BOOL WINAPI hook_gmh_ex_w(DWORD flags, LPCWSTR name, HMODULE* out)
{
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: `name` is a code address, not a string.
    // Kernel_Hooks.cpp:239 never compares a name unless the flags are exactly PIN; the
    // first version here ran wcsrchr() over code and died in ucrtbase (measured:
    // GetModuleHandleExW from is_own_module -> AV inside wcsrchr).
    const bool by_name = (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0;
    if (!t_bypass && by_name && cfg().ngx_core_proxy != 0 && name != nullptr &&
        core_name_w(name) && out != nullptr) {
        *out = g_self;
        return TRUE;
    }
    return (g_real_gmh_ex_w[SET] != nullptr) ? g_real_gmh_ex_w[SET](flags, name, out) : FALSE;
}

template <int SET>
BOOL WINAPI hook_gmh_ex_a(DWORD flags, LPCSTR name, HMODULE* out)
{
    const bool by_name = (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0;
    if (!t_bypass && by_name && cfg().ngx_core_proxy != 0 && name != nullptr &&
        core_name_a(name) && out != nullptr) {
        *out = g_self;
        return TRUE;
    }
    return (g_real_gmh_ex_a[SET] != nullptr) ? g_real_gmh_ex_a[SET](flags, name, out) : FALSE;
}

// ntdll!LdrLoadDll - what Streamline really uses for its plugins and runtimes.
template <int SET>
NTSTATUS NTAPI hook_ldr(PWSTR path, PULONG flags, PUNICODE_STRING file_name, PHANDLE out)
{
    if (!t_bypass && file_name != nullptr && file_name->Buffer != nullptr && out != nullptr) {
        const std::wstring name(file_name->Buffer, file_name->Length / sizeof(wchar_t));
        if (cfg().ngx_core_proxy != 0 && core_name_w(name.c_str())) {
            ensure_real_core(name.c_str());
            *out = reinterpret_cast<HANDLE>(g_self);
            static bool told = false;
            if (!told) { told = true; logf("kernel: LdrLoadDll(%ls) -> our module %p\n", name.c_str(), (void*)g_self); }
            return 0;   // STATUS_SUCCESS
        }
    }
    const NTSTATUS status = (g_real_ldr[SET] != nullptr)
                                ? g_real_ldr[SET](path, flags, file_name, out)
                                : static_cast<NTSTATUS>(0xC0000135);
    if (!t_bypass && status == 0 && out != nullptr) {
        hook_sl_plugin(reinterpret_cast<HMODULE>(*out));
    }
    return status;
}
// ---------------------------------------------------------------- architecture gate
// Streamline_Hooks.h:13-41 are OptiScaler's own copies of NVIDIA's SystemCaps layout, and
// Streamline_Hooks.cpp:925 spoofArch() is what opens the DLSS-G gate: the SL2 plugin
// parameter block `sl.param.common.gpuInfo` is read, and when the highest architecture is
// below the target the adapters are set to `maxArch` (0xFFFFFFFF).
struct SlSystemAdapter
{
    LUID     id{};
    uint32_t vendor{};
    uint32_t bit{};
    uint32_t architecture{};
    uint32_t implementation{};
    uint32_t revision{};
    uint32_t deviceId{};
    void*    nativeInterface{};
};

constexpr uint32_t kMaxSlGpus = 8;

struct SlSystemCaps
{
    uint32_t        gpuCount{};
    uint32_t        osVersionMajor{};
    uint32_t        osVersionMinor{};
    uint32_t        osVersionBuild{};
    uint32_t        driverVersionMajor{};
    uint32_t        driverVersionMinor{};
    SlSystemAdapter adapters[kMaxSlGpus]{};
    uint32_t        gpuLoad[kMaxSlGpus]{};
    bool            hwsSupported{};
    bool            laptopDevice{};
};

constexpr uint32_t kNvArchMax = 0xFFFFFFFF;   // spoofArch(): `maxArch`

pfn_sl_get_plugin_function g_plugin_original[kMaxPluginSlots]{};
HMODULE                    g_plugin_module[kMaxPluginSlots]{};
int                        g_plugin_kind[kMaxPluginSlots]{};   // 1 = DLSS, 2 = DLSS-G, 0 other
int                        g_plugin_count = 0;
pfn_sl_on_plugin_load      g_sl_on_plugin_load[kMaxPluginSlots]{};

// Streamline_Hooks.cpp:1048 hkdlssg_slOnPluginLoad: OptiScaler removes the DLSS-G
// plugin's own swapchain hooks when it owns the DLSSG path. We keep the plugin loaded
// (so its NVNGX caller still reaches our provider) but strip the hooks that would make
// the render GPU present its generated frames.
void patch_dlssg_plugin_json(const char** plugin_json)
{
    if (plugin_json == nullptr || *plugin_json == nullptr) { return; }
    try {
        auto j = nlohmann::json::parse(*plugin_json);
        bool changed = false;
        if (j.contains("hooks")) { j["hooks"].clear(); changed = true; }
        if (j.contains("exclusive_hooks")) { j["exclusive_hooks"].clear(); changed = true; }
        if (j.contains("external") && j["external"].contains("feature") &&
            j["external"]["feature"].contains("tags")) {
            j["external"]["feature"]["tags"].clear();
            changed = true;
        }
        if (!changed) { return; }
        static std::string patched;
        patched = j.dump();
        *plugin_json = patched.c_str();
        logf("sl: DLSS-G plugin JSON hooks cleared (capture owns presentation)\n");
    } catch (const std::exception& e) {
        logf("sl: DLSS-G plugin JSON patch failed: %s\n", e.what());
    }
}

// Streamline_Hooks.cpp:1043 hkdlssg_slOnPluginLoad: only the DLSS-G plugin gets its
// architecture raised; every other plugin is forwarded untouched.
template <int N>
bool sl_on_plugin_load(sl::param::IParameters* params, const char* loader_json,
                       const char** plugin_json)
{
    SlSystemCaps* caps = nullptr;
    if (params != nullptr) {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &caps, true);
    }
    uint32_t saved[kMaxSlGpus]{};
    bool     spoofed = false;
    if (caps != nullptr) {
        const uint32_t count = (caps->gpuCount < kMaxSlGpus) ? caps->gpuCount : kMaxSlGpus;
        uint32_t current = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (caps->adapters[i].architecture > current) { current = caps->adapters[i].architecture; }
        }
        static int told = 0;
        if (told < 6) {
            ++told;
            logf("arch: plugin kind=%d SystemCaps arch=0x%X gpus=%u\n", g_plugin_kind[N],
                 current, caps->gpuCount);
        }
        if (g_plugin_kind[N] == 2 && current != 0 && cfg().arch_spoof != 0) {
            for (uint32_t i = 0; i < count; ++i) {
                saved[i] = caps->adapters[i].architecture;
                caps->adapters[i].architecture = kNvArchMax;
            }
            spoofed = true;
            logf("arch: SystemCaps spoofed 0x%X -> 0x%X for the DLSS-G plugin\n", current,
                 kNvArchMax);
        }
    }

    const bool ok = (g_sl_on_plugin_load[N] != nullptr)
                        ? g_sl_on_plugin_load[N](params, loader_json, plugin_json)
                        : true;   // `true` = plugin load accepted, as OptiScaler answers

    if (g_plugin_kind[N] == 2 && plugin_json != nullptr &&
        cfg().fg_output == static_cast<int>(FgOutputSink::Capture) &&
        cfg().fg_input == static_cast<int>(FgInputSource::NVNGX)) {
        patch_dlssg_plugin_json(plugin_json);
    }

    // Streamline_Hooks.cpp:962-965 / 1055-1076 put it back right after the plugin's own
    // load:  `auto result = o_dlssg_slOnPluginLoad(...);  if (shouldSpoofArch)
    // setArch(currentArch);`  The block is shared (sl.param.common.gpuInfo), so a value
    // left at 0xFFFFFFFF leaks into every plugin that loads afterwards - DLSS-SR then asks
    // NGX for an architecture the driver has no model for, and the game drops the DLSS
    // option from its menu entirely (measured on CP2077).
    if (spoofed) {
        const uint32_t count = (caps->gpuCount < kMaxSlGpus) ? caps->gpuCount : kMaxSlGpus;
        for (uint32_t i = 0; i < count; ++i) { caps->adapters[i].architecture = saved[i]; }
        logf("arch: SystemCaps restored to 0x%X after the DLSS-G plugin load\n", saved[0]);
    }
    return ok;
}

void* sl_on_plugin_load_for(int slot)
{
    switch (slot) {
    case 0: return reinterpret_cast<void*>(&sl_on_plugin_load<0>);
    case 1: return reinterpret_cast<void*>(&sl_on_plugin_load<1>);
    case 2: return reinterpret_cast<void*>(&sl_on_plugin_load<2>);
    case 3: return reinterpret_cast<void*>(&sl_on_plugin_load<3>);
    case 4: return reinterpret_cast<void*>(&sl_on_plugin_load<4>);
    case 5: return reinterpret_cast<void*>(&sl_on_plugin_load<5>);
    case 6: return reinterpret_cast<void*>(&sl_on_plugin_load<6>);
    default: return reinterpret_cast<void*>(&sl_on_plugin_load<7>);
    }
}

// Streamline_Hooks.cpp:1583 hkdlss_slGetPluginFunction: the plugin's own entry table is
// handed out, except `slOnPluginLoad`, which is answered by us.
template <int N>
void* plugin_get_function(const char* functionName)
{
    if (functionName == nullptr || g_plugin_original[N] == nullptr) { return nullptr; }

    if (strcmp(functionName, "slOnPluginLoad") == 0) {
        g_sl_on_plugin_load[N] =
            reinterpret_cast<pfn_sl_on_plugin_load>(g_plugin_original[N](functionName));
        static int told = 0;
        if (told < 6) {
            ++told;
            logf("sl: plugin %p (kind %d) slOnPluginLoad -> our hook\n",
                 (void*)g_plugin_module[N], g_plugin_kind[N]);
        }
        return sl_on_plugin_load_for(N);
    }
    return g_plugin_original[N](functionName);
}

void* plugin_get_function_for(int slot)
{
    switch (slot) {
    case 0: return reinterpret_cast<void*>(&plugin_get_function<0>);
    case 1: return reinterpret_cast<void*>(&plugin_get_function<1>);
    case 2: return reinterpret_cast<void*>(&plugin_get_function<2>);
    case 3: return reinterpret_cast<void*>(&plugin_get_function<3>);
    case 4: return reinterpret_cast<void*>(&plugin_get_function<4>);
    case 5: return reinterpret_cast<void*>(&plugin_get_function<5>);
    case 6: return reinterpret_cast<void*>(&plugin_get_function<6>);
    default: return reinterpret_cast<void*>(&plugin_get_function<7>);
    }
}

// Streamline's plugin copies live in ...\sl_dlss_0\versions\NNN\files\190_XXXXXXX.dll
bool is_sl_plugin(const wchar_t* path)
{
    if (path == nullptr) { return false; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    if (_wcsnicmp(base, L"sl.", 3) == 0) { return true; }
    return wcsstr(path, L"\\sl_dlss_") != nullptr || wcsstr(path, L"\\sl_dlss_g_") != nullptr ||
           wcsstr(path, L"\\sl_reflex_") != nullptr || wcsstr(path, L"\\sl_common_") != nullptr ||
           wcsstr(path, L"\\sl_pcl_") != nullptr;
}

int plugin_kind_of(const wchar_t* path)
{
    if (path == nullptr || !is_sl_plugin(path)) { return 0; }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    if (wcsstr(path, L"\\sl_dlss_g_") != nullptr || _wcsicmp(base, L"sl.dlss_g.dll") == 0) { return 2; }
    if (wcsstr(path, L"\\sl_dlss_") != nullptr || _wcsicmp(base, L"sl.dlss.dll") == 0) { return 1; }
    return 0;
}

void hook_sl_plugin(HMODULE m)
{
    if (m == nullptr || m == g_self) { return; }

    // Signal the loader thread the moment the interposer appears (OptiScaler installs its
    // Streamline hooks from LibraryLoad_Hooks; the install itself still has to happen off
    // this thread, so the hook only signals).
    {
        wchar_t probe[MAX_PATH]{};
        if (GetModuleFileNameW(m, probe, MAX_PATH) != 0) {
            const wchar_t* base = wcsrchr(probe, L'\\');
            base = (base != nullptr) ? base + 1 : probe;
            if (_wcsicmp(base, L"sl.interposer.dll") == 0) { sl_note_interposer_loaded(); }
        }
    }

    // NGX runtimes hook themselves as they load (OptiScaler hooks module exports at load,
    // it does not scan the process repeatedly).
    nvngx_module_loaded(m);

    if (g_plugin_count >= kMaxPluginSlots) { return; }
    for (int i = 0; i < g_plugin_count; ++i) {
        if (g_plugin_module[i] == m) { return; }
    }

    void* addr = reinterpret_cast<void*>(GetProcAddress(m, "slGetPluginFunction"));
    if (addr == nullptr) { return; }

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);

    const int slot = g_plugin_count;
    g_plugin_original[slot] = reinterpret_cast<pfn_sl_get_plugin_function>(addr);
    g_plugin_module[slot] = m;
    g_plugin_kind[slot] = plugin_kind_of(path);
    if (MH_CreateHook(addr, plugin_get_function_for(slot),
                      reinterpret_cast<void**>(&g_plugin_original[slot])) != MH_OK ||
        MH_EnableHook(addr) != MH_OK) {
        g_plugin_original[slot] = nullptr;
        g_plugin_module[slot] = nullptr;
        g_plugin_kind[slot] = 0;
        return;
    }
    ++g_plugin_count;
    logf("sl: plugin module %p (kind %d) hooked: %ls\n", (void*)m, g_plugin_kind[slot], path);
}

bool hook_one(HMODULE module, const char* name, void* hook, void** original)
{
    if (module == nullptr) { return false; }
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(module, name));
    if (addr == nullptr) { return false; }
    // Create only. Enabling is batched by kernel_install_if_possible(): every
    // MH_EnableHook() suspends all threads, and OptiScaler installs a whole batch in one
    // Detour transaction (DetourTransactionBegin/Commit). MH_ALL_HOOKS is the MinHook
    // equivalent - one suspension instead of one per hook, which is the startup stutter.
    if (MH_CreateHook(addr, hook, original) != MH_OK) { return false; }
    return true;
}

// One patch site: kernel32 (SET 0) or kernelbase (SET 1). kernel32's exports are jump
// thunks into kernelbase, so both have to be installed for callers of either to be seen.
template <int SET>
int install_set(HMODULE m)
{
    if (m == nullptr) { return 0; }
    int n = 0;
    n += hook_one(m, "LoadLibraryExW", reinterpret_cast<void*>(&hook_llex<SET>),
                  reinterpret_cast<void**>(&g_real_llex[SET])) ? 1 : 0;
    n += hook_one(m, "LoadLibraryW", reinterpret_cast<void*>(&hook_llw<SET>),
                  reinterpret_cast<void**>(&g_real_llw[SET])) ? 1 : 0;
    n += hook_one(m, "GetModuleHandleW", reinterpret_cast<void*>(&hook_gmh_w<SET>),
                  reinterpret_cast<void**>(&g_real_gmh_w[SET])) ? 1 : 0;
    n += hook_one(m, "GetModuleHandleA", reinterpret_cast<void*>(&hook_gmh_a<SET>),
                  reinterpret_cast<void**>(&g_real_gmh_a[SET])) ? 1 : 0;
    n += hook_one(m, "GetModuleHandleExW", reinterpret_cast<void*>(&hook_gmh_ex_w<SET>),
                  reinterpret_cast<void**>(&g_real_gmh_ex_w[SET])) ? 1 : 0;
    n += hook_one(m, "GetModuleHandleExA", reinterpret_cast<void*>(&hook_gmh_ex_a<SET>),
                  reinterpret_cast<void**>(&g_real_gmh_ex_a[SET])) ? 1 : 0;
    return n;
}

} // namespace

// Called from the loader thread, before Streamline loads anything.
//
// These hooks have two jobs and only one of them is optional:
//   * hook the Streamline plugin modules when they load (LibraryLoad_Hooks.cpp is the
//     same layer in OptiScaler) - always wanted, it is how the arch gate and the
//     slOnPluginLoad redirection exist at all;
//   * answer the NGX core name with our own module - opt-in, `NgxCoreProxy`.
void kernel_install_if_possible()
{
    if (g_kernel_installed) { return; }

    // PIN: we are handed out as the NGX core module without a reference of our own, so a
    // caller's FreeLibrary must never unload us.
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&kernel_install_if_possible), &g_self) == 0 ||
        g_self == nullptr) {
        logf("kernel: cannot resolve our own module\n");
        return;
    }
    const MH_STATUS mi = MH_Initialize();
    if (mi != MH_OK && mi != MH_ERROR_ALREADY_INITIALIZED) {
        logf("kernel: MinHook init failed (%d)\n", static_cast<int>(mi));
        return;
    }

    HMODULE k32   = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb    = GetModuleHandleW(L"kernelbase.dll");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    int hooked = install_set<0>(k32);
    hooked += install_set<1>(kb);
    hooked += hook_one(ntdll, "LdrLoadDll", reinterpret_cast<void*>(&hook_ldr<0>),
                       reinterpret_cast<void**>(&g_real_ldr[0])) ? 1 : 0;

    // One thread suspension for the whole batch (OptiScaler: one Detour transaction).
    if (hooked > 0 && MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        logf("kernel: MH_EnableHook(all) failed - the module layer stays inert\n");
        hooked = 0;
    }

    g_kernel_installed = hooked > 0;
    logf("kernel: %d module-lookup hooks (kernel32 + kernelbase: LoadLibrary /"
         " GetModuleHandle / LdrLoadDll) - the NGX core resolves to our module\n", hooked);
}

HMODULE ngx_real_core()
{
    return g_real_core;
}

} // namespace fakerfg



