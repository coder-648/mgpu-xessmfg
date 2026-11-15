#pragma once
#include "FramePacingOptions.h"
#include <rapidjson/document.h>

namespace Magpie {

// Missing fields inherit the migration fallback. Present but malformed fields
// use factory defaults, independently of the other two fields.
inline FrameSyncSettings ReadProfileFrameSync(
	const rapidjson::GenericObject<true, rapidjson::Value>& object,
	FrameSyncSettings fallback = {}) noexcept {
	const FrameSyncSettings defaults;
	if (const auto field = object.FindMember("frontEdgeSync"); field != object.MemberEnd())
		fallback.enabled = field->value.IsBool() ? field->value.GetBool() : defaults.enabled;
	if (const auto field = object.FindMember("frontEdgeSyncFrameRate"); field != object.MemberEnd()) {
		const auto& value = field->value;
		fallback.frameRate = value.IsNumber() && std::isfinite(value.GetDouble()) &&
			value.GetDouble() >= 0 && value.GetDouble() <= 1000 &&
			(value.GetDouble() == 0 || value.GetDouble() >= 1)
			? float(value.GetDouble()) : defaults.frameRate;
	}
	if (const auto field = object.FindMember("frameSyncMode"); field != object.MemberEnd())
		fallback.mode = field->value.IsUint() && field->value.GetUint() <= uint32_t(FrameSyncMode::Reflex)
			? FrameSyncMode(field->value.GetUint()) : defaults.mode;
	return fallback;
}

template<class Writer>
void WriteProfileFrameSync(Writer& writer, const FrameSyncSettings& settings) {
	writer.Key("frontEdgeSync"); writer.Bool(settings.enabled);
	writer.Key("frontEdgeSyncFrameRate"); writer.Double(SanitizePresentationFrameRate(settings.frameRate));
	writer.Key("frameSyncMode"); writer.Uint(uint32_t(IsValidFrameSyncMode(settings.mode)
		? settings.mode : FrameSyncMode::FrontEdge));
}

inline bool HasProfileFrameSync(const rapidjson::GenericObject<true, rapidjson::Value>& object) noexcept {
	return object.HasMember("frontEdgeSync") && object.HasMember("frontEdgeSyncFrameRate") &&
		object.HasMember("frameSyncMode");
}
}
