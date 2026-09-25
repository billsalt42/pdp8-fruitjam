import sys
def load_bin(data, mem=None, field_override=None):
    """Load a BIN tape into a dict {(field<<12)|addr: word}. Returns mem."""
    if mem is None: mem = {}
    i=0; n=len(data); field=0; newf=0; origin=0; rub=False
    def getc():
        nonlocal i, rub, newf
        while i<n:
            c=data[i]; i+=1
            if rub:
                if c==0o377: rub=False
                continue
            if c==0o377: rub=True; continue
            if c>0o200: newf=(c&0o70)>>3; continue
            return c
        return None
    # skip leader
    while True:
        hi=getc()
        if hi is None: return mem
        if hi!=0 and hi<0o200: break
    csum=0
    while True:
        lo=getc()
        if lo is None: raise ValueError('eof')
        wd=(hi<<6)|lo; t=hi
        hi=getc()
        if hi is None: raise ValueError('eof2')
        if hi==0o200:
            if (csum-wd)&0o7777: raise ValueError('checksum %o %o'%(csum&0o7777,wd))
            return mem
        csum+=t+lo
        if wd>0o7777: origin=wd&0o7777
        else:
            f = field if field_override is None else field_override
            mem[(f<<12)|origin]=wd; origin=(origin+1)&0o7777
        field=newf
if __name__=='__main__':
    m=load_bin(open(sys.argv[1],'rb').read())
    fs=sorted(set(a>>12 for a in m)); print(sys.argv[1], len(m),'words fields',fs, 'range %o-%o'%(min(m),max(m)))
