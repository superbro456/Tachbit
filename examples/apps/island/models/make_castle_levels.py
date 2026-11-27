# Крепость 1–10: zamok_1.vox … zamok_10.vox
#   python3 make_castle_levels.py
# Двор со стенами и зубцами, угловые башни с крышами, главная башня (донжон) с белым куполом (как в её наброске),
# ворота спереди (+y в MagicaVoxel = к камере). С уровнем: выше стены, больше башен, цвета по её схеме
# (1–2 серый, 3–4 золото + синий, 5–6 + аметист, 7–8 красный, 9–10 крупнее и золото).
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import load, render
from texture import write, KEEP

HERE = os.path.dirname(os.path.abspath(__file__))
_, base_pal, _ = load(os.path.join(HERE, 'platform_1.vox'))
base_pal = [tuple(p) for p in base_pal]


def hh(*a):
    h = 0
    for n in a:
        h = (h * 1000003 ^ (n + 7) * 2654435761) & 0xffffffff
    return h


class Pal:
    def __init__(self):
        self.pal = list(base_pal)
        self.free = [i for i in range(1, 256) if i not in KEEP]
        self.cache = {}

    def __call__(self, rgb):
        rgb = tuple(max(0, min(255, int(c))) for c in rgb)
        if rgb not in self.cache:
            i = self.free.pop(0)
            self.pal[i - 1] = rgb
            self.cache[rgb] = i
        return self.cache[rgb]


def shade(rgb, k):
    return tuple(c * k for c in rgb)


LEVELS = {
    # W — сторона двора, H — высота стен, tr/th — угловые башни, kr/kh — донжон
    1: dict(W=56, H=12, tr=5, th=18, kr=8, kh=22, stone=(128, 128, 132), roof=(70, 80, 95), tip=(200, 160, 40), dome=(225, 225, 230), trim=None, gate=False, mid=False, inner=False, banner=None),
    2: dict(W=58, H=13, tr=5, th=20, kr=8, kh=24, stone=(128, 128, 132), roof=(70, 80, 95), tip=(220, 175, 40), dome=(230, 230, 235), trim=None, gate=True, mid=False, inner=False, banner=(60, 90, 170)),
    3: dict(W=60, H=14, tr=6, th=22, kr=9, kh=26, stone=(140, 138, 134), roof=(45, 95, 185), tip=(235, 190, 50), dome=(235, 235, 240), trim=(235, 190, 50), gate=True, mid=False, inner=False, banner=(45, 95, 185)),
    4: dict(W=62, H=15, tr=6, th=24, kr=9, kh=28, stone=(142, 140, 136), roof=(45, 95, 185), tip=(240, 195, 55), dome=(240, 240, 245), trim=(235, 190, 50), gate=True, mid=True, inner=False, banner=(45, 95, 185)),
    5: dict(W=64, H=16, tr=6, th=26, kr=10, kh=30, stone=(138, 134, 142), roof=(120, 70, 175), tip=(240, 195, 55), dome=(240, 238, 250), trim=(160, 110, 210), gate=True, mid=True, inner=False, banner=(120, 70, 175)),
    6: dict(W=66, H=17, tr=7, th=28, kr=10, kh=32, stone=(140, 136, 146), roof=(120, 70, 175), tip=(245, 200, 60), dome=(245, 242, 252), trim=(160, 110, 210), gate=True, mid=True, inner=True, banner=(120, 70, 175)),
    7: dict(W=68, H=18, tr=7, th=30, kr=11, kh=34, stone=(146, 138, 134), roof=(185, 45, 45), tip=(245, 200, 60), dome=(245, 240, 235), trim=(200, 60, 55), gate=True, mid=True, inner=True, banner=(185, 45, 45)),
    8: dict(W=70, H=19, tr=7, th=32, kr=11, kh=36, stone=(150, 140, 136), roof=(185, 45, 45), tip=(250, 205, 65), dome=(250, 244, 238), trim=(235, 190, 50), gate=True, mid=True, inner=True, banner=(185, 45, 45)),
    9: dict(W=76, H=21, tr=8, th=35, kr=12, kh=40, stone=(158, 150, 140), roof=(170, 35, 40), tip=(250, 210, 70), dome=(245, 205, 80), trim=(235, 190, 50), gate=True, mid=True, inner=True, banner=(170, 35, 40)),
    10: dict(W=82, H=23, tr=8, th=38, kr=13, kh=44, stone=(165, 158, 146), roof=(170, 35, 40), tip=(255, 215, 80), dome=(245, 205, 80), trim=(245, 200, 60), gate=True, mid=True, inner=True, banner=(240, 195, 55)),
}


def build(lv):
    p = LEVELS[lv]
    P = Pal()
    vox = {}
    W, H = p['W'], p['H']
    S = p['stone']
    pad = p['tr'] + 2
    x0, y0 = pad, pad
    x1, y1 = pad + W, pad + W
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2

    def put(x, y, z, c):
        if 0 <= x < 256 and 0 <= y < 256 and 0 <= z < 256:
            vox[(x, y, z)] = c

    # кирпич: ряды по 3, шов светлее/темнее, кирпичи немного разные
    def brick(x, y, z, rgb, u):
        row = z // 3
        off = 3 if row % 2 else 0
        if z % 3 == 2 or (u + off) % 6 == 0:
            return P(shade(rgb, 0.72))
        k = [0.86, 0.94, 1.0, 1.08][hh(x // 6, y // 6, row, u // 6) % 4]
        return P(shade(rgb, k))

    # земля двора: утоптанная трава и дорожка из плит к донжону
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            if abs(x - cx) <= 4 and y >= cy:
                c = P(shade((170, 165, 150), [0.9, 1.0, 1.06][hh(x // 3, y // 3) % 3]))
            else:
                c = P(shade((95, 150, 75), [0.9, 1.0, 1.08][hh(x, y) % 3]))
            put(x, y, 0, c)

    # стены двора: толщина 3, с ходом и зубцами
    T = 3
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            ring = x < x0 + T or x > x1 - T or y < y0 + T or y > y1 - T
            if not ring:
                continue
            u = x + y
            for z in range(0, H + 1):
                put(x, y, z, brick(x, y, z, S, u))
            # зубцы только по внешнему краю
            outer = x == x0 or x == x1 or y == y0 or y == y1
            if outer and ((x + y) // 3) % 2 == 0:
                for z in (H + 1, H + 2):
                    put(x, y, z, brick(x, y, z, S, u))
    # полоса цвета под зубцами (с 3-го уровня)
    if p['trim']:
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                if x == x0 or x == x1 or y == y0 or y == y1:
                    put(x, y, H - 1, P(p['trim']))

    # бойницы в стенах
    for t in range(x0 + 8, x1 - 6, 10):
        for z in range(H - 6, H - 2):
            put(t, y1, z, P((25, 25, 30)))
            put(t, y0, z, P((25, 25, 30)))
            put(x0, t, z, P((25, 25, 30)))
            put(x1, t, z, P((25, 25, 30)))

    def tower(tx, ty, r, h, roof=True):
        for z in range(0, h + 1):
            for x in range(tx - r, tx + r + 1):
                for y in range(ty - r, ty + r + 1):
                    if math.hypot(x - tx, y - ty) <= r + 0.3:
                        a = math.atan2(y - ty, x - tx)
                        put(x, y, z, brick(x, y, z, S, int((a + math.pi) * r * 1.2)))
        # карниз шире на 1
        for x in range(tx - r - 1, tx + r + 2):
            for y in range(ty - r - 1, ty + r + 2):
                d = math.hypot(x - tx, y - ty)
                if r - 0.5 <= d <= r + 1.3:
                    put(x, y, h + 1, P(p['trim'] or shade(S, 0.9)))
        # окна-бойницы на 4 стороны
        for k in range(4):
            a = math.radians(45 + 90 * k)
            wx = tx + int(round(r * math.cos(a)))
            wy = ty + int(round(r * math.sin(a)))
            for z in range(h - 7, h - 3):
                put(wx, wy, z, P((30, 28, 34)) if lv < 5 else P((255, 200, 90)))
        if roof:
            n = int(r * 1.8) + 3
            for k in range(n):
                rr = (r + 1) * (1 - k / n)
                for x in range(tx - r - 1, tx + r + 2):
                    for y in range(ty - r - 1, ty + r + 2):
                        if math.hypot(x - tx, y - ty) <= rr + 0.2:
                            sh = [0.86, 1.0, 1.1][(k // 2 + (x + y) // 3) % 3]
                            put(x, y, h + 2 + k, P(shade(p['roof'], sh)))
            for k in range(3):
                put(tx, ty, h + 2 + n + k, P(p['tip']))
            return h + 2 + n + 3
        # без крыши: зубцы
        for x in range(tx - r, tx + r + 1):
            for y in range(ty - r, ty + r + 1):
                d = math.hypot(x - tx, y - ty)
                if r - 1 <= d <= r + 0.3 and (int(math.degrees(math.atan2(y - ty, x - tx)) + 180) // 30) % 2 == 0:
                    put(x, y, h + 2, brick(x, y, h + 2, S, x + y))
        return h + 3

    def flag(fx, fy, fz, color, lenx=6):
        for k in range(10):
            put(fx, fy, fz + k, P((90, 60, 35)))
        for k in range(5, 10):
            for j in range(1, lenx):
                put(fx + j, fy, fz + k, P(shade(color, 1.0 if (j + k) % 2 else 0.88)))

    # угловые башни
    r, th = p['tr'], p['th']
    corners = [(x0, y0), (x1, y0), (x0, y1), (x1, y1)]
    for tx, ty in corners:
        top = tower(tx, ty, r, th)
    # башни посреди боковых стен
    if p['mid']:
        for tx, ty in [(x0, cy), (x1, cy), (cx, y0)]:
            tower(tx, ty, r - 1, th - 4)

    # ворота: две башенки и арка с деревянной дверью
    gw = 5
    for x in range(cx - gw, cx + gw + 1):
        for z in range(0, 10):
            arch = z < 7 or abs(x - cx) < gw - (z - 6)
            if arch:
                for y in range(y1 - T + 1, y1 + 1):
                    vox.pop((x, y, z), None)
                if abs(x - cx) <= gw - 1:
                    put(x, y1 - 1, z, P(shade((115, 72, 40), 0.9 if x % 2 else 1.05)))
    if p['gate']:
        for gx in (cx - gw - 3, cx + gw + 3):
            tower(gx, y1 + 1, 3, H + 6, roof=False)
        # решётка над аркой
        for x in range(cx - gw, cx + gw + 1):
            put(x, y1 + 1, 10, P(p['trim'] or shade(S, 0.8)))

    # донжон в центре двора: квадратная башня, сверху белый купол со шпилем
    kr, kh = p['kr'], p['kh']
    for x in range(cx - kr, cx + kr + 1):
        for y in range(cy - kr - 4, cy + kr - 4 + 1):
            for z in range(0, kh + 1):
                put(x, y, z, brick(x, y, z, shade(S, 1.06), x + y))
    ky = cy - 4
    # окна донжона
    for wx in range(cx - kr + 3, cx + kr - 1, 5):
        for z in (kh - 8, kh - 7, kh - 6, kh - 5):
            c = P((255, 205, 100)) if lv >= 3 else P((30, 28, 34))
            put(wx, ky + kr, z, c)
            put(wx + 1, ky + kr, z, c)
    # дверь донжона
    for x in range(cx - 2, cx + 3):
        for z in range(0, 7 if abs(x - cx) < 2 else 6):
            put(x, ky + kr, z, P((105, 66, 38)))
    # зубцы донжона
    for x in range(cx - kr, cx + kr + 1):
        for y in range(ky - kr, ky + kr + 1):
            edge = x in (cx - kr, cx + kr) or y in (ky - kr, ky + kr)
            if edge and ((x + y) // 2) % 2 == 0:
                put(x, y, kh + 1, brick(x, y, kh + 1, S, x + y))
    if p['trim']:
        for x in range(cx - kr, cx + kr + 1):
            for y in range(ky - kr, ky + kr + 1):
                if x in (cx - kr, cx + kr) or y in (ky - kr, ky + kr):
                    put(x, y, kh - 1, P(p['trim']))
    # купол (полусфера) на барабане
    dr = kr - 2
    for x in range(cx - dr, cx + dr + 1):
        for y in range(ky - dr, ky + dr + 1):
            d = math.hypot(x - cx, y - ky)
            if d <= dr + 0.3:
                for z in range(kh + 1, kh + 4):
                    put(x, y, z, P(shade(p['dome'], 0.92)))
                hz = int(math.sqrt(max(0, (dr + 0.3) ** 2 - d * d)))
                for z in range(kh + 4, kh + 4 + hz + 1):
                    sh = 1.0 if (z + x) % 4 else 0.94
                    put(x, y, z, P(shade(p['dome'], sh)))
    top = kh + 4 + dr + 1
    for k in range(4):
        put(cx, ky, top + k, P(p['tip']))
    flag(cx, ky, top + 4, p['banner'] or (200, 40, 50), 7)

    # маленькие купола на угловых башенках донжона (с 6-го уровня)
    if p['inner']:
        for sx in (-1, 1):
            for sy in (-1, 1):
                tx, ty = cx + sx * kr, ky + sy * kr
                for z in range(kh, kh + 6):
                    for x in range(tx - 2, tx + 3):
                        for y in range(ty - 2, ty + 3):
                            if math.hypot(x - tx, y - ty) <= 2.3:
                                put(x, y, z, brick(x, y, z, S, x + y))
                for z in range(3):
                    rr = 2.3 - z * 0.8
                    for x in range(tx - 2, tx + 3):
                        for y in range(ty - 2, ty + 3):
                            if math.hypot(x - tx, y - ty) <= rr:
                                put(x, y, kh + 6 + z, P(p['dome']))
                put(tx, ty, kh + 9, P(p['tip']))

    # знамёна на передней стене по бокам от ворот
    if p['banner']:
        for bx in (cx - 14, cx + 14):
            for z in range(H - 8, H):
                for x in range(bx - 2, bx + 3):
                    sh = 1.0 if z > H - 3 or (x + z) % 2 else 0.9
                    put(x, y1 + 1, z, P(shade(p['banner'], sh)))
            put(bx, y1 + 1, H - 9, P(p['tip']))

    # флажки на угловых башнях (с 4-го уровня)
    if lv >= 4:
        for tx, ty in corners:
            ztop = max(z for (x, y, z) in vox if x == tx and y == ty)
            flag(tx, ty, ztop + 1, p['banner'], 5)

    # внутренние блоки не видно: убираем, файл и загрузка легче
    def hidden(x, y, z):
        return all((x + a, y + b, z + c) in vox for a, b, c in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)))
    vs = [(x, y, z, c) for (x, y, z), c in vox.items() if not hidden(x, y, z)]
    mx = max(x for x, y, z, c in vs) + 1
    my = max(y for x, y, z, c in vs) + 2
    mz = max(z for x, y, z, c in vs) + 1
    out = os.path.join(HERE, 'zamok_%d.vox' % lv)
    write(out, vs, P.pal, (max(mx, my), max(mx, my), mz))
    return out


if __name__ == '__main__':
    lv_list = [int(a) for a in sys.argv[1:]] or list(range(1, 11))
    for lv in lv_list:
        f = build(lv)
        if os.environ.get('PREVIEW'):
            render(f, os.environ['PREVIEW'] + '/zamok_%d.png' % lv, 6)
        print(f)
