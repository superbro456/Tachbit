# Замок 1 из её наброска: кирпич, угловые башни с зубцами, окна, ворота, купол со шпилем и флагом
import struct,sys,os,math
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
from texture import texture,write
src,outp=sys.argv[1],sys.argv[2]
v,pal,size=load(src)
def idx(rgb):return min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)))+1
STONE=idx((125,125,128));LIGHT=idx((175,175,178));DARKW=idx((25,25,30));ROOF=idx((70,80,95))
GOLD=110;WOOD=idx((110,70,40));RED=17
vox={}
for x,y,z,c in v: vox[(x,y,z)]=c
BX0,BX1,BY0,BY1,BZ1=24,78,13,78,25
out={}
for (x,y,z),c in vox.items():
    if c==253: out[(x,y,z)]=STONE
    else:
        if 26<=y<=55 and 37<=x<=66 and z>=16: out[(x,y,z)]=LIGHT   # купол
        else: out[(x,y,z)]=LIGHT
# окна: внутри белых рамок на западной стене — тёмное стекло
for y0 in (19,34,49,64):
    for y in range(y0+2,y0+7):
        for z in range(7,16):
            out[(BX0,y,z)]=DARKW
            out.pop((BX0-1,y,z),None)
# зубцы по краю крыши
def merlon_ring(x0,x1,y0,y1,z):
    for x in range(x0,x1+1):
        for y in range(y0,y1+1):
            edge=x in(x0,x1) or y in(y0,y1)
            if edge and ((x+y)//2)%2==0:
                for k in (0,1): out[(x,y,z+k)]=STONE
            elif edge: out[(x,y,z)]=STONE
merlon_ring(BX0,BX1,BY0,BY1,BZ1+1)
# угловые круглые башни
def tower(cx,cy,r,h):
    for z in range(0,h+1):
        for x in range(cx-r,cx+r+1):
            for y in range(cy-r,cy+r+1):
                d=math.hypot(x-cx,y-cy)
                if d<=r+0.3: out[(x,y,z)]=STONE
    # зубцы
    for x in range(cx-r,cx+r+1):
        for y in range(cy-r,cy+r+1):
            d=math.hypot(x-cx,y-cy)
            if r-1<=d<=r+0.3 and (int(math.degrees(math.atan2(y-cy,x-cx))+180)//25)%2==0:
                for k in (1,2): out[(x,y,h+k)]=STONE
    # конус крыши
    for k in range(0,r+3):
        rr=r-1-k*0.75
        if rr<0: break
        for x in range(cx-r,cx+r+1):
            for y in range(cy-r,cy+r+1):
                if math.hypot(x-cx,y-cy)<=rr+0.3: out[(x,y,h+3+k)]=ROOF
    top=h+3+int((r-1)/0.75)+1
    for k in range(3): out[(cx,cy,top+k)]=GOLD
    # бойницы
    for a in range(0,360,90):
        x=cx+int(round(r*math.cos(math.radians(a+45))));y=cy+int(round(r*math.sin(math.radians(a+45))))
        for z in range(h-8,h-3): out[(x,y,z)]=DARKW
for cx,cy in ((BX0,BY0),(BX1,BY0),(BX0,BY1),(BX1,BY1)):
    tower(cx,cy,6,32)
# ворота в передней круглой башне (y до 92) и в стене
for x in range(47,57):
    for z in range(0,11):
        if z<9 or abs(x-51.5)<3.5:
            for y in range(88,93):
                if (x,y,z) in out: out[(x,y,z)]=WOOD if y==88 else None
out={k:c for k,c in out.items() if c is not None}
# шпиль и флаг на куполе
dz=max(z for (x,y,z) in out if 37<=x<=66 and 26<=y<=55)
cx,cy=51,40
for k in range(1,9): out[(cx,cy,dz+k)]=GOLD
for k in range(5,9):
    for j in range(1,6): out[(cx+j,cy,dz+k)]=RED
vs=[(x,y,z,c) for (x,y,z),c in out.items() if 0<=x<256 and 0<=y<256 and 0<=z<256]
mz=max(z for x,y,z,c in vs)+1
vs,np_=texture(vs,pal)
write(outp,vs,np_,(96,96,max(96,mz)))
render(outp,outp[:-4]+'.png')
