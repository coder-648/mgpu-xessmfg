"""Extract production shortcut paths; mock OS input, not application decisions."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])

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

settings = read('src/Magpie/AppSettings.cpp')
service = read('src/Magpie/ShortcutService.cpp')
control = read('src/Magpie/ShortcutControl.cpp')
shortcut = read('src/Magpie/Shortcut.cpp')
idl = read('src/Magpie/ShortcutControl.idl')
enum = idl[idl.index('enum ShortcutAction'):idl.index('};') + 2].replace('enum ShortcutAction', 'enum class ShortcutAction')
parts = [enum]
for source, signatures in [
	(read('src/Magpie/ShortcutHelper.cpp'), ['bool ShortcutHelper::IsValidKeyCode(']),
    (shortcut, ['bool Shortcut::IsEmpty()', 'void Shortcut::Clear()', 'std::wstring Shortcut::ToString()']),
    (settings, ['static uint32_t EncodeShortcut(', 'static void DecodeShortcut(',
                'void AppSettings::_SetDefaultShortcuts()', 'bool AppSettings::_LoadShortcuts(', 'void AppSettings::SetShortcut(']),
    (service, ['void ShortcutService::_RegisterShortcut(', 'void ShortcutService::_FireShortcut(',
               'LRESULT CALLBACK ShortcutService::_LowLevelKeyboardProc(']),
    (control, ['void ShortcutControl::_ClearShortcut()', 'void ShortcutControl::_StopEditing()',
               'void ShortcutControl::ClearButton_Click(', 'void ShortcutControl::_ShortcutDialog_Closing(']),
]:
    parts.extend(block(source, signature) for signature in signatures)
# Use the exact production serialization block, including all eight zero fields.
start = settings.index('\twriter.Key("shortcuts");')
end = settings.index('\n\twriter.Key("countdownSeconds");', start)
parts.append('std::string AppSettings::Serialize() const {\n'
             'rapidjson::StringBuffer buffer; rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);\n'
             'writer.StartObject(); const auto& data = *this;\n' + settings[start:end] +
             '\nwriter.EndObject(); return buffer.GetString();\n}')
parts.append(block(read('src/Magpie.Core/OverlayDrawer.cpp'), 'static std::string FormatToolbarTooltip('))
(output / 'ShortcutClearProduction.inc').write_text('\n\n'.join(parts[1:]), encoding='utf-8')
(output / 'ShortcutAction.inc').write_text(enum, encoding='utf-8')

def require(value, message):
    if not value:
        raise AssertionError(message)

# These checks complement compiled decisions; they do not simulate native XAML.
ui = ET.fromstring(read('src/Magpie/HomePage.xaml'))
ns = '{using:Magpie}'
xuid = '{http://schemas.microsoft.com/winfx/2006/xaml}Uid'
rows = [n for n in ui.iter(ns + 'ShortcutControl') if n.get('IsClearButtonVisible') == 'True']
require(len(rows) == 6 and all('Home_Toolbar_' in n.get(xuid, '') for n in rows), 'Only six toolbar rows expose inline clearing')
require(len(list(ui.iter(ns + 'ShortcutControl'))) == 8, 'All eight shortcut editors remain available')
grid = ET.fromstring(read('src/Magpie/ShortcutControl.xaml'))
xname = '{http://schemas.microsoft.com/winfx/2006/xaml}Name'
nodes = {n.get(xname): n for n in grid.iter() if n.get(xname)}
require(nodes['ClearButton'].get('Grid.Column') == '1', 'Trash is right of the editor')
require(nodes['NotSetLabel'].get(xuid) == 'ShortcutControl_NotSet', 'Localized empty state')
require('IsSecondaryButtonEnabled(!_shortcut.IsEmpty())' in control, 'Clear depends on saved binding, not preview validity')
require('IsPrimaryButtonEnabled(!_previewShortcut.IsEmpty()' in control, 'Empty preview cannot be saved normally')
require('auto lifetime = get_strong()' in control and 'wil::scope_exit' in control and '_that = nullptr;' in control,
        'Dialog lifetime/hook cleanup handles failure as well as ordinary closing')
require('wil::scope_exit' in read('src/Magpie/ContentDialogHelper.cpp'), 'Failed ShowAsync releases global dialog state')
required_keys = ['ShortcutDialog_Clear', 'ShortcutControl_ClearAutomationName', 'ShortcutControl_NotSet.Text',
                 'ShortcutControl_ClearButton.[using:Windows.UI.Xaml.Controls]ToolTipService.ToolTip',
                 'Overlay_Parameters_InputHintWithoutShortcut']
for locale in ['en-US', 'zh-Hans', 'zh-Hant']:
    resources = ET.fromstring(read(f'src/Magpie/Resources.language-{locale}.resw'))
    values = {n.get('name'): n.findtext('value') for n in resources.findall('data')}
    require(len(values) == len(resources.findall('data')), f'No duplicate resource keys: {locale}')
    require(all(values.get(key) for key in required_keys), f'Complete new strings: {locale}')
    require('{0}' in values['ShortcutControl_ClearAutomationName'], f'Action-specific accessibility: {locale}')
require('UpdateToolbarShortcutLabels(GetToolbarShortcutLabels())' in read('src/Magpie/ScalingService.cpp'),
        'Setting events update live toolbar hints')
require('Overlay_Parameters_InputHintWithoutShortcut' in read('src/Magpie.Core/OverlayDrawer.cpp'), 'No dangling empty shortcut hint')
print('Shortcut static contracts: eight editors, six trash buttons, resources, accessibility, cleanup and live hints passed.')
