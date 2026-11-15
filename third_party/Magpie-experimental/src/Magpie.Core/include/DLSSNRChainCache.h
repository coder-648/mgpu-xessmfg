#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Magpie {

// Cached outputs belong to individual passes; only the dirty suffix is recorded.
struct DLSSNRChainCache {
	bool valid = false;
	uint64_t frame = std::numeric_limits<uint64_t>::max();
	uint64_t inputRevision = 0;
	std::array<uint64_t, 3> revisions{};

	size_t FirstDirty(uint64_t currentFrame, uint64_t currentInput,
		bool sameGuidance, std::span<const uint64_t> currentRevisions) const noexcept {
		if (!valid || frame != currentFrame || inputRevision != currentInput || !sameGuidance) return 0;
		for (size_t i = 0; i < currentRevisions.size(); ++i)
			if (revisions[i] != currentRevisions[i]) return i;
		return currentRevisions.size();
	}
	void Commit(uint64_t currentFrame, uint64_t currentInput,
		std::span<const uint64_t> currentRevisions) noexcept {
		frame = currentFrame;
		inputRevision = currentInput;
		for (size_t i = 0; i < currentRevisions.size(); ++i) revisions[i] = currentRevisions[i];
		valid = true;
	}
};

}
