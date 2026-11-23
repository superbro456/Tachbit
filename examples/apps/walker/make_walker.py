#!/usr/bin/env python3
# Делает walker.glb — человечка из коробок с костями (skin) и тремя анимациями: walk, wave, idle.
# Нужен только Python 3: python3 make_walker.py
import json, math, struct, os

joints = [  # имя, родитель, сустав (оси glTF: y вверх, лицом к +z)
    ("hips", -1, (0, 0.92, 0)), ("chest", 0, (0, 1.0, 0)), ("head", 1, (0, 1.47, 0)),
    ("arm_l", 1, (0.25, 1.42, 0)), ("fore_l", 3, (0.26, 1.13, 0)),
    ("arm_r", 1, (-0.25, 1.42, 0)), ("fore_r", 5, (-0.26, 1.13, 0)),
    ("leg_l", 0, (0.1, 0.88, 0)), ("shin_l", 7, (0.1, 0.48, 0)),
    ("leg_r", 0, (-0.1, 0.88, 0)), ("shin_r", 9, (-0.1, 0.48, 0)),
]
J = {n: i for i, (n, _, _) in enumerate(joints)}
# коробки: материал, кость (или две кости снизу/сверху с весами), x0 y0 z0 x1 y1 z1
boxes = [
    ("pants", "hips", None, (-0.17, 0.82, -0.1, 0.17, 1.0, 0.1)),
    ("shirt", "hips", "chest", (-0.2, 1.0, -0.12, 0.2, 1.45, 0.12)),
    ("skin", "head", None, (-0.05, 1.45, -0.05, 0.05, 1.52, 0.05)),
    ("skin", "head", None, (-0.12, 1.52, -0.12, 0.12, 1.8, 0.13)),
    ("eyes", "head", None, (-0.08, 1.64, 0.13, -0.03, 1.69, 0.14)),
    ("eyes", "head", None, (0.03, 1.64, 0.13, 0.08, 1.69, 0.14)),
    ("shirt", "arm_l", None, (0.21, 1.12, -0.06, 0.31, 1.44, 0.06)),
    ("skin", "fore_l", None, (0.22, 0.86, -0.05, 0.3, 1.13, 0.05)),
    ("shirt", "arm_r", None, (-0.31, 1.12, -0.06, -0.21, 1.44, 0.06)),
    ("skin", "fore_r", None, (-0.3, 0.86, -0.05, -0.22, 1.13, 0.05)),
    ("pants", "leg_l", None, (0.03, 0.47, -0.07, 0.17, 0.88, 0.07)),
    ("pants", "shin_l", None, (0.04, 0.06, -0.06, 0.16, 0.48, 0.06)),
    ("shoes", "shin_l", None, (0.03, 0.0, -0.07, 0.17, 0.07, 0.17)),
    ("pants", "leg_r", None, (-0.17, 0.47, -0.07, -0.03, 0.88, 0.07)),
    ("pants", "shin_r", None, (-0.16, 0.06, -0.06, -0.04, 0.48, 0.06)),
    ("shoes", "shin_r", None, (-0.17, 0.0, -0.07, -0.03, 0.07, 0.17)),
]
mats = {"skin": (0.9, 0.62, 0.45), "shirt": (0.1, 0.35, 0.8), "pants": (0.12, 0.12, 0.16), "shoes": (0.35, 0.15, 0.05), "eyes": (0.02, 0.02, 0.02)}

def box(b):
    x0, y0, z0, x1, y1, z1 = b
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 4, 7, 3), (1, 2, 6, 5), (3, 7, 6, 2), (0, 1, 5, 4)]   # против часовой снаружи
    tris = []
    for a, b2, c, d in f:
        tris += [a, b2, c, a, c, d]
    return v, tris

blob = bytearray()
def put(data, fmt):
    global blob
    while len(blob) % 4: blob.append(0)
    off = len(blob)
    for x in data: blob += struct.pack(fmt, *x) if isinstance(x, tuple) else struct.pack(fmt, x)
    return off, len(blob) - off

views, accs = [], []
def acc(data, fmt, ctype, typ, count, target=None, mm=False):
    off, n = put(data, fmt)
    views.append({"buffer": 0, "byteOffset": off, "byteLength": n, **({"target": target} if target else {})})
    a = {"bufferView": len(views) - 1, "componentType": ctype, "count": count, "type": typ}
    if mm:
        a["min"] = [min(p[i] for p in data) for i in range(3)]
        a["max"] = [max(p[i] for p in data) for i in range(3)]
    accs.append(a)
    return len(accs) - 1

prims = []
mat_names = list(mats)
for mname in mat_names:
    P, Jn, W, I = [], [], [], []
    for mat, bone, bone2, b in boxes:
        if mat != mname: continue
        v, tris = box(b)
        base = len(P)
        for (x, y, z) in v:
            P.append((x, y, z))
            if bone2 and y <= b[1] + 1e-6:
                Jn.append((J[bone], J[bone2], 0, 0)); W.append((0.5, 0.5, 0.0, 0.0))
            elif bone2:
                Jn.append((J[bone2], 0, 0, 0)); W.append((1.0, 0.0, 0.0, 0.0))
            else:
                Jn.append((J[bone], 0, 0, 0)); W.append((1.0, 0.0, 0.0, 0.0))
        I += [base + t for t in tris]
    attrs = {"POSITION": acc(P, "<3f", 5126, "VEC3", len(P), 34962, True),
             "JOINTS_0": acc(Jn, "<4B", 5121, "VEC4", len(Jn), 34962),
             "WEIGHTS_0": acc(W, "<4f", 5126, "VEC4", len(W), 34962)}
    prims.append({"attributes": attrs, "indices": acc(I, "<H", 5123, "SCALAR", len(I), 34963), "material": mat_names.index(mname)})

ibm = []
for n, p, (x, y, z) in joints:
    ibm.append((1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1))
ibm_acc = acc(ibm, "<16f", 5126, "MAT4", len(ibm))

nodes = [{"name": "walker", "mesh": 0, "skin": 0}]
for i, (n, p, pos) in enumerate(joints):
    pp = joints[p][2] if p >= 0 else (0, 0, 0)
    nodes.append({"name": n, "translation": [pos[k] - pp[k] for k in range(3)]})
for i, (n, p, pos) in enumerate(joints):
    kids = [k + 1 for k, (_, q, _) in enumerate(joints) if q == i]
    if kids: nodes[i + 1]["children"] = kids

def q_axis(ax, deg):
    a = math.radians(deg) / 2
    s = math.sin(a)
    return (ax[0] * s, ax[1] * s, ax[2] * s, math.cos(a))

anims = []
def anim(name, length, keys, tracks):
    # tracks: (кость, "rotation" | "translation", функция t → значение)
    times = [length * i / keys for i in range(keys + 1)]
    ta = acc(times, "<f", 5126, "SCALAR", len(times))
    accs[ta]["min"] = [0.0]; accs[ta]["max"] = [length]
    samplers, chans = [], []
    for bone, path, fn in tracks:
        vals = [fn(t) for t in times]
        va = acc(vals, "<4f" if path == "rotation" else "<3f", 5126, "VEC4" if path == "rotation" else "VEC3", len(vals))
        samplers.append({"input": ta, "output": va, "interpolation": "LINEAR"})
        chans.append({"sampler": len(samplers) - 1, "target": {"node": J[bone] + 1, "path": path}})
    anims.append({"name": name, "samplers": samplers, "channels": chans})

X, Z = (1, 0, 0), (0, 0, 1)
w = lambda t: 2 * math.pi * t
hp = joints[0][2]
anim("walk", 1.0, 16, [
    ("leg_l", "rotation", lambda t: q_axis(X, -32 * math.sin(w(t)))),
    ("leg_r", "rotation", lambda t: q_axis(X, 32 * math.sin(w(t)))),
    ("shin_l", "rotation", lambda t: q_axis(X, 30 * max(0, math.sin(w(t) + 1.2)) + 4)),
    ("shin_r", "rotation", lambda t: q_axis(X, 30 * max(0, -math.sin(w(t) + 1.2)) + 4)),
    ("arm_l", "rotation", lambda t: q_axis(X, 28 * math.sin(w(t)))),
    ("arm_r", "rotation", lambda t: q_axis(X, -28 * math.sin(w(t)))),
    ("fore_l", "rotation", lambda t: q_axis(X, -20 - 10 * math.sin(w(t)))),
    ("fore_r", "rotation", lambda t: q_axis(X, -20 + 10 * math.sin(w(t)))),
    ("hips", "translation", lambda t: (0, hp[1] + 0.03 * math.cos(2 * w(t)), 0)),
    ("chest", "rotation", lambda t: q_axis((0, 1, 0), 6 * math.sin(w(t)))),
])
anim("wave", 2.0, 32, [
    ("arm_r", "rotation", lambda t: q_axis(Z, -150)),
    ("fore_r", "rotation", lambda t: q_axis(Z, 25 * math.sin(2 * w(t / 2)))),
    ("head", "rotation", lambda t: q_axis(Z, -8)),
    ("chest", "rotation", lambda t: q_axis(Z, 3 * math.sin(w(t / 2)))),
])
anim("idle", 2.0, 16, [
    ("chest", "rotation", lambda t: q_axis(X, 3 * math.sin(w(t / 2)))),
    ("head", "rotation", lambda t: q_axis((0, 1, 0), 15 * math.sin(w(t / 2)))),
    ("arm_l", "rotation", lambda t: q_axis(Z, 6 + 2 * math.sin(w(t / 2)))),
    ("arm_r", "rotation", lambda t: q_axis(Z, -6 - 2 * math.sin(w(t / 2)))),
])

gl = {
    "asset": {"version": "2.0", "generator": "Tachbit make_walker.py"},
    "scene": 0, "scenes": [{"nodes": [0, 1]}], "nodes": nodes,
    "meshes": [{"name": "walker", "primitives": prims}],
    "materials": [{"name": n, "pbrMetallicRoughness": {"baseColorFactor": [*mats[n], 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.8}} for n in mat_names],
    "skins": [{"joints": [i + 1 for i in range(len(joints))], "inverseBindMatrices": ibm_acc, "skeleton": 1}],
    "animations": anims, "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(blob)}],
}
js = json.dumps(gl, separators=(",", ":")).encode()
while len(js) % 4: js += b" "
while len(blob) % 4: blob.append(0)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "walker.glb")
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
    f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
    f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))
print("готово:", out, len(js) + len(blob), "байт")
