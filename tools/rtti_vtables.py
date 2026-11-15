# Minimal PE32 parser: find MSVC RTTI CompleteObjectLocators and dump vtable sizes.
import struct, sys, re
def load(path):
    d=open(path,'rb').read()
    pe=struct.unpack_from('<I',d,0x3c)[0]
    nsec=struct.unpack_from('<H',d,pe+6)[0]; optsz=struct.unpack_from('<H',d,pe+20)[0]
    base=struct.unpack_from('<I',d,pe+24+28)[0]
    secs=[]
    off=pe+24+optsz
    for i in range(nsec):
        name=d[off:off+8].rstrip(b'\0').decode(); vs,va,rs,ra,_,_,_,_,ch=struct.unpack_from('<IIIIIIHHI',d,off+8)
        secs.append((name,va,max(vs,rs),ra,ch)); off+=40
    return d,base,secs
def v2o(secs,base,va):
    rva=va-base
    for n,sva,sz,ra,ch in secs:
        if sva<=rva<sva+sz: return ra+rva-sva,ch
    return None,0
def main(path,classes):
    d,base,secs=load(path)
    res={}
    for cls in classes:
        name=('.?AV%s@@'%cls).encode()
        i=d.find(name+b'\0')
        if i<0: res[cls]=None; continue
        tdoff=i-8
        # file offset -> va
        for n,sva,sz,ra,ch in secs:
            if ra<=tdoff<ra+sz: tdva=base+sva+tdoff-ra
        out=[]
        for m in re.finditer(re.escape(struct.pack('<I',tdva)),d):
            col=m.start()-12
            sig,offs,cd,td=struct.unpack_from('<IIII',d,col)
            if sig!=0 or td!=tdva: continue
            for n,sva,sz,ra,ch in secs:
                if ra<=col<ra+sz: colva=base+sva+col-ra
            for m2 in re.finditer(re.escape(struct.pack('<I',colva)),d):
                vt=m2.start()+4; cnt=0
                while True:
                    p=struct.unpack_from('<I',d,vt+4*cnt)[0]
                    o,ch=v2o(secs,base,p)
                    if o is None or not (ch & 0x20000000): break
                    cnt+=1
                for n,sva,sz,ra,ch in secs:
                    if ra<=vt<ra+sz: vtva=base+sva+vt-ra
                out.append((offs,hex(vtva),cnt))
        res[cls]=out
    return res
if __name__=='__main__':
    r=main(sys.argv[1],sys.argv[2:])
    for k,v in r.items(): print(k,v)
