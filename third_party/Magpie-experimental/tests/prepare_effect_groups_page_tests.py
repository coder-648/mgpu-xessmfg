"""Extract production XAML/resources for an isolated native layout regression."""
from copy import deepcopy
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
ns = {'p': 'http://schemas.microsoft.com/winfx/2006/xaml/presentation',
      'x': 'http://schemas.microsoft.com/winfx/2006/xaml', 'local': 'using:Magpie'}
for prefix, uri in ns.items():
    ET.register_namespace('' if prefix == 'p' else prefix, uri)
uid = '{' + ns['x'] + '}Uid'
name = '{' + ns['x'] + '}Name'
page = ET.parse(repo / 'src/Magpie/ScalingModesPage.xaml').getroot()
frame = page.find('local:PageFrame', ns)
actions = frame.find('local:PageFrame.HeaderAction/local:SimpleStackPanel', ns)
expected = ['Import', 'Export', 'ConfigFolder', 'Reset']
buttons = actions.findall('p:Button', ns)
assert len(buttons) == 1 and buttons[0].get(uid) == 'ScalingModes_General_MoreOptions'
menu = buttons[0].find('p:Button.Flyout/p:MenuFlyout', ns)
items = menu.findall('p:MenuFlyoutItem', ns)
assert [node.get(uid) for node in items] == ['ScalingModes_General_' + key for key in expected]
assert [node.get('Click') for node in items] == [
    '{x:Bind ViewModel.Import}', '{x:Bind ViewModel.Export}',
    'OpenConfigFolderButton_Click', 'ResetScalingModesButton_Click']
assert [node.tag.rsplit('}', 1)[-1] for node in menu] == [
    'MenuFlyoutItem', 'MenuFlyoutItem', 'MenuFlyoutItem', 'MenuFlyoutSeparator', 'MenuFlyoutItem']
assert all(item.find('p:MenuFlyoutItem.Icon/p:FontIcon', ns) is not None for item in items)
menu_fixture = deepcopy(menu)
for item in menu_fixture.findall('p:MenuFlyoutItem', ns):
    key = item.attrib.pop(uid)
    del item.attrib['Click']
    item.set('Text', key.removeprefix('ScalingModes_General_'))
menu_xaml = ET.tostring(menu_fixture, encoding='unicode')
style = page.find("p:Page.Resources/p:Style[@x:Key='ScalingModesHeaderButtonStyle']", ns)
frame_doc = ET.parse(repo / 'src/Magpie/PageFrame.xaml').getroot()
frame_resources = ''.join(ET.tostring(node, encoding='unicode')
                          for node in frame_doc.find('p:UserControl.Resources', ns))
style_xaml = ('<ResourceDictionary xmlns="' + ns['p'] + '" xmlns:x="' + ns['x'] + '">' +
              frame_resources + ET.tostring(style, encoding='unicode') + '</ResourceDictionary>')
action_resources = actions.find('local:SimpleStackPanel.Resources/p:ResourceDictionary', ns)
action_resources_xaml = ET.tostring(action_resources, encoding='unicode') if action_resources is not None else (
    '<ResourceDictionary xmlns="' + ns['p'] + '" />')
header = deepcopy(frame_doc.find(
    ".//p:Grid[@x:Name='HeaderGrid']", ns))
# The icon slot is collapsed on this page. Keep the actual title/presenter/style.
header.remove(header.find("p:ContentControl[@x:Name='IconContainer']", ns))
for node in header.iter():
    for key, value in list(node.attrib.items()):
        if value.startswith('{x:Bind') or key.startswith('{using:Magpie}'):
            del node.attrib[key]
header_xaml = ET.tostring(header, encoding='unicode')
main = frame.find('local:SimpleStackPanel', ns)
view = main.find('p:ListView', ns)
footer = main[-1]
assert footer.tag == '{using:Magpie}SimpleStackPanel'
button = deepcopy(footer.find('p:Button', ns))
assert button.get('Click') == 'NewScalingModeButton_Click'
del button.attrib['Click']
for node in button.iter():
    if node.tag == '{using:Magpie}SimpleStackPanel':
        node.tag = '{' + ns['p'] + '}StackPanel'
    if uid in node.attrib:
        del node.attrib[uid]
        node.set('Text', 'New effect group')
button_xaml = ET.tostring(button, encoding='unicode')
padding = [float(v) for v in view.get('Padding').split(',')]
margin = [float(v) for v in footer.get('Margin', '0,0,0,0').split(',')]
assert margin[1] >= 0, 'The footer must have a positive normal layout extent.'
localization = (repo / 'src/Magpie/LocalizationService.cpp').read_text(encoding='utf-8-sig')
supported = re.findall(r'L"([a-z-]+)"', localization.split('SUPPORTED_LANGUAGES{')[1].split('};')[0])
resources = {path.stem.removeprefix('Resources.language-').lower(): path
             for path in (repo / 'src/Magpie').glob('Resources.language-*.resw')}
cases = []
for language in supported:
    nodes = ET.parse(resources[language]).getroot().findall('data')
    values = {node.get('name'): node.findtext('value') for node in nodes}
    assert len(values) == len(nodes), f'Duplicate resource key in {language}'
    labels = [values.get('ScalingModes_General_MoreOptions.Content')]
    assert all(labels), f'Missing entrance Content resource in {language}'
    menu_labels = [values.get('ScalingModes_General_' + key + '.Text') for key in expected]
    assert all(menu_labels), language
    assert not any(values.get('ScalingModes_General_' + key + '.Content') for key in expected), language
    title = values['ScalingModes_PageFrame.Title']
    def wide(text):
        return 'L"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'
    cases.append('{' + ','.join(map(wide, [language, title, *labels])) +
                 ',{' + ','.join(map(wide, menu_labels)) + '}}')
fixture = '\n'.join([
    '#pragma once',
    'constexpr auto HeaderStyleXaml = LR"fixture(' + style_xaml + ')fixture";',
    'constexpr auto HeaderActionResourcesXaml = LR"fixture(' + action_resources_xaml + ')fixture";',
    'constexpr auto HeaderGridXaml = LR"fixture(' + header_xaml + ')fixture";',
    'constexpr auto OptionsMenuXaml = LR"fixture(' + menu_xaml + ')fixture";',
    'constexpr auto NewButtonXaml = LR"fixture(' + button_xaml + ')fixture";',
    'constexpr double HeaderSpacing = ' + actions.get('Spacing') + ';',
    'constexpr winrt::Thickness ListPadding{' + ','.join(map(str, padding)) + '};',
    'constexpr winrt::Thickness FooterMargin{' + ','.join(map(str, margin)) + '};',
    'struct LanguageCase { const wchar_t* language; const wchar_t* title; const wchar_t* labels[1]; const wchar_t* menuLabels[4]; };',
    'constexpr LanguageCase LanguageCases[]{' + ',\n'.join(cases) + '};',
])
(output / 'EffectGroupsPageFixture.h').write_text(fixture, encoding='utf-8')
source = (repo / 'src/Magpie/ScalingModesPage.cpp').read_text(encoding='utf-8-sig')
start = source.index('void ScalingModesPage::OpenConfigFolderButton_Click(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
(output / 'ConfigFolderHandler.inc').write_text(source[start:end], encoding='utf-8')
print(f'Extracted production menu/header/footer and Content/Text resources for {len(cases)} languages.')
