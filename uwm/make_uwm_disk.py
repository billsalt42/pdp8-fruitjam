#!/usr/bin/env python3
"""Build UWM TSS/8.25 for the Adafruit Fruit Jam PDP-8/E emulator.

  1. Assemble the UWM monitor (SI, FIP, INIT, TS8, TS8II) from src/ with the
     Fruit Jam configuration (src/FRUITJAM.PA) and lay it on RF08 tracks 0-4.
  2. Keep DEC TSS/8.24's file system (accounts, passwords and the library)
     from the base disk: the UWM sources on bitsavers include no library.
  3. Install the library programs that must match the UWM monitor:
     SYSTAT and LOGOUT (DEC sources ported to UWM's tables, lib/) and a
     LOGIN message-of-the-day program (lib/LOGIN.PA).

usage:  make_uwm_disk.py [BASE_RF.dsk] [OUTDIR]
        defaults: ../sdcard/tss8_rf.dsk and ../sdcard
Needs palbart (PDP-8 cross assembler) on PATH or in $PALBART.
Writes OUTDIR/uwm_rf.dsk and OUTDIR/uwm_init.bin.
"""
import os, sys, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from binload import load_bin
from tssfs import FS, set_ext, replace_file, rename_file

PALBART = os.environ.get('PALBART', 'palbart')
BASE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'sdcard', 'tss8_rf.dsk'))
OUTDIR = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'sdcard'))
OUT = os.path.join(OUTDIR, 'uwm_rf.dsk')
INIT = os.path.join(OUTDIR, 'uwm_init.bin')
LIB = 2                                    # the library account [0,2]

subprocess.run([sys.executable, os.path.join(HERE, 'build_uwm.py'), 'FRUITJAM.PA', OUT, BASE, INIT],
               check=True, cwd=HERE)

def pal(name):
    """Assemble lib/<name>.PA into build/ and return its core image."""
    bdir = os.path.join(HERE, 'build')
    src = open(os.path.join(HERE, 'lib', name + '.PA')).read()
    open(os.path.join(bdir, name + '.PA'), 'w').write(src)
    for ext in ('.bin', '.err', '.lst'):
        p = os.path.join(bdir, name + ext)
        if os.path.exists(p): os.remove(p)
    subprocess.run([PALBART, '-d', name + '.PA'], cwd=bdir, capture_output=True)
    err = os.path.join(bdir, name + '.err')
    if os.path.exists(err) and 'error' in open(err).read().lower():
        sys.exit(open(err).read())
    return load_bin(open(os.path.join(bdir, name + '.bin'), 'rb').read())

# UWM runs its system programs from files with extension code 34 (".SVP"),
# which marks them privileged so that they may PEEK at the monitor tables.
for name in ('SYSTAT', 'LOGOUT', 'CAT'):
    set_ext(OUT, LIB, name, 0o34)
for name in ('SYSTAT', 'LOGOUT'):
    top, cap = replace_file(OUT, LIB, name, pal(name))
    print('installed %-6s %4d of %4d words' % (name, top, cap))

# UWM starts the library program LOGIN after every login (message of the day).
# It takes over the slot of DTTEST, a DECtape test that cannot run here.
names = [f['name'] for u in FS(OUT).mfd() if u['ppn'] == LIB for f in FS(OUT).ufd(u['segs'])]
if 'LOGIN' not in names:
    rename_file(OUT, LIB, 'DTTEST', 'LOGIN')
set_ext(OUT, LIB, 'LOGIN', 0)
top, cap = replace_file(OUT, LIB, 'LOGIN', pal('LOGIN'))
print('installed LOGIN  %4d of %4d words' % (top, cap))
print('done:', OUT, INIT)
