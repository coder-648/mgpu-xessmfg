// Production function bodies are extracted at run time. Only OS, GPU and
// asynchronous persistence dependencies are doubled; no user settings change.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dkmthk.h>
#include <fmt/format.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "../src/Magpie/JsonHelper.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

struct FakeAdapter {
    UINT idx = 0, vendor = 0x10de, device = 0x1234;
    HRESULT descStatus = S_OK;
    NTSTATUS openStatus = 0, queryStatus = 0, closeStatus = 0;
    bool indirect = false, render = true, creatable = true;
    HRESULT GetDesc1(DXGI_ADAPTER_DESC1* desc) noexcept {
        *desc = {};
        desc->VendorId = vendor;
        desc->DeviceId = device;
        desc->AdapterLuid = {idx, -1};
        return descStatus;
    }
};
static std::vector<FakeAdapter> inventory;
static unsigned openCount = 0, queryCount = 0, closeCount = 0, cases = 0;
static D3DKMT_HANDLE lastOpened = 0, lastClosed = 0;
static std::vector<std::string> warnings;
static NTSTATUS MockOpen(const D3DKMT_OPENADAPTERFROMLUID* args) {
    ++openCount;
    const auto& adapter = inventory.at(args->AdapterLuid.LowPart);
    assert(args->AdapterLuid.HighPart == -1);
    const_cast<D3DKMT_OPENADAPTERFROMLUID*>(args)->hAdapter = adapter.idx + 1;
    lastOpened = adapter.idx + 1;
    return adapter.openStatus;
}
static NTSTATUS MockQuery(const D3DKMT_QUERYADAPTERINFO* args) {
    ++queryCount;
    assert(args->Type == KMTQAITYPE_ADAPTERTYPE);
    assert(args->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERTYPE));
    const auto& adapter = inventory.at(args->hAdapter - 1);
    auto* type = static_cast<D3DKMT_ADAPTERTYPE*>(args->pPrivateDriverData);
    type->IndirectDisplayDevice = adapter.indirect;
    type->RenderSupported = adapter.render;
    return adapter.queryStatus;
}
NTSTATUS MockClose(const D3DKMT_CLOSEADAPTER* args) {
    ++closeCount;
    lastClosed = args->hAdapter;
    return inventory.at(args->hAdapter - 1).closeStatus;
}
NTSTATUS MockSkippedClose(const D3DKMT_CLOSEADAPTER* args) {
    return inventory.at(args->hAdapter - 1).closeStatus;
}
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Warn(std::string_view msg) { warnings.emplace_back(msg); }
    void ComWarn(std::string_view msg, HRESULT hr) {
        warnings.push_back(fmt::format("{} HRESULT=0x{:08X}", msg, static_cast<uint32_t>(hr)));
    }
    void ComError(std::string_view msg, HRESULT hr) { ComWarn(msg, hr); }
};
namespace winrt {
template<class T> struct com_ptr {
    T* value = nullptr;
    T* get() const { return value; }
    T* operator->() const { return value; }
    T** put() { value = nullptr; return &value; }
};
}
namespace wil {
struct unique_event_nothrow { HANDLE get() const { return nullptr; } };
struct srwlock {
    struct Guard { ~Guard() {} };
    Guard lock_exclusive() const { return {}; }
};
}
using winrt::com_ptr;
template<class T> using SmallVector = std::vector<T>;

namespace Magpie {
#include "GraphicsCardIdProduction.inc"
struct Profile { GraphicsCardId graphicsCardId; };
#include "AdapterConfigProduction.inc"
static std::string Serialize(const Profile& profile) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    WriteGraphicsCardId(writer, profile);
    writer.EndObject();
    return buffer.GetString();
}
static Profile Load(const std::string& json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    Profile profile;
    const rapidjson::Document& constant = doc;
    LoadGraphicsCardId(constant.GetObj(), profile);
    return profile;
}
struct AppSettings {
    Profile defaultProfile;
    std::vector<Profile> profiles;
    unsigned saves = 0;
    std::string savedDefault;
    std::vector<std::string> savedProfiles;
    static AppSettings& Get() { static AppSettings settings; return settings; }
    Profile& DefaultProfile() { return defaultProfile; }
    std::vector<Profile>& Profiles() { return profiles; }
    void SaveAsync() {
        ++saves;
        savedDefault = Serialize(defaultProfile);
        savedProfiles.clear();
        for (const auto& profile : profiles) savedProfiles.push_back(Serialize(profile));
    }
};
struct AdapterInfo { uint32_t idx, vendorId, deviceId; std::wstring description; };
}
using namespace Magpie;
struct FakeFactory {
    FakeAdapter warp{.vendor=0x1414, .device=0x8c};
    HRESULT registerStatus = S_OK, warpStatus = S_OK;
    HRESULT EnumAdapters1(UINT idx, FakeAdapter** output) {
        if (idx >= inventory.size()) return DXGI_ERROR_NOT_FOUND;
        *output = &inventory[idx];
        return S_OK;
    }
    HRESULT EnumWarpAdapter(com_ptr<FakeAdapter>* output) {
        output->value = &warp;
        return warpStatus;
    }
    HRESULT RegisterAdaptersChangedEvent(HANDLE, DWORD* cookie) {
        *cookie = 1;
        return registerStatus;
    }
};
static FakeFactory factory;
static HRESULT factoryStatus = S_OK;
static HRESULT MockCreateFactory(com_ptr<FakeFactory>* output) {
    output->value = &factory;
    return factoryStatus;
}
static std::vector<UINT> flChecked;
static HRESULT MockCreateDevice(FakeAdapter* adapter, D3D_DRIVER_TYPE, HMODULE,
    UINT, const D3D_FEATURE_LEVEL* levels, UINT count, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**) {
    assert(count == 1 && *levels == D3D_FEATURE_LEVEL_11_0);
    flChecked.push_back(adapter->idx);
    return adapter->creatable ? S_OK : E_FAIL;
}
struct App {
    struct Queue { template<class F> void TryEnqueue(F&& callback) { callback(); } } queue;
    static App& Get() { static App app; return app; }
    Queue& Dispatcher() { return queue; }
};
struct Win32Helper {
    template<class F> static void RunParallel(F&& callback, uint32_t count) {
        for (uint32_t i=0; i<count; ++i) callback(i);
    }
};
struct DirectXHelper {
    static bool IsDisplayOnlyAdapter(FakeAdapter*) noexcept;
#include "AdapterWarpProduction.inc"
};
struct AdaptersService {
    std::vector<AdapterInfo> _adapterInfos;
    struct Event { unsigned count = 0; void Invoke() { ++count; } } AdaptersChanged;
    bool Initialize() noexcept;
    bool _GatherAdapterInfos(com_ptr<FakeFactory>&, wil::unique_event_nothrow&, DWORD&) noexcept;
    bool _UpdateProfileGraphicsCardId(Profile&) noexcept;
    void _UpdateProfiles() noexcept;
};
struct DeviceResources {
    FakeFactory* _dxgiFactory = &factory;
    int selected = -2;
    std::vector<int> attempted;
    bool _ObtainAdapterAndDevice(GraphicsCardId, bool) noexcept;
    bool _TryCreateD3DDevice(const com_ptr<FakeAdapter>& adapter, bool) {
        attempted.push_back(adapter.get() == &factory.warp ? -1 : static_cast<int>(adapter->idx));
        if (!adapter->creatable) return false;
        selected = attempted.back();
        return true;
    }
};
#define IDXGIAdapter1 FakeAdapter
#define IDXGIFactory7 FakeFactory
#define D3DKMTOpenAdapterFromLuid MockOpen
#define D3DKMTQueryAdapterInfo MockQuery
#define D3DKMTCloseAdapter MockClose
#define CreateDXGIFactory1 MockCreateFactory
#define D3D11CreateDevice MockCreateDevice
#undef IID_PPV_ARGS
#define IID_PPV_ARGS(arg) arg
#include "AdapterDecisionsProduction.inc"

static void Check(bool result, const char* label) {
    if (!result) { std::printf("FAIL: %s\n", label); std::exit(1); }
    ++cases;
}
static void Reset(std::vector<FakeAdapter> adapters) {
    inventory = std::move(adapters);
    for (UINT i=0; i<inventory.size(); ++i) inventory[i].idx = i;
    openCount = queryCount = closeCount = 0;
    lastOpened = lastClosed = 0;
    warnings.clear();
    flChecked.clear();
    factory = {};
    factoryStatus = S_OK;
    AppSettings::Get() = {};
}
static bool Same(const GraphicsCardId& a, const GraphicsCardId& b) {
    return a.idx == b.idx && a.vendorId == b.vendorId && a.deviceId == b.deviceId;
}
static bool HasWarning(std::string_view text) {
    return std::any_of(warnings.begin(), warnings.end(), [&](const auto& msg) {
        return msg.find(text) != std::string::npos;
    });
}
int main() {
    constexpr NTSTATUS failure = static_cast<NTSTATUS>(0xC000000D);
    for (bool indirect : {false, true}) for (bool render : {false, true}) {
        Reset({FakeAdapter{.indirect=indirect, .render=render}});
        Check(DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) == (indirect && !render), "all adapter-type flag combinations");
        Check(openCount==1 && queryCount==1 && closeCount==1 && lastOpened==lastClosed, "exact opened handle closes once");
        Check(warnings.empty(), "successful type query is quiet");
    }
    Reset({FakeAdapter{.descStatus=E_FAIL}});
    Check(!DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) && openCount==0 && closeCount==0, "GetDesc failure keeps candidate");
    Check(HasWarning("GetDesc1") && HasWarning("LUID unavailable") && HasWarning("HRESULT=0x80004005"), "description failure diagnosis");
    Reset({FakeAdapter{.openStatus=failure}});
    Check(!DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) && queryCount==0 && closeCount==0, "open failure never queries or closes invalid handle");
    Check(HasWarning("D3DKMTOpenAdapterFromLuid") && HasWarning("LUID=FFFFFFFF:00000000") && HasWarning("NTSTATUS=0xC000000D"), "open failure preserves raw status and signed-high LUID bits");
    Reset({FakeAdapter{.queryStatus=failure, .indirect=true, .render=false}});
    Check(!DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) && closeCount==1 && lastClosed==lastOpened, "query failure closes handle and ignores invalid flags");
    Check(HasWarning("D3DKMTQueryAdapterInfo") && HasWarning("NTSTATUS=0xC000000D"), "query failure diagnosis");
    Reset({FakeAdapter{.closeStatus=failure, .indirect=true, .render=false}});
    Check(DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) && closeCount==1, "close failure does not undo successful filtering");
    Check(HasWarning("D3DKMTCloseAdapter") && HasWarning("NTSTATUS=0xC000000D"), "close failure diagnosis");
    Reset({FakeAdapter{.openStatus=1, .queryStatus=1, .indirect=true, .render=false}});
    Check(DirectXHelper::IsDisplayOnlyAdapter(&inventory[0]) && closeCount==1, "nonnegative informational NTSTATUS is successful");

    Reset({FakeAdapter{}, FakeAdapter{.vendor=0x1414,.device=0x8c},
        FakeAdapter{.indirect=true,.render=false}, FakeAdapter{.vendor=0x8086},
        FakeAdapter{.indirect=true,.render=true}});
    auto& settings = AppSettings::Get();
    settings.defaultProfile.graphicsCardId = {2,0x10de,0x1234};
    settings.profiles = {Profile{{2,0x10de,0x1234}}, Profile{{4,0x10de,0x1234}}, Profile{}};
    AdaptersService service;
    Check(service.Initialize(), "startup enumeration succeeds");
    Check(service._adapterInfos.size()==3 && service._adapterInfos[0].idx==0 &&
        service._adapterInfos[1].idx==3 && service._adapterInfos[2].idx==4, "startup preserves raw indices through WARP/virtual gaps");
    Check(flChecked.empty(), "startup still avoids slow FL11 device probes");
    Check(settings.defaultProfile.graphicsCardId.idx==0 && settings.profiles[0].graphicsCardId.idx==0, "default and application virtual selections remap");
    Check(settings.profiles[1].graphicsCardId.idx==4 && settings.profiles[2].graphicsCardId.idx==-1, "second identical render-capable card and automatic choice stay intact");
    Check(settings.saves==1, "changed profiles request one batch save");
    Check(Same(Load(settings.savedDefault).graphicsCardId, settings.defaultProfile.graphicsCardId) &&
        Same(Load(settings.savedProfiles[0]).graphicsCardId, settings.profiles[0].graphicsCardId), "saved graphics-card JSON roundtrips for default and application profiles");
    service._UpdateProfiles();
    Check(settings.saves==1, "stable enumeration avoids repeated saves");
    settings.defaultProfile = Load(settings.savedDefault);
    settings.profiles[0] = Load(settings.savedProfiles[0]);
    AdaptersService restarted;
    Check(restarted.Initialize() && settings.saves==1 && settings.defaultProfile.graphicsCardId.idx==0, "simulated restart retains corrected selection without another save");

    auto legacy = Load("{\"graphicsCard\":2}");
    Check(service._UpdateProfileGraphicsCardId(legacy) && legacy.graphicsCardId.idx==-1 && legacy.graphicsCardId.vendorId==0, "index-only virtual legacy profile falls back to automatic");
    legacy = Load("{\"graphicsAdapter\":4}");
    Check(service._UpdateProfileGraphicsCardId(legacy) && legacy.graphicsCardId.idx==3 && legacy.graphicsCardId.vendorId==0x8086, "one-based preview index maps to retained raw index");
    Profile missing{{9,0x1002,0x9988}};
    Check(service._UpdateProfileGraphicsCardId(missing) && missing.graphicsCardId.idx==-1 && missing.graphicsCardId.vendorId==0x1002, "unplugged card retains identity for recovery");
    Check(!service._UpdateProfileGraphicsCardId(missing), "missing identity already in automatic fallback stays unchanged");
    service._adapterInfos.push_back({7,0x1002,0x9988,{}});
    Check(service._UpdateProfileGraphicsCardId(missing) && missing.graphicsCardId.idx==7, "reconnected identity recovers new raw index");

    // Re-enumeration uses the actual _GatherAdapterInfos and update/save code.
    Reset({FakeAdapter{.indirect=true,.render=false}, FakeAdapter{.creatable=false},
        FakeAdapter{.indirect=true,.render=true}});
    settings.defaultProfile.graphicsCardId = {0,0x10de,0x1234};
    settings.profiles = {Profile{{0,0x10de,0x1234}}};
    com_ptr<FakeFactory> dxgi;
    wil::unique_event_nothrow event;
    DWORD cookie = 0;
    Check(service._GatherAdapterInfos(dxgi,event,cookie), "hotplug enumeration succeeds");
    Check(service._adapterInfos.size()==1 && service._adapterInfos[0].idx==2 &&
        flChecked==std::vector<UINT>{1,2}, "hotplug skips virtual before FL11 probes and keeps real index after FL11 rejection");
    Check(settings.defaultProfile.graphicsCardId.idx==2 && settings.profiles[0].graphicsCardId.idx==2 &&
        settings.saves==1 && service.AdaptersChanged.count==1, "hotplug migrates all profiles and dispatches save/event");
    factory.registerStatus = E_FAIL;
    Check(!service._GatherAdapterInfos(dxgi,event,cookie) && service.AdaptersChanged.count==1, "registration failure never publishes an incomplete list");
    Reset({});
    factoryStatus = E_FAIL;
    AdaptersService unavailable;
    Check(!unavailable.Initialize(), "startup factory failure propagates");

    Reset({FakeAdapter{},FakeAdapter{.indirect=true,.render=false}});
    DeviceResources d1;
    Check(d1._ObtainAdapterAndDevice({1,0x10de,0x1234},false) && d1.selected==0 && d1.attempted==std::vector<int>{0}, "saved virtual card is skipped before device creation");
    Reset({FakeAdapter{.indirect=true,.render=false},FakeAdapter{}});
    DeviceResources d2;
    Check(d2._ObtainAdapterAndDevice({},false) && d2.selected==1 && d2.attempted==std::vector<int>{1}, "automatic choice skips first virtual card");
    Reset({FakeAdapter{.indirect=true,.render=false}, FakeAdapter{.vendor=0x8086}, FakeAdapter{}});
    DeviceResources rematch;
    Check(rematch._ObtainAdapterAndDevice({9,0x10de,0x1234},false) && rematch.selected==2 && rematch.attempted==std::vector<int>{2}, "ID rematch skips same-ID virtual before other-vendor automatic fallback");
    Reset({FakeAdapter{},FakeAdapter{}});
    DeviceResources d3;
    Check(d3._ObtainAdapterAndDevice({1,0x10de,0x1234},true) && d3.selected==1, "explicit second identical physical card stays selected");
    Reset({FakeAdapter{.indirect=true,.render=true}});
    DeviceResources d4;
    Check(d4._ObtainAdapterAndDevice({},false) && d4.selected==0, "render-capable indirect adapter remains usable");
    Reset({FakeAdapter{.creatable=false},FakeAdapter{.device=0x5678}});
    DeviceResources d5;
    Check(d5._ObtainAdapterAndDevice({0,0x10de,0x1234},false) && d5.selected==1 &&
        d5.attempted==std::vector<int>{0,1}, "failed specified card is not retried before working-card fallback");
    Reset({FakeAdapter{.indirect=true,.render=false}});
    DeviceResources d6;
    Check(d6._ObtainAdapterAndDevice({},false) && d6.selected==-1 && d6.attempted==std::vector<int>{-1}, "WARP remains final fallback");
    for (bool openFailure : {false,true}) {
        Reset({FakeAdapter{.openStatus=openFailure ? failure : 0, .queryStatus=openFailure ? 0 : failure}});
        DeviceResources d7;
        Check(d7._ObtainAdapterAndDevice({},false) && d7.selected==0, "probe failure preserves original D3D creation behavior");
    }
    Reset({});
    factory.warpStatus = E_FAIL;
    DeviceResources noWarp;
    Check(!noWarp._ObtainAdapterAndDevice({},false), "WARP enumeration failure propagates");
    factory.warpStatus = S_OK;
    factory.warp.creatable = false;
    DeviceResources failedWarp;
    Check(!failedWarp._ObtainAdapterAndDevice({},false), "WARP device creation failure propagates");
    std::printf("PASS: %u adapter filtering/selection/migration/JSON/lifecycle checks against current production code.\n", cases);
}
