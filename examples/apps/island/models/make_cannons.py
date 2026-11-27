# Собирает пушку 1–5: python3 make_cannons.py platform_N.vox pushka.vox cannon_N.vox N  (части: platform стоит, lafet крутится, stvol наклоняется)
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
plat_path,gun_path,out=sys.argv[1],sys.argv[2],sys.argv[3]
level=int(sys.argv[4]) if len(sys.argv)>4 else 2
pv,pal,_=load(plat_path)
gv,gpal,_=load(gun_path)
gpal_map={}
def to_plat_pal(c):
    rgb=gpal[c-1]
    if rgb in pal: return pal.index(rgb)+1
    return min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)))+1
GOLD=110
main={1:253,2:198,3:GOLD,4:215,5:17}[level]
ring={1:251,2:GOLD,3:GOLD,4:GOLD,5:GOLD}[level]
top=max(z for x,y,z,c in pv if abs(x-19)<=2 and abs(y-20)<=2)
dz=top+1-10
lafet=[(x+11,y,z+dz,to_plat_pal(c)) for x,y,z,c in gv if x<=11]
lafet=[(x,y,z,main if c==GOLD else c) for x,y,z,c in lafet]
raw=[(38-x,34-y,z,c) for x,y,z,c in gv if x>=17]
extra={1:-1,2:0,3:0,4:1,5:2}[level]
# lengthen/shorten barrel at the plain part y 20..22 (front of middle ring)
st=[]
for x,y,z,c in raw:
    if y>=21: st.append((x,y+extra,z,c))
    elif y<21+min(extra,0) : st.append((x,y,z,c))
    if extra>0 and y==20:
        for k in range(1,extra+1): st.append((x,y+k,z,c))
stvol=[(x,y+4,z+dz,to_plat_pal(c)) for x,y,z,c in st]
stvol=[(x,y,z,ring if c==GOLD else c) for x,y,z,c in stvol]
def i32(n):return struct.pack('<i',n)
def txt(s):b=s.encode();return i32(len(b))+b
def dic(d):return i32(len(d))+b''.join(txt(k)+txt(v) for k,v in d.items())
def ch(i,b):return i+struct.pack('<ii',len(b),0)+b
objs=[('platform',pv),('lafet',lafet),('stvol',stvol)]
body=b''
for name,vs in objs:
    body+=ch(b'SIZE',struct.pack('<iii',40,40,40))+ch(b'XYZI',i32(len(vs))+b''.join(bytes(t) for t in vs))
# scene: 0 nTRN -> 1 nGRP -> (2+2k nTRN -> 3+2k nSHP)
body+=ch(b'nTRN',i32(0)+dic({})+i32(1)+i32(-1)+i32(-1)+i32(1)+dic({}))
kids=[2+2*k for k in range(len(objs))]
body+=ch(b'nGRP',i32(1)+dic({})+i32(len(kids))+b''.join(i32(k) for k in kids))
for k,(name,vs) in enumerate(objs):
    body+=ch(b'nTRN',i32(2+2*k)+dic({'_name':name})+i32(3+2*k)+i32(-1)+i32(0)+i32(1)+dic({'_t':'0 0 20'}))
    body+=ch(b'nSHP',i32(3+2*k)+dic({})+i32(1)+i32(k)+dic({}))
body+=ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
open(out,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
# flat copy for picture
flat=out[:-4]+'_flat.vox'
vs=pv+lafet+stvol
fb=ch(b'SIZE',struct.pack('<iii',40,40,40))+ch(b'XYZI',i32(len(vs))+b''.join(bytes(t) for t in vs))+ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
open(flat,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(fb))+fb)
render(flat,out[:-4]+'.png')
