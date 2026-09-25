#!/usr/bin/env python3
"""Add reliquary's extra VS projects (des, fatfs, libcdio, libusb, microvu_table_generator) to upstream's
PCSX2_qt.slnx (upstream replaced PCSX2_qt.sln with .slnx in 37a8be7b4). Idempotent."""
import re
p = 'PCSX2_qt.slnx'; s = open(p, encoding='utf-8').read()
blk = re.search(r'    <Project Path="3rdparty/lzma/lzma.vcxproj" Id="[^"]+">.*?</Project>\r?\n', s, re.S).group(0)
def mk(path, gid, top=False):
    b = re.sub(r'Id="[^"]+"', 'Id="%s"' % gid.lower(), blk.replace('3rdparty/lzma/lzma.vcxproj', path), 1)
    return '\n'.join(l[2:] if top and l.startswith('  ') else l for l in b.split('\n'))
add3 = ''.join(mk(a, g) for a, g in [
    ('3rdparty/des/des.vcxproj', 'EB12076D-4CEF-4D9D-8F34-5D0043798051'),
    ('3rdparty/fatfs/fatfs.vcxproj', 'F45D0FF0-617C-4A12-B1C3-7D716ED8A315'),
    ('3rdparty/libcdio/libcdio.vcxproj', 'CA84151A-201C-4971-A57B-0BF24A62BED5'),
    ('3rdparty/libusb/libusb.vcxproj', '349EE8F9-7D25-4909-AAF5-FF3FADE72187')] if a not in s)
s = s.replace(blk, blk + add3, 1)
if 'tools/microvu_table_generator.vcxproj' not in s:
    i = s.index('  <Project Path="updater/updater.vcxproj"')
    s = s[:i] + mk('tools/microvu_table_generator.vcxproj', '6F4E37C9-BD5A-4B65-90AF-473C88162E81', True) + s[i:]
open(p, 'w', encoding='utf-8', newline='').write(s)
