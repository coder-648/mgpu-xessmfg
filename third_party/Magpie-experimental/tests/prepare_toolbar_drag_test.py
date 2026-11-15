"""Extract checked-out toolbar UI/persistence; OS/GPU services are test doubles."""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)

def read(path):
    return (repo / path).read_text(encoding='utf-8-sig')

def block(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

drawer = read('src/Magpie.Core/OverlayDrawer.cpp')
settings = read('src/Magpie/AppSettings.cpp')
parts = [block(drawer, name) for name in [
    'static std::string FormatToolbarTooltip(',
    'bool OverlayDrawer::_DrawToolbar(',
    'void OverlayDrawer::_DrawToolbarDockHints(',
    'bool OverlayDrawer::IsToolbarAt(',
    'float OverlayDrawer::_CalcToolbarAlpha()',
    'OverlaySessionState OverlayDrawer::CaptureSessionState()',
    'void OverlayDrawer::RestoreSessionState(',
]]
session = block(read('src/Magpie.Core/include/ScalingOptions.h'), 'struct OverlaySessionState')
parts.insert(0, session + ';')
parts.append(block(read('src/Magpie/ScalingService.cpp'), 'void ScalingService::_SaveToolbarDock('))
parts.append(block(read('src/Magpie/JsonHelper.cpp'), 'bool JsonHelper::ReadUInt('))
load = re.search(r'\{\s*uint32_t fullscreen = 0, windowed = 0;.*?profile.toolbarDocks = .*?;\s*\}', settings, re.S).group()
start = settings.index('\twriter.Key("fullscreenToolbarDock");')
end = settings.index('\twriter.Key("scalingMode");', start)
save = settings[start:end]
parts.append('void LoadDocks(Profile& profile, const char* text) {\n'
             'const rapidjson::Document document = [](const char* t) { rapidjson::Document d; d.Parse(t); return d; }(text);\n'
             'assert(!document.HasParseError()); const auto profileObj = document.GetObject();\n' + load + '\n}')
parts.append('std::string SaveDocks(const Profile& profile) {\n'
             'rapidjson::StringBuffer buffer; rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);\n'
             'writer.StartObject();\n' + save + '\nwriter.EndObject(); return buffer.GetString();\n}')
# Declare the session before the mock OverlayDrawer declaration uses it.
(output / 'ToolbarSession.inc').write_text(parts.pop(0), encoding='utf-8')
# Instrument the real Button calls to validate each resulting item rectangle.
(output / 'ToolbarDragProduction.inc').write_text('\n\n'.join(parts).replace('ImGui::Button(', 'TrackedToolbarButton(').replace('ImGui::TextUnformatted(fpsText.c_str());','TrackedToolbarText(fpsText.c_str());'), encoding='utf-8')
icons = read('src/Magpie.Core/OverlayHelper.h')
icons = icons[icons.index('\tstruct SegoeIcons {'):icons.index('\n\tstatic constexpr const ImColor TIMELINE_COLORS')]
(output / 'ToolbarIcons.inc').write_text(icons, encoding='utf-8')

for locale in ['en-US', 'zh-Hans', 'zh-Hant']:
    resources = ET.fromstring(read(f'src/Magpie/Resources.language-{locale}.resw'))
    values = {n.get('name'): n.findtext('value') for n in resources.findall('data')}
    assert len(values) == len(resources.findall('data')), locale
    assert 'Overlay_Toolbar_Move' not in values, locale
    assert values.get('Home_Toolbar.Description'), locale

profile = read('src/Magpie/Profile.h')
assert 'toolbarDocks = other.toolbarDocks;' in block(profile, 'void Copy(')
assert 'runtimeIdentity' not in block(profile, 'void Copy(')
assert 'toolbarDocks = profile.toolbarDocks;' in read('src/Magpie/ScalingService.cpp')
hit = block(read('src/Magpie.Core/ScalingWindow.cpp'), 'case WM_NCHITTEST:')
assert hit.index('IsToolbarAt') < hit.index('SrcHitTest')
assert 'HTCAPTION' not in hit
assert 'ImGuiWindowFlags_NoSavedSettings' in block(drawer, 'bool OverlayDrawer::_DrawToolbar(')
assert 'GetBackgroundDrawList' in block(drawer, 'void OverlayDrawer::_DrawToolbarDockHints(')
assert 'toolbar' not in block(read('src/Magpie.Core/include/ScalingOptions.h'), 'struct OverlayOptions')
print('Toolbar source contracts: profile copying/identity, client grip hit, draw-only hints, runtime-only horizontal state and three locales passed.')
