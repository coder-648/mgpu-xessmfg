#pragma once
#include "OverlayWindowGeometry.h"
#include <array>
#include <optional>

namespace Magpie {

enum class ToolbarDock : uint8_t { Top, Bottom };

constexpr ToolbarDock SanitizeToolbarDock(uint32_t value) noexcept {
	return value == uint32_t(ToolbarDock::Bottom) ? ToolbarDock::Bottom : ToolbarDock::Top;
}

// Only these two values are serialized with the default/application profile.
struct ToolbarDockSettings {
	ToolbarDock fullscreen = ToolbarDock::Top;
	ToolbarDock windowed = ToolbarDock::Top;
	ToolbarDock& ForMode(bool windowedMode) noexcept { return windowedMode ? windowed : fullscreen; }
	ToolbarDock ForMode(bool windowedMode) const noexcept { return windowedMode ? windowed : fullscreen; }
};

struct ToolbarPositionState {
	ToolbarDockSettings docks;
	// DIPs, per mode. -1 keeps an untouched toolbar centered after size changes.
	// This state belongs to one user-started run and never enters OverlayOptions.
	std::array<float, 2> horizontal{ -1.0f, -1.0f };
};

struct ToolbarGeometry {
	float viewportWidth, viewportHeight, dpiScale, scale, width, height, rounding, zone;

	ToolbarGeometry(float viewportWidth, float viewportHeight, float dpiScale) noexcept
		: viewportWidth(std::max(1.0f, viewportWidth)), viewportHeight(std::max(1.0f, viewportHeight)),
		dpiScale(dpiScale > 0.0f ? dpiScale : 1.0f) {
		scale = std::min({ this->dpiScale, this->viewportWidth / 456.0f, this->viewportHeight / 37.0f });
		width = 456.0f * scale;
		height = 37.0f * scale;
		rounding = 6.0f * scale;
		zone = std::min(48.0f * scale, this->viewportHeight / 2.0f);
	}

	float DockY(ToolbarDock dock) const noexcept {
		return dock == ToolbarDock::Top ? -rounding : viewportHeight - height + rounding;
	}
	float ClampX(float x) const noexcept {
		return std::clamp(x, 0.0f, std::max(0.0f, viewportWidth - width));
	}
	std::optional<ToolbarDock> Target(float x, float y) const noexcept {
		if (x < 0.0f || x >= viewportWidth || y < 0.0f || y >= viewportHeight) return std::nullopt;
		if (y < zone) return ToolbarDock::Top;
		if (y >= viewportHeight - zone) return ToolbarDock::Bottom;
		return std::nullopt;
	}
};

class ToolbarPlacement {
public:
	ToolbarPositionState state;

	bool IsDragging() const noexcept { return _dragging; }
	std::optional<ToolbarDock> Target() const noexcept { return _target; }
	bool IsCenterSnapped() const noexcept { return _dragging && _centerSnapped; }
	ToolbarDock Dock(bool windowed) const noexcept { return state.docks.ForMode(windowed); }

	OverlayWindowRect Layout(bool windowed, const ToolbarGeometry& geometry) noexcept {
		if (_dragging) return { _dragX, _dragY, geometry.width, geometry.height };
		float& horizontal = state.horizontal[windowed ? 1 : 0];
		if (!std::isfinite(horizontal) || horizontal < 0.0f) horizontal = -1.0f;
		const float x = geometry.ClampX(horizontal < 0.0f ?
			(geometry.viewportWidth - geometry.width) / 2.0f : horizontal * geometry.dpiScale);
		if (horizontal >= 0.0f) horizontal = x / geometry.dpiScale;
		return { x, geometry.DockY(Dock(windowed)), geometry.width, geometry.height };
	}

	void Begin(bool windowed, const ToolbarGeometry& geometry, float mouseX, float mouseY) noexcept {
		const auto rect = Layout(windowed, geometry);
		_windowed = windowed;
		_grabX = (mouseX - rect.x) / geometry.dpiScale;
		_grabY = (mouseY - rect.y) / geometry.dpiScale;
		_dragX = rect.x;
		_dragY = rect.y;
		_dragging = true;
		Update(geometry, mouseX, mouseY);
	}

	void Update(const ToolbarGeometry& geometry, float mouseX, float mouseY) noexcept {
		if (!_dragging) return;
		_dragX = geometry.ClampX(mouseX - _grabX * geometry.dpiScale);
		_dragY = std::clamp(mouseY - _grabY * geometry.dpiScale,
			-geometry.rounding, geometry.viewportHeight - geometry.height + geometry.rounding);
		_target = geometry.Target(mouseX, mouseY);
		// Test the fresh, unsnapped bar center every frame, in output pixels.
		// Reusing the previous snapped coordinate would trap a drag at center.
		const float centeredX = (geometry.viewportWidth - geometry.width) / 2.0f;
		_centerSnapped = _target.has_value() && std::abs(_dragX - centeredX) <= 12.0f;
		if (_centerSnapped) _dragX = centeredX;
	}

	OverlayWindowRect Preview(const ToolbarGeometry& geometry) const noexcept {
		return { _dragX, geometry.DockY(_target.value_or(Dock(_windowed))), geometry.width, geometry.height };
	}

	// A horizontal-only drop is a runtime change, with no persistence request.
	std::optional<ToolbarDock> Release(const ToolbarGeometry& geometry, float mouseX, float mouseY) noexcept {
		if (!_dragging) return std::nullopt;
		Update(geometry, mouseX, mouseY);
		std::optional<ToolbarDock> changed;
		if (_target) {
			if (*_target != Dock(_windowed)) changed = _target;
			state.docks.ForMode(_windowed) = *_target;
			state.horizontal[_windowed ? 1 : 0] = _dragX / geometry.dpiScale;
		}
		Cancel();
		return changed;
	}

	void Cancel() noexcept {
		_dragging = false;
		_centerSnapped = false;
		_target.reset();
	}

private:
	bool _dragging = false, _windowed = false, _centerSnapped = false;
	float _grabX = 0.0f, _grabY = 0.0f, _dragX = 0.0f, _dragY = 0.0f;
	std::optional<ToolbarDock> _target;
};

}
