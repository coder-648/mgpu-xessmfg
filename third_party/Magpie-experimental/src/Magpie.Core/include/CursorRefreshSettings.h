#pragma once
#include <cmath>

namespace Magpie {

// Persisted per profile; these are presentation settings, not input polling.
struct CursorRefreshSettings {
	bool preferOriginalFrames = false;
	bool minimumRefreshEnabled = true;
	float minimumRefreshRate = 60.0f;
	bool automaticRefreshRate = false;

	static float ValidateRate(double value) noexcept {
		return std::isfinite(value) && value >= 1.0 && value <= 1000.0
			? static_cast<float>(value) : 60.0f;
	}
};

}
