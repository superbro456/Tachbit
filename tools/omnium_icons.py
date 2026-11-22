# Значки Омниума: рисуются в 4 раза крупнее и уменьшаются (гладкие края), кладутся в omnium/icons/*.png.
# Запуск: python3 tools/omnium_icons.py   (нужен Pillow)
import math, os
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), "..", "omnium", "icons")
K = 4

def canvas(n):
    return Image.new("RGBA", (n * K, n * K), (0, 0, 0, 0))

def grad(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g.resize((w, h))

def rr_mask(size, box, r):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, r, fill=255)
    return m

def paste_grad(im, box, r, top, bottom, ellipse=False):
    m = Image.new("L", im.size, 0)
    d = ImageDraw.Draw(m)
    if ellipse:
        d.ellipse(box, fill=255)
    else:
        d.rounded_rectangle(box, r, fill=255)
    g = grad((box[2] - box[0], box[3] - box[1]), top, bottom)
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    layer.paste(g, (box[0], box[1]))
    im.alpha_composite(Image.composite(layer, Image.new("RGBA", im.size, (0, 0, 0, 0)), m))

def soft_shadow(im, box, r, alpha=55, dy=2, blur=2, ellipse=False):
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    b = (box[0], box[1] + dy * K, box[2], box[3] + dy * K)
    if ellipse:
        d.ellipse(b, fill=(30, 40, 80, alpha))
    else:
        d.rounded_rectangle(b, r, fill=(30, 40, 80, alpha))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur * K)))

def gloss(im, box, r):
    # светлая полоса сверху: как стекло на значке
    w, h = box[2] - box[0], box[3] - box[1]
    g = grad((w, h // 2), (255, 255, 255, 70), (255, 255, 255, 0))
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    layer.paste(g, (box[0], box[1]))
    m = rr_mask(im.size, box, r)
    im.alpha_composite(Image.composite(layer, Image.new("RGBA", im.size, (0, 0, 0, 0)), m))

def save(im, n, name):
    im.resize((n, n), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))

def tile(n, top, bottom):
    im = canvas(n)
    p = int(n * 0.1) * K
    box = (p, p, n * K - p, n * K - p - K)
    r = int((box[2] - box[0]) * 0.27)
    soft_shadow(im, box, r)
    paste_grad(im, box, r, top, bottom)
    gloss(im, box, r)
    return im, box

def line(d, pts, w, c):
    d.line(pts, fill=c, width=w, joint="curve")
    for x, y in (pts[0], pts[-1]):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=c)

def terminal(n=48):
    im, b = tile(n, (58, 70, 92, 255), (28, 34, 48, 255))
    d = ImageDraw.Draw(im)
    s = (b[2] - b[0]) / 100
    x0, y0 = b[0], b[1]
    w = int(7 * s)
    line(d, [(x0 + 26 * s, y0 + 34 * s), (x0 + 44 * s, y0 + 50 * s), (x0 + 26 * s, y0 + 66 * s)], w, (236, 242, 250, 255))
    line(d, [(x0 + 52 * s, y0 + 68 * s), (x0 + 74 * s, y0 + 68 * s)], w, (120, 230, 150, 255))
    return im

def notes(n=48):
    im, b = tile(n, (98, 168, 255, 255), (48, 112, 245, 255))
    d = ImageDraw.Draw(im)
    s = (b[2] - b[0]) / 100
    x0, y0 = b[0], b[1]
    d.rounded_rectangle((x0 + 24 * s, y0 + 20 * s, x0 + 76 * s, y0 + 82 * s), 8 * s, fill=(255, 255, 255, 245))
    for i, ln in enumerate((40, 34, 40, 22)):
        y = y0 + (36 + i * 12) * s
        line(d, [(x0 + 34 * s, y), (x0 + (34 + ln) * s, y)], int(4.5 * s), (120, 160, 230, 255))
    return im

def folder_img(n, back_top, back_bot, front_top, front_bot, tile_bg=False):
    im = canvas(n)
    s = n * K / 100
    soft_shadow(im, (int(10 * s), int(30 * s), int(90 * s), int(84 * s)), int(10 * s), alpha=45, dy=2, blur=2)
    paste_grad(im, (int(10 * s), int(18 * s), int(46 * s), int(36 * s)), int(7 * s), back_top, back_bot)
    paste_grad(im, (int(10 * s), int(25 * s), int(90 * s), int(80 * s)), int(9 * s), back_top, back_bot)
    paste_grad(im, (int(10 * s), int(34 * s), int(90 * s), int(84 * s)), int(9 * s), front_top, front_bot)
    gloss(im, (int(10 * s), int(34 * s), int(90 * s), int(84 * s)), int(9 * s))
    return im

def about(n=48):
    im = canvas(n)
    p = int(n * 0.1) * K
    box = (p, p, n * K - p, n * K - p - K)
    soft_shadow(im, box, 0, ellipse=True)
    paste_grad(im, box, 0, (196, 160, 255, 255), (140, 96, 240, 255), ellipse=True)
    d = ImageDraw.Draw(im)
    s = (box[2] - box[0]) / 100
    x0, y0 = box[0], box[1]
    d.ellipse((x0 + 44 * s, y0 + 22 * s, x0 + 56 * s, y0 + 34 * s), fill=(255, 255, 255, 255))
    d.rounded_rectangle((x0 + 44 * s, y0 + 42 * s, x0 + 56 * s, y0 + 78 * s), 6 * s, fill=(255, 255, 255, 255))
    return im

def clock(n=48):
    im = canvas(n)
    p = int(n * 0.1) * K
    box = (p, p, n * K - p, n * K - p - K)
    soft_shadow(im, box, 0, ellipse=True)
    paste_grad(im, box, 0, (255, 150, 180, 255), (240, 90, 130, 255), ellipse=True)
    d = ImageDraw.Draw(im)
    s = (box[2] - box[0]) / 100
    x0, y0 = box[0], box[1]
    d.ellipse((x0 + 14 * s, y0 + 14 * s, x0 + 86 * s, y0 + 86 * s), fill=(255, 255, 255, 255))
    c = (x0 + 50 * s, y0 + 50 * s)
    line(d, [c, (c[0], c[1] - 24 * s)], int(6 * s), (50, 58, 80, 255))
    line(d, [c, (c[0] + 16 * s, c[1] + 6 * s)], int(6 * s), (50, 58, 80, 255))
    return im

def doc(n=64):
    im = canvas(n)
    s = n * K / 100
    box = (int(24 * s), int(12 * s), int(76 * s), int(88 * s))
    soft_shadow(im, box, int(7 * s), alpha=40, dy=2, blur=2)
    d = ImageDraw.Draw(im)
    f = int(16 * s)
    d.rounded_rectangle(box, int(7 * s), fill=(255, 255, 255, 255))
    d.polygon([(box[2] - f, box[1]), (box[2], box[1] + f), (box[2] - f, box[1] + f)], fill=(222, 228, 240, 255))
    d.rectangle((box[2] - f, box[1] - 2, box[2] + 2, box[1] + f - 1), fill=(0, 0, 0, 0))
    d.polygon([(box[2] - f, box[1]), (box[2], box[1] + f), (box[2] - f + int(3 * s), box[1] + f)], fill=(210, 218, 232, 255))
    im2 = canvas(n)
    im2.alpha_composite(im)
    d = ImageDraw.Draw(im2)
    for i, ln in enumerate((30, 24, 30, 18)):
        y = int((40 + i * 11) * s)
        line(d, [(34 * s, y), ((34 + ln) * s, y)], int(4 * s), (180, 190, 210, 255))
    return im2

def photo(n=64):
    im = canvas(n)
    s = n * K / 100
    box = (int(12 * s), int(20 * s), int(88 * s), int(80 * s))
    soft_shadow(im, box, int(9 * s), alpha=45, dy=2, blur=2)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle(box, int(9 * s), fill=(255, 255, 255, 255))
    inner = (int(17 * s), int(25 * s), int(83 * s), int(75 * s))
    paste_grad(im, inner, int(6 * s), (130, 190, 255, 255), (200, 228, 255, 255))
    pic = canvas(n)
    d = ImageDraw.Draw(pic)
    d.ellipse((62 * s, 32 * s, 74 * s, 44 * s), fill=(255, 200, 70, 255))
    d.polygon([(17 * s, 75 * s), (38 * s, 46 * s), (60 * s, 75 * s)], fill=(70, 150, 120, 255))
    d.polygon([(42 * s, 75 * s), (62 * s, 54 * s), (83 * s, 75 * s)], fill=(50, 120, 100, 255))
    m = rr_mask(im.size, inner, int(6 * s))
    im.alpha_composite(Image.composite(pic, Image.new("RGBA", im.size, (0, 0, 0, 0)), m))
    return im

def logo(n):
    im = canvas(n)
    c = n * K / 2
    R = n * K * 0.46
    cols = [(255, 170, 60), (255, 90, 120), (170, 110, 255), (70, 150, 255), (60, 200, 170), (150, 220, 80)]
    for arm in range(6):
        for i in range(24):
            t = i / 23
            a = arm * math.pi / 3 + t * 1.25
            d = R * (0.28 + 0.66 * t)
            rr = R * (0.06 + 0.12 * t)
            x, y = c + d * math.cos(a), c + d * math.sin(a)
            col = cols[arm] + (int(170 + 85 * t),)
            ImageDraw.Draw(im).ellipse((x - rr, y - rr, x + rr, y + rr), fill=col)
    glow = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((c - R * 0.42, c - R * 0.42, c + R * 0.42, c + R * 0.42), fill=(255, 255, 255, 230))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(R * 0.08)))
    ImageDraw.Draw(im).ellipse((c - R * 0.24, c - R * 0.24, c + R * 0.24, c + R * 0.24), fill=(255, 255, 255, 255))
    return im


# ---------- значки-линии (боковая панель, капсула справа, редактор) ----------
INK = (52, 60, 82, 255)

def stroke_icon(n, draw):
    im = canvas(n)
    d = ImageDraw.Draw(im)
    s = n * K / 24          # рисуем в сетке 24×24
    w = max(int(1.9 * s), 2)
    def L(*pts):
        line(d, [(x * s, y * s) for x, y in pts], w, INK)
    def A(cx, cy, r, a0, a1):
        d.arc(((cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s), a0, a1, fill=INK, width=w)
    def R(x0, y0, x1, y1, r, fill=None):
        d.rounded_rectangle((x0 * s, y0 * s, x1 * s, y1 * s), r * s, outline=None if fill else INK, width=w, fill=fill)
    def C(cx, cy, r, fill=None):
        d.ellipse(((cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s), outline=None if fill else INK, width=w, fill=fill)
    draw(L, A, R, C, d, s)
    return im

def i_home(L, A, R, C, d, s):
    L((3.5, 11), (12, 3.8), (20.5, 11))
    R(6, 9.5, 18, 20.5, 2.5)
    R(10.2, 14, 13.8, 20.5, 1.2)

def i_snap(L, A, R, C, d, s):
    A(12, 12, 8.5, 300, 600)
    L((3.4, 6.5), (3.6, 10.2), (7.2, 9.6))
    L((12, 7.5), (12, 12), (15, 14))

def i_disk(L, A, R, C, d, s):
    R(3, 6, 21, 18, 3)
    L((6.5, 14.2), (12, 14.2))
    C(16.8, 14.2, 1.3, fill=INK)

def i_usb(L, A, R, C, d, s):
    R(7, 9, 17, 21.5, 2.6)
    R(9, 3, 15, 9, 1.2)
    L((11, 5.6), (11, 6.2))
    L((13, 5.6), (13, 6.2))

def i_wifi(L, A, R, C, d, s):
    A(12, 19, 15, 228, 312)
    A(12, 19, 10, 226, 314)
    A(12, 19, 5, 222, 318)
    C(12, 19, 1.7, fill=INK)

def i_battery(L, A, R, C, d, s):
    R(7, 4, 17, 22, 3)
    R(10, 1.6, 14, 3.4, 0.8, fill=INK)
    R(9.6, 9.5, 14.4, 19.4, 1.4, fill=(70, 190, 120, 255))

def i_bell(L, A, R, C, d, s):
    d.line([(0, 0), (0, 0)])
    A(12, 10, 6, 180, 360)
    L((6, 10), (6, 15), (4.2, 17.8), (19.8, 17.8), (18, 15), (18, 10))
    A(12, 18.6, 2.6, 0, 180)
    L((12, 2.6), (12, 4))

def i_sun(L, A, R, C, d, s):
    C(12, 12, 4.2)
    for i in range(8):
        a = i * math.pi / 4
        L((12 + 7.4 * math.cos(a), 12 + 7.4 * math.sin(a)), (12 + 9.8 * math.cos(a), 12 + 9.8 * math.sin(a)))

def i_moon(L, A, R, C, d, s):
    # месяц: круг минус сдвинутый круг (заливка)
    m = Image.new("L", d.im.size if hasattr(d, "im") else (1, 1), 0)
    md = ImageDraw.Draw(m)
    md.ellipse((3.5 * s, 3.5 * s, 20.5 * s, 20.5 * s), fill=255)
    md.ellipse((9 * s, 0.5 * s, 24 * s, 15.5 * s), fill=0)
    d.bitmap((0, 0), m, fill=INK)

def i_page(L, A, R, C, d, s):
    L((14, 3), (7, 3), (5.5, 4.5), (5.5, 19.5), (7, 21), (17, 21), (18.5, 19.5), (18.5, 7.5), (14, 3))
    L((14, 3), (14, 7.5), (18.5, 7.5))
    L((8.8, 12.5), (15, 12.5))
    L((8.8, 16.5), (13, 16.5))

def i_search(L, A, R, C, d, s):
    C(10.5, 10.5, 6.5)
    L((15.3, 15.3), (20.5, 20.5))

def i_play(L, A, R, C, d, s):
    L((7, 4.5), (19, 12), (7, 19.5), (7, 4.5))

def i_grid(L, A, R, C, d, s):
    for x, y in ((4, 4), (13.5, 4), (4, 13.5), (13.5, 13.5)):
        R(x, y, x + 6.5, y + 6.5, 1.8)

os.makedirs(OUT, exist_ok=True)
save(terminal(), 48, "terminal")
save(notes(), 48, "notes")
save(folder_img(48, (250, 176, 40, 255), (240, 150, 20, 255), (255, 214, 92, 255), (255, 186, 52, 255)), 48, "files")
save(about(), 48, "about")
save(clock(), 48, "clock")
save(folder_img(64, (70, 140, 245, 255), (50, 110, 230, 255), (120, 180, 255, 255), (80, 145, 250, 255)), 64, "folder")
save(doc(), 64, "doc")
save(photo(), 64, "photo")
save(logo(44), 44, "logo")
save(logo(24), 24, "logo_small")
for nm, fn, n in (("home", i_home, 22), ("snap", i_snap, 22), ("disk", i_disk, 22), ("usb", i_usb, 22),
                  ("wifi", i_wifi, 28), ("battery", i_battery, 28), ("bell", i_bell, 28), ("sun", i_sun, 28), ("moon", i_moon, 28),
                  ("page", i_page, 22), ("search", i_search, 22), ("play", i_play, 22), ("grid", i_grid, 22)):
    save(stroke_icon(n, fn), n, nm)
print("значки готовы:", sorted(os.listdir(OUT)))
