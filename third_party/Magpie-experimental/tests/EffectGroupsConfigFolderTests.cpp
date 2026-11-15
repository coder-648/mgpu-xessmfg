// Extracted production handler; shell/toast substitutes never open user folders.
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <fmt/xchar.h>

struct IInspectable {};
struct RoutedEventArgs {};
struct Toast { std::wstring title, message; std::chrono::milliseconds duration; };
struct TestState {
	std::filesystem::path config;
	bool canOpen = true;
	std::vector<std::wstring> opened, keys;
	std::vector<Toast> toasts;
} state;
struct AppSettings {
	static AppSettings& Get() { static AppSettings instance; return instance; }
	const auto& ConfigDir() { return state.config; }
};
struct Win32Helper {
	static bool ShellOpen(const wchar_t* path) { state.opened.emplace_back(path); return state.canOpen; }
};
struct CommonSharedConstants { static constexpr auto APP_RESOURCE_MAP_ID = L"Magpie/Resources"; };
struct ResourceLoader {
	static ResourceLoader GetForCurrentView(const wchar_t*) { return {}; }
	std::wstring GetString(const wchar_t* key) const {
		state.keys.emplace_back(key);
		if (state.keys.back() == L"ScalingModes_General_ConfigFolder/Text") return L"Config folder";
		assert(state.keys.back() == L"ErrorDetails_OpenConfigFailed");
		return L"The folder could not be opened.";
	}
};
struct ToastService {
	static ToastService& Get() { static ToastService instance; return instance; }
	void ShowMessageInApp(std::wstring_view title, std::wstring_view message,
		std::chrono::milliseconds duration) {
		state.toasts.push_back({std::wstring(title), std::wstring(message), duration});
	}
};
struct ScalingModesPage {
	void OpenConfigFolderButton_Click(IInspectable const&, RoutedEventArgs const&);
};
#include "ConfigFolderHandler.inc"

int main() {
	ScalingModesPage page;
	for (const auto* path : { L"C:\\Fixture\\AppData\\Local\\Magpie\\config\\v4e",
		L"D:\\Portable fixture\\config\\v4e" }) {
		for (const bool succeeds : {true, false}) {
			state = {};
			state.config = path;
			state.canOpen = succeeds;
			page.OpenConfigFolderButton_Click({}, {});
			assert(state.opened.size() == 1 && state.opened[0] == path);
			assert(state.toasts.size() == (succeeds ? 0u : 1u));
			if (!succeeds) {
				assert(state.keys.size() == 2);
				assert(state.toasts[0].title == L"Config folder");
				assert(state.toasts[0].message == std::wstring(L"The folder could not be opened.\n") + path);
				assert(state.toasts[0].duration == std::chrono::seconds(8));
			}
		}
	}
}
