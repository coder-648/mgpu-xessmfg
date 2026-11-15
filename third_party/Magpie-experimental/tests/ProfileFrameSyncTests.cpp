#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
#include "ProfileFrameSync.h"
#include "ProfileIdentity.h"
#include "ConfigRecovery.h"
using namespace Magpie;

struct TestProfile {
	std::string name;
	int sharedScalingMode = 0;
	FrameSyncSettings frameSync;
	std::shared_ptr<const uint8_t> runtimeIdentity = std::make_shared<const uint8_t>(0);
};
FrameSyncSettings Read(const char* json, FrameSyncSettings fallback = {}) {
	rapidjson::Document doc; doc.Parse(json); assert(!doc.HasParseError());
	return ReadProfileFrameSync(static_cast<const rapidjson::Document&>(doc).GetObj(), fallback);
}
std::string Write(FrameSyncSettings settings) {
	rapidjson::StringBuffer buffer; rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject(); WriteProfileFrameSync(writer, settings); writer.EndObject();
	return buffer.GetString();
}
int main() {
	const FrameSyncSettings legacy{false, 91, FrameSyncMode::Async};
	assert(Read("{}", legacy) == legacy);
	assert(Read("{}") == FrameSyncSettings{});
	assert((Read(R"({"frontEdgeSync":true})", legacy) == FrameSyncSettings{true,91,FrameSyncMode::Async}));
	assert((Read(R"({"frontEdgeSyncFrameRate":0,"frameSyncMode":2})", legacy) == FrameSyncSettings{false,0,FrameSyncMode::Reflex}));
	assert(Read(R"({"frontEdgeSync":"bad","frontEdgeSyncFrameRate":0.5,"frameSyncMode":99})", legacy) == FrameSyncSettings{});
	for (const auto* invalid : {"null", "-1", "1001", "1e300", "\"60\"", "true"}) {
		const auto text = std::string("{\"frontEdgeSyncFrameRate\":") + invalid + "}";
		assert(Read(text.c_str(), legacy).frameRate == 60);
	}
	for (auto mode : {FrameSyncMode::FrontEdge, FrameSyncMode::Async, FrameSyncMode::Reflex}) {
		for (float rate : {0.f, 1.f, 60.f, 1000.f}) {
			const FrameSyncSettings value{false, rate, mode};
			assert(Read(Write(value).c_str(), legacy) == value);
			assert(Read(Write(Read(Write(value).c_str())).c_str()) == value);
		}
	}
	TestProfile defaults{"default",0,Read("{}",legacy)};
	std::vector<TestProfile> profiles{{"A",0,Read("{}",legacy)},{"B",0,Read("{}",legacy)}};
	const std::weak_ptr<const uint8_t> identity = profiles[0].runtimeIdentity;
	auto* target = FindProfileByIdentity(defaults,profiles,identity); assert(target == &profiles[0]);
	const auto baseline = target->frameSync;
	profiles[1].frameSync = {true,144,FrameSyncMode::Reflex};
	assert(target->frameSync == legacy && defaults.frameSync == legacy);
	target->name = "renamed"; std::swap(profiles[0],profiles[1]);
	target = FindProfileByIdentity(defaults,profiles,identity); assert(target == &profiles[1]);
	profiles.reserve(100); target = FindProfileByIdentity(defaults,profiles,identity);
	assert(target && target->name == "renamed");
	// A stale request edits only its chosen field, keeping a newer mode.
	target->frameSync.mode = FrameSyncMode::Reflex;
	assert(MergeFrameSyncSettings(target->frameSync,baseline,{false,100,FrameSyncMode::Async}));
	assert((target->frameSync == FrameSyncSettings{false,100,FrameSyncMode::Reflex}));
	const auto beforeConflict = target->frameSync;
	assert(!MergeFrameSyncSettings(target->frameSync,baseline,{false,120,FrameSyncMode::Async}));
	assert(target->frameSync == beforeConflict);
	assert(profiles[0].frameSync.frameRate == 144 && defaults.frameSync == legacy);
	assert(ResolveFrameRateFilterTarget(true,0,20,target->frameSync.frameRate,60,1) == 100);
	// Even a retained old identity cannot bind to a same-name replacement.
	const auto retained = identity.lock(); profiles.erase(profiles.begin()+1);
	profiles.push_back({"renamed",0,legacy});
	assert(!FindProfileByIdentity(defaults,profiles,identity));
	std::weak_ptr<const uint8_t> expired;
	assert(!FindProfileByIdentity(defaults,profiles,expired));
	assert(FindProfileByIdentity(defaults,profiles,std::weak_ptr<const uint8_t>(defaults.runtimeIdentity)) == &defaults);
	const auto repaired = ConfigRecovery::Prepare(R"({"version":4,"scalingModes":[],"profiles":[{"frontEdgeSync":"bad","frontEdgeSyncFrameRate":0.1,"frameSyncMode":-1}]})", "", "Recovered");
	assert(repaired.fields.size() >= 3);
	assert(ReadProfileFrameSync(static_cast<const rapidjson::Document&>(repaired.document)["profiles"][0].GetObj(),legacy) == FrameSyncSettings{});
	assert(ConfigRecovery::Prepare(ConfigRecovery::Serialize(repaired.document),"","Recovered").kind == ConfigRecovery::Kind::None);
	std::cout << "PASS profile frame sync: legacy inheritance, independent values, malformed fields, 12 round trips, identity after rename/reorder/reallocation/deletion, field merge/conflict and active target.\n";
}
