#pragma once
#include "include/CursorRefreshSettings.h"
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace Magpie {

struct CursorContentIdentity {
	uint64_t frameId = 0;
	uint64_t captureSequence = 0;
	uint64_t resourceGeneration = 0;
	bool operator==(const CursorContentIdentity&) const noexcept = default;
};

struct CursorVisualState {
	uintptr_t handle = 0;
	int32_t x = 0, y = 0;
	bool captured = false;
	bool toolbarMove = false;
	bool suppressed = false;

	bool SameImage(const CursorVisualState& other) const noexcept {
		return handle == other.handle && (!handle || (x == other.x && y == other.y));
	}
};

// Frontend-owned. Draw prepares a snapshot; only a successful presentation
// commits it. Redrawing cached pixels never postpones the cursor deadline.
class CursorRefreshPolicy {
public:
	using Clock = std::chrono::steady_clock;
	void Configure(CursorRefreshSettings settings, double displayRate) noexcept {
		*this = {};
		_settings = settings;
		SetDisplayRate(displayRate);
	}
	void SetDisplayRate(double displayRate) noexcept {
		if (!(displayRate >= 1.0 && displayRate <= 1000.0)) displayRate = 60.0;
		const double rate = std::min<double>(
			_settings.automaticRefreshRate ? displayRate :
				CursorRefreshSettings::ValidateRate(_settings.minimumRefreshRate), displayRate);
		_interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(1.0 / rate));
	}
	void ResetVisual() noexcept {
		_started = _hasPublished = _prepared = false;
		_published = {};
	}
	void ObserveContent(CursorContentIdentity identity, bool generated) noexcept {
		// Generated images may carry the same capture ID as the later original.
		if (!generated && identity.frameId) _availableOriginal = identity;
	}
	bool HasNewOriginal() const noexcept {
		return _availableOriginal.frameId && _availableOriginal != _committedOriginal;
	}
	bool NeedsRedraw(const CursorVisualState& state, Clock::time_point now) const noexcept {
		return Changed(state) && CanPublish(state, now);
	}
	bool MinimumDue(const CursorVisualState& state, Clock::time_point now) const noexcept {
		return Changed(state) && _settings.minimumRefreshEnabled &&
			(!_started || now - _lastPublished >= _interval);
	}
	bool Transition(const CursorVisualState& state) const noexcept {
		// Visibility and cursor ownership must not leave a stale or double cursor.
		return !_hasPublished || bool(state.handle) != bool(_published.handle) ||
			state.captured != _published.captured || state.toolbarMove != _published.toolbarMove ||
			state.suppressed != _published.suppressed;
	}
	CursorVisualState Prepare(const CursorVisualState& state, Clock::time_point now) noexcept {
		_pending = CanPublish(state, now) ? state : _published;
		_pendingOriginal = _availableOriginal;
		_prepared = true;
		return _pending;
	}
	bool Presented(bool success, Clock::time_point now) noexcept {
		if (!_prepared) return false;
		_prepared = false;
		if (!success) return false;
		const bool changed = Changed(_pending);
		if (changed) {
			_lastPublished = now;
			_started = true;
		}
		_published = _pending;
		_hasPublished = true;
		_committedOriginal = _pendingOriginal;
		return changed;
	}
	// XeSS can successfully submit content without changing its independent UI
	// surface. Consume an unchanged original so a later mouse move cannot use it.
	void AcknowledgeUnchangedOriginal(const CursorVisualState& state) noexcept {
		if (_hasPublished && !Changed(state)) _committedOriginal = _availableOriginal;
	}
	std::chrono::nanoseconds PollInterval(Clock::time_point now) const noexcept {
		auto interval = std::min(_interval, std::chrono::nanoseconds(std::chrono::milliseconds(8)));
		if (_settings.minimumRefreshEnabled && _started) {
			const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
				_lastPublished + _interval - now);
			if (remaining > std::chrono::nanoseconds::zero()) interval = std::min(interval, remaining);
		}
		return interval;
	}
private:
	bool Changed(const CursorVisualState& state) const noexcept {
		return !_hasPublished || !state.SameImage(_published) || Transition(state);
	}
	bool CanPublish(const CursorVisualState& state, Clock::time_point now) const noexcept {
		return Transition(state) || !_settings.preferOriginalFrames || HasNewOriginal() ||
			MinimumDue(state, now);
	}
	CursorRefreshSettings _settings;
	CursorContentIdentity _availableOriginal{}, _committedOriginal{}, _pendingOriginal{};
	CursorVisualState _published{}, _pending{};
	Clock::time_point _lastPublished{};
	std::chrono::nanoseconds _interval{16'666'666};
	bool _hasPublished = false, _started = false, _prepared = false;
};

}
