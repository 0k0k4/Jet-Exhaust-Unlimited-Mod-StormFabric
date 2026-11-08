import struct
def load(path):
    data=open(path,'rb').read()
    pe=struct.unpack_from('<I',data,0x3c)[0]; nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
    secs=[];off=pe+24+opt
    for i in range(nsec):
        n=data[off:off+8].rstrip(b'\0').decode();vs,va,rs,raw=struct.unpack_from('<IIII',data,off+8);secs.append((n,va,raw,rs));off+=40
    return data,secs
def rva2fo(secs,r):
    for n,va,raw,rs in secs:
        if va<=r<va+rs: return raw+(r-va)
A,sA=load(r'H:/Jeux/StormworksV1.15.23/Stormworks/stormworks64.exe')
B,sB=load(r'H:/Jeux/Stormworks/stormworks64.exe')
L=64
def relmask(secs,data,rva):
    f=rva2fo(secs,rva); pat=data[f:f+L]; mask=bytearray([1]*L)
    mx=max(s[1]+s[3] for s in secs)
    for j in range(L-4):
        v=struct.unpack_from('<i',pat,j)[0]
        for end in range(j+4,j+9):
            if 0x1000<=rva+end+v<mx: mask[j:j+4]=bytes(4); break
    return pat,mask
def find(rva18, name):
    p18,m18=relmask(sB,B,rva18)
    tb=[s for s in sA if s[0]=='.text'][0]; tva,traw,trs=tb[1],tb[2],tb[3]; hits=[]
    for o in range(traw,traw+trs-L):
        if all(not m18[j] or A[o+j]==p18[j] for j in range(L)): hits.append(tva+(o-traw))
    if len(hits)==1:
        p23,m23=relmask(sA,A,hits[0]); pat=bytearray(p23); mask=bytearray([1]*L)
        for i in range(L):
            if not m23[i] or not m18[i] or p23[i]!=p18[i]: mask[i]=0
        print('%-32s 18=%#x -> 23=%#x' % (name, rva18, hits[0]))
        print('  bytes:',','.join('0x%02X'%b for b in pat))
        print('  mask :',','.join('0xFF' if m else '0x00' for m in mask))
    else:
        print('%-32s 18=%#x -> hits=%s' % (name, rva18, [hex(h) for h in hits[:5]]))
find(0x5f1fd0, 'ComponentRegistry::Clear')
