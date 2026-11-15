// ============================================================================
//  fg_swapchain_hooks.cpp - the FG swap chain, hooked the way OptiScaler does it.
//
//  OptiScaler hands the title the swap chain libxess created and then detours the
//  concrete object's vtable slots (FGHooks::HookFGSwapchain, FG_Hooks.cpp:617-680):
//  Release 2, Present 8, SetFullscreenState 10, GetFullscreenState 11, ResizeBuffers 13,
//  ResizeTarget 14, GetFullscreenDesc 19, Present1 22, GetFrameLatencyWaitableObject 33,
//  ResizeBuffers1 39.
//
//  Same here: `fg_hook_swapchain()` detours those slots of the chain we own, every detour
//  forwards unless `This` is that chain, and everything not detoured is the library's own
//  implementation - so the title sees the real object, exactly as it would with OptiScaler
//  (no proxy, no facade, no identity difference).
// ============================================================================
#include "fg_internal.h"

#include <MinHook.h>

namespace fakerfg {
namespace {

IDXGISwapChain* g_hooked_chain = nullptr;

bool    g_excl_requested = false;
bool    g_borderless_pending = false;
bool    g_window_saved = false;
LONG_PTR g_saved_style = 0;
LONG_PTR g_saved_ex = 0;
RECT    g_saved_rect{};
IDXGIOutput* g_pending_output = nullptr;
HANDLE  g_waitable_semaphore = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT,
                                                 const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetFullscreenState)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT,
                                                       DXGI_FORMAT, UINT, const UINT*,
                                                       IUnknown* const*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetFullscreenDesc)(IDXGISwapChain*,
                                                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC*);
typedef HANDLE(STDMETHODCALLTYPE* PFN_GetFrameLatencyWaitableObject)(IDXGISwapChain*);

PFN_Present                       o_present = nullptr;
PFN_Present1                      o_present1 = nullptr;
PFN_SetFullscreenState            o_set_fs = nullptr;
PFN_GetFullscreenState            o_get_fs = nullptr;
PFN_ResizeBuffers                 o_resize = nullptr;
PFN_ResizeBuffers1                o_resize1 = nullptr;
PFN_ResizeTarget                  o_resize_target = nullptr;
PFN_GetFullscreenDesc             o_get_fs_desc = nullptr;
PFN_GetFrameLatencyWaitableObject o_get_waitable = nullptr;

HWND chain_hwnd(IDXGISwapChain* chain)
{
    IDXGISwapChain1* sc1 = nullptr;
    HWND h = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr) {
        sc1->GetHwnd(&h);
        sc1->Release();
    }
    return h;
}

void apply_borderless(IDXGISwapChain* chain, IDXGIOutput* target)
{
    HWND h = chain_hwnd(chain);
    if (h == nullptr) { return; }
    if (IsIconic(h)) { g_borderless_pending = true; return; }
    HMONITOR mon = nullptr;
    if (target != nullptr) {
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(target->GetDesc(&od))) { mon = od.Monitor; }
    }
    if (mon == nullptr) { mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST); }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) { return; }

    const LONG_PTR style = WS_POPUP | WS_VISIBLE;
    const LONG_PTR ex_style = WS_EX_APPWINDOW;
    RECT r{};
    GetWindowRect(h, &r);
    const long mw = mi.rcMonitor.right - mi.rcMonitor.left;
    const long mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (GetWindowLongPtrW(h, GWL_STYLE) == style && GetWindowLongPtrW(h, GWL_EXSTYLE) == ex_style &&
        r.left == mi.rcMonitor.left && r.top == mi.rcMonitor.top &&
        (r.right - r.left) == mw && (r.bottom - r.top) == mh) {
        return;
    }
    if (!g_window_saved) {
        g_saved_style = GetWindowLongPtrW(h, GWL_STYLE);
        g_saved_ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        g_saved_rect = r;
        g_window_saved = true;
    }
    SetWindowLongPtrW(h, GWL_STYLE, style);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex_style);
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    static bool told = false;
    if (!told) {
        told = true;
        logf("xefg: borderless emulation applied (%ldx%ld at %ld,%ld)\n", mw, mh,
             mi.rcMonitor.left, mi.rcMonitor.top);
    }
}

void restore_window(IDXGISwapChain* chain, UINT width, UINT height)
{
    HWND h = chain_hwnd(chain);
    if (h == nullptr) { return; }
    if (IsIconic(h)) { g_borderless_pending = true; return; }
    if (g_window_saved) {
        SetWindowLongPtrW(h, GWL_STYLE, g_saved_style);
        SetWindowLongPtrW(h, GWL_EXSTYLE, g_saved_ex);
        SetWindowPos(h, nullptr, g_saved_rect.left, g_saved_rect.top,
                     g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        g_window_saved = false;
        return;
    }
    if (width == 0 || height == 0) { return; }
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT r{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_OVERLAPPEDWINDOW);
    SetWindowLongPtrW(h, GWL_STYLE, style);
    SetWindowLongPtrW(h, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

void apply_pending_borderless(IDXGISwapChain* chain)
{
    if (!g_borderless_pending || !force_borderless()) { return; }
    g_borderless_pending = false;
    IDXGIOutput* target = g_pending_output;
    g_pending_output = nullptr;
    if (g_excl_requested) { apply_borderless(chain, target); }
    else if (g_window_saved) { restore_window(chain, 0, 0); }
    if (target != nullptr) { target->Release(); }
}

UINT keep_flags(IDXGISwapChain* chain, UINT wanted)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc))) { return wanted; }
    UINT flags = wanted;
    if (force_borderless()) { flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; }
    if (desc.Flags != flags) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("xefg: preventing ResizeBuffers flag change 0x%X -> 0x%X (keeping 0x%X)\n",
                 static_cast<unsigned>(desc.Flags), static_cast<unsigned>(flags),
                 static_cast<unsigned>(desc.Flags));
        }
        flags = desc.Flags;
    }
    return flags;
}

void release_waitable()
{
    if (g_waitable_semaphore != nullptr && simulate_waitable_object()) {
        ReleaseSemaphore(g_waitable_semaphore, 1, nullptr);
    }
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* This, UINT sync, UINT flags)
{
    if (This != g_hooked_chain || o_present == nullptr) {
        return (o_present != nullptr) ? o_present(This, sync, flags) : E_FAIL;
    }
    apply_pending_borderless(This);
    if ((flags & DXGI_PRESENT_TEST) == 0 && !xefg_should_present()) {
        release_waitable();
        return S_OK;
    }
    xefg_before_present((flags & DXGI_PRESENT_TEST) == 0);
    const HRESULT hr = o_present(This, sync, flags);
    // v22: present_ring_commit() removed (the publication is the identity).
    release_waitable();
    xefg_present_result(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain* This, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params)
{
    if (This != g_hooked_chain || o_present1 == nullptr) {
        return (o_present1 != nullptr) ? o_present1(This, sync, flags, params) : E_FAIL;
    }
    apply_pending_borderless(This);
    if ((flags & DXGI_PRESENT_TEST) == 0 && !xefg_should_present()) {
        release_waitable();
        return S_OK;
    }
    xefg_before_present((flags & DXGI_PRESENT_TEST) == 0);
    const HRESULT hr = o_present1(This, sync, flags, params);
    // v22: present_ring_commit() removed (the publication is the identity).
    release_waitable();
    xefg_present_result(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain* This, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT format, UINT flags)
{
    if (This != g_hooked_chain || o_resize == nullptr) {
        return (o_resize != nullptr) ? o_resize(This, count, w, h, format, flags) : E_FAIL;
    }
    const HRESULT hr = o_resize(This, count, w, h, format, keep_flags(This, flags));
    xefg_after_resize();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize_buffers1(IDXGISwapChain* This, UINT count, UINT w, UINT h,
                                             DXGI_FORMAT format, UINT flags, const UINT* nodes,
                                             IUnknown* const* queues)
{
    if (This != g_hooked_chain || o_resize1 == nullptr) {
        return (o_resize1 != nullptr) ? o_resize1(This, count, w, h, format, flags, nodes, queues)
                                      : E_FAIL;
    }
    const HRESULT hr = o_resize1(This, count, w, h, format,
                                 keep_flags(This, flags), nodes, queues);
    xefg_after_resize();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize_target(IDXGISwapChain* This, const DXGI_MODE_DESC* target)
{
    if (This != g_hooked_chain || o_resize_target == nullptr) {
        return (o_resize_target != nullptr) ? o_resize_target(This, target) : E_FAIL;
    }
    if (force_borderless() && g_excl_requested) { return S_OK; }
    return o_resize_target(This, target);
}

HRESULT STDMETHODCALLTYPE hk_set_fullscreen(IDXGISwapChain* This, BOOL fullscreen,
                                            IDXGIOutput* target)
{
    if (This != g_hooked_chain || o_set_fs == nullptr) {
        return (o_set_fs != nullptr) ? o_set_fs(This, fullscreen, target) : E_FAIL;
    }
    logf("xefg: SetFullscreenState(%d, target=%p) borderless=%d requested=%d\n",
         static_cast<int>(fullscreen), static_cast<void*>(target), force_borderless() ? 1 : 0,
         g_excl_requested ? 1 : 0);
    if (force_borderless()) {
        if (fullscreen) {
            g_excl_requested = true;
            g_borderless_pending = true;
            if (g_pending_output != nullptr) { g_pending_output->Release(); g_pending_output = nullptr; }
            if (target != nullptr) { target->AddRef(); g_pending_output = target; }
            return S_OK;
        }
        if (g_excl_requested) {
            g_excl_requested = false;
            g_borderless_pending = false;
            if (g_pending_output != nullptr) { g_pending_output->Release(); g_pending_output = nullptr; }
            DXGI_SWAP_CHAIN_DESC desc{};
            This->GetDesc(&desc);
            restore_window(This, desc.BufferDesc.Width, desc.BufferDesc.Height);
            return S_OK;
        }
    }
    return o_set_fs(This, fullscreen, target);
}

HRESULT STDMETHODCALLTYPE hk_get_fullscreen(IDXGISwapChain* This, BOOL* fullscreen,
                                            IDXGIOutput** target)
{
    if (This != g_hooked_chain || o_get_fs == nullptr) {
        return (o_get_fs != nullptr) ? o_get_fs(This, fullscreen, target) : E_FAIL;
    }
    if (force_borderless() && g_excl_requested) {
        if (fullscreen != nullptr) { *fullscreen = TRUE; }
        if (target != nullptr && *target == nullptr) { This->GetContainingOutput(target); }
        return S_OK;
    }
    return o_get_fs(This, fullscreen, target);
}

HRESULT STDMETHODCALLTYPE hk_get_fs_desc(IDXGISwapChain* This,
                                         DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc)
{
    if (This != g_hooked_chain || o_get_fs_desc == nullptr) {
        return (o_get_fs_desc != nullptr) ? o_get_fs_desc(This, desc) : E_FAIL;
    }
    const HRESULT hr = o_get_fs_desc(This, desc);
    if (hr == S_OK && desc != nullptr && force_borderless() && g_excl_requested) {
        desc->Windowed = FALSE;
    }
    return hr;
}

HANDLE STDMETHODCALLTYPE hk_get_waitable(IDXGISwapChain* This)
{
    if (This != g_hooked_chain || o_get_waitable == nullptr) {
        return (o_get_waitable != nullptr) ? o_get_waitable(This) : nullptr;
    }
    if (!simulate_waitable_object()) { return o_get_waitable(This); }
    if (g_waitable_semaphore == nullptr) {
        g_waitable_semaphore = CreateSemaphoreW(nullptr, 16, 16, nullptr);
        if (g_waitable_semaphore == nullptr) { return nullptr; }
    }
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), g_waitable_semaphore, GetCurrentProcess(), &dup, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return dup;
}

} // namespace

// ---------------------------------------------------------------- install
// FGHooks::HookFGSwapchain(): the same slots, detoured with MinHook instead of Detours.
bool fg_hook_swapchain(IDXGISwapChain* chain)
{
    if (chain == nullptr) { return false; }
    if (g_hooked_chain != nullptr) { return g_hooked_chain == chain; }

    void** vt = *reinterpret_cast<void***>(chain);
    if (vt == nullptr || vt[8] == nullptr) { return false; }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        logf("xefg: MH_Initialize failed %d\n", static_cast<int>(st));
        return false;
    }

    o_present       = reinterpret_cast<PFN_Present>(vt[8]);
    o_set_fs        = reinterpret_cast<PFN_SetFullscreenState>(vt[10]);
    o_get_fs        = reinterpret_cast<PFN_GetFullscreenState>(vt[11]);
    o_resize        = reinterpret_cast<PFN_ResizeBuffers>(vt[13]);
    o_resize_target = reinterpret_cast<PFN_ResizeTarget>(vt[14]);
    o_get_fs_desc   = reinterpret_cast<PFN_GetFullscreenDesc>(vt[19]);
    o_present1      = reinterpret_cast<PFN_Present1>(vt[22]);
    o_get_waitable  = reinterpret_cast<PFN_GetFrameLatencyWaitableObject>(vt[33]);
    o_resize1       = reinterpret_cast<PFN_ResizeBuffers1>(vt[39]);

    // MinHook writes the TRAMPOLINE back into the original variable; passing nullptr here
    // leaves the original pointing at the patched entry, so the detour calls itself until
    // the stack is gone (measured: 0xC00000FD, rip bouncing in dxgi.dll+0xB4EA/+0xB4EF).
    auto hook = [&](int slot, void* detour, void** orig, const char* name) {
        if (vt[slot] == nullptr || orig == nullptr) { return; }
        *orig = vt[slot];
        const MH_STATUS c = MH_CreateHook(vt[slot], detour, orig);
        if (c == MH_OK) {
            const MH_STATUS e = MH_EnableHook(vt[slot]);
            logf("xefg: hooked swap chain %s (%s)\n", name,
                 e == MH_OK ? "enabled" : "enable failed");
            return;
        }

        // MinHook failed (PBZ: status 9 = MEMORY_ALLOC for the trampoline). Fall back to
        // replacing the vtable slot directly; *orig already holds the real entry, so the
        // detour's call chain stays intact. The vtable page is read-only in the module,
        // hence VirtualProtect. This is the OptiScaler shape and does not need trampolines.
        DWORD old_protect = 0;
        if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
            vt[slot] = detour;
            DWORD ignored = 0;
            VirtualProtect(&vt[slot], sizeof(void*), old_protect, &ignored);
            logf("xefg: swap chain %s hooked via manual vtable patch (MinHook %d)\n",
                 name, static_cast<int>(c));
            return;
        }
        logf("xefg: MH_CreateHook(%s) failed %d and VirtualProtect failed %lu\n",
             name, static_cast<int>(c), GetLastError());
    };

    hook(8, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&o_present), "Present");
    hook(22, reinterpret_cast<void*>(&hk_present1), reinterpret_cast<void**>(&o_present1), "Present1");
    hook(13, reinterpret_cast<void*>(&hk_resize_buffers), reinterpret_cast<void**>(&o_resize), "ResizeBuffers");
    hook(39, reinterpret_cast<void*>(&hk_resize_buffers1), reinterpret_cast<void**>(&o_resize1), "ResizeBuffers1");
    hook(14, reinterpret_cast<void*>(&hk_resize_target), reinterpret_cast<void**>(&o_resize_target), "ResizeTarget");
    hook(10, reinterpret_cast<void*>(&hk_set_fullscreen), reinterpret_cast<void**>(&o_set_fs), "SetFullscreenState");
    hook(11, reinterpret_cast<void*>(&hk_get_fullscreen), reinterpret_cast<void**>(&o_get_fs), "GetFullscreenState");
    hook(19, reinterpret_cast<void*>(&hk_get_fs_desc), reinterpret_cast<void**>(&o_get_fs_desc), "GetFullscreenDesc");
    if (simulate_waitable_object()) {
        hook(33, reinterpret_cast<void*>(&hk_get_waitable),
             reinterpret_cast<void**>(&o_get_waitable), "GetFrameLatencyWaitableObject");
    }

    g_hooked_chain = chain;      // identity check for every detour
    logf("xefg: swap chain vtable hooked (OptiScaler shape, object %p)\n",
         static_cast<void*>(chain));
    return true;
}

} // namespace fakerfg
