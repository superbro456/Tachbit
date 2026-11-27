import struct,sys
from PIL import Image,ImageDraw
def load(p):
    d=open(p,'rb').read();i=8;vox=[];pal=None;size=None
    while i<len(d):
        cid=d[i:i+4];n,c=struct.unpack('<ii',d[i+4:i+12]);b=d[i+12:i+12+n]
        if cid==b'MAIN': i+=12;continue
        if cid==b'SIZE': size=struct.unpack('<iii',b[:12])
        if cid==b'XYZI':
            k=struct.unpack('<i',b[:4])[0]
            vox+= [tuple(b[4+j*4:8+j*4]) for j in range(k)]
        if cid==b'RGBA': pal=[tuple(b[j*4:j*4+3]) for j in range(256)]
        i+=12+n+c
    return vox,pal,size
def render(p,out,S=10):
    vox,pal,size=load(p)
    print(len(vox),'voxels size',size)
    xs=[v[0] for v in vox];ys=[v[1] for v in vox];zs=[v[2] for v in vox]
    print('bounds',min(xs),max(xs),min(ys),max(ys),min(zs),max(zs))
    from collections import Counter
    print('colors',Counter(v[3] for v in vox).most_common(12))
    W=int((max(xs)-min(xs)+max(ys)-min(ys)+4)*S*0.9)+40;H=int((max(zs)-min(zs)+1)*S+(max(xs)-min(xs)+max(ys)-min(ys)+4)*S*0.5)+40
    im=Image.new('RGB',(W,H),(235,240,245));dr=ImageDraw.Draw(im)
    ox=(max(ys)-min(ys)+2)*S*0.9+20;oy=(max(zs)-min(zs)+1)*S+20
    occ=set((v[0],v[1],v[2]) for v in vox)
    def pt(x,y,z):return (ox+(x-min(xs))*S*0.866-(y-min(ys))*S*0.866, oy+((x-min(xs))+(y-min(ys)))*S*0.5-(z-min(zs))*S)
    for x,y,z,c in sorted(vox,key=lambda v:(v[0]+v[1],v[2])):
        col=pal[c-1] if pal else (180,180,180)
        sh=lambda f:tuple(int(a*f) for a in col)
        if (x,y,z+1) not in occ: dr.polygon([pt(x,y,z+1),pt(x+1,y,z+1),pt(x+1,y+1,z+1),pt(x,y+1,z+1)],fill=sh(1.0))
        if (x+1,y,z) not in occ: dr.polygon([pt(x+1,y,z),pt(x+1,y+1,z),pt(x+1,y+1,z+1),pt(x+1,y,z+1)],fill=sh(0.8))
        if (x,y+1,z) not in occ: dr.polygon([pt(x,y+1,z),pt(x+1,y+1,z),pt(x+1,y+1,z+1),pt(x,y+1,z+1)],fill=sh(0.62))
    im.save(out)
if __name__=="__main__": render(sys.argv[1],sys.argv[2])
