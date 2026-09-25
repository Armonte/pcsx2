#!/usr/bin/env python3
"""Resolve git conflict hunks per index: resolve_hunks.py FILE CHOICE[,CHOICE...]
CHOICE per hunk (in order): o=ours  t=theirs  ot=ours then theirs  to=theirs then ours  s=skip (leave markers)."""
import sys
path, choices = sys.argv[1], sys.argv[2].split(',')
lines = open(path, encoding='utf-8', newline='').read().split('\n')
out, i, h = [], 0, 0
while i < len(lines):
    l = lines[i]
    if l.startswith('<<<<<<< '):
        j = i + 1; ours = []
        while not lines[j].startswith(('=======', '||||||| ')): ours.append(lines[j]); j += 1
        if lines[j].startswith('||||||| '):
            while not lines[j].startswith('======='): j += 1
        j += 1; theirs = []
        while not lines[j].startswith('>>>>>>> '): theirs.append(lines[j]); j += 1
        c = choices[h] if h < len(choices) else 's'
        if c == 's': out += lines[i:j + 1]
        else: out += {'o': ours, 't': theirs, 'ot': ours + theirs, 'to': theirs + ours}[c]
        h += 1; i = j + 1; continue
    out.append(l); i += 1
open(path, 'w', encoding='utf-8', newline='').write('\n'.join(out))
print(f'{path}: {h} hunks')
