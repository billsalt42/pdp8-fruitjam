#!/usr/bin/env python3
"""TSS/8 (8.24 / UWM 8.25) RF disc file-system reader.  base = first word of file area."""
import sys
SEG = 0o400
def sixbit(w): return chr(((w >> 6) & 0o77) + 32) + chr((w & 0o77) + 32)
class FS:
    def __init__(self, path, jobmax=20, swdex=5):
        b = open(path, 'rb').read()
        self.w = [b[2*i] | (b[2*i+1] << 8) for i in range(len(b)//2)]
        self.base = (swdex + jobmax) * 4096
    def seg(self, n): return self.base + (n - 1) * SEG
    def dir_word(self, segs, off):            # word at offset 'off' of a file made of segs
        return self.w[self.seg(segs[off // SEG]) + off % SEG]
    def window_chain(self, parent_segs, ptr):
        segs = []
        while True:
            win = [self.dir_word(parent_segs, ptr + k) for k in range(8)]
            segs += [s for s in win[1:] if s]
            if win[0] == 0: return segs
            ptr = win[0]
    def mfd(self):
        mseg = [1]
        out = []; off = 0
        while True:
            e = [self.dir_word(mseg, off + k) for k in range(8)]
            if off > 0:
                out.append({'ppn': e[0], 'pw': sixbit(e[1]) + sixbit(e[2]), 'segs': self.window_chain(mseg, e[7])})
            off = e[3]
            if off == 0: return out
    def ufd(self, segs):
        out = []; off = 0
        while True:
            e = [self.dir_word(segs, off + k) for k in range(8)]
            if off > 0:
                out.append({'name': (sixbit(e[0]) + sixbit(e[1]) + sixbit(e[2])).strip(), 'ext': e[4] >> 6,
                            'prot': e[4] & 0o77, 'size': e[5], 'date': e[6], 'segs': self.window_chain(segs, e[7])})
            off = e[3]
            if off == 0: return out
    def read(self, fsegs):
        data = []
        for s in fsegs: data += self.w[self.seg(s): self.seg(s) + SEG]
        return data
EXTS = {0: '', 1: '.ASC', 2: '.SAV', 3: '.BIN', 4: '.BAS', 5: '.BAC', 6: '.FCL', 7: '.TMP', 8: '', 9: '.DAT', 10: '.LST', 11: '.PAL'}
if __name__ == '__main__':
    fs = FS(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20)
    for u in fs.mfd():
        print('[%2o,%2o] pw=%s' % (u['ppn'] >> 6, u['ppn'] & 0o77, u['pw']))
        for f in fs.ufd(u['segs']):
            print('   %-6s%-4s size %3d prot %02o ext %o segs %s' % (f['name'], EXTS.get(f['ext'], '?'), f['size'], f['prot'], f['ext'], f['segs'][:4]))

def entry_addr(fs, segs, name):
    """Disk word address of the 8-word UFD entry for 'name' (None if absent)."""
    off = 0
    while True:
        e = [fs.dir_word(segs, off + k) for k in range(8)]
        if off > 0 and (sixbit(e[0]) + sixbit(e[1]) + sixbit(e[2])).strip() == name:
            return fs.seg(segs[off // SEG]) + off % SEG
        off = e[3]
        if off == 0: return None

def set_ext(path, acct, name, ext, jobmax=20):
    fs = FS(path, jobmax)
    u = [x for x in fs.mfd() if x['ppn'] == acct][0]
    a = entry_addr(fs, u['segs'], name)
    assert a is not None, name
    old = fs.w[a + 4]
    new = (ext << 6) | (old & 0o77)
    b = bytearray(open(path, 'rb').read())
    b[2*(a+4)] = new & 0xff; b[2*(a+4)+1] = new >> 8
    open(path, 'wb').write(b)
    return old, new

def _sixbit_word(two):
    return ((ord(two[0]) - 32) & 0o77) << 6 | ((ord(two[1]) - 32) & 0o77)

def _put(b, addr, val):
    b[2*addr] = val & 0xff; b[2*addr+1] = (val >> 8) & 0xff

def replace_file(path, acct, name, mem, jobmax=20):
    """Overwrite the data of an existing file with a core image {addr: word}
    (a TSS/8 .SAV file is simply locations 0..n of the user's field).  The new
    image must fit in the segments already allocated to the file."""
    fs = FS(path, jobmax)
    u = [x for x in fs.mfd() if x['ppn'] == acct][0]
    f = [x for x in fs.ufd(u['segs']) if x['name'] == name][0]
    cap = len(f['segs']) * SEG
    top = max(a & 0o7777 for a in mem) + 1
    assert top <= cap, '%s: image %d words > %d allocated' % (name, top, cap)
    b = bytearray(open(path, 'rb').read())
    img = [0] * cap
    for a, v in mem.items(): img[a & 0o7777] = v
    for i, s in enumerate(f['segs']):
        for k in range(SEG):
            _put(b, fs.seg(s) + k, img[i * SEG + k])
    open(path, 'wb').write(b)
    return top, cap

def rename_file(path, acct, old, new, jobmax=20):
    fs = FS(path, jobmax)
    u = [x for x in fs.mfd() if x['ppn'] == acct][0]
    a = entry_addr(fs, u['segs'], old)
    assert a is not None, old
    n = (new + '      ')[:6]
    b = bytearray(open(path, 'rb').read())
    for k in range(3): _put(b, a + k, _sixbit_word(n[2*k:2*k+2]))
    open(path, 'wb').write(b)
