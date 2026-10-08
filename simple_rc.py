#!/usr/bin/env python3
"""Re-apply the direct-rc.exe patch to the ninja RC rules (run after any CMake
reconfigure; the patch lives in CMakeFiles/rules.ninja, which configure
regenerates).

Why: cmake_llvm_rc cannot build version.rc.res on this setup -- the
preprocessed .pp trips rc.exe (historically it hung outright). The fix calls
the SDK rc.exe directly with the SDK um+shared include dirs so winres.h
resolves.

Usage:  python simple_rc.py   (from the repository root, before building)
"""
import glob
import io
import re
import sys

rcs = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\rc.exe'))
incs = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\Include\10.*'))
if not rcs or not incs:
    sys.exit('Windows SDK rc.exe / Include dirs not found')
rc, sdk = rcs[-1], incs[-1]

cmd = ('"%s" /nologo $DEFINES -I SOURCE_DIR $INCLUDES $FLAGS '
       r'/i "%s\um" /i "%s\shared" /fo $out $in' % (rc, sdk, sdk))

patched = 0
for path in glob.glob('build/*/CMakeFiles/rules.ninja'):
    with io.open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    changed = False
    for i, ln in enumerate(lines):
        if 'cmake_llvm_rc' in ln:
            m = re.match(r'^(\s*command = .*?)"[^"]*cmake\.exe"', ln)
            prefix = m.group(1) if m else '  command = '
            lines[i] = prefix + cmd + '\n'
            changed = True
    if changed:
        with io.open(path, 'w', encoding='utf-8', newline='') as f:
            f.writelines(lines)
        print('patched', path)
        patched += 1
print('done,', patched, 'file(s)')
