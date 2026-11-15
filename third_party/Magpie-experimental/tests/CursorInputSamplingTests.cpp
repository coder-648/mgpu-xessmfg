// Real production sampling with fixture input/window state; no mouse injection.
#include <Windows.h>
#include "../src/Magpie.Core/CursorRefreshPolicy.h"
#include "../src/Magpie.Core/FrameTrace.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

inline bool operator==(const POINT& a, const POINT& b) { return a.x == b.x && a.y == b.y; }
static HCURSOR MockLoadCursor(HINSTANCE, LPCWSTR) { return reinterpret_cast<HCURSOR>(uintptr_t(3)); }
#undef LoadCursor
#define LoadCursor MockLoadCursor

namespace Magpie {
struct ScalingOptions { std::optional<float> autoHideCursorDelay; };
struct CursorManager {
	POINT pos{100, 200};
	HCURSOR handle = reinterpret_cast<HCURSOR>(uintptr_t(1));
	bool captured = true;
	POINT CursorPos() const { return pos; }
	HCURSOR CursorHandle() const { return handle; }
	bool IsCursorCaptured() const { return captured; }
};
struct Renderer { bool move = false; bool IsToolbarMoveCursor() const { return move; } };
struct Tracker { bool moving = false; bool IsMoving() const { return moving; } };
struct ScalingWindow {
	ScalingOptions options;
	Magpie::CursorManager cursor;
	Magpie::Renderer renderer;
	Tracker tracker;
	RECT rect{10, 20, 1000, 1000};
	bool moving = false;
	static ScalingWindow& Get() { static ScalingWindow window; return window; }
	const ScalingOptions& Options() const { return options; }
	const Magpie::CursorManager& CursorManager() const { return cursor; }
	const Magpie::Renderer& Renderer() const { return renderer; }
	const RECT& RendererRect() const { return rect; }
	const Tracker& SrcTracker() const { return tracker; }
	bool IsResizingOrMoving() const { return moving; }
};
struct CursorDrawer {
	CursorRefreshPolicy _refreshPolicy;
	HCURSOR _lastRawCursorHandle = nullptr;
	POINT _lastRawCursorPos{-1, -1};
	std::chrono::steady_clock::time_point _lastCursorActiveTime{};
	bool _isCursorVisible = true;
	bool _drawSucceeded = false;
	CursorVisualState _SampleCursorState() noexcept;
	bool NeedRedraw() noexcept;
	bool IsMinimumRefreshDue() noexcept;
	bool HasVisibilityTransition() noexcept;
	void OnPresent(bool success, bool independentLayer = false) noexcept;
};
#include "CursorInputProduction.inc"
}
#undef LoadCursor

using namespace Magpie;
using namespace std::chrono_literals;
static int checks = 0;
static void Check(bool value, const char* message) {
	++checks;
	if (!value) throw std::runtime_error(message);
}
int main() {
	auto& window = ScalingWindow::Get();
	window.options.autoHideCursorDelay = 0.1f;
	CursorDrawer drawer;
	drawer._refreshPolicy.Configure({true, false, 60}, 144);
	const auto first = drawer._SampleCursorState();
	Check(first.x == 90 && first.y == 180, "Renderer-local coordinates wrong");
	drawer._refreshPolicy.ObserveContent({1, 1, 1}, false);
	drawer._refreshPolicy.Prepare(first, CursorRefreshPolicy::Clock::now());
	drawer._refreshPolicy.Presented(true, CursorRefreshPolicy::Clock::now());
	window.cursor.pos.x += 20;
	Check(!drawer.NeedRedraw(), "Strict visual throttle was bypassed by sampling");
	Check(drawer._lastRawCursorPos.x == 110, "Real input was throttled with visual state");
	const auto moved = drawer._SampleCursorState();
	Check(drawer._refreshPolicy.Prepare(moved, CursorRefreshPolicy::Clock::now()).x == 90,
		"Drawing snapshot sampled new input without original");
	drawer._refreshPolicy.Presented(true, CursorRefreshPolicy::Clock::now());
	drawer._lastCursorActiveTime = CursorRefreshPolicy::Clock::now() - 1s;
	Check(drawer._SampleCursorState().handle == 0, "Held snapshot prevented auto hide");
	Check(drawer.HasVisibilityTransition() && drawer.NeedRedraw(), "Auto hide transition did not wake strict mode");
	window.cursor.pos.x += 1;
	Check(drawer._SampleCursorState().handle == 1, "Movement failed to restore auto-hidden cursor");
	Check(CursorRefreshPolicy::Clock::now() - drawer._lastCursorActiveTime < 50ms,
		"Activity time follows presentation instead of input");
	drawer._lastCursorActiveTime = CursorRefreshPolicy::Clock::now() - 1s;
	window.cursor.captured = false;
	Check(drawer._SampleCursorState().handle == 1, "Overlay cursor auto-hidden");
	window.cursor.captured = true;
	window.renderer.move = true;
	Check(drawer._SampleCursorState().handle == 3, "Toolbar move cursor not sampled");
	drawer._isCursorVisible = false;
	Check(drawer._SampleCursorState().handle == 0 && drawer._SampleCursorState().suppressed,
		"Screenshot suppression not sampled");
	drawer._isCursorVisible = true;
	window.renderer.move = false;
	window.cursor.handle = nullptr;
	Check(drawer._SampleCursorState().handle == 0, "System cursor handoff retained software cursor");
	window.cursor.handle = reinterpret_cast<HCURSOR>(uintptr_t(1));
	window.rect = {100, 200, 1000, 1000};
	Check(drawer._SampleCursorState().x == 21 && drawer._SampleCursorState().y == 0,
		"Cross-screen coordinates kept old renderer origin");
	drawer._refreshPolicy.Configure({true, false, 60}, 144);
	const auto stable = drawer._SampleCursorState();
	drawer._refreshPolicy.Prepare(stable, CursorRefreshPolicy::Clock::now());
	drawer._drawSucceeded = true;
	drawer.OnPresent(true);
	Check(!drawer.NeedRedraw(), "Successful unchanged input requested replay");
	drawer._refreshPolicy.Prepare(stable, CursorRefreshPolicy::Clock::now());
	drawer._drawSucceeded = false;
	drawer.OnPresent(true);
	Check(drawer.NeedRedraw(), "Successful surface with failed cursor draw lost recovery refresh");
	std::cout << "Production cursor input/activity sampling: " << checks << " checks passed\n";
}
