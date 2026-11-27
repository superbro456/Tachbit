# Делает tower_gen_1..5.vox из tower_2.vox: python3 make_tower_levels.py tower_2.vox папка (iso.py рядом рисует картинки)
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from iso import load,render
src,out=sys.argv[1],sys.argv[2]
v,pal,size=load(src)
def idx(rgb):return min(range(255),key=lambda i:sum((a-b)**2 for a,b in zip(pal[i],rgb)))+1
STONE,DARK,BLUE=251,253,198
GOLD,RED,PUR,WHITE=110,17,215,234
role={STONE:'stone',DARK:'dark',BLUE:'acc'}
Z={}
for x,y,z,c in v: Z.setdefault(z,{})[(x,y)]=role[c]
top=max(Z)
for z in range(10,20):
    for k,r in Z[z].items():
        if r=='acc': Z[z][k]='win'
def edge(l,k):return any((k[0]+a,k[1]+b) not in l for a,b in((1,0),(-1,0),(0,1),(0,-1)))
for k in list(Z[22]):
    if edge(Z[22],k): Z[22][k]='band'
def layers(extra):
    ls=[dict(Z[z]) for z in range(top+1)]
    # shaft layers 5..8 repeat
    shaft=[dict(Z[z]) for z in (5,6,7,8)]
    add=[shaft[i%4] for i in range(extra)] if extra>0 else []
    if extra<0: return ls[:5]+ls[5-extra:]
    return ls[:9]+add+ls[9:]
def crown(ls,n=2):
    for l in ls[-n:]:
        for k,r in l.items():
            if r=='stone': l[k]='crown'
    return ls
def posts(ls):
    l=ls[-1];cx=sum(x for x,y in l)/len(l);cy=sum(y for x,y in l)/len(l)
    far=sorted(l,key=lambda p:-(abs(p[0]-cx)+abs(p[1]-cy)))
    pick=[]
    for p in far:
        if all(abs(p[0]-q[0])+abs(p[1]-q[1])>4 for q in pick): pick.append(p)
        if len(pick)==4:break
    ls+= [ {p:'crown' for p in pick}, {p:'crown' for p in pick}, {p:'tip' for p in pick} ]
    return ls
def flag(ls,h=7):
    l=ls[-1] if len(ls[-1])>8 else ls[-4]
    xs=[x for x,y in Z[top]];ys=[y for x,y in Z[top]]
    cx=(min(xs)+max(xs))//2;cy=(min(ys)+max(ys))//2
    base=len(ls)
    for i in range(h): ls.append({(cx,cy):'pole'})
    for i in range(3):
        for j in range(1,5): ls[base+h-1-i][(cx+j,cy)]='cloth'
    return ls
def plinth(ls):
    l=ls[0];new=dict(l)
    for k in l:
        for a,b in((1,0),(-1,0),(0,1),(0,-1)):
            q=(k[0]+a,k[1]+b)
            if q not in l: new[q]='band'
    ls[0]=new;ls.insert(1,{k:('band' if k not in ls[1] else ls[1][k]) for k in new})
    return ls
def build(ls,cm):return [(x,y,z,cm[r]) for z,l in enumerate(ls) for (x,y),r in l.items()]
def write(path,vs,sz=None):
    xyzi=struct.pack('<i',len(vs))+b''.join(bytes(t) for t in vs)
    rgba=b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff'
    ch=lambda i,b:i+struct.pack('<ii',len(b),0)+b
    body=ch(b'SIZE',struct.pack('<iii',*(sz or size)))+ch(b'XYZI',xyzi)+ch(b'RGBA',rgba)
    open(path,'wb').write(b'VOX '+struct.pack('<i',150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
AME,AME_L,DRED=209,213,idx((140,20,45))
def hh(x,y,z):return (x*73856093^y*19349663^z*83492791)&0xffff
c1={'stone':STONE,'dark':DARK,'acc':DARK,'win':DARK,'band':DARK,'crown':STONE,'tip':WHITE}
c2={'stone':GOLD,'dark':BLUE,'acc':BLUE,'win':BLUE,'band':BLUE,'crown':GOLD,'tip':WHITE,'pole':GOLD,'cloth':BLUE}
c3={'stone':GOLD,'dark':'mix','acc':AME,'win':AME,'band':BLUE,'crown':GOLD,'tip':AME_L,'pole':GOLD,'cloth':AME}
c4={'stone':RED,'dark':DRED,'acc':GOLD,'win':GOLD,'band':GOLD,'crown':GOLD,'tip':WHITE,'pole':GOLD,'cloth':RED}
c5={'stone':RED,'dark':'mix5','acc':GOLD,'win':AME,'band':GOLD,'crown':GOLD,'tip':AME_L,'pole':GOLD,'cloth':AME}
def color(cm,x,y,z,r):
    c=cm[r]
    if c=='mix': return [BLUE,AME,AME_L][hh(x,y,z)%3]
    if c=='mix5': return [DRED,AME,GOLD][hh(x,y,z)%3]
    return c
def build2(ls,cm):return [(x,y,z,color(cm,x,y,z,r)) for z,l in enumerate(ls) for (x,y),r in l.items()]
def scale(vs,k):
    import math
    src={(x,y,z):c for x,y,z,c in vs}
    xs=[v[0] for v in vs];ys=[v[1] for v in vs];zs=[v[2] for v in vs]
    ox=(min(xs)+max(xs)+1)/2;oy=(min(ys)+max(ys)+1)/2
    out=[]
    W=math.ceil((max(xs)-min(xs)+1)*k)+2;H=math.ceil((max(zs)+1)*k)+1
    for X in range(int(ox-W/2),int(ox+W/2)+1):
        for Y in range(int(oy-W/2),int(oy+W/2)+1):
            for Z in range(H):
                q=(math.floor(ox+(X+0.5-ox)/k),math.floor(oy+(Y+0.5-oy)/k),math.floor((Z+0.5)/k))
                if q in src: out.append((X,Y,Z,src[q]))
    return out
sets=[(1,layers(-2),c1,1),(2,flag(layers(0),6),c2,1),(3,flag(crown(layers(2)),6),c3,1),(4,flag(posts(crown(layers(3)))),c4,1),(5,flag(posts(plinth(crown(layers(2)))),7),c5,1)]
for n,ls,cm,k in sets:
    vs=build2(ls,cm)
    sz=None
    if k!=1:
        vs=scale(vs,k);sz=(40,40,max(v[2] for v in vs)+1)
    p=f'{out}/tower_gen_{n}.vox';write(p,vs,sz);render(p,f'{out}/tg{n}.png')
