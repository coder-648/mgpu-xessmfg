// ============================================================================
//  proxy_dxgi.cpp - we are `dxgi.dll`, and that buys two things:
//
//    1. the early call site: a D3D12 title imports dxgi at load, so our DllMain
//       runs before slInit and before nvngx is resolved;
//    2. the swap chain: IDXGIFactory2::CreateSwapChainForHwnd is where a title
//       asks for its chain, and that is the only place XeFG can be made the
//       presenter - XeFG creates its own chain (InitFromSwapChainDesc) and hands
//       that back instead of the one the title asked for.
//
//  Technique: one vtable entry, replaced with a plain pointer write on a *copy*
//  of the factory's table (what ReShade does to COM objects, CAPTURE_RULES 2).
//  No wrapped factory is returned to the game: the object it receives is DXGI's
//  own, with one entry redirected. Everything else passes through untouched.
//
//  If anything is missing (no flip-model desc, no D3D12 queue, libxess_fg absent)
//  the original call runs and the title keeps its own chain - the FG pipeline is
//  then simply not active, and the provider refuses to answer (provider.cpp).
// ============================================================================
#include "fg_internal.h"

namespace fakerfg {
namespace {

HMODULE g_real_dxgi = nullptr;
bool    g_took_over = false;

constexpr int kVtSlots = 96;                    // IDXGIFactory7 is ~34; keep slack
// Slot arithmetic, 0-based: IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11,
// IDXGIFactory1 12-13, IDXGIFactory2 IsWindowedStereoEnabled 14, then
// CreateSwapChainForHwnd 15. (Index 16 is CreateSwapChainForCoreWindow - patching
// that one is what made the first live run silent.)
constexpr int kVtCreateSwapChainForHwnd = 15;
// IDXGIFactory::CreateSwapChain (the old DESC form). OptiScaler hooks it too
// (DxgiFactory_Hooks.cpp:300) and converts the desc to DESC1 before creating.
constexpr int kVtCreateSwapChain = 10;

// ReShade 6.7+ answers this IID with the object it wrapped. Anything we hand to
// XeFG has to be the driver's object, not ReShade's proxy (OptiScaler unwraps for
// exactly this reason - Util.cpp "GetReShadeOriginal").
constexpr GUID kReShadeUnwrapped = {
    0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};

// One entry per distinct factory class we meet. We patch the *shared* vtable entry
// (not one object's vtable pointer): the title may hold a factory it obtained from
// IDXGIDevice::GetAdapter()->GetParent() rather than from our export, and that
// object has the same table. Measured on CP2077: the object-level write never fired.
struct VtPatch
{
    void** vt = nullptr;
    void*  orig_create_swapchain = nullptr;   // slot 10 (IDXGIFactory), their CreateSwapChain
    void*  orig_create_for_hwnd = nullptr;    // slot 15
};

constexpr int kMaxPatches = 4;
VtPatch g_patches[kMaxPatches]{};
int     g_patch_count = 0;

typedef HRESULT(STDMETHODCALLTYPE* pfn_create_swapchain_for_hwnd)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

typedef HRESULT(STDMETHODCALLTYPE* pfn_create_swapchain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

HRESULT STDMETHODCALLTYPE hook_create_swapchain_for_hwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_to, IDXGISwapChain1** out);

HRESULT STDMETHODCALLTYPE hook_create_swapchain(
    IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);

VtPatch* patch_for(void** vt)
{
    for (int i = 0; i < g_patch_count; ++i) {
        if (g_patches[i].vt == vt) { return &g_patches[i]; }
    }
    return nullptr;
}

void* patch_slot(void** vt, int slot, void* hook, int which)
{
    void* target = vt[slot];
    if (target == nullptr) { return nullptr; }

    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) { return nullptr; }
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    (void)which;
    return target;
}

// Registers one factory class: slots 10 (CreateSwapChain) and 15 (CreateSwapChainForHwnd),
// the two entries OptiScaler hooks with Detour (DxgiFactory_Hooks.cpp:300/327).
VtPatch* ensure_patch(void** vt, const char* what)
{
    if (vt == nullptr) { return nullptr; }
    if (VtPatch* p = patch_for(vt)) { return p; }
    if (g_patch_count >= kMaxPatches) { return nullptr; }

    if (vt[kVtCreateSwapChainForHwnd] == nullptr) { return nullptr; }

    VtPatch& p = g_patches[g_patch_count];
    p.orig_create_swapchain =
        patch_slot(vt, kVtCreateSwapChain, reinterpret_cast<void*>(&hook_create_swapchain), 0);
    p.orig_create_for_hwnd = patch_slot(vt, kVtCreateSwapChainForHwnd,
                                        reinterpret_cast<void*>(&hook_create_swapchain_for_hwnd), 1);
    if (p.orig_create_for_hwnd == nullptr) { return nullptr; }

    p.vt = vt;
    ++g_patch_count;
    logf("dxgi: factory class #%d (%s) vtable=%p slots 10/15 hooked (CreateSwapChain=%p,"
         " ForHwnd=%p)\n", g_patch_count, what, (void*)vt, p.orig_create_swapchain,
         p.orig_create_for_hwnd);
    return &p;
}

// The original entry of whichever class this object belongs to.
pfn_create_swapchain_for_hwnd original_for(IDXGIFactory2* self)
{
    if (self == nullptr) { return nullptr; }
    VtPatch* p = patch_for(*reinterpret_cast<void***>(self));
    if (p == nullptr) { return nullptr; }
    return reinterpret_cast<pfn_create_swapchain_for_hwnd>(p->orig_create_for_hwnd);
}

pfn_create_swapchain original_create_swapchain(IDXGIFactory* self)
{
    if (self == nullptr) { return nullptr; }
    VtPatch* p = patch_for(*reinterpret_cast<void***>(self));
    if (p == nullptr) { return nullptr; }
    return reinterpret_cast<pfn_create_swapchain>(p->orig_create_swapchain);
}

template <typename T>
T* unwrap_reshade(T* object)
{
    if (object == nullptr) { return nullptr; }
    IUnknown* original = nullptr;
    if (object->QueryInterface(kReShadeUnwrapped, reinterpret_cast<void**>(&original)) != S_OK ||
        original == nullptr) {
        return nullptr;
    }
    T* typed = nullptr;
    const HRESULT hr = original->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&typed));
    original->Release();
    if (hr != S_OK || typed == nullptr) { return nullptr; }
    return typed;      // caller owns this reference
}

// Installs the entry on this factory's class (idempotent).
bool redirect_factory(IUnknown* factory, const char* what)
{
    if (factory == nullptr) { return false; }
    return ensure_patch(*reinterpret_cast<void***>(factory), what) != nullptr;
}

bool flip_model(DXGI_SWAP_EFFECT e)
{
    return e == DXGI_SWAP_EFFECT_FLIP_DISCARD || e == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
}

HRESULT STDMETHODCALLTYPE hook_create_swapchain(
    IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    auto original = original_create_swapchain(self);
    if (original == nullptr) { return E_FAIL; }
    if (desc == nullptr || out == nullptr) { return original(self, device, desc, out); }

    // DxgiFactoryHooks::CreateSwapChain: the old DESC form is converted to DESC1 and then
    // takes the same route as CreateSwapChainForHwnd.
    DXGI_SWAP_CHAIN_DESC1 d1{};
    d1.Width       = desc->BufferDesc.Width;
    d1.Height      = desc->BufferDesc.Height;
    d1.Format      = desc->BufferDesc.Format;
    d1.SampleDesc  = desc->SampleDesc;
    d1.BufferUsage = desc->BufferUsage;
    d1.BufferCount = desc->BufferCount;
    switch (desc->BufferDesc.Scaling) {
    case DXGI_MODE_SCALING_CENTERED:  d1.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH; break;
    case DXGI_MODE_SCALING_STRETCHED: d1.Scaling = DXGI_SCALING_STRETCH; break;
    default:                          d1.Scaling = DXGI_SCALING_NONE; break;
    }
    d1.SwapEffect = desc->SwapEffect;
    d1.AlphaMode  = DXGI_ALPHA_MODE_IGNORE;
    d1.Flags      = desc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.RefreshRate      = desc->BufferDesc.RefreshRate;
    fs.Scaling          = desc->BufferDesc.Scaling;
    fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fs.Windowed         = desc->Windowed;

    IDXGISwapChain1* chain1 = nullptr;
    const HRESULT hr = hook_create_swapchain_for_hwnd(
        reinterpret_cast<IDXGIFactory2*>(self), device, desc->OutputWindow, &d1, &fs,
        nullptr, &chain1);
    if (FAILED(hr) || chain1 == nullptr) { return original(self, device, desc, out); }

    IDXGISwapChain* chain = nullptr;
    const HRESULT qi = chain1->QueryInterface(IID_PPV_ARGS(&chain));
    chain1->Release();
    if (FAILED(qi) || chain == nullptr) { return original(self, device, desc, out); }

    *out = chain;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE hook_create_swapchain_for_hwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_to, IDXGISwapChain1** out)
{
    auto original = original_for(self);
    if (original == nullptr) { return E_FAIL; }

    // Every create is logged once: the count and the window tell whether the title ends up
    // with a second, real chain (two threads create at startup) - that is how a title can
    // render into one chain and present the other.
    if (desc != nullptr && out != nullptr) {
        static std::atomic<int> seq{ 0 };
        logf("dxgi: create#%d hwnd=%p %ux%u fmt=%u effect=%u flags=0x%X%s\n", ++seq,
             static_cast<void*>(hwnd), desc->Width, desc->Height,
             static_cast<unsigned>(desc->Format), static_cast<unsigned>(desc->SwapEffect),
             static_cast<unsigned>(desc->Flags),
             (fs != nullptr && fs->Windowed == FALSE) ? " FULLSCREEN" : "");
    }

    // Cheap retry: nvapi may only appear once the game has loaded its NGX stack,
    // which can be long after the loader thread gave up.
    arch_install_if_possible();
    sl_install_if_possible();

    const Config& c = cfg();
    bool want = c.own_swapchain != 0 && c.passthrough == 0;

    // NoFSRFGFirstSwapchain (GameConfigs.cpp, UE titles): the throw-away chain they build at
    // startup is left alone, so we neither pay its init nor inherit its size/format. Same
    // `firstCall` handling as FGHooks::CreateSwapChainForHwnd (DxgiFactory_Hooks.cpp:829).
    if (want && has_game_quirk(Quirk_NoFSRFGFirstSwapchain)) {
        static bool first_call = true;
        if (first_call) {
            first_call = false;
            logf("dxgi: NoFSRFGFirstSwapchain - first swap chain passes through\n");
            want = false;
        }
    }

    // The Steam overlay and friends create their own small swap chains through the
    // same factory. Taking those over would hand the *overlay* XeFG's chain and the
    // game would keep its own - so anything suspiciously small is left alone
    // (OptiScaler's Util::IsOverlaySwapchain does exactly this check).
    if (desc != nullptr && out != nullptr && want) {
        UINT w = desc->Width, h = desc->Height;
        if ((w == 0 || h == 0) && hwnd != nullptr) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                if (w == 0) { w = static_cast<UINT>(rc.right - rc.left); }
                if (h == 0) { h = static_cast<UINT>(rc.bottom - rc.top); }
            }
        }
        if (w < 100 || h < 100) {
            static bool told_overlay = false;
            if (!told_overlay) {
                told_overlay = true;
                logf("dxgi: %ux%u swap chain looks like an overlay - passing through\n", w, h);
            }
            return original(self, device, hwnd, desc, fs, restrict_to, out);
        }
    }

    if (!want || out == nullptr || desc == nullptr) {
        static bool told = false;
        if (!told && want) {
            told = true;
            logf("dxgi: swap chain left alone (effect=%u out=%p)\n",
                 desc ? static_cast<unsigned>(desc->SwapEffect) : 0u, (void*)out);
        }
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }

    // FGHooks.cpp:485-494 - DX12 has no bitblt presentation, so the library does not bail on
    // a non-flip description, it upgrades it:
    //     SEQUENTIAL -> FLIP_SEQUENTIAL
    //     DISCARD    -> FLIP_DISCARD
    // and a flip chain needs at least two buffers. Bailing here (the previous behaviour) left
    // UE5 titles - which ask for DISCARD - on their own chain, i.e. no FG at all.
    DXGI_SWAP_CHAIN_DESC1 flip{};
    const DXGI_SWAP_CHAIN_DESC1* use_desc = desc;
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL ||
        desc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD) {
        flip = *desc;
        flip.SwapEffect = (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
                              ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                              : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        use_desc = &flip;
        logf("dxgi: swap effect %u is not supported in DX12 - using %u for the FG chain\n",
             static_cast<unsigned>(desc->SwapEffect), static_cast<unsigned>(flip.SwapEffect));
    }
    if (flip.BufferCount != 0 && flip.BufferCount < 2) { flip.BufferCount = 2; }
    else if (use_desc == desc && desc->BufferCount != 0 && desc->BufferCount < 2) {
        flip = *desc;                       // flip model needs 2 buffers
        flip.BufferCount = 2;
        use_desc = &flip;
    }
    if (!flip_model(use_desc->SwapEffect)) {
        static bool told2 = false;
        if (!told2) {
            told2 = true;
            logf("dxgi: swap effect %u is not a flip model - leaving the chain alone\n",
                 static_cast<unsigned>(use_desc->SwapEffect));
        }
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }

    // IsOtherWindowWhileFGSwapchainExists (FG_Hooks.cpp:177-186, :312-313): only ONE window
    // may own the FG chain. A second window (launcher, editor, overlay helper) keeps its own
    // chain - OptiScaler answers E_NOINTERFACE there; passing through has the same effect and
    // cannot break a title that ignores the error.
    if (g_took_over && hwnd != nullptr && g_owner_hwnd != nullptr && hwnd != g_owner_hwnd) {
        static bool told_other = false;
        if (!told_other) {
            told_other = true;
            logf("dxgi: FG chain already belongs to window %p - %p keeps its own\n",
                 (void*)g_owner_hwnd, (void*)hwnd);
        }
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }

    if (g_took_over) {
        // XeFG_Dx12::CreateSwapchain's FGPreserveSwapChain branch: a second chain for the
        // SAME window gets the chain we already own (ResizeBuffers + AddRef), instead of a
        // second real chain the title would then render into - which is how a title that
        // re-creates its chain on a settings change ends up with a black window.
        if (IDXGISwapChain1* mine = xefg_owned_chain(hwnd, desc)) {
            *out = mine;                        // reference is ours to hand over
            logf("dxgi: same window asked for a second chain - reusing XeFG's %p\n", (void*)mine);
            return S_OK;
        }
        logf("dxgi: second swap chain requested - passing through (only the first is ours)\n");
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }

    // The queue, the device and the factory may be ReShade's proxies: XeFG must
    // get the driver's objects, so unwrap whatever answers the ReShade IID.
    ID3D12CommandQueue* queue = nullptr;
    if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) ||
        queue == nullptr) {
        logf("dxgi: swap chain device is not an ID3D12CommandQueue - passing through\n");
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }
    if (ID3D12CommandQueue* real = unwrap_reshade(queue)) {
        logf("dxgi: unwrapped ReShade's command queue (%p -> %p)\n", (void*)queue, (void*)real);
        queue->Release();
        queue = real;
    }

    ID3D12Device* d3d_device = nullptr;
    queue->GetDevice(IID_PPV_ARGS(&d3d_device));
    if (d3d_device != nullptr) {
        if (ID3D12Device* real = unwrap_reshade(d3d_device)) {
            logf("dxgi: unwrapped ReShade's device (%p -> %p)\n", (void*)d3d_device, (void*)real);
            d3d_device->Release();
            d3d_device = real;
        }
    }

    IDXGIFactory2* factory = self;
    IDXGIFactory2* real_factory = unwrap_reshade(self);
    if (real_factory != nullptr) {
        logf("dxgi: unwrapped ReShade's factory (%p -> %p)\n", (void*)self, (void*)real_factory);
        factory = real_factory;
    }

    // XeFG: "Interpolation is not supported in fullscreen mode." OptiScaler answers that with
    // FGXeFGForceBorderless (DxgiFactory_Hooks.cpp:487-497): the FG chain is created
    // borderless whatever the title asked for, ALLOW_MODE_SWITCH is dropped and the scaling
    // is STRETCH. The title's own chain is untouched - only the FG output is windowed.
    // The title's own request, before the desc is made windowed below: OptiScaler keeps it as
    // SCExclusiveFullscreenRequested, i.e. the borderless window is emulated from the first
    // present on (DxgiFactory_Hooks.cpp:487-494).
    const bool title_fullscreen = fs != nullptr && !fs->Windowed;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_borderless{};
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* use_fs = fs;
    DXGI_SWAP_CHAIN_DESC1 borderless{};
    if (title_fullscreen) {
        fs_borderless = *fs;
        fs_borderless.Windowed = TRUE;
        use_fs = &fs_borderless;
        borderless = *use_desc;          // either the title's desc or the flip-upgraded copy
        use_desc = &borderless;
        borderless.Flags = use_desc->Flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        borderless.Scaling = DXGI_SCALING_STRETCH;
        logf("dxgi: XeFG needs borderless - FG chain created windowed (title asked"
             " fullscreen)\n");
    }

    IDXGISwapChain1* chain = nullptr;
    const bool started = xefg_start(d3d_device, queue, factory, hwnd, use_desc, use_fs,
                                    title_fullscreen, &chain);

    if (real_factory != nullptr) { real_factory->Release(); }
    if (d3d_device != nullptr) { d3d_device->Release(); }
    queue->Release();

    if (!started || chain == nullptr) {
        logf("dxgi: XeFG could not take the swap chain - the game keeps its own"
             " (frame generation stays off, nothing else changes)\n");
        return original(self, device, hwnd, desc, fs, restrict_to, out);
    }

    g_took_over = true;
    *out = chain;
    logf("dxgi: handed the game XeFG's swap chain %p (%ux%u)\n", (void*)chain,
         desc->Width, desc->Height);
    return S_OK;
}

} // namespace

void dxgi_set_real_module(HMODULE m) { g_real_dxgi = m; }

HMODULE dxgi_real_module() { return g_real_dxgi; }

bool dxgi_install_factory_hook(IUnknown* factory)
{
    if (g_real_dxgi == nullptr) {
        wchar_t path[MAX_PATH]{};
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
        g_real_dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        logf("dxgi: real module %ls -> %p\n", path, (void*)g_real_dxgi);
        if (g_real_dxgi == nullptr) { return false; }
    }
    arch_install_if_possible();
    return redirect_factory(factory, "CreateDXGIFactory*");
}

bool dxgi_took_over_swapchain() { return g_took_over; }

namespace {

HMODULE ensure_real()
{
    if (g_real_dxgi != nullptr) { return g_real_dxgi; }
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
    g_real_dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    logf("dxgi: real module %ls -> %p\n", path, (void*)g_real_dxgi);
    return g_real_dxgi;
}

void* real_proc(const char* name)
{
    HMODULE m = ensure_real();
    return (m != nullptr) ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

} // namespace

HMODULE dxgi_ensure_real() { return ensure_real(); }

} // namespace fakerfg

// ============================================================================
//  The DXGI exports. Five of them are the ones a D3D12 title actually imports;
//  the rest are forwarded so that a static import table never fails to resolve
//  (that failure is "the game does not start", the least useful outcome).
// ============================================================================
extern "C" {

typedef HRESULT(WINAPI* pfn_create_factory)(REFIID, void**);
typedef HRESULT(WINAPI* pfn_create_factory2)(UINT, REFIID, void**);

HRESULT WINAPI fg_dxgi_create(REFIID riid, void** factory)
{
    auto fn = reinterpret_cast<pfn_create_factory>(fakerfg::real_proc("CreateDXGIFactory"));
    if (fn == nullptr) { return E_FAIL; }
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) {
        fakerfg::dxgi_install_factory_hook(static_cast<IUnknown*>(*factory));
    }
    return hr;
}

HRESULT WINAPI fg_dxgi_create1(REFIID riid, void** factory)
{
    auto fn = reinterpret_cast<pfn_create_factory>(fakerfg::real_proc("CreateDXGIFactory1"));
    if (fn == nullptr) { return E_FAIL; }
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) {
        fakerfg::dxgi_install_factory_hook(static_cast<IUnknown*>(*factory));
    }
    return hr;
}

HRESULT WINAPI fg_dxgi_create2(UINT flags, REFIID riid, void** factory)
{
    auto fn = reinterpret_cast<pfn_create_factory2>(fakerfg::real_proc("CreateDXGIFactory2"));
    if (fn == nullptr) { return E_FAIL; }
    const HRESULT hr = fn(flags, riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) {
        fakerfg::dxgi_install_factory_hook(static_cast<IUnknown*>(*factory));
    }
    return hr;
}

HRESULT WINAPI fg_dxgi_declare_removal()
{
    typedef HRESULT(WINAPI* pfn)();
    auto fn = reinterpret_cast<pfn>(fakerfg::real_proc("DXGIDeclareAdapterRemovalSupport"));
    return (fn != nullptr) ? fn() : E_NOTIMPL;
}

HRESULT WINAPI fg_dxgi_debug1(UINT flags, REFIID riid, void** dbg)
{
    typedef HRESULT(WINAPI* pfn)(UINT, REFIID, void**);
    auto fn = reinterpret_cast<pfn>(fakerfg::real_proc("DXGIGetDebugInterface1"));
    return (fn != nullptr) ? fn(flags, riid, dbg) : E_NOTIMPL;
}

} // extern "C"

// ---------------------------------------------------------------- blind forwards
// Anything else dxgi exports: x64 has a single calling convention, so a
// six-pointer thunk with the same return size covers every one of them.
extern "C" {

typedef void* (WINAPI* dxgi_pfn_any6)(void*, void*, void*, void*, void*, void*);

#define FG_DXGI_FORWARD_LIST(X)          \
    X(ApplyCompatResolutionQuirking)     \
    X(CompatString)                      \
    X(CompatValue)                       \
    X(DXGIDisableVBlankVirtualization)   \
    X(DXGIDumpJournal)                   \
    X(DXGIReportAdapterConfiguration)    \
    X(DXGID3D10CreateDevice)             \
    X(DXGID3D10CreateLayeredDevice)      \
    X(DXGID3D10GetLayeredDeviceSize)     \
    X(DXGID3D10RegisterLayers)           \
    X(PIXBeginCapture)                   \
    X(PIXEndCapture)                     \
    X(PIXGetCaptureState)                \
    X(SetAppCompatStringPointer)         \
    X(UpdateHMDEmulationStatus)

#define X(n)                                                                              \
    void* __cdecl dxgi_fwd_##n(void* a, void* b, void* c, void* d,  \
                                                     void* e, void* f)                     \
    {                                                                                     \
        static void* target = nullptr;                                                     \
        if (target == nullptr) { target = fakerfg::real_proc(#n); }                        \
        auto fn = reinterpret_cast<dxgi_pfn_any6>(target);                                 \
        return (fn != nullptr) ? fn(a, b, c, d, e, f) : nullptr;                           \
    }
FG_DXGI_FORWARD_LIST(X)
#undef X
#undef FG_DXGI_FORWARD_LIST

} // extern "C"
