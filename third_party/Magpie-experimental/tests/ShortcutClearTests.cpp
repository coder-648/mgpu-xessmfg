// Production functions are extracted verbatim. Only Windows input and XAML
// surfaces are mocked; this is not native UI or actual hotkey registration QA.
#include <cstdint>
#include <string>
#include "Shortcut.h"
#include "ConfigRecovery.h"
#include <rapidjson/prettywriter.h>
#include <array>
#include <bitset>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

static void Require(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}

namespace fmt {
template<class... Args> std::string format(Args&&...) { return {}; }
}

namespace Magpie {
#include "ShortcutAction.inc"
constexpr size_t ACTION_COUNT = (size_t)ShortcutAction::COUNT_OR_NONE;
static std::array<std::optional<std::pair<UINT, UINT>>, ACTION_COUNT> registered;
static unsigned registrations = 0, unregistrations = 0;
static UINT pressedModifiers = 0;
static BOOL TestRegisterHotKey(HWND, int id, UINT modifiers, UINT code) {
	++registrations;
	for (size_t i = 0; i < registered.size(); ++i) {
		if (i != (size_t)id && registered[i] == std::pair{ modifiers, code }) return FALSE;
	}
	registered[id] = std::pair{ modifiers, code };
	return TRUE;
}
static BOOL TestUnregisterHotKey(HWND, int id) {
	++unregistrations;
	registered[id].reset();
	return TRUE;
}
static SHORT TestGetAsyncKeyState(int key) {
	const auto flag = key == VK_CONTROL ? MOD_CONTROL : key == VK_SHIFT ? MOD_SHIFT :
		key == VK_MENU ? MOD_ALT : MOD_WIN;
	return (pressedModifiers & flag) ? (SHORT)0x8000 : 0;
}
static LRESULT TestCallNextHookEx(HHOOK, int, WPARAM, LPARAM) { return 77; }
static UINT TestSendInput(UINT, LPINPUT, int) { return 1; }
#define RegisterHotKey TestRegisterHotKey
#define UnregisterHotKey TestUnregisterHotKey
#define GetAsyncKeyState TestGetAsyncKeyState
#define CallNextHookEx TestCallNextHookEx
#define SendInput TestSendInput

struct Logger {
	unsigned errors = 0;
	static Logger& Get() { static Logger instance; return instance; }
	void Info(std::string) {}
	void Win32Error(std::string) { ++errors; }
};
struct Win32Helper { static std::wstring GetKeyName(uint8_t code) { return std::wstring(1, code); } };
struct StrHelper {
	static std::string UTF16ToUTF8(std::wstring_view text) { return { text.begin(), text.end() }; }
};
enum class ShortcutError { NoError, Invalid, InUse };
struct ShortcutHelper {
	static unsigned checks;
	static bool IsValidKeyCode(uint8_t code) noexcept;
	static ShortcutError CheckShortcut(Shortcut value) {
		++checks;
		return value.IsEmpty() || !(value.alt || value.win || value.ctrl || value.shift) ?
			ShortcutError::Invalid : ShortcutError::NoError;
	}
	static std::string ToString(ShortcutAction) { return {}; }
};
unsigned ShortcutHelper::checks = 0;
struct FakeEvent {
	unsigned count = 0;
	std::function<void(ShortcutAction)> callback;
	void Invoke(ShortcutAction action) { ++count; if (callback) callback(action); }
};
struct AppSettings {
	std::array<Shortcut, ACTION_COUNT> _shortcuts{};
	FakeEvent ShortcutChanged;
	unsigned saves = 0;
	std::string lastSnapshot;
	static AppSettings& Get() { static AppSettings instance; return instance; }
	const Shortcut& GetShortcut(ShortcutAction action) const { return _shortcuts[(size_t)action]; }
	void SetShortcut(ShortcutAction action, const Shortcut& value);
	void SaveAsync() { ++saves; lastSnapshot = Serialize(); }
	void _SetDefaultShortcuts() noexcept;
	bool _LoadShortcuts(const rapidjson::GenericObject<true, rapidjson::Value>& root) noexcept;
	std::string Serialize() const;
};
struct FakeHandle { HWND get() const { return nullptr; } };
struct FakeHook { bool active = true; void reset() { active = false; } };
struct ShortcutService {
	struct Info { std::chrono::steady_clock::time_point lastFireTime{}; bool isError = true; };
	std::array<Info, ACTION_COUNT> _shortcutInfos;
	FakeHandle _hwndHotkey;
	FakeEvent ShortcutActivated;
	bool _isKeyboardHookActive = true, _keyboardHookShortcutActivated = false;
	unsigned restarts = 0;
	static ShortcutService& Get() { static ShortcutService instance; return instance; }
	bool IsError(ShortcutAction action) const { return _shortcutInfos[(size_t)action].isError; }
	void StartKeyboardHook() { _isKeyboardHookActive = true; ++restarts; }
	void _RegisterShortcut(ShortcutAction action);
	void _FireShortcut(ShortcutAction action);
	static LRESULT CALLBACK _LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
};
struct Dispatcher {
	std::vector<std::function<void()>> pending;
	void TryEnqueue(std::function<void()> callback) { pending.push_back(std::move(callback)); }
	void Drain() { auto copy = std::move(pending); pending.clear(); for (auto& callback : copy) callback(); }
};
struct App {
	struct Dispatcher dispatcher;
	static App& Get() { static App instance; return instance; }
	auto& Dispatcher() { return dispatcher; }
};
struct ScalingService {
	static ScalingService& Get() { static ScalingService instance; return instance; }
	void OnTaskSwitch() {}
};
enum class ContentDialogResult { None, Primary, Secondary };
struct ContentDialog {};
struct ContentDialogClosingEventArgs {
	ContentDialogResult result;
	ContentDialogResult Result() const { return result; }
};
struct IInspectable {};
struct RoutedEventArgs {};
struct ContentDialogHelper {
	static inline bool active = false;
	static bool IsAnyDialogOpen() { return active; }
};
struct ShortcutControl {
	ShortcutAction _action = ShortcutAction::COUNT_OR_NONE;
	Shortcut _shortcut, _previewShortcut;
	FakeHook _keyboardHook;
	static inline ShortcutControl* _that = nullptr;
	ShortcutAction Action() const { return _action; }
	void _ClearShortcut();
	void _StopEditing();
	void ClearButton_Click(IInspectable const&, RoutedEventArgs const&);
	void _ShortcutDialog_Closing(ContentDialog const&, ContentDialogClosingEventArgs const&);
};
#include "ShortcutClearProduction.inc"

static bool Load(AppSettings& settings, std::string_view json) {
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	Require(!document.HasParseError(), "Fixture JSON parses");
	return settings._LoadShortcuts(static_cast<const rapidjson::Document&>(document).GetObj());
}
static LRESULT Key(uint8_t code) {
	KBDLLHOOKSTRUCT info{ .vkCode = code };
	return ShortcutService::_LowLevelKeyboardProc(HC_ACTION, WM_KEYDOWN, (LPARAM)&info);
}

static void ConfigTests(const std::filesystem::path& output) {
	AppSettings initial;
	Require(Load(initial, "{}"), "Missing shortcut object needs defaults");
	const auto defaults = initial._shortcuts;
	constexpr char codes[] = { 'A', 'Q', 'D', 'P', 'E', 'S', 'F', 'C' };
	for (size_t i = 0; i < ACTION_COUNT; ++i) {
		Require(defaults[i].code == codes[i] && defaults[i].alt && defaults[i].shift, "Default bindings unchanged");
	}
	Require(Load(initial, R"({"shortcuts":{"scale":0,"profiler":577}})"), "Missing individual fields inherit defaults");
	Require(initial._shortcuts[0].IsEmpty() && initial._shortcuts[3].ctrl && initial._shortcuts[3].code == 'A', "Zero and custom fields preserved");
	for (size_t i = 0; i < ACTION_COUNT; ++i) {
		AppSettings saved;
		saved._shortcuts = defaults;
		saved._shortcuts[i].Clear();
		AppSettings reloaded;
		Require(!Load(reloaded, saved.Serialize()), "All eight explicit fields need no default rewrite");
		Require(reloaded._shortcuts == saved._shortcuts, "Every explicit zero survives reload with other bindings unchanged");
	}
	AppSettings empty;
	AppSettings reloaded;
	Require(!Load(reloaded, empty.Serialize()), "All-clear config remains complete");
	Require(reloaded._shortcuts == empty._shortcuts, "All-clear config stays empty");
	Require(Load(reloaded, R"({"hotkeys":{"scale":0,"windowedModeScale":0,"overlay":0}})"), "Legacy fields load");
	Require(reloaded._shortcuts[0].IsEmpty() && reloaded._shortcuts[1].IsEmpty() && reloaded._shortcuts[2].IsEmpty(), "Legacy explicit zero preserved");
	Load(reloaded, R"({"shortcuts":{"toolbar":0,"overlay":3137}})");
	Require(reloaded._shortcuts[2].IsEmpty(), "Modern zero overrides legacy alias");
	Load(reloaded, R"({"shortcuts":{"scale":null,"profiler":4096,"screenshot":-1,"comparison":"0"}})");
	Require(reloaded._shortcuts == defaults, "Malformed fields are not treated as clearing");

	for (const char* key : { "shortcuts", "hotkeys" }) {
		const std::string input = std::string("{\"") + key + R"(":{"scale":0,"overlay":0,"toolbar":0,"profiler":4096,"screenshot":"0"},"scalingModes":[]})";
		auto repaired = ConfigRecovery::Prepare(input, {}, "Recovered");
		Require(repaired.kind == ConfigRecovery::Kind::Repaired, "Invalid shortcut fields participate in recovery");
		Require(repaired.document[key]["scale"].GetUint() == 0 && repaired.document[key]["toolbar"].GetUint() == 0, "Recovery retains zero");
		Require(!repaired.document[key].HasMember("profiler") && !repaired.document[key].HasMember("screenshot"), "Recovery removes only corrupt fields");
		Load(reloaded, ConfigRecovery::Serialize(repaired.document));
		Require(reloaded._shortcuts[0].IsEmpty() && reloaded._shortcuts[2].IsEmpty() && reloaded._shortcuts[3] == defaults[3], "Recovered defaults and clears coexist");
	}
	auto backup = ConfigRecovery::Prepare("{broken", empty.Serialize(), "Recovered");
	Require(backup.kind == ConfigRecovery::Kind::Backup, "Backup recovery works");
	Load(reloaded, ConfigRecovery::Serialize(backup.document));
	Require(reloaded._shortcuts == empty._shortcuts, "Backup migration preserves explicit clears");

	const auto file = output / (L"shortcut-roundtrip-" + std::to_wstring(GetCurrentProcessId()) + L".json");
	ConfigSaveState state;
	Require(ConfigPersistence::WriteAtomic(file, empty.Serialize(), 1, state), "Atomic persistence succeeds");
	Load(reloaded, ConfigPersistence::Read(file));
	Require(reloaded._shortcuts == empty._shortcuts, "Real-file zero persistence survives reload");
	Require(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY), "Set read-only fixture");
	const bool wrote = ConfigPersistence::WriteAtomic(file, initial.Serialize(), 2, state);
	SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
	Require(!wrote && ConfigPersistence::Read(file) == empty.Serialize(), "Failed save preserves existing explicit zeros");
}

static void RuntimeAndDialogTests() {
	auto& settings = AppSettings::Get();
	auto& service = ShortcutService::Get();
	settings._SetDefaultShortcuts();
	settings.ShortcutChanged.callback = [&](ShortcutAction action) { service._RegisterShortcut(action); };
	for (size_t i = 0; i < ACTION_COUNT; ++i) service._RegisterShortcut((ShortcutAction)i);
	const auto defaults = settings._shortcuts;
	for (size_t i = 0; i < ACTION_COUNT; ++i) {
		const auto action = (ShortcutAction)i;
		const auto checks = ShortcutHelper::checks, errors = Logger::Get().errors, count = registrations;
		settings.SetShortcut(action, {});
		Require(!registered[i] && !service.IsError(action), "Clear unregisters without error");
		Require(ShortcutHelper::checks == checks && Logger::Get().errors == errors && registrations == count, "Empty shortcut skips validation and registration");
		pressedModifiers = MOD_ALT | MOD_SHIFT;
		Require(Key(defaults[i].code) == 77, "Old shortcut is not swallowed after clearing");
		const auto fires = service.ShortcutActivated.count;
		service._FireShortcut(action);
		Require(service.ShortcutActivated.count == fires, "Queued callback cannot activate cleared action");
		settings.SetShortcut(action, defaults[i]);
		Require(registered[i].has_value() && !service.IsError(action), "Rebinding restores normal registration");
	}
	settings.SetShortcut(ShortcutAction::Profiler, defaults[0]);
	Require(service.IsError(ShortcutAction::Profiler), "Conflicting rebind still fails");
	settings.SetShortcut(ShortcutAction::Profiler, {});
	Require(!service.IsError(ShortcutAction::Profiler), "Conflict binding can be cleared");
	settings.SetShortcut(ShortcutAction::Profiler, defaults[3]);
	// Execute the actual hook path and drain its queued callback after clearing.
	Require(Key(defaults[2].code) == 1 && !App::Get().dispatcher.pending.empty(), "Bound hook matches and queues");
	const auto fires = service.ShortcutActivated.count;
	settings.SetShortcut(ShortcutAction::Toolbar, {});
	App::Get().dispatcher.Drain();
	Require(service.ShortcutActivated.count == fires, "Pending hook callback dropped after clearing");
	settings.SetShortcut(ShortcutAction::Toolbar, defaults[2]);

	ShortcutControl control;
	control._action = ShortcutAction::Scale;
	control._shortcut = defaults[0];
	control._previewShortcut = { .code = 'Z', .ctrl = true };
	ShortcutControl::_that = &control;
	service._isKeyboardHookActive = false;
	const auto saves = settings.saves;
	control._ShortcutDialog_Closing({}, { ContentDialogResult::None });
	Require(settings.GetShortcut(control._action) == defaults[0] && settings.saves == saves, "Cancel never submits preview");
	Require(!control._keyboardHook.active && !ShortcutControl::_that && service._isKeyboardHookActive, "Closing releases editor hook and restarts service");
	control._ShortcutDialog_Closing({}, { ContentDialogResult::Primary });
	Require(settings.GetShortcut(control._action) == control._previewShortcut, "Save submits preview");
	control._shortcut = control._previewShortcut;
	control._previewShortcut = { .code = 'Q' }; // Invalid preview must not affect clear.
	control._ShortcutDialog_Closing({}, { ContentDialogResult::Secondary });
	Require(settings.GetShortcut(control._action).IsEmpty() && !service.IsError(control._action), "Secondary clears saved binding, independent of preview");
	settings.SetShortcut(control._action, defaults[0]);
	control._shortcut = defaults[0];
	ContentDialogHelper::active = true;
	control.ClearButton_Click({}, {});
	Require(!settings.GetShortcut(control._action).IsEmpty(), "Inline clear cannot run while a dialog is open");
	ContentDialogHelper::active = false;
	control.ClearButton_Click({}, {});
	Require(settings.GetShortcut(control._action).IsEmpty(), "Inline and dialog clear use the same setting path");
	control._shortcut.Clear();
	const auto clearedSaves = settings.saves;
	control.ClearButton_Click({}, {});
	Require(settings.saves == clearedSaves, "Already-empty clear is a no-op");
	Require(FormatToolbarTooltip("Parameters", "") == "Parameters", "Empty hint has no brackets");
	Require(FormatToolbarTooltip("Parameters", "Ctrl+E").find("Ctrl+E") != std::string::npos, "Rebound hint retains keys");
}
}

int wmain(int argc, wchar_t** argv) {
	try {
		Require(argc == 2, "Pass the fixture output directory");
		Magpie::ConfigTests(argv[1]);
		Magpie::RuntimeAndDialogTests();
		std::cout << "Shortcut clear production regressions passed: eight zero roundtrips, legacy/missing/corrupt config, recovery, atomic persistence/failure, unregister/rebind/conflict, hook delivery, stale callbacks and save/clear/cancel.\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAILED: " << error.what() << '\n';
		return 1;
	}
}
