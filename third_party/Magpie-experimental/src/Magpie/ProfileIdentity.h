#pragma once
#include <memory>
#include <cstdint>

namespace Magpie {
template<class Profile, class Profiles>
Profile* FindProfileByIdentity(Profile& defaultProfile, Profiles& profiles,
	const std::weak_ptr<const uint8_t>& weakIdentity) noexcept {
	const auto identity = weakIdentity.lock();
	if (!identity) return nullptr;
	if (defaultProfile.runtimeIdentity == identity) return &defaultProfile;
	for (auto& profile : profiles) if (profile.runtimeIdentity == identity) return &profile;
	return nullptr;
}
}
