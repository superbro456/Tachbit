#version 450
// те же правила точек, что у рисования на процессоре (runtime.h):
// вид 0 — прямоугольник (m = 1 — треугольник, углы в vq и vo), 1 — круг i*i + j*j <= r*r + r, 2 — буква 8×16 из шрифта, size — масштаб,
// 3 — картинка: vcm.x — где её точки, ширина в m; точка видна, если прозрачность не меньше 128
layout(location = 0) flat in ivec2 vo;
layout(location = 1) flat in uvec2 vcm;
layout(location = 2) flat in ivec4 vq;
layout(set = 0, binding = 0) uniform Font { uvec4 f[161]; } font;
layout(std430, set = 0, binding = 1) readonly buffer Img { uint px[]; } img;   // все картинки подряд
layout(location = 0) out vec4 col;
// сторона a→b треугольника: то же правило, что tb_tri_edge в runtime.h
bool edge(ivec2 a, ivec2 b, ivec2 p) {
    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint m = vcm.y, k = m >> 30;
    if (m == 1u) {
        if (!edge(vq.xy, vq.zw, p) || !edge(vq.zw, vo, p) || !edge(vo, vq.xy, p)) discard;
    } else if (k == 1u) {
        int r = int(m & 0x3FFFFFu), i = p.x - vo.x, j = p.y - vo.y;
        if (i * i + j * j > r * r + r) discard;
    } else if (k == 2u) {
        int s = int(m & 0x3FFFFFu), cx = (p.x - vo.x) / s, ry = (p.y - vo.y) / s;
        uint g = (m >> 22) & 255u;
        uint b = (font.f[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;
        if ((b & (0x80u >> uint(cx))) == 0u) discard;
    }
    uint c = vcm.x;
    if (k == 3u) {
        uint v = img.px[vcm.x + uint(p.y - vo.y) * (m & 0x3FFFFFu) + uint(p.x - vo.x)];
        if ((v >> 24) < 128u) discard;
        c = v;
    }
    col = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
}
