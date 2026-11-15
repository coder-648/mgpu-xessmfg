#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Summarise a producer log (game_capture.log) into the facts a run is judged on.

    ./scripts/capture_report.py <log> [more logs...]

Works for both the ReShade add-on log and the retired own-host log, because the
interesting lines never came from the host: build/config, hoisted hooks, the
pinned input source, the harvest/semantics declaration, the sl tag sets, the
measured stream bytes and the reasons a copy was skipped.
"""
import io, re, sys, collections

def load(path):
    return io.open(path, encoding='utf-8', errors='replace').read().splitlines()

def first(lines, pat):
    r = re.compile(pat)
    for l in lines:
        if r.search(l):
            return l.strip()
    return None

def last(lines, pat):
    r = re.compile(pat)
    for l in reversed(lines):
        if r.search(l):
            return l.strip()
    return None

def all_of(lines, pat):
    r = re.compile(pat)
    return [l.strip() for l in lines if r.search(l)]

def report(path):
    lines = load(path)
    print('=' * 78)
    print(path, '- %d lines' % len(lines))
    print('=' * 78)

    # ---- identity / build
    for pat, label in ((r'=== game_capture', 'build   '),
                       (r'AddonInit \(', 'addon   '),
                       (r'reshade::register_addon FAILED', 'REGISTER'),
                       (r'loader hook \w+ OK', 'loader  ')):
        v = first(lines, pat)
        if v:
            print('%s %s' % (label, v))

    # ---- config (timestamp stripped: every line of this log carries one)
    def body(l):
        return re.sub(r'^\[[\d:.]+\]\s*', '', l)

    seen = set()
    streams_rows = []
    for l in lines:
        b = body(l)
        if re.match(r'streams\s*: \d+ frames', b):
            streams_rows.append(b)
            continue
        if not re.match(r'(config|validity|MEASUREMENT|copy|fg mode|lanes)\s*:', b):
            continue
        if b in seen:
            continue
        seen.add(b)
        print('config   %s' % b)
    if not seen:
        print('config   (none - the add-on did not reach load_config)')
    if streams_rows:
        print('config   %s' % streams_rows[0])
        if len(streams_rows) > 2:
            print('config   (%d more 300-frame windows)' % (len(streams_rows) - 2))
        if len(streams_rows) > 1:
            print('config   %s' % streams_rows[-1])

    # ---- hooks the sources installed ("hook cpu: slSetTag" style lines only)
    hooks = collections.Counter()
    for l in all_of(lines, r'\bhook\s+\w+:|NOT hooked'):
        m = re.search(r'\bhook\s+\w+:\s*(\S+)|NOT hooked', l)
        if m:
            hooks[m.group(1) or 'DLSSG-NOT-hooked'] += 1
    if hooks:
        print('hooks    ' + ', '.join('%s' % k for k in sorted(hooks)))

    # ---- which source won
    src = all_of(lines, r'input source: ')
    print('source   ' + (src[-1] if src else '(never pinned)'))
    if not src:
        waiting = all_of(lines, r'no upscaler input yet')
        lanes = collections.Counter()
        for l in all_of(lines, r'lane \w+ poll='):
            lanes[re.sub(r'.*lane (\w+).*', r'\1', l)] += 1
        if waiting:
            print('         still waiting: %s' % waiting[-1].split('waiting for')[-1].strip())
            print('         lanes polled: %s' % dict(lanes))

    # ---- the FgMode decision and the interrupt
    for label, pat in (('fgmode   ', r'^fg mode:'),
                       ('lanes    ', r'^lanes:'),
                       ('interrupt', r'generation INTERRUPTED|FgSuppressNgx|FgCut=|Claim=1'),
                       ('enable   ', r'slIsFeatureSupported|GetFeatureRequirements\(DLSS-G\)|capability query')):
        r = re.compile(pat)
        hits = []
        for l in lines:
            if r.search(body(l)):
                hits.append(body(l))
        if hits:
            print('%s %s' % (label, hits[0][:130]))
            if len(hits) > 1 and label in ('interrupt', 'enable'):
                print('         (%d more)' % (len(hits) - 1))

    # ---- what data exists
    for label, pat in (('harvest  ', r'harvest:'),
                       ('semantics', r'semantics:'),
                       ('camera   ', r'camera:'),
                       ('shared   ', r'shared: producer')):
        v = last(lines, pat)
        if v:
            print('%s %s' % (label, v))

    # ---- sl tag sets: the first distinct ones are the whole story
    tags = all_of(lines, r'fg: sl\.tags')
    if tags:
        print('sl.tags  %d sets, kinds: %s' % (len(tags), _tag_kinds(lines)))
        shown = 0
        in_set = False
        for l in lines:
            if 'fg: sl.tags' in l:
                if shown >= 2:
                    break
                shown += 1
                in_set = True
                print('         ' + l.split('fg: sl.tags')[-1].strip())
                continue
            if in_set:
                if re.match(r'\s{2,}(MOTION_VECTORS|DEPTH|HUDLESS_COLOR|SCALING_INPUT|OTHER|UI)', l):
                    print('         ' + l.strip())
                else:
                    in_set = False

    # ---- measured bytes
    streams = all_of(lines, r'streams: \d+ frames')
    if streams:
        print('streams  ' + streams[-1])
        rows = []
        for l in streams:
            m = re.search(r'one copy: colour=([\d.]+) mv=([\d.]+) depth=([\d.]+) ui=([\d.]+)'
                          r' MB \| total ([\d.]+)', l)
            if m:
                rows.append(tuple(float(x) for x in m.groups()))
        if rows:
            print('         %d windows, last per-copy: mv=%.2f depth=%.2f ui=%.2f MB,'
                  ' total=%.2f MB/frame' % (len(rows), rows[-1][1], rows[-1][2],
                                            rows[-1][3], rows[-1][4]))
    else:
        print('streams  (no "streams:" line - nothing was copied for 300 frames)')

    pend = all_of(lines, r'pending: \d+ calls')
    if pend:
        print('pending  ' + pend[-1])

    # ---- why copies were skipped (dedupe to counts)
    skips = collections.Counter()
    for l in all_of(lines, r'skipped|does not match|not ready|no free slot'):
        key = re.sub(r'^\S+\s+', '', l)
        key = re.sub(r'0x[0-9A-Fa-f]+|\d+', 'N', key)[:110]
        skips[key] += 1
    for k, c in skips.most_common(6):
        print('skip x%-5d %s' % (c, k))

    # ---- device removal / fatal
    bad = [l for l in lines if re.search(
        r'DEVICE_REMOVED|device removed|EXCEPTION|crash|Gpu Crash', l, re.I)]
    print('fatal    ' + ('none' if not bad else '%d lines, last: %s' % (len(bad), bad[-1].strip()[:110])))
    print()

def _tag_kinds(lines):
    kinds = collections.Counter()
    for l in lines:
        m = re.match(r'\s+(MOTION_VECTORS|DEPTH|HUDLESS_COLOR|SCALING_INPUT|UI|OTHER)\b', l)
        if m:
            kinds[m.group(1)] += 1
    return dict(kinds)

if __name__ == '__main__':
    args = sys.argv[1:] or ['game_capture.log']
    missing = [a for a in args if not __import__('os').path.isfile(a)]
    for m in missing:
        print('! no such log: %s' % m)
    for p in [a for a in args if a not in missing]:
        report(p)
    sys.exit(0)
