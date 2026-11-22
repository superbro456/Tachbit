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
print("значки готовы:", sorted(os.listdir(OUT)))
