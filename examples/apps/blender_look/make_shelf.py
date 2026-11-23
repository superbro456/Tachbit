#!/usr/bin/env python3
# Делает shelf.glb — то же, что получилось бы из Blender (File → Export → glTF 2.0, .glb):
# материалы Principled BSDF с картами. Нужен только Python 3: python3 make_shelf.py
#   шар «ржавчина»  — карта цвета + карта металла/шероховатости (металл, где нет ржавчины)
#   плита «камень»   — карта цвета + карта нормалей (рельеф швов) + шероховатость
#   кольцо «неон»    — свечение (emission) цветом и силой, без карт
#   куб «стекло»     — прозрачность (alpha blend)
import json, math, struct, zlib, os

def png(w, h, px):  # px(x, y) -> (r, g, b)
    raw = b"".join(b"\0" + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b"")

def noise(x, y, s):
    v = 0.0
    for o in range(4):
        f = 2 ** o
        v += (math.sin(x * 0.11 * f + s) * math.cos(y * 0.13 * f - s * 1.7) + math.sin((x + y) * 0.07 * f + s * 2.3)) / (2 * f)
    return v

N = 128
def rust_amount(x, y): return max(0.0, min(1.0, 0.5 + noise(x, y, 1.0) * 0.9))
rust_col = png(N, N, lambda x, y: tuple(int(a + (b - a) * rust_amount(x, y)) for a, b in zip((205, 210, 220), (140, 70, 35))))
rust_mr = png(N, N, lambda x, y: (255, int(255 * (0.18 + 0.75 * rust_amount(x, y))), int(255 * (1 - rust_amount(x, y)))))
def stone_h(x, y):  # плитки 4×4 со швами
    gx, gy = (x % 32) / 32.0, ((y + (16 if (x // 32) % 2 else 0)) % 32) / 32.0
    e = min(gx, 1 - gx, gy, 1 - gy)
    return min(1.0, e * 9) * (0.85 + 0.15 * noise(x, y, 3.0))
stone_col = png(N, N, lambda x, y: tuple(int(c * (0.55 + 0.45 * stone_h(x, y))) for c in (135, 125, 112)))
def stone_n(x, y):
    dx = (stone_h((x + 1) % N, y) - stone_h((x - 1) % N, y)) * 3
    dy = (stone_h(x, (y + 1) % N) - stone_h(x, (y - 1) % N)) * 3
    l = math.sqrt(dx * dx + dy * dy + 1)
    return (int((-dx / l * 0.5 + 0.5) * 255), int((dy / l * 0.5 + 0.5) * 255), int((1 / l * 0.5 + 0.5) * 255))
stone_nrm = png(N, N, stone_n)
images = [rust_col, rust_mr, stone_col, stone_nrm]

# --- геометрия (оси glTF: y вверх, z к зрителю) ---
def sphere(cx, cy, cz, r, n=24):
    P, Nn, T, I = [], [], [], []
    for i in range(n + 1):
        a = math.pi * i / n
        for j in range(2 * n + 1):
            b = 2 * math.pi * j / (2 * n)
            d = (math.sin(a) * math.cos(b), math.cos(a), math.sin(a) * math.sin(b))
            P.append((cx + r * d[0], cy + r * d[1], cz + r * d[2])); Nn.append(d); T.append((j / (2 * n) * 2, i / n))
    w = 2 * n + 1
    for i in range(n):
        for j in range(2 * n):
            a, b, c, d = i * w + j, i * w + j + 1, (i + 1) * w + j + 1, (i + 1) * w + j
            I += [a, b, c, a, c, d]
    return P, Nn, T, I

def box(x0, y0, z0, x1, y1, z1, uvk=1.0):
    P, Nn, T, I = [], [], [], []
    faces = [((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]), ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
             ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]), ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
             ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]), ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)])]
    for nrm, q in faces:
        b = len(P)
        P += q; Nn += [nrm] * 4; T += [(0, uvk), (uvk, uvk), (uvk, 0), (0, 0)]
        I += [b, b + 1, b + 2, b, b + 2, b + 3]
    return P, Nn, T, I

def ring(cx, cy, cz, R, r, n=40, m=10):
    P, Nn, T, I = [], [], [], []
    for i in range(n + 1):
        a = 2 * math.pi * i / n
        for j in range(m + 1):
            b = 2 * math.pi * j / m
            d = (math.cos(a) * math.cos(b), math.sin(b), math.sin(a) * math.cos(b))
            P.append((cx + (R + r * math.cos(b)) * math.cos(a), cy + r * math.sin(b), cz + (R + r * math.cos(b)) * math.sin(a))); Nn.append(d); T.append((i / n, j / m))
    for i in range(n):
        for j in range(m):
            a, b, c, d = i * (m + 1) + j, (i + 1) * (m + 1) + j, (i + 1) * (m + 1) + j + 1, i * (m + 1) + j + 1
            I += [a, c, b, a, d, c]
    return P, Nn, T, I

objs = [("rust_ball", sphere(-1.6, 0.7, 0, 0.7), 0), ("stone_slab", box(-3.5, -0.2, -2.5, 3.5, 0.0, 2.5, 3.0), 1),
        ("neon_ring", ring(0.2, 0.75, 0, 0.55, 0.09), 2), ("glass_cube", box(1.4, 0.0, -0.45, 2.3, 0.9, 0.45), 3)]
materials = [
    {"name": "rust", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1}, "metallicFactor": 1.0, "roughnessFactor": 1.0}},
    {"name": "stone", "pbrMetallicRoughness": {"baseColorTexture": {"index": 2}, "metallicFactor": 0.0, "roughnessFactor": 0.8}, "normalTexture": {"index": 3, "scale": 1.0}},
    {"name": "neon", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1], "metallicFactor": 0.0, "roughnessFactor": 0.4}, "emissiveFactor": [0.1, 1.0, 0.8], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2.0}}},
    {"name": "glass", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.75, 1.0, 0.35], "metallicFactor": 0.0, "roughnessFactor": 0.05}, "alphaMode": "BLEND"},
]

bin_ = bytearray(); views = []; accs = []
def add_view(data):
    while len(bin_) % 4: bin_.append(0)
    views.append({"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data)}); bin_.extend(data); return len(views) - 1
def add_acc(vals, typ, n):
    flat = [c for v in vals for c in (v if isinstance(v, tuple) else (v,))]
    if typ == "SCALAR":
        v = add_view(struct.pack("<%dI" % len(flat), *flat)); accs.append({"bufferView": v, "componentType": 5125, "count": len(flat), "type": "SCALAR"})
    else:
        v = add_view(struct.pack("<%df" % len(flat), *flat)); a = {"bufferView": v, "componentType": 5126, "count": len(vals), "type": typ}
        if typ == "VEC3": a["min"] = [min(p[i] for p in vals) for i in range(3)]; a["max"] = [max(p[i] for p in vals) for i in range(3)]
        accs.append(a)
    return len(accs) - 1
meshes, nodes = [], []
for name, (P, Nn, T, I), mat in objs:
    prim = {"attributes": {"POSITION": add_acc(P, "VEC3", 3), "NORMAL": add_acc(Nn, "VEC3", 3), "TEXCOORD_0": add_acc(T, "VEC2", 2)}, "indices": add_acc(I, "SCALAR", 1), "material": mat}
    meshes.append({"name": name, "primitives": [prim]}); nodes.append({"name": name, "mesh": len(meshes) - 1})
imgs = []
for data in images:
    imgs.append({"bufferView": add_view(data), "mimeType": "image/png"})
while len(bin_) % 4: bin_.append(0)
gl = {"asset": {"version": "2.0", "generator": "make_shelf.py"}, "extensionsUsed": ["KHR_materials_emissive_strength"], "scene": 0, "scenes": [{"nodes": list(range(len(nodes)))}],
      "nodes": nodes, "meshes": meshes, "materials": materials, "textures": [{"source": i} for i in range(len(imgs))], "images": imgs,
      "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(bin_)}]}
js = json.dumps(gl, separators=(",", ":")).encode()
while len(js) % 4: js += b" "
out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)) + struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(bin_), 0x004E4942) + bytes(bin_)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "shelf.glb"), "wb").write(out)
print("shelf.glb:", len(out), "байт")
