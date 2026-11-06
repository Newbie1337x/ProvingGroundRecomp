#!/usr/bin/env python3
"""Turns a Direct3D trace (--d3d_trace=<file>) into a list of the engine functions that call
Direct3D, which is where a native renderer hooks the engine.

The trace records the guest return address of each call. The generated code sets ctx.lr to
that address right before the call, so the caller is the generated function with the highest
start address at or below it.

Usage: python tools/native/d3d_trace_report.py <trace file> [--top N]
"""
import bisect
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIBRARIES = (0x82550000, 0x825C0000)


def function_starts():
    starts = set()
    func_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
    for path in glob.glob(os.path.join(ROOT, 'generated', 'pg_recomp.*.cpp')):
        text = open(path, encoding='utf-8', errors='replace').read()
        starts.update(int(m, 16) for m in func_re.findall(text))
    return sorted(starts)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    top = 40
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    names = {}
    names_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'd3d_names.txt')
    for line in open(names_path, encoding='utf-8'):
        parts = line.split('#')[0].split()
        if len(parts) >= 2 and parts[0].startswith('sub_'):
            names[parts[0]] = ' '.join(parts[1:])

    def label(name):
        return '%s %s' % (name, names[name]) if name in names else name

    starts = function_starts()
    if not starts:
        sys.exit('generated/ is empty: build the game first')

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else 0

    frames = 0
    entries = []
    by_engine = collections.Counter()
    engine_entries = collections.defaultdict(collections.Counter)
    for line in open(args[0], encoding='utf-8'):
        if line.startswith('# pg Direct3D trace:'):
            frames = int(line.split()[4])
            continue
        if line.startswith('#') or line.startswith('args ') or not line.strip():
            continue
        head, threads, callers = (part.strip() for part in line.split('|'))
        name, per_frame, max_frame, total, game_callers, lines = head.split()
        total = int(total)
        if not total:
            continue
        entries.append((name, float(per_frame), int(max_frame), total, threads))
        for item in callers.split():
            addr, count = item.split(':')
            fn = owner(int(addr, 16))
            # Calls from inside the libraries are Direct3D calling itself.
            if LIBRARIES[0] <= fn < LIBRARIES[1]:
                continue
            by_engine[fn] += int(count)
            engine_entries[fn][name] += int(count)

    print('%d frames traced, %d of the Direct3D entry points called\n' % (frames, len(entries)))
    print('Direct3D entry points by calls per frame:')
    print('  %-50s %9s %7s  threads' % ('entry', 'per frame', 'max'))
    for name, per_frame, max_frame, total, threads in entries[:top]:
        print('  %-50s %9.1f %7d  %s' % (label(name), per_frame, max_frame, threads))
    print('\nEngine functions that call Direct3D, by calls per frame:')
    for fn, count in by_engine.most_common(top):
        calls = ', '.join('%s x%.1f' % (names.get(n, n).split()[0], c / max(frames, 1))
                          for n, c in engine_entries[fn].most_common(5))
        print('  %-40s %9.1f  %s' % (label('sub_%08X' % fn), count / max(frames, 1), calls))


if __name__ == '__main__':
    main()
