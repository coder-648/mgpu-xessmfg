"""Extract current adapter decisions and graphics-card JSON fields verbatim.

DXGI, KMT, device creation, dispatcher and SaveAsync are deterministic doubles.
The JSON checks cover the actual graphicsCardId read/write blocks, not a full
settings-file write. The optional native probe uses real DXGI and KMT calls.
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)


def read(path):
    return (repo / path).read_text(encoding="utf-8-sig")


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


helper = block(read("src/Magpie.Core/DirectXHelper.cpp"),
               "bool DirectXHelper::IsDisplayOnlyAdapter(")
device = block(read("src/Magpie.Core/DeviceResources.cpp"),
               "bool DeviceResources::_ObtainAdapterAndDevice(")
service = read("src/Magpie/AdaptersService.cpp")
methods = "\n\n".join(block(service, signature) for signature in (
    "bool AdaptersService::Initialize(",
    "bool AdaptersService::_GatherAdapterInfos(",
    "bool AdaptersService::_UpdateProfileGraphicsCardId(",
    "void AdaptersService::_UpdateProfiles("))
(out / "AdapterDecisionsProduction.inc").write_text(
    helper + "\n\n" + device + "\n\n" + methods, encoding="utf-8")
(out / "AdapterNativeProduction.inc").write_text(helper, encoding="utf-8")
(out / "AdapterWarpProduction.inc").write_text(
    block(read("src/Magpie.Core/include/DirectXHelper.h"), "static bool IsWARP("),
    encoding="utf-8")
(out / "GraphicsCardIdProduction.inc").write_text(
    block(read("src/Magpie.Core/include/ScalingOptions.h"), "struct GraphicsCardId") + ";",
    encoding="utf-8")

settings = read("src/Magpie/AppSettings.cpp")
write_start = settings.index('\twriter.Key("graphicsCardId");')
# Stop at this object's closing write, independently of the next profile field.
# Refresh settings no longer serialize the legacy limiter field that followed it.
write_end = settings.index('\twriter.EndObject();', write_start) + len('\twriter.EndObject();')
write = settings[write_start:write_end]
load = block(settings, "\t{\n\t\tauto graphicsCardIdNode =")
json_helpers = "\n\n".join(block(read("src/Magpie/JsonHelper.cpp"), signature)
                             for signature in ("bool JsonHelper::ReadInt(",
                                               "bool JsonHelper::ReadUInt("))
(out / "AdapterConfigProduction.inc").write_text(
    json_helpers + "\n\n"
    "static void WriteGraphicsCardId(rapidjson::Writer<rapidjson::StringBuffer>& writer, "
    "const Profile& profile) {\n" + write + "\n}\n"
    "static void LoadGraphicsCardId(const rapidjson::GenericObject<true, rapidjson::Value>& "
    "profileObj, Profile& profile) {\n" + load + "\n}\n", encoding="utf-8")

# Deliberately break the original bug fix and handle cleanup. Both variants
# must fail the same behavior tests, proving these regressions are detected.
mutations = {
    "accept-display-only": ("return status >= 0 && type.IndirectDisplayDevice && !type.RenderSupported;",
                            "return false;"),
    "leak-query-handle": ("D3DKMTCloseAdapter(&close)", "MockSkippedClose(&close)"),
}
for name, (before, after) in mutations.items():
    assert before in helper, f"Mutation target changed: {name}"
    variant = out / name
    variant.mkdir(exist_ok=True)
    (variant / "AdapterDecisionsProduction.inc").write_text(
        helper.replace(before, after, 1) + "\n\n" + device + "\n\n" + methods,
        encoding="utf-8")
print("Extracted current filtering, startup/hotplug enumeration, selection, migration and graphics-card JSON.")
