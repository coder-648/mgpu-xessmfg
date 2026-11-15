// Optional read-only probe: real Windows SDK APIs, no rendering or settings.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <fmt/format.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Warn(std::string_view msg) { std::printf("%.*s\n", int(msg.size()), msg.data()); }
    void ComWarn(std::string_view msg, HRESULT hr) {
        Warn(fmt::format("{} HRESULT=0x{:08X}", msg, static_cast<uint32_t>(hr)));
    }
};
struct DirectXHelper { static bool IsDisplayOnlyAdapter(IDXGIAdapter1*) noexcept; };
#include "AdapterNativeProduction.inc"
int main() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    unsigned checked = 0;
    for (UINT i=0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return 2;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) return 3;
        const bool skip = DirectXHelper::IsDisplayOnlyAdapter(adapter.Get());
        std::wprintf(L"idx=%u name=%ls vendor=%04x device=%04x displayOnly=%u\n",
            i, desc.Description, desc.VendorId, desc.DeviceId, skip);
        ++checked;
    }
    std::printf("PASS: production helper linked with Gdi32.lib and queried %u local adapters.\n", checked);
}
