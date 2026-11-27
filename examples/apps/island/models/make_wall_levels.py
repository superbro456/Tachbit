# Делает stena_2, stena_3 из stena_1 (её стена): python3 make_wall_levels.py stena_1.vox папка
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
src,out=sys.argv[1],sys.argv[2]
v,pal,size=load(src)
GOLD,BLUE,BLUE_L,AME,AME_L,RED,WHITE=110,198,194,209,213,17,234
def idx(rgb):return min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)))+1
DRED=idx((140,20,45))
MAIN,DARK,VEIN,CAP=252,254,234,250
top=max(z for x,y,z,c in v)
xs=[x for x,y,z,c in v if z==top];ys=[y for x,y,z,c in v]
x0,x1,y0,y1=min(xs),max(xs),min(ys),max(ys)
def write(path,vs):
    i32=lambda n:struct.pack('<i',n)
    ch=lambda i,b:i+struct.pack('<ii',len(b),0)+b
    body=ch(b'SIZE',struct.pack('<iii',*size))+ch(b'XYZI',i32(len(vs))+b''.join(bytes(t) for t in vs))+ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
    open(path,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
def merlons(h,col,tip=None):
    add=[]
    for y in range(y0,y1+1):
        if (y-y0)%4<2:
            for x in range(x0,x1+1):
                for k in range(h): add.append((x,y,top+1+k,tip if (tip and k==h-1) else col))
    return add
def pillars(h,col,tip):
    add=[]
    for ya in (range(y0,y0+3),range(y1-2,y1+1)):
        for y in ya:
            for x in range(x0-1,x1+2):
                for z in range(0,top+1+h):
                    add.append((x,y,z,tip if z==top+h else col))
    return add
def paint(m):return [(x,y,z,m.get(c,c)) for x,y,z,c in v]
l2=paint({MAIN:GOLD,DARK:BLUE,VEIN:BLUE_L,CAP:BLUE})+merlons(1,GOLD)
base3=[t for t in paint({MAIN:RED,DARK:DRED,VEIN:GOLD,CAP:GOLD})]
p3=pillars(4,GOLD,AME_L)
occ={(x,y,z) for x,y,z,c in p3}
l3=[t for t in base3 if t[:3] not in occ]+p3+[t for t in merlons(2,AME,GOLD) if y0+3<=t[1]<=y1-3]
for n,vs in ((2,l2),(3,l3)):
    p=f'{out}/stena_{n}.vox';write(p,vs);render(p,f'{out}/w{n}.png')
