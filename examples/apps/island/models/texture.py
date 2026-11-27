# Кирпичная обработка любого .vox: большие цвета → кирпичи разных оттенков со швами, мелкие → лёгкий шум.
#   python3 texture.py in.vox out.vox
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
# индексы палитры, которые трогать нельзя (цвета схемы и пушек)
KEEP={17,54,110,194,198,209,213,215,234,249,250,251,252,253,254,255,237,238,240,244,143,147}
def hh(*a):
    h=0
    for n in a:h=(h*1000003^(n+7)*2654435761)&0xffffffff
    return h
def shade(rgb,k):return tuple(max(0,min(255,int(c*k))) for c in rgb)
def texture(vs,pal,size_hint=None):
    pal=[tuple(p) for p in pal]
    occ={(x,y,z):c for x,y,z,c in vs}
    surf=[k for k in occ if any((k[0]+a,k[1]+b,k[2]+c) not in occ for a,b,c in((1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)))]
    from collections import Counter
    cnt=Counter(occ[k] for k in surf)
    total=max(1,len(surf))
    used=set(occ.values())
    free=[i for i in range(1,256) if i not in used and i not in KEEP]
    newpal=list(pal);cache={}
    def col(rgb):
        if rgb in cache:return cache[rgb]
        i=free.pop(0);newpal[i-1]=rgb;cache[rgb]=i;return i
    out=[]
    for (x,y,z),c in occ.items():
        rgb=pal[c-1];lum=sum(rgb)/3
        is_surf=(x,y,z) in set_surf if False else None
        out.append([x,y,z,c])
    sset=set(surf)
    res=[]
    for x,y,z,c in out:
        rgb=pal[c-1];lum=sum(rgb)/3
        if (x,y,z) not in sset or lum<45:
            res.append((x,y,z,c));continue
        big=cnt[c]/total>=0.12 and lum<235
        if big:
            row=z//3;off=3 if row%2 else 0;u=x+y+off
            if z%3==2 or u%5==0: k=0.74
            else: k=[0.86,0.95,1.04,1.12][hh(row,u//5,c)%4]
        else:
            k=[0.94,1.0,1.0,1.06][hh(x,y,z)%4]
        if abs(k-1.0)<0.001: res.append((x,y,z,c))
        else: res.append((x,y,z,col(shade(rgb,k))))
    return res,newpal
def write(path,vs,pal,size):
    i32=lambda n:struct.pack('<i',n)
    ch=lambda i,b:i+struct.pack('<ii',len(b),0)+b
    body=ch(b'SIZE',struct.pack('<iii',*size))+ch(b'XYZI',i32(len(vs))+b''.join(bytes(t) for t in vs))+ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
    open(path,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
if __name__=="__main__":
    v,pal,size=load(sys.argv[1])
    xs=[a[0] for a in v]+[0];ys=[a[1] for a in v];zs=[a[2] for a in v]
    size=(max(size[0],max(xs)+1),max(size[1],max(ys)+1),max(size[2],max(zs)+1))
    r,np=texture(v,pal)
    write(sys.argv[2],r,np,size)
    if len(sys.argv)<4: render(sys.argv[2],sys.argv[2][:-4]+'.png')
