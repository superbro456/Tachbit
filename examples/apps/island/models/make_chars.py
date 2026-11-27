# Персонажи (без уровней): воины и враги из блоков. Смотрят вперёд по +y (в игре +z).
#   python3 make_chars.py папка
import struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
out=sys.argv[1]
class M:
    def __init__(s):s.v={};s.pal=[]
    def c(s,rgb):
        if rgb not in s.pal:s.pal.append(rgb)
        return s.pal.index(rgb)+1
    def box(s,x0,x1,y0,y1,z0,z1,rgb):
        k=s.c(rgb)
        for x in range(x0,x1+1):
            for y in range(y0,y1+1):
                for z in range(z0,z1+1): s.v[(x,y,z)]=k
    def dot(s,x,y,z,rgb):s.v[(x,y,z)]=s.c(rgb)
    def save(s,name,size=32):
        o=16-0  # center x,y at 16
        vs=[(x+16,y+16,z,c) for (x,y,z),c in s.v.items()]
        pal=s.pal+[(0,0,0)]*(256-len(s.pal))
        i32=lambda n:struct.pack('<i',n)
        ch=lambda i,b:i+struct.pack('<ii',len(b),0)+b
        body=ch(b'SIZE',struct.pack('<iii',size,size,size))+ch(b'XYZI',i32(len(vs))+b''.join(bytes(t) for t in vs))+ch(b'RGBA',b''.join(bytes(p)+b'\xff' for p in pal[:255])+b'\x00\x00\x00\xff')
        p=os.path.join(out,name+'.vox');open(p,'wb').write(b'VOX '+i32(150)+b'MAIN'+struct.pack('<ii',0,len(body))+body)
        return p
SKIN=(240,190,150);DARK=(40,35,35);STEEL=(190,195,205);STEEL_D=(120,125,135);GOLD=(240,190,40)
def human(m,tunic,pants,hx=0):
    m.box(-2,-1,-1,0,0,3,pants);m.box(1,2,-1,0,0,3,pants)       # ноги
    m.box(-2,-1,-1,1,0,0,DARK);m.box(1,2,-1,1,0,0,DARK)          # сапоги
    m.box(-3,3,-1,1,4,8,tunic)                                     # тело
    m.box(-3,3,-1,1,6,6,(110,70,40))                               # пояс
    m.dot(0,1,6,GOLD)
    m.box(-4,-4,-1,0,5,8,tunic);m.box(4,4,-1,0,5,8,tunic)        # руки
    m.box(-4,-4,-1,0,4,4,SKIN);m.box(4,4,-1,0,4,4,SKIN)
    m.box(-2,2,-2,2,9,12,SKIN)                                     # голова
    m.dot(-1,2,11,DARK);m.dot(1,2,11,DARK)                         # глаза
# мечник: синий, шлем, щит, меч
m=M();BLUE=(40,90,190)
human(m,BLUE,(60,60,80))
m.box(-3,3,-3,3,12,13,STEEL);m.box(-2,2,-2,2,14,14,STEEL);m.box(-3,3,2,3,11,11,STEEL_D);m.dot(0,0,15,(200,40,40))
m.box(5,5,0,1,3,4,(110,70,40));m.box(5,5,1,8,4,4,STEEL);m.box(5,5,1,1,3,5,GOLD)   # меч вперёд
m.box(-6,-5,-1,2,3,9,(150,40,40));m.box(-6,-6,0,1,5,7,GOLD)                       # щит
m.save('voin_mechnik')
# лучник: зелёный капюшон, лук, колчан
m=M();GREEN=(50,130,60)
human(m,GREEN,(90,70,50))
m.box(-3,3,-3,2,12,13,(40,100,45));m.box(-2,2,-3,1,14,14,(40,100,45));m.box(-3,3,-3,-3,8,12,(40,100,45))
m.box(-2,-1,-3,-2,7,13,(120,80,40));m.box(-2,-1,-3,-2,14,15,(230,230,230))       # колчан со стрелами
for z in range(3,13):
    x=5 if 5<=z<=10 else 4
    m.dot(x,2,z,(140,90,40))
m.box(4,4,2,2,4,11,(230,230,220))   # тетива
m.save('voin_luchnik')
# конница: конь + всадник
m=M();HORSE=(130,80,45);HORSE_D=(90,55,30);RED=(190,40,50)
for (x,y) in ((-2,-5),(2,-5),(-2,4),(2,4)): m.box(x,x+1,y,y+1,0,5,HORSE_D)
m.box(-3,3,-6,6,6,10,HORSE);m.box(-2,2,6,9,9,14,HORSE);m.box(-2,2,9,11,12,14,HORSE)  # тело, шея, голова
m.dot(-2,10,13,DARK);m.dot(2,10,13,DARK)
m.box(-1,1,4,8,15,15,DARK);m.box(-1,1,-8,-6,7,10,DARK)          # грива, хвост
m.box(-4,4,-2,2,10,11,RED)                                     # попона
m.box(-3,3,-1,1,12,16,(200,200,210));m.box(-2,2,-1,1,17,19,SKIN)   # всадник
m.box(-3,3,-2,2,20,21,STEEL);m.dot(0,0,22,RED)
m.box(4,4,-1,10,15,15,(150,110,60));m.box(4,4,10,12,15,15,STEEL)  # копьё
m.save('voin_konnica')
# краб
m=M();CR=(220,70,50);CRD=(170,45,35)
m.box(-5,5,-3,3,2,5,CR);m.box(-4,4,-2,2,6,6,CR)
for x in (-6,-7,6,7):
    for y in (-2,0,2): m.box(x,x,y,y,0,3,CRD)
m.box(-6,-4,4,6,3,6,CR);m.box(4,6,4,6,3,6,CR);m.dot(-6,7,6,CRD);m.dot(-4,7,6,CRD);m.dot(4,7,6,CRD);m.dot(6,7,6,CRD)
m.box(-2,-2,3,3,7,8,DARK);m.box(2,2,3,3,7,8,DARK);m.dot(-2,3,9,(250,250,250));m.dot(2,3,9,(250,250,250))
m.save('vrag_krab')
# гоблин
m=M();GS=(110,170,60);GD=(80,130,40)
m.box(-2,-1,-1,0,0,3,GD);m.box(1,2,-1,0,0,3,GD)
m.box(-3,3,-1,1,4,7,(120,80,50));m.box(-3,3,-1,1,8,8,GS)
m.box(-4,-4,-1,0,4,7,GS);m.box(4,4,-1,0,4,7,GS)
m.box(-3,3,-2,2,9,13,GS);m.box(-5,-4,0,0,11,13,GS);m.box(4,5,0,0,11,13,GS)   # голова, уши
m.dot(-1,2,12,(250,220,40));m.dot(1,2,12,(250,220,40));m.box(-1,1,2,2,10,10,DARK)
m.box(5,5,0,1,3,4,(100,70,40));m.box(5,5,1,6,4,5,(120,85,50));m.box(5,5,5,6,3,6,(90,60,35))  # дубинка
m.save('vrag_goblin')
# тролль: большой
m=M();TS=(120,140,130);TD=(90,105,100)
m.box(-5,-2,-2,1,0,6,TD);m.box(2,5,-2,1,0,6,TD)
m.box(-6,6,-3,3,7,16,TS);m.box(-6,6,-3,3,7,9,(100,80,60))
m.box(-9,-7,-2,1,9,16,TS);m.box(7,9,-2,1,9,16,TS);m.box(-9,-7,-2,1,6,8,TD);m.box(7,9,-2,1,6,8,TD)
m.box(-3,3,-2,3,17,21,TS);m.dot(-2,4,20,(230,60,40));m.dot(2,4,20,(230,60,40));m.box(-2,2,4,4,17,18,(240,240,220))
m.box(8,8,0,2,5,7,(100,70,40));m.box(8,9,2,10,6,8,(110,80,50));m.box(7,10,8,11,5,9,(90,65,40))   # дубина
m.save('vrag_troll')
# колдун: фиолетовая мантия, шляпа, посох с шаром
m=M();ROBE=(110,50,160);ROBE_D=(80,35,120)
m.box(-3,3,-2,2,0,8,ROBE);m.box(-4,4,-3,3,0,1,ROBE_D);m.box(-3,3,-2,2,5,5,GOLD)
m.box(-4,-4,-1,0,5,8,ROBE);m.box(4,4,-1,0,5,8,ROBE)
m.box(-2,2,-2,2,9,12,(200,200,190));m.dot(-1,2,11,(255,200,50));m.dot(1,2,11,(255,200,50));m.box(-1,1,2,3,8,9,(230,230,230))  # лицо, борода
m.box(-4,4,-4,4,13,13,ROBE_D);m.box(-3,3,-3,3,14,15,ROBE);m.box(-2,2,-2,2,16,17,ROBE);m.box(-1,1,-1,1,18,19,ROBE);m.dot(0,0,20,ROBE);m.dot(0,-1,21,GOLD)
m.box(5,5,1,1,0,14,(110,80,50));m.box(4,6,0,2,15,17,(80,230,255));m.dot(5,1,18,(220,250,255))
m.save('vrag_koldun')
