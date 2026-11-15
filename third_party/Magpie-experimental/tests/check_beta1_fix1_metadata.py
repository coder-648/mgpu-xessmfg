"""Check display-only metadata/resources; native popup positioning needs GUI QA."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
read = lambda path: (repo / path).read_text(encoding='utf-8-sig')
shader = read('src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl')
blocks = re.findall(r'//!PARAMETER\n(.*?^(?:float|int) (\w+);)', shader, re.S | re.M)
assert len(blocks) == 41
order = list(dict.fromkeys(re.search(r'//!GROUP (.+)', block)[1] for block, name in blocks))
assert order == ['Detail Control', 'Advanced Adjustments', 'DLSSNR · Pass 1', 'DLSSNR · Pass 2', 'DLSSNR · Pass 3']
assert all('\\n' not in re.search(r'//!LABEL (.+)', block)[1] for block, name in blocks)
for language in ('en-US', 'zh-Hans', 'zh-Hant'):
    root = ET.fromstring(read(f'src/Magpie/Resources.language-{language}.resw'))
    values = {n.get('name'): n.findtext('value') for n in root.findall('data')}
    assert len(values) == len(root.findall('data')), language
    for block, name in blocks:
        base = re.sub(r'^pass[23]_', '', name)
        key = 'EffectParam_DLSSNR_DLSSNR_AI_Filter_' + base
        assert values.get(key+'_Description'), (language, name)
        label = values[key+'_Label']
        assert '\n' not in label and not any(term in label for term in ('HSL','Oklab'))
    assert not any('residualColorMode' in key or 'residualShowProtection' in key for key in values)
xaml = ET.fromstring(read('src/Magpie/ScalingModesPage.xaml'))
ns = '{http://schemas.microsoft.com/winfx/2006/xaml/presentation}'
flyout = next(n for n in xaml.iter(ns+'Flyout') if n.get('Opening')=='EffectParametersFlyout_Opening')
assert flyout.get('ShouldConstrainToRootBounds') == 'False'
template = next(n for n in xaml.iter(ns+'DataTemplate') if n.get('{http://schemas.microsoft.com/winfx/2006/xaml}Key')=='EffectParametersFlyout')
tips = list(template.iter(ns+'ToolTip'))
assert len(tips)==4 and all(n.get('MaxWidth')=='360' for n in tips)
assert all(n.find(ns+'TextBlock').get('TextWrapping')=='Wrap' for n in tips)
rules = read('src/Magpie.Core/include/EffectParameterRules.h')
assert 'DLSSNRColorMode' not in rules and 'IsDLSSNRAdvancedParameter' in rules
filter_source = read('src/Magpie.Core/DLSSNRFilter.cpp')
assert 'RGBToHSL' not in filter_source and 'ApplyResidualControls' not in filter_source
assert 'residualColorMode' not in filter_source
print('PASS fix1 metadata: 41 compact labels, five ordered groups, 3-language descriptions, name-only wrapped XAML help, desktop popup and unique residual path.')
