#version 450
#extension GL_ARB_gpu_shader_int64 : require
// Tachbit: 3D на видеокарте. Одна фигура = один треугольник сетки в одном draw_mesh:
// номер фигуры = запись * ntri + треугольник. Углы поворачиваются, смотрятся камерой и ложатся на экран
// здесь, в целых числах — та же формула, что tb_m3_corner и tb_m3_tri в runtime.h. Поэтому кадр видеокарты
// совпадает с кадром процессора точка в точку на любой видеокарте (дроби разные карты считают чуть по-разному).
// Дальше — как обычный треугольник с глубиной: квадрат вокруг, точки решает tb.frag.
layout(push_constant) uniform P { vec2 size; int tbase, ntri, rbase, f, gw, gh, cull; } pc;
layout(std430, set = 0, binding = 3) readonly buffer Mesh { ivec4 v[]; } mesh;   // углы (x, y, z) × 4096 и треугольники (a, b, c, цвет)
layout(std430, set = 0, binding = 4) readonly buffer Rec { ivec4 r[]; } rec;     // на draw_mesh 3 строки: поворот × 2^30, сдвиг × 4096
layout(location = 0) flat out ivec2 vo;
layout(location = 1) flat out uvec2 vcm;
layout(location = 2) flat out ivec4 vq;
layout(location = 3) flat out uvec4 vp;
// деление с округлением вниз (b > 0), как tb_fdiv
int64_t fdiv(int64_t a, int64_t b) {
    if (a >= 0l) return int64_t(uint64_t(a) / uint64_t(b));
    return -int64_t((uint64_t(-a) + uint64_t(b) - 1ul) / uint64_t(b));
}
// угол: место на экране и глубина (tb_m3_corner)
i64vec3 corner(ivec4 p, int ri) {
    int64_t c[3];
    for (int k = 0; k < 3; k++) {
        ivec4 m = rec.r[ri + k];
        c[k] = ((int64_t(m.x) * int64_t(p.x) + int64_t(m.y) * int64_t(p.y) + int64_t(m.z) * int64_t(p.z)) >> 30) + int64_t(m.w);
    }
    int64_t f = int64_t(pc.f);
    return i64vec3((int64_t(pc.gw) * 128l + fdiv(f * c[0], c[2]) + 128l) >> 8,
                   (int64_t(pc.gh) * 128l - fdiv(f * c[1], c[2]) + 128l) >> 8,
                   16777216l - fdiv(205l << 24, c[2]));
}
void main() {
    int iid = gl_InstanceIndex, d = iid / pc.ntri, t = iid - d * pc.ntri;
    ivec4 tr = mesh.v[pc.tbase + t];
    int ri = pc.rbase + d * 3;
    i64vec3 a = corner(mesh.v[tr.x], ri), b = corner(mesh.v[tr.y], ri), c = corner(mesh.v[tr.z], ri);
    // tb_m3_tri: площадь, невидимые грани, углы по кругу, плоскость глубины
    int64_t area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    bool skip = area == 0l || (pc.cull > 0 && area > 0l) || (pc.cull < 0 && area < 0l);
    if (area < 0l) { i64vec3 k = b; b = c; c = k; area = -area; }
    if (skip) area = 1l;
    int64_t e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y, d1 = b.z - a.z, d2 = c.z - a.z;
    int64_t A = fdiv(64l * (d1 * e2y - d2 * e1y) + area, 2l * area), B = fdiv(64l * (d2 * e1x - d1 * e2x) + area, 2l * area);
    int64_t C = 64l * a.z - 2l * A * a.x - 2l * B * a.y;
    ivec4 q = ivec4(int(a.x), int(a.y), int(b.x), int(b.y));
    ivec2 o = ivec2(int(c.x), int(c.y));
    ivec4 bb;
    bb.xy = min(min(q.xy, q.zw), o);
    bb.zw = max(max(q.xy, q.zw), o) + 1;
    bb = clamp(bb, ivec4(0), ivec4(ivec2(pc.size), ivec2(pc.size)));
    if (skip) bb = ivec4(0);
    int v = gl_VertexIndex;
    vec2 p = vec2((v & 1) != 0 ? bb.z : bb.x, (v & 2) != 0 ? bb.w : bb.y);
    gl_Position = vec4(p / pc.size * 2.0 - 1.0, 0.0, 1.0);
    vo = o;
    vcm = uvec2(uint(tr.w) & 0xFFFFFFu, 3u);
    vq = q;
    vp = uvec4(uint(uint64_t(A) & 0xFFFFFFFFul), uint(uint64_t(B) & 0xFFFFFFFFul), uint(uint64_t(C) & 0xFFFFFFFFul), 0u);
}
