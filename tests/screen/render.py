import re, struct
from PIL import Image
src=open('../../firmware/font8x16.c').read()
glyphs=[[int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',line)] for line in src.splitlines() if line.strip().startswith('{0x') or line.strip().startswith('{ 0x')]
import sys
src_bin=sys.argv[1] if len(sys.argv)>1 else 'screen.bin'
dst_png=sys.argv[2] if len(sys.argv)>2 else 'screen.png'
data=open(src_bin,'rb').read()
cells=struct.unpack('<%dH'%(80*30),data)
img=Image.new('RGB',(640,480),(0,0,0))
px=img.load()
fg=(0,255,0)
for r in range(30):
  for c in range(80):
    v=cells[r*80+c]; g=glyphs[v&0x7f]
    rev = v & 0x100
    for y in range(16):
      b=g[y] ^ (0xff if rev else 0)
      for x in range(8):
        if b & (0x80>>x): px[c*8+x,r*16+y]=fg
img.save(dst_png); print(len(glyphs))
