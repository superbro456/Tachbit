# Стена 1 «качественнее» из её stena_1_mramor.vox: python3 make_wall_1.py stena_1_mramor.vox stena_1.vox
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
src,outp=sys.argv[1],sys.argv[2]
v,pal,size=load(src)
def idx(rgb):return min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)))+1
MORTAR=idx((88,88,92));B=[idx((120,120,124)),idx((138,136,134)),idx((105,104,108)),idx((150,148,145))]
CAP=idx((170,170,172));CAPD=idx((140,140,142));MOSS=idx((80,120,60));MOSS2=idx((100,140,70));SLIT=idx((20,20,22))
occ={(x,y,z) for x,y,z,c in v}
top=max(z for x,y,z in occ)
ys=[y for x,y,z in occ];y0,y1=min(ys),max(ys)
def hh(*a):
    h=0
    for n in a:h=(h*1000003^n*2654435761)&0xffffffff
    return h
def brick(x,y,z):
    row=z//3
    off=3 if row%2 else 0
    col=(y+x+off)//5
    if z%3==2 or (y+x+off)%5==0: return MORTAR
    return B[hh(row,col,x>47)%4]
out=[]
for x,y,z in occ:
    surf=any((x+a,y+b,z+c) not in occ for a,b,c in((1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1)))
    if z>=top-1:
        c=CAP if z==top else CAPD
    elif not surf: c=B[0]
    else:
        c=brick(x,y,z)
        if z<=1 and c!=MORTAR and hh(x,y,z)%3==0: c=MOSS if hh(y,z)%2 else MOSS2
    out.append((x,y,z,c))
# arrow slits on the middle step (z 9..12), every 6 blocks
xs_mid=[x for x,y,z in occ if z==10];xa,xb=min(xs_mid),max(xs_mid)
out=[(x,y,z,SLIT if (9<=z<=12 and (y-y0)%6==3 and x in(xa,xb)) else c) for x,y,z,c in out]
# merlons on top: 2 wide every 4, 2 tall
xt=[x for x,y,z in occ if z==top]
for y in range(y0,y1+1):
    if (y-y0)%4<2:
        for x in range(min(xt),max(xt)+1):
            for k in (1,2): out.append((x,y,top+k,CAP if k==2 else B[hh(x,y,k)%4]))
i32=lambda n:struct.pack('<i',n)
ch=lambda i,b:i+struct.pack('<ii',len(b),0)+b
body=ch(b'SIZE',struct.pack('<iii',*size))+ch(b'XYZI',i32(len(out))+b''.join(bytes(t) for t in out))+ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
open(outp,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
render(outp,outp[:-4]+'.png')
