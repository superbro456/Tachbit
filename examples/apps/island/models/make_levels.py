# Делает platform_1,3,4,5.vox (и картинки p1..p5.png) из platform_2.vox:
#   python3 make_levels.py platform_2.vox папка_для_файлов
# iso.py рядом — читает .vox и рисует картинку (нужен Pillow)
import struct,sys
import os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
src,out=sys.argv[1],sys.argv[2]
v,pal,size=load(src)
def idx(rgb):
    best=min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)));return best+1
GOLD,BLUE,RED,PUR,WHITE=110,198,17,215,234
DG,LG,ST=idx((68,68,68)),idx((119,119,119)),idx((170,170,170))
role={GOLD:'base',BLUE:'body',RED:'arm',PUR:'acc',WHITE:'top'}
L={}
for x,y,z,c in v: L.setdefault(z,{})[(x,y)]=role[c]
def stack(extra):
    # repeat body layer z1 'extra' times
    layers=[L[0],L[1]]+[L[1]]*extra+[L[2],L[3],L[4]]
    return [dict(l) for l in layers]
def ring(layers):
    f=layers[0];new=dict(f)
    for (x,y) in f:
        for dx,dy in((1,0),(-1,0),(0,1),(0,-1)):
            if (x+dx,y+dy) not in f: new[(x+dx,y+dy)]='base'
    return [new]+layers
def rim(layer):
    out=dict(layer)
    for (x,y),r in layer.items():
        if r=='top' and any((x+dx,y+dy) not in layer or layer[(x+dx,y+dy)]!='top' for dx,dy in((1,0),(-1,0),(0,1),(0,-1))): out[(x,y)]='rim'
    return out
def build(layers,cmap,posts=None):
    vs=[]
    for z,l in enumerate(layers):
        for (x,y),r in l.items(): vs.append((x,y,z,cmap[r]))
    if posts:
        top=len(layers)
        for (x,y) in posts:
            for k in range(3): vs.append((x,y,top+k,cmap['base'] if k<2 else cmap['top']))
    return vs
def write(path,vs):
    xyzi=struct.pack('<i',len(vs))+b''.join(bytes(t) for t in vs)
    rgba=b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff'
    def ch(i,b):return i+struct.pack('<ii',len(b),0)+b
    body=ch(b'SIZE',struct.pack('<iii',*size))+ch(b'XYZI',xyzi)+ch(b'RGBA',rgba)
    open(path,'wb').write(b'VOX '+struct.pack('<i',150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
c1={'base':DG,'body':LG,'arm':DG,'acc':DG,'top':ST,'rim':DG}
c2={'base':GOLD,'body':BLUE,'arm':RED,'acc':PUR,'top':WHITE,'rim':GOLD}
c4={'base':GOLD,'body':PUR,'arm':BLUE,'acc':GOLD,'top':WHITE,'rim':GOLD}
c5={'base':GOLD,'body':RED,'arm':PUR,'acc':GOLD,'top':WHITE,'rim':GOLD}
l1=[dict(L[0]),dict(L[1]),dict(L[3]),dict(L[4])]
l3=stack(1);l3[-1]=rim(l3[-1])
l4=ring(stack(1));l4[-1]=rim(l4[-1])
l5=ring(stack(2));l5[-1]=rim(l5[-1])
posts=[(15,18),(23,18),(15,22),(23,22)]
for n,ls,cm,po in[(1,l1,c1,None),(3,l3,c2,None),(4,l4,c4,None),(5,l5,c5,posts)]:
    p=f'{out}/platform_{n}.vox';write(p,build(ls,cm,po));render(p,f'{out}/p{n}.png')
