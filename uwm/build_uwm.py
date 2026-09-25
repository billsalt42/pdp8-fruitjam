#!/usr/bin/env python3
"""Assemble the UWM TSS/8 (V25) monitor and build an RF08 disk image.
   tracks: 0=SI 1=FIP 2=INIT 3=TS8 4=TS8II  (4K words each)
   usage: build_uwm.py CONFIG.PA OUT_RF.dsk [BASE_RF.dsk] [OUT_INIT.bin]
   Sources are read from src/, intermediate files go to build/."""
import os, sys, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binload import load_bin
PALBART = os.environ.get('PALBART', 'palbart')
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'src')
OUTD = os.path.join(HERE, 'build')
conf = os.path.join(SRC, sys.argv[1] if len(sys.argv) > 1 else 'FRUITJAM.PA')
out_rf = sys.argv[2] if len(sys.argv) > 2 else 'uwm_rf.dsk'
base = sys.argv[3] if len(sys.argv) > 3 else None
out_init = sys.argv[4] if len(sys.argv) > 4 else 'uwm_init.bin'
os.makedirs(OUTD, exist_ok=True)
import re
comps = ['SI', 'FIP', 'INIT', 'TS8', 'TS8II']
images = {}

def parse_link(fname):
    """PARB/PARC: list of (name, value, comment) in file order."""
    out = []
    for l in open(fname).read().splitlines():
        m = re.match(r'^([A-Z0-9]+)=\s*([0-7]+)\s*(.*)$', l)
        out.append((m.group(1), int(m.group(2), 8), m.group(3)) if m else (None, None, l))
    return out

def write_link(fname, entries, values, strip=()):
    lines = []
    for n, v, c in entries:
        if n is None: lines.append(c)
        elif n in strip: continue
        else: lines.append('%s=\t%04o\t%s' % (n, values.get(n, v), c))
    open(fname, 'w').write('\n'.join(lines) + '\n')

def assemble(m, parb, parc):
    src = ''.join(open(f).read() for f in [conf, os.path.join(SRC, 'PARA.PA'), parb, parc, os.path.join(SRC, m + '.PA')])
    open(os.path.join(OUTD, '%s.pal' % m), 'w').write(src)
    for ext in ('.err', '.bin', '.lst'):
        if os.path.exists(os.path.join(OUTD, m + ext)): os.remove(os.path.join(OUTD, m + ext))
    r = subprocess.run([PALBART, '-d', '%s.pal' % m], cwd=OUTD, capture_output=True, text=True)
    errs = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l.lower()]
    if os.path.exists(os.path.join(OUTD, '%s.err' % m)):
        errs += [l.strip() for l in open(os.path.join(OUTD, '%s.err' % m)) if 'error' in l.lower()]
    lst = open(os.path.join(OUTD, '%s.lst' % m)).read()
    tab = lst[lst.find('Symbol Table') if 'Symbol Table' in lst else -20000:]
    syms = {k: int(v, 8) & 0o7777 for k, v in re.findall(r'\b([A-Z][A-Z0-9]*)\s+([0-7]{4,5})\b', tab)}
    return errs, syms

# --- link: iterate TS8 / TS8II until the PARB/PARC linkage values are stable
parb, parc = parse_link(os.path.join(SRC, 'PARB.PA')), parse_link(os.path.join(SRC, 'PARC.PA'))
vals = {n: v for n, v, c in parb + parc if n}
bnames = {n for n, v, c in parb if n}; cnames = {n for n, v, c in parc if n}
for it in range(8):
    write_link(os.path.join(OUTD, 'PARB.X'), parb, vals, strip=bnames); write_link(os.path.join(OUTD, 'PARC.X'), parc, vals)
    e1, s1 = assemble('TS8', os.path.join(OUTD, 'PARB.X'), os.path.join(OUTD, 'PARC.X'))
    new = dict(vals); new.update({n: s1[n] for n in bnames if n in s1})
    write_link(os.path.join(OUTD, 'PARB.X'), parb, new); write_link(os.path.join(OUTD, 'PARC.X'), parc, new, strip=cnames)
    e2, s2 = assemble('TS8II', os.path.join(OUTD, 'PARB.X'), os.path.join(OUTD, 'PARC.X'))
    new.update({n: s2[n] for n in cnames if n in s2})
    changed = {n: (vals[n], new[n]) for n in new if vals.get(n) != new[n]}
    vals = new
    print('link pass %d: %d changed %s' % (it + 1, len(changed), ' '.join('%s:%04o->%04o' % (k, a, b) for k, (a, b) in sorted(changed.items()))))
    if not changed: break
else:
    print('linkage did not converge'); sys.exit(1)
write_link(os.path.join(OUTD, 'PARB.PA'), parb, vals); write_link(os.path.join(OUTD, 'PARC.PA'), parc, vals)

for i, m in enumerate(comps):
    errs, _ = assemble(m, os.path.join(OUTD, 'PARB.PA'), os.path.join(OUTD, 'PARC.PA'))
    if errs:
        print(m, 'ASSEMBLY ERRORS:\n' + '\n'.join(errs)); sys.exit(1)
    mem = load_bin(open(os.path.join(OUTD, '%s.bin' % m), 'rb').read())
    img = [0] * 4096
    for a, v in mem.items(): img[a & 0o7777] = v
    images[m] = img
    print('%-6s %4d words -> track %d' % (m, len(mem), i))
# disk image (SIMH RF format: 16-bit LE words, 256K words)
words = [0] * 262144
if base:
    b = open(base, 'rb').read()
    for i in range(min(len(b) // 2, 262144)): words[i] = b[2*i] | (b[2*i+1] << 8)
SAT_LO = 0o7777 - (0o530 + 2) + 1        # SAT lives at the top of FIP's field
old_sat = words[4096 + SAT_LO: 4096 + 4096] if base else None
for i, m in enumerate(comps):
    words[i*4096:(i+1)*4096] = images[m]
if base:
    assert all(images['FIP'][a] == 0 for a in range(SAT_LO, 4096)), 'FIP code overlaps SAT'
    words[4096 + SAT_LO: 8192] = old_sat      # keep the existing disc allocation table
    print('kept the storage allocation table from', base)
with open(out_rf, 'wb') as f:
    f.write(b''.join(w.to_bytes(2, 'little') for w in words))
# bootable INIT tape: INIT image loaded into field 2
def bin_tape(img, field):
    out = bytearray([0o200] * 64)
    out.append(0o300 | (field << 3))
    csum = 0
    def word(w):
        nonlocal csum
        hi, lo = (w >> 6) & 0o77, w & 0o77
        out.extend([hi, lo]); csum += hi + lo
    addr = None
    for a in range(4096):
        v = img[a]
        if v == 0 and a not in used: addr = None; continue
        if addr != a:
            hi, lo = 0o100 | ((a >> 6) & 0o77), a & 0o77
            out.extend([hi, lo]); csum += hi + lo
        word(v); addr = a + 1
    c = csum & 0o7777
    out.extend([(c >> 6) & 0o77, c & 0o77]); out.extend([0o200] * 64)
    return bytes(out)
initmem = load_bin(open(os.path.join(OUTD, 'INIT.bin'), 'rb').read())
used = set(a & 0o7777 for a in initmem)
open(out_init, 'wb').write(bin_tape(images['INIT'], 2))
chk = load_bin(open(out_init, 'rb').read())
assert all(chk[(2 << 12) | a] == images['INIT'][a] for a in used)
print('wrote', out_rf, 'and', out_init)
