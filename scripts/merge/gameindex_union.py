#!/usr/bin/env python3
"""GameIndex.yaml union for integration: base = reliquary (full retail DB + Python entries),
plus every x6 (Namco 246/256 arcade) entry whose serial is not in base. x6 ships an arcade-only DB.
usage: gameindex_union.py BASE.yaml ARCADE.yaml OUT.yaml"""
import sys, re
def blocks(text):
    out, key, cur = {}, None, []
    order = []
    for line in text.split('\n'):
        m = re.match(r'^([^\s#][^:]*):\s*$', line)
        if m:
            if key: out[key] = cur
            key, cur = m.group(1), [line]; order.append(key)
        elif key: cur.append(line)
    if key: out[key] = cur
    return order, out
base_txt = open(sys.argv[1], encoding='utf-8').read(); arc_txt = open(sys.argv[2], encoding='utf-8').read()
bo, b = blocks(base_txt); ao, a = blocks(arc_txt)
add = [k for k in ao if k not in b]
dup = [k for k in ao if k in b]
out = base_txt.rstrip('\n') + '\n'
if add:
    out += '\n'.join('\n'.join(a[k]).rstrip('\n') for k in add) + '\n'
open(sys.argv[3], 'w', encoding='utf-8', newline='\n').write(out)
print(f'base {len(bo)} entries, arcade {len(ao)}: appended {len(add)}, already present {len(dup)} {dup[:10]}')
