#!/usr/bin/env python3
"""Worst-case stack depth per entry point, from GCC's -fcallgraph-info=su output.

usage: stack_report.py <dir with *.ci> [root-regex]     (default root-regex: ^oz_lock_)

Build the .ci files with:  make -f port/ak1037/Makefile EXTRA="-fstack-usage -fcallgraph-info=su"

Limits, printed with the report: functions outside ozcore (libc, SDK) count as 0 bytes, an
indirect call counts as 0 plus a note, and recursion is reported, not followed.
"""
import glob, os, re, sys

NODE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
BYTES = re.compile(r'\\n(\d+) bytes \((static|dynamic[^)]*)\)')

frame, kind, calls = {}, {}, {}
for path in glob.glob(os.path.join(sys.argv[1], '*.ci')):
    for line in open(path, encoding='utf-8', errors='replace'):
        m = NODE.search(line)
        if m:
            b = BYTES.search(m.group(2))
            if b:                                   # defined here, with a known frame
                frame[m.group(1)] = int(b.group(1))
                kind[m.group(1)] = b.group(2)
            continue
        m = EDGE.search(line)
        if m:
            calls.setdefault(m.group(1), []).append(m.group(2))

notes, memo = set(), {}

def depth(fn, stack):
    """Bytes of the deepest call chain starting at fn, and the chain itself."""
    if fn in memo:
        return memo[fn]
    if fn in stack:
        notes.add('recursion: ' + ' -> '.join(stack[stack.index(fn):] + [fn]))
        return 0, []
    if fn not in frame:
        if fn.startswith('__indirect_call'):
            notes.add('indirect call from ' + stack[-1] + ' (counted as 0)')
        return 0, []
    best, chain = 0, []
    for callee in calls.get(fn, []):
        d, c = depth(callee, stack + [fn])
        if d > best:
            best, chain = d, c
    memo[fn] = (frame[fn] + best, [fn] + chain)
    return memo[fn]

root_re = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'^oz_lock_')
roots = sorted(f for f in frame if root_re.search(f))
for r in roots:
    d, chain = depth(r, [])
    print(f'{d:6d} B  {r}')
    print('          ' + ' -> '.join(f'{c.split(":")[-1]}({frame[c]})' for c in chain))
dyn = sorted(f for f, k in kind.items() if k != 'static')
if dyn:
    print('dynamic frames (alloca/VLA, not bounded here):', ', '.join(dyn))
for n in sorted(notes):
    print('note:', n)
