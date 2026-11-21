import re, sys
src_path='third_party/OptiScaler-master/OptiScaler/misc/GameConfigs.cpp'
s=open(src_path,encoding='utf-8',errors='replace').read()
key='const std::vector<GameConfig> gameConfigs = {'
i=s.index(key)+len(key)-1  # at '{'
# find matching close
depth=0; end=None
for j in range(i,len(s)):
    c=s[j]
    if c=='{': depth+=1
    elif c=='}':
        depth-=1
        if depth==0:
            end=j; break
body=s[i+1:end]

# split top-level entries
def split_entries(b):
    out=[]; depth=0; start=None
    for k,c in enumerate(b):
        if c=='{':
            if depth==0: start=k
            depth+=1
        elif c=='}':
            depth-=1
            if depth==0 and start is not None:
                out.append(b[start:k+1]); start=None
    return out
entries=split_entries(body)

def field(entry, name):
    m=re.search(r'\.'+name+r'\s*=\s*\{', entry)
    if not m: return None
    a=m.end()-1; depth=0
    for j in range(a,len(entry)):
        if entry[j]=='{': depth+=1
        elif entry[j]=='}':
            depth-=1
            if depth==0: return entry[a+1:j]
    return None

def quoted(t):
    return re.findall(r'"([^"]*)"', t or '')

def flat(t):
    if not t: return ''
    t=re.sub(r'//[^\n]*','',t)
    t=re.sub(r'\s+',' ',t).strip().strip(',').strip()
    return t

rows=[]
for e in entries:
    exes=quoted(field(e,'exes'))
    ue=quoted(field(e,'ueExes'))
    defaults=flat(field(e,'defaults'))
    quirks=flat(field(e,'quirks'))
    if not (exes or ue): continue
    rows.append((exes,ue,defaults,quirks))
print('entries:',len(rows))

# only rows that mention anything FG-input related (plus all ue rows for detection)
import collections
fg_tokens=('FG','XeFG','ForceUnrealEngine','ForceFGRenderSizeMVs','IgnoreValidUntilEvaluateForFG','IgnoreTagsWithoutHudlessForFG','ForceDepthD32S8','XeFGCameraMotionFill','CyberpunkHudlessState')
def relevant(r):
    exes,ue,defaults,quirks=r
    blob=defaults+' '+quirks
    return any(t in blob for t in fg_tokens) or bool(ue)
rel=[r for r in rows if relevant(r)]
print('fg/ue relevant:',len(rel))
for r in rel[:12]: print('  ',r)

# emit header
def cstr(x):
    return '"'+x.replace('\\','\\\\').replace('"','\\"')+'"'
out=[]
out.append('// GENERATED - do not edit by hand.')
out.append('// Source: third_party/OptiScaler-master/OptiScaler/misc/GameConfigs.cpp')
out.append('//         (GPL-3.0; keep this provenance and third_party/optiscaler/LICENSE).')
out.append('// Generator: scripts/gen_input_quirks.py. Rows kept when they mention an FG-input')
out.append('// setting/quirks or declare .ueExes (UE detection).')
out.append('//')
out.append('// Fields are OptiScaler\'s own text, unmodified: `defaults` = "Setting = value; ...",')
out.append('// `quirks` = GameQuirk names. 025_input_quirks.inc maps them onto our switches.')
out.append('#pragma once')
out.append('')
out.append('struct OptiGameRow')
out.append('{')
out.append('    const char* exes;     // lowercase exe names, comma separated')
out.append('    const char* ue_exes;  // UE project prefixes, comma separated')
out.append('    const char* defaults; // OptiScaler "Setting = value" list')
out.append('    const char* quirks;   // OptiScaler GameQuirk names')
out.append('};')
out.append('')
out.append('static const OptiGameRow kOptiGameRows[] = {')
for exes,ue,defaults,quirks in rel:
    out.append('    { %s, %s, %s, %s },' % (cstr(', '.join(exes)), cstr(', '.join(ue)), cstr(defaults), cstr(quirks)))
out.append('};')
out.append('static const unsigned kOptiGameRowCount = sizeof(kOptiGameRows)/sizeof(kOptiGameRows[0]);')
open('lock_frame/input_quirks_data.h','w',encoding='utf-8').write('\n'.join(out)+'\n')
print('wrote lock_frame/input_quirks_data.h', len(out),'lines')
