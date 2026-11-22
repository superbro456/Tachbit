#version 450
#extension GL_ARB_gpu_shader_int64 : require
// Tachbit: треугольник сетки с картинкой (mesh_image). Как треугольник с глубиной в tb.frag, плюс точка картинки:
// W = (2^24 - глубина) >> 4 (это 1/z), u = (плоскость u·W) / W — с перспективой. Всё в целых, та же формула,
// что tb_tex_px в runtime.h: кадр совпадает с процессором точка в точку
layout(location = 0) flat in ivec2 vo;
layout(location = 1) flat in uvec2 vcm;
layout(location = 2) flat in ivec4 vq;
layout(location = 3) flat in uvec4 vp;
layout(std430, set = 0, binding = 1) readonly buffer Img { uint px[]; } img;
layout(std430, set = 0, binding = 2) readonly buffer Pl { uvec4 pl[]; } planes;
layout(std430, set = 0, binding = 5) readonly buffer Tex { ivec4 t[]; } tex;   // на треугольник 4 строки: плоскости u и v (по 64 бита), картинка
layout(location = 0) out vec4 col;
bool edge(ivec2 a, ivec2 b, ivec2 p) {
    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
int64_t fdiv(int64_t a, int64_t b) {
    if (a >= 0l) return int64_t(uint64_t(a) / uint64_t(b));
    return -int64_t((uint64_t(-a) + uint64_t(b) - 1ul) / uint64_t(b));
}
int64_t i64(int lo, int hi) { return (int64_t(hi) << 32) | int64_t(uint(lo)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (!edge(vq.xy, vq.zw, p) || !edge(vq.zw, vo, p) || !edge(vo, vq.xy, p)) discard;
    uvec4 P = planes.pl[(vcm.y & ~(1u << 29)) >> 2];
    uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);
    int d = clamp(int(kk) >> 6, 0, 16777215);
    int r = int(P.w) * 4;
    ivec4 t0 = tex.t[r], t1 = tex.t[r + 1], t2 = tex.t[r + 2], t3 = tex.t[r + 3];
    int64_t au = i64(t0.x, t0.y), bu = i64(t0.z, t0.w), cu = i64(t1.x, t1.y), av = i64(t1.z, t1.w), bv = i64(t2.x, t2.y), cv = i64(t2.z, t2.w);
    int64_t W = int64_t((16777216 - d) >> 4);
    if (W < 1l) W = 1l;
    int64_t X = int64_t(2 * p.x + 1), Y = int64_t(2 * p.y + 1);
    int64_t su = (cu + au * X + bu * Y) >> 6, sv = (cv + av * X + bv * Y) >> 6;
    int64_t w = int64_t(uint(t3.y)), h = int64_t(uint(t3.z));
    int64_t u = fdiv(fdiv(su, W), 16l), v = fdiv(fdiv(sv, W), 16l);
    u -= fdiv(u, w) * w;   // повтор картинки: остаток всегда 0…w-1 (% с минусом в GLSL не определён)
    v -= fdiv(v, h) * h;
    uint tp = img.px[uint(t3.x) + uint(v) * uint(w) + uint(u)];
    if ((tp >> 24) < 128u) discard;
    uint c = vcm.x;
    uvec3 k = (uvec3((tp >> 16) & 255u, (tp >> 8) & 255u, tp & 255u) * uvec3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) + 127u) / 255u;
    gl_FragDepth = float(d) / 16777216.0;
    col = vec4(vec3(k), 255.0) / 255.0;
}
