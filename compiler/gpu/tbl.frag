#version 450
#extension GL_ARB_gpu_shader_int64 : require
// Tachbit: «новый» треугольник 3D — свет по углам (солнце, лампы, блики, свечение), тени, туман, прозрачность.
// Та же целая формула, что tb_lit_px в runtime.h: кадр совпадает с процессором точка в точку.
// Значение в точке = (e0·v0·W0 + e1·v1·W1 + e2·v2·W2) / (e0·W0 + e1·W1 + e2·W2): e — стороны треугольника
// для этой точки (как в edge), W — 1/z угла
layout(location = 0) flat in ivec2 vo;
layout(location = 1) flat in uvec2 vcm;
layout(location = 2) flat in ivec4 vq;
layout(location = 3) flat in uvec4 vp;
layout(std430, set = 0, binding = 1) readonly buffer Img { uint px[]; } img;
layout(std430, set = 0, binding = 2) readonly buffer Pl { uvec4 pl[]; } planes;
layout(std430, set = 0, binding = 5) readonly buffer Tex { ivec4 t[]; } tex;
layout(std430, set = 0, binding = 6) readonly buffer Lit { int r[]; } lit;     // tb_glit_t: 48 чисел на треугольник
layout(std430, set = 0, binding = 7) readonly buffer Sm { uint d[]; } sm;      // карты теней
layout(location = 0) out vec4 col;
bool edge(ivec2 a, ivec2 b, ivec2 p) {
    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
int64_t ledge(ivec2 a, ivec2 b, ivec2 p) {
    return int64_t(b.x - a.x) * int64_t(2 * p.y + 1 - 2 * a.y) - int64_t(b.y - a.y) * int64_t(2 * p.x + 1 - 2 * a.x);
}
int64_t fdiv(int64_t a, int64_t b) {
    if (a >= 0l) return int64_t(uint64_t(a) / uint64_t(b));
    return -int64_t((uint64_t(-a) + uint64_t(b) - 1ul) / uint64_t(b));
}
int64_t i64(int lo, int hi) { return (int64_t(hi) << 32) | int64_t(uint(lo)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 A = vq.xy, B = vq.zw, C = vo;
    if (!edge(A, B, p) || !edge(B, C, p) || !edge(C, A, p)) discard;
    uvec4 P = planes.pl[(vcm.y & 0x07FFFFFFu) >> 2];
    uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);
    int d = clamp(int(kk) >> 6, 0, 16777215);
    int R = int(P.w) * 48;
    int flags = lit.r[R + 3];
    uint base = vcm.x;
    int ti = lit.r[R + 43];
    if (ti >= 0) {   // картинка: как tbt.frag
        int r = ti * 4;
        ivec4 t0 = tex.t[r], t1 = tex.t[r + 1], t2 = tex.t[r + 2], t3 = tex.t[r + 3];
        int64_t au = i64(t0.x, t0.y), bu = i64(t0.z, t0.w), cu = i64(t1.x, t1.y), av = i64(t1.z, t1.w), bv = i64(t2.x, t2.y), cv = i64(t2.z, t2.w);
        int64_t W = int64_t((16777216 - d) >> 4);
        if (W < 1l) W = 1l;
        int64_t X = int64_t(2 * p.x + 1), Y = int64_t(2 * p.y + 1);
        int64_t su = (cu + au * X + bu * Y) >> 6, sv = (cv + av * X + bv * Y) >> 6;
        int64_t w = int64_t(uint(t3.y)), h = int64_t(uint(t3.z));
        int64_t u = fdiv(fdiv(su, W), 16l), v = fdiv(fdiv(sv, W), 16l);
        u -= fdiv(u, w) * w;
        v -= fdiv(v, h) * h;
        uint tp = img.px[uint(t3.x) + uint(v) * uint(w) + uint(u)];
        if ((tp >> 24) < 128u) discard;
        base = ((((tp >> 16) & 255u) * ((base >> 16) & 255u) + 127u) / 255u) << 16 | ((((tp >> 8) & 255u) * ((base >> 8) & 255u) + 127u) / 255u) << 8
             | (((tp & 255u) * (base & 255u) + 127u) / 255u);
    }
    int64_t e0 = ledge(B, C, p), e1 = ledge(C, A, p), e2 = ledge(A, B, p);
    int64_t dn = e0 * int64_t(lit.r[R]) + e1 * int64_t(lit.r[R + 1]) + e2 * int64_t(lit.r[R + 2]);
    if (dn < 1l) dn = 1l;
    int64_t v[11];
    int nv = (flags & 1) != 0 ? 11 : 8;
    for (int k = 0; k < nv; k++)
        v[k] = fdiv(e0 * int64_t(lit.r[R + 4 + k]) + e1 * int64_t(lit.r[R + 16 + k]) + e2 * int64_t(lit.r[R + 28 + k]), dn);
    int64_t sh = 256l;
    if ((flags & 1) != 0) {
        int64_t N = int64_t(lit.r[R + 44]), lx = v[8] - 8l, ly = v[9] - 8l, ld = v[10];
        int64_t ix = lx >> 4, fx = lx & 15l, iy = ly >> 4, fy = ly & 15l;
        sh = 0l;
        for (int k = 0; k < 4; k++) {
            int64_t i = ix + int64_t(k & 1), j = iy + int64_t(k >> 1);
            int64_t w = ((k & 1) != 0 ? fx : 16l - fx) * ((k >> 1) != 0 ? fy : 16l - fy);
            if (i < 0l || j < 0l || i >= N || j >= N || ld <= int64_t(sm.d[uint(int64_t(lit.r[R + 45]) + j * N + i)])) sh += w;
        }
    }
    int64_t ft = 0l;
    if ((flags & 2) != 0) {
        int64_t zq = fdiv(205l << 24, 16777216l - int64_t(d)), fn = int64_t(lit.r[R + 41]), ff = int64_t(lit.r[R + 42]);
        ft = zq <= fn ? 0l : zq >= ff ? 256l : (zq - fn) * 256l / (ff - fn);
    }
    uint a = uint(flags >> 8) & 255u, fc = uint(lit.r[R + 40]);
    uvec3 o;
    for (int k = 0; k < 3; k++) {
        int s = 16 - 8 * k;
        int64_t cb = int64_t((base >> uint(s)) & 255u), M = v[k] + ((v[6] * sh) >> 8), Ad = v[3 + k] + ((v[7] * sh) >> 8);
        int64_t r = ((cb * M + 128l) >> 8) + ((Ad + 8l) >> 4);
        if (r > 255l) r = 255l;
        if ((flags & 2) != 0) r = (r * (256l - ft) + int64_t((fc >> uint(s)) & 255u) * ft + 128l) >> 8;
        o[k] = uint(r);
    }
    gl_FragDepth = float(d) / 16777216.0;
    // прозрачный: своя часть цвета (c · a / 255) здесь, фон смешивает видеокарта (как tb_draw_image_a)
    uvec3 kk3 = (o * a + 127u) / 255u;
    col = vec4(vec3(kk3), float(a)) / 255.0;
}
