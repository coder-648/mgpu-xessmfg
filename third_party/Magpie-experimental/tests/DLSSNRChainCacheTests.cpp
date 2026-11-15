#include <cassert>
#include <iostream>
#include "DLSSNRChainCache.h"

int main() {
	using Magpie::DLSSNRChainCache;
	for (size_t count : {1u, 2u, 3u}) {
		DLSSNRChainCache cache;
		std::array<uint64_t, 3> revisions{};
		const std::span<const uint64_t> active{revisions.data(), count};
		assert(cache.FirstDirty(7, 2, true, active) == 0);
		cache.Commit(7, 2, active);
		assert(cache.FirstDirty(7, 2, true, active) == count);
		assert(cache.FirstDirty(8, 2, true, active) == 0);
		assert(cache.FirstDirty(6, 2, true, active) == 0);
		assert(cache.FirstDirty(7, 3, true, active) == 0);
		assert(cache.FirstDirty(7, 2, false, active) == 0);
		// A post edit leaves the NR revisions intact. A later pass edit preserves
		// the prefix and recomputes that pass and its complete dependent suffix.
		for (size_t i = count; i-- > 0;) {
			++revisions[i];
			assert(cache.FirstDirty(7, 2, true, active) == i);
			cache.Commit(7, 2, active);
			assert(cache.FirstDirty(7, 2, true, active) == count);
		}
		cache.valid = false; // partial evaluate or submit failure
		assert(cache.FirstDirty(7, 2, true, active) == 0);
	}
	std::cout << "Production chain cache: duplicates, dirty suffix, upstream revisions, guidance and failure invalidation passed.\n";
}
