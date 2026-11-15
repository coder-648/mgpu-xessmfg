// ============================================================================
//  fg_swapchain.h - the object the game gets instead of XeFG's chain.
//
//  OptiScaler's shape (wrapped/wrapped_swapchain.cpp, `WrappedIDXGISwapChain4`):
//  the game never receives the FG backend's own swap chain object; it receives a
//  thin wrapper that forwards every method to it. Their `LocalPresent()` then owns
//  the one place where FG meets presentation:
//
//      if (willPresent && fgFeatureActive) fg->Present();   // SetPresentId + present
//      ...
//      pSwapChain->Present(SyncInterval, Flags);            // the backend's chain
//
//  Copying that structure fixes two things our vtable patch could not:
//    * `SetPresentId` happens exactly once per present, before the present
//      (in Evaluate it drifts as soon as a frame is evaluated but not presented);
//    * nothing is written into the backend's object, so a truncated vtable cannot
//      redirect a method into garbage (that was a crash).
//
//  Every other method is a straight forward. QI answers our own interfaces and
//  delegates anything foreign to the real chain.
// ============================================================================
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>

namespace fakerfg {

void logf(const char* fmt, ...);   // fg_internal.h
// ForceBorderless: XeFG refuses exclusive fullscreen ("Interpolation is not supported in
// fullscreen mode"), so the facade swallows the title's SetFullscreenState(TRUE).
bool force_borderless();
bool simulate_waitable_object();   // OptiScaler SimulateWaitableObject

// Set by backend_xefg.cpp: what the present path must do.
struct PresentHooks
{
    void (*before_present)(void* user);
    // wrapped_swapchain.cpp:1212 -> `if (result == DXGI_ERROR_DEVICE_REMOVED)
    // Util::GetDeviceRemovedReason(...)`. The title only gets the HRESULT back; without
    // this the log never says whether a bad frame removed the device or hung it.
    void (*present_failed)(long hr, void* user);
    // wrapped_swapchain.cpp:1099-1104: a resize invalidates the FG output's buffers and
    // everything captured from them, so the feature pauses (UpdateTarget + Deactivate).
    void (*after_resize)(void* user);
    void* user = nullptr;
};

class FgSwapChain final : public IDXGISwapChain4
{
public:
    // `title_fullscreen`: the title created its chain with Windowed = FALSE
    // (DxgiFactory_Hooks.cpp:487-494 `SCExclusiveFullscreenRequested = !localDesc.Windowed`).
    // FGHooks::SetFGSwapchain arms the emulation right there (:572-573), so a title that never
    // calls SetFullscreenState - it just builds a fullscreen chain - still gets the borderless
    // window on its first present.
    FgSwapChain(IDXGISwapChain* real, PresentHooks hooks, bool title_fullscreen)
        : m_real(real), m_hooks(hooks)
    {
        m_exclusive_requested = force_borderless() && title_fullscreen;
        m_borderless_pending  = m_exclusive_requested;
        logf("xefg: facade created (borderless=%d, title asked fullscreen=%d)\n",
             force_borderless() ? 1 : 0, title_fullscreen ? 1 : 0);
    }

    // ---------------------------------------------------------------- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) { return E_POINTER; }
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IDXGIObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain1)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain2)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain3)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain4))) {
            AddRef();
            *out = this;
            return S_OK;
        }
        // Foreign IIDs belong to the backend's object, not to this facade.
        return m_real->QueryInterface(riid, out);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ~FgSwapChain()
    {
        if (m_pending_output != nullptr) { m_pending_output->Release(); }
        if (m_sc1 != nullptr) { m_sc1->Release(); }
        if (m_sc2 != nullptr) { m_sc2->Release(); }
        if (m_sc3 != nullptr) { m_sc3->Release(); }
        if (m_real != nullptr) { m_real->Release(); }
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0) { delete this; }
        return left;
    }

    // ---------------------------------------------------------------- IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override
    { return m_real->SetPrivateData(name, size, data); }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* obj) override
    { return m_real->SetPrivateDataInterface(name, obj); }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override
    { return m_real->GetPrivateData(name, size, data); }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    { return m_real->GetParent(riid, parent); }

    // ---------------------------------------------------------------- IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override
    { return m_real->GetDevice(riid, device); }

    // ---------------------------------------------------------------- IDXGISwapChain
    // The one method with a decision in it (see the file header).
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
    {
        apply_pending_borderless();
        if ((flags & DXGI_PRESENT_TEST) == 0 && m_hooks.before_present != nullptr) {
            m_hooks.before_present(m_hooks.user);
        }
        const HRESULT hr = m_real->Present(sync, flags);
        release_waitable();
        if (m_hooks.present_failed != nullptr && (hr == DXGI_ERROR_DEVICE_REMOVED ||
                                                  hr == DXGI_ERROR_DEVICE_RESET)) {
            m_hooks.present_failed(hr, m_hooks.user);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override
    {
        const HRESULT hr = m_real->GetBuffer(buffer, riid, surface);
        // Rate-limited (one line every 2 s): tells whether the title keeps acquiring buffers
        // from *this* chain - the alternative (a second, real chain) is a black window.
        static std::atomic<uint64_t> last{ 0 };
        const uint64_t now = GetTickCount64();
        uint64_t prev = last.load();
        if (SUCCEEDED(hr) && surface != nullptr && now - prev >= 2000 &&
            last.compare_exchange_strong(prev, now)) {
            auto* res = reinterpret_cast<ID3D12Resource*>(*surface);
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            logf("xefg: GetBuffer(%u) -> %p %llux%u fmt=%u (real chain %p, facade %p)\n",
                 buffer, *surface, static_cast<unsigned long long>(d.Width), d.Height,
                 static_cast<unsigned>(d.Format), static_cast<void*>(m_real),
                 static_cast<void*>(this));
        }
        return hr;
    }

    // FG_Hooks.cpp:700-730 - with UseBorderless() the title's exclusive-fullscreen request is
    // swallowed, remembered, and EMULATED with a borderless window; without the emulation the
    // title believes the switch happened while the window never changed, which is what crashed
    // it (AV writing to 1 right after the request). ApplyBorderless/RestoreWindow are the
    // window side of it (FG_Hooks.cpp:222-290).
    HWND owner_hwnd()
    {
        IDXGISwapChain1* s1 = sc1();
        HWND h = nullptr;
        if (s1 != nullptr) { s1->GetHwnd(&h); }
        return h;
    }

    void apply_borderless(IDXGIOutput* target = nullptr)
    {
        HWND h = owner_hwnd();
        if (h == nullptr) { return; }
        if (IsIconic(h)) {                    // minimized has no usable rect; retried later
            m_borderless_pending = true;
            return;
        }
        // FG_Hooks.cpp:235-240 / Util::GetMonitorInfoForOutput: the title's mode-switch output
        // wins over the monitor the window happens to be on (multi-monitor titles).
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
        const LONG_PTR exStyle = WS_EX_APPWINDOW;
        RECT r{};
        GetWindowRect(h, &r);
        const long mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const long mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        // Re-applying sends WM_SIZE, and the title resizes again: stop when it already matches.
        if (GetWindowLongPtrW(h, GWL_STYLE) == style && GetWindowLongPtrW(h, GWL_EXSTYLE) == exStyle &&
            r.left == mi.rcMonitor.left && r.top == mi.rcMonitor.top &&
            (r.right - r.left) == mw && (r.bottom - r.top) == mh) {
            return;
        }
        if (!m_window_saved) {
            m_saved_style  = GetWindowLongPtrW(h, GWL_STYLE);
            m_saved_ex     = GetWindowLongPtrW(h, GWL_EXSTYLE);
            m_saved_rect   = r;
            m_window_saved = true;
        }
        SetWindowLongPtrW(h, GWL_STYLE, style);
        SetWindowLongPtrW(h, GWL_EXSTYLE, exStyle);
        // Safety net: libxess refuses to interpolate as soon as *its own* chain believes it is
        // fullscreen ("Interpolation is not supported in fullscreen mode. Interpolation won't
        // be enabled."), and that leaves the title at 2X with no generated frames - a black
        // window. The emulation owns the window, so the backend chain is put back to windowed.
        if (m_real != nullptr) {
            BOOL backend_fs = FALSE;
            if (SUCCEEDED(m_real->GetFullscreenState(&backend_fs, nullptr)) && backend_fs) {
                logf("xefg: backend chain was in fullscreen - forcing windowed for XeFG\n");
                m_real->SetFullscreenState(FALSE, nullptr);
            }
        }

        // FG_Hooks.cpp:276 - SWP_SHOWWINDOW: a title that hid its window for the mode switch
        // stays hidden otherwise, i.e. it presents into nothing.
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        static bool told = false;
        if (!told) {
            told = true;
            logf("xefg: borderless emulation applied (%ldx%ld at %ld,%ld)\n", mw, mh,
                 mi.rcMonitor.left, mi.rcMonitor.top);
        }
    }

    // FG_Hooks.cpp:279-290: if nothing was saved (the chain was windowized at creation, the
    // title never had a borderless window), the client area is rebuilt from the chain's size.
    void restore_window(UINT width = 0, UINT height = 0)
    {
        HWND h = owner_hwnd();
        if (h == nullptr) { return; }
        if (IsIconic(h)) {
            m_borderless_pending = true;
            return;
        }
        if (m_window_saved) {
            SetWindowLongPtrW(h, GWL_STYLE, m_saved_style);
            SetWindowLongPtrW(h, GWL_EXSTYLE, m_saved_ex);
            SetWindowPos(h, nullptr, m_saved_rect.left, m_saved_rect.top,
                         m_saved_rect.right - m_saved_rect.left,
                         m_saved_rect.bottom - m_saved_rect.top,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
            m_window_saved = false;
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

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override
    {
        logf("xefg: SetFullscreenState(%d, target=%p) borderless=%d requested=%d\n",
             static_cast<int>(fullscreen), static_cast<void*>(target),
             force_borderless() ? 1 : 0, m_exclusive_requested ? 1 : 0);
        if (force_borderless()) {
            if (fullscreen) {
                // FG_Hooks.cpp:228-232 / :572 - never restyle the window inside the title's own
                // mode-switch call stack; mark it pending and do it on the next present. The
                // output is kept: ApplyBorderless uses its monitor, not the window's.
                m_exclusive_requested = true;
                m_borderless_pending  = true;
                if (m_pending_output != nullptr) {
                    m_pending_output->Release();
                    m_pending_output = nullptr;
                }
                if (target != nullptr) {
                    target->AddRef();
                    m_pending_output = target;
                }
                return S_OK;
            }
            if (m_exclusive_requested) {
                m_exclusive_requested = false;
                m_borderless_pending = false;
                if (m_pending_output != nullptr) {
                    m_pending_output->Release();
                    m_pending_output = nullptr;
                }
                DXGI_SWAP_CHAIN_DESC scDesc{};
                m_real->GetDesc(&scDesc);
                restore_window(scDesc.BufferDesc.Width, scDesc.BufferDesc.Height);
                return S_OK;
            }
        }
        return m_real->SetFullscreenState(fullscreen, target);
    }

    // FG_Hooks.cpp:800-812: while emulating, it reports exclusive fullscreen (that is what the
    // title asked for) and hands out the output, because games dereference it.
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override
    {
        if (force_borderless() && m_exclusive_requested) {
            if (fullscreen != nullptr) { *fullscreen = TRUE; }
            if (target != nullptr && *target == nullptr) { m_real->GetContainingOutput(target); }
            return S_OK;
        }
        return m_real->GetFullscreenState(fullscreen, target);
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override
    { return m_real->GetDesc(desc); }

    // A resize rebuilds XeFG's internal buffers; the facade keeps working because
    // the backend's chain object stays the same.
    // A "keep everything" resize - BufferCount 0, size 0x0, format UNKNOWN - is what UE
    // titles send while loading, and DXGI treats it as a no-op. Forwarding it to the FG
    // chain makes libxess_fg rebuild its 4K buffers for nothing (measured: one 925 ms
    // present right after such a call). OptiScaler forwards everything, but its
    // WrappedIDXGISwapChain4::ResizeBuffers also does `WaitForGPUIdle` first, i.e. it never
    // leaves the library a resize that overlaps an in-flight frame - the cheapest faithful
    // equivalent here is not to hand it a request that changes nothing.
    // FG_Hooks.cpp:844-873, verbatim: the title's ResizeBuffers flags are not allowed to
    // change on a flip chain - DXGI rejects "add or remove ALLOW_TEARING / FRAME_LATENCY_
    // WAITABLE_OBJECT", and UE5 turns that into a fatal (D3D12Viewport.cpp:384, measured:
    // created with 0x842, resized with 0x802). ForceBorderless also drops ALLOW_MODE_SWITCH.
    UINT keep_flags(UINT wanted)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(m_real->GetDesc(&desc))) { return wanted; }
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

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                            UINT flags) override
    {
        // OptiScaler (WrappedIDXGISwapChain4::ResizeBuffers) forwards unconditionally: in DXGI
        // Width/Height 0 means "use the window's client size", NOT "keep the current size", so
        // skipping such a call leaves the FG chain sized differently from the window and the
        // next Present answers E_ABORT -> UE5 raises a LowLevelFatalError
        // (D3D12Viewport.cpp:640 PresentInternal).
        const HRESULT hr = m_real->ResizeBuffers(count, w, h, format, keep_flags(flags));
        // No apply_borderless() here: SetWindowPos raises WM_SIZE, the title answers with
        // another ResizeBuffers, and that loop is measured as 5 fps with 8 s stalls during
        // loading. OptiScaler re-fits the window from SetFullscreenState/ResizeTarget only.
        if (m_hooks.after_resize != nullptr) { m_hooks.after_resize(m_hooks.user); }
        return hr;
    }

    // FG_Hooks.cpp:1013-1020 - while the emulation owns the window, ResizeTarget is swallowed:
    // "Windowed games resize their window with it", i.e. forwarding it undoes the borderless
    // rect right after it was applied.
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) override
    {
        if (force_borderless() && m_exclusive_requested) { return S_OK; }
        return m_real->ResizeTarget(target);
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override
    { return m_real->GetContainingOutput(output); }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override
    { return m_real->GetFrameStatistics(stats); }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override
    { return m_real->GetLastPresentCount(count); }

    // ---------------------------------------------------------------- IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override
    { return sc1()->GetDesc1(desc); }

    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override
    {
        const HRESULT hr = sc1()->GetFullscreenDesc(desc);
        // FG_Hooks.cpp:787-793: while emulating, the chain reports itself as fullscreen - that
        // is what the title asked for and what GetFullscreenState already answers. Reporting
        // "windowed" here makes titles that branch on it undo their own fullscreen state.
        if (hr == S_OK && desc != nullptr && force_borderless() && m_exclusive_requested) {
            desc->Windowed = FALSE;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override
    { return sc1()->GetHwnd(hwnd); }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** window) override
    { return sc1()->GetCoreWindow(riid, window); }

    // Present1 gets the same treatment as Present: the id describes this present.
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) override
    {
        if ((flags & DXGI_PRESENT_TEST) == 0 && m_hooks.before_present != nullptr) {
            m_hooks.before_present(m_hooks.user);
        }
        const HRESULT hr = sc1()->Present1(sync, flags, params);
        release_waitable();
        if (m_hooks.present_failed != nullptr && (hr == DXGI_ERROR_DEVICE_REMOVED ||
                                                  hr == DXGI_ERROR_DEVICE_RESET)) {
            m_hooks.present_failed(hr, m_hooks.user);
        }
        return hr;
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
    { return sc1()->IsTemporaryMonoSupported(); }

    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override
    { return sc1()->GetRestrictToOutput(output); }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override
    { return sc1()->SetBackgroundColor(color); }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override
    { return sc1()->GetBackgroundColor(color); }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override
    { return sc1()->SetRotation(rotation); }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override
    { return sc1()->GetRotation(rotation); }

    // ---------------------------------------------------------------- IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override
    { return sc2()->SetSourceSize(width, height); }

    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override
    { return sc2()->GetSourceSize(width, height); }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override
    { return sc2()->SetMaximumFrameLatency(latency); }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override
    { return sc2()->GetMaximumFrameLatency(latency); }

    // FG_Hooks.cpp:763-786 (create/duplicate) + :1590-1596 (ReleaseSemaphore after present).
    // Both halves live behind SimulateWaitableObject in OptiScaler; handing the semaphore out
    // without the release is a hang, so the pair is kept together here and stays inert unless
    // the switch is on.
    // Deferred window restyle: safe point (outside the title's mode-switch stack).
    void apply_pending_borderless()
    {
        if (!m_borderless_pending || !force_borderless()) { return; }
        m_borderless_pending = false;
        IDXGIOutput* target = m_pending_output;
        m_pending_output = nullptr;
        // FG_Hooks.cpp:1326-1334 - pending means "apply while fullscreen is emulated, else put
        // the window back". apply_borderless/restore_window re-arm it while minimized.
        if (m_exclusive_requested)      { apply_borderless(target); }
        else if (m_window_saved)        { restore_window(); }
        if (target != nullptr) { target->Release(); }
    }

    // FG_Hooks.cpp:1590-1596: exactly one release per present, only while the switch is on.
    void release_waitable()
    {
        if (m_semaphore != nullptr && simulate_waitable_object()) {
            ReleaseSemaphore(m_semaphore, 1, nullptr);
        }
    }

    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        if (!simulate_waitable_object()) { return sc2()->GetFrameLatencyWaitableObject(); }
        if (m_semaphore == nullptr) {
            m_semaphore = CreateSemaphoreW(nullptr, 16, 16, nullptr);
            if (m_semaphore == nullptr) { return nullptr; }
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), m_semaphore, GetCurrentProcess(), &dup, 0,
                             FALSE, DUPLICATE_SAME_ACCESS)) {
            return nullptr;
        }
        return dup;
    }

    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override
    { return sc2()->SetMatrixTransform(matrix); }

    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override
    { return sc2()->GetMatrixTransform(matrix); }

    // ---------------------------------------------------------------- IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
    {
        IDXGISwapChain3* s3 = nullptr;
        if (SUCCEEDED(m_real->QueryInterface(IID_PPV_ARGS(&s3))) && s3 != nullptr) {
            const UINT index = s3->GetCurrentBackBufferIndex();
            s3->Release();
            return index;
        }
        return 0;
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space,
                                                     UINT* support) override
    { return sc3()->CheckColorSpaceSupport(space, support); }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override
    { return sc3()->SetColorSpace1(space); }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                             UINT flags, const UINT* node_mask,
                                             IUnknown* const* present_queue) override
    {
        const HRESULT hr = sc3()->ResizeBuffers1(count, w, h, format, keep_flags(flags),
                                                 node_mask, present_queue);
        // Same pause as ResizeBuffers: the proxy chain's buffers were just replaced.
        if (m_hooks.after_resize != nullptr) { m_hooks.after_resize(m_hooks.user); }
        return hr;
    }

    // ---------------------------------------------------------------- IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size,
                                             void* data) override
    {
        IDXGISwapChain4* s4 = nullptr;
        if (SUCCEEDED(m_real->QueryInterface(IID_PPV_ARGS(&s4))) && s4 != nullptr) {
            const HRESULT hr = s4->SetHDRMetaData(type, size, data);
            s4->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

private:
    IDXGISwapChain1* sc1()
    {
        if (m_sc1 == nullptr) { m_real->QueryInterface(IID_PPV_ARGS(&m_sc1)); }
        return (m_sc1 != nullptr) ? m_sc1 : nullptr;
    }
    IDXGISwapChain2* sc2()
    {
        if (m_sc2 == nullptr) { m_real->QueryInterface(IID_PPV_ARGS(&m_sc2)); }
        return (m_sc2 != nullptr) ? m_sc2 : nullptr;
    }
    IDXGISwapChain3* sc3()
    {
        if (m_sc3 == nullptr) { m_real->QueryInterface(IID_PPV_ARGS(&m_sc3)); }
        return (m_sc3 != nullptr) ? m_sc3 : nullptr;
    }

    IDXGISwapChain*     m_real = nullptr;
    IDXGISwapChain1*    m_sc1  = nullptr;
    IDXGISwapChain2*    m_sc2  = nullptr;
    IDXGISwapChain3*    m_sc3  = nullptr;
    HANDLE              m_semaphore = nullptr;
    bool                m_exclusive_requested = false;
    bool                m_borderless_pending = false;
    IDXGIOutput*        m_pending_output = nullptr;   // output of the last fullscreen request
    bool                m_window_saved = false;
    LONG_PTR            m_saved_style = 0;
    LONG_PTR            m_saved_ex = 0;
    RECT                m_saved_rect{};
    PresentHooks        m_hooks{};
    std::atomic<ULONG>  m_refs{ 1 };
};

} // namespace fakerfg
