#version 450
// те же правила точек, что у рисования на процессоре (runtime.h):
// вид 0 — прямоугольник (m = 1 — треугольник, углы в vq и vo), 1 — круг i*i + j*j <= r*r + r, 2 — буква 8×16 из шрифта, size — масштаб,
// 3 — картинка: vcm.x — где её точки, ширина в m; точка видна, если прозрачность не меньше 128;
// 3 с битом 29 — буква шрифта: точка смешивается с фоном (a/255), как tb_draw_image_a
layout(location = 0) flat in ivec2 vo;
layout(location = 1) flat in uvec2 vcm;
layout(location = 2) flat in ivec4 vq;
layout(location = 3) flat in uvec4 vp;
layout(set = 0, binding = 0) uniform Font { uvec4 f[161]; } font;
layout(std430, set = 0, binding = 1) readonly buffer Img { uint px[]; } img;   // все картинки подряд
layout(std430, set = 0, binding = 2) readonly buffer Pl { uvec4 pl[]; } planes;  // плоскости глубины 3D
layout(location = 0) out vec4 col;
// сторона a→b треугольника: то же правило, что tb_tri_edge в runtime.h
bool edge(ivec2 a, ivec2 b, ivec2 p) {
    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint m = vcm.y, k = m >> 30;
    float depth = 0.0;
    if (m != 0u && k == 0u) {   // треугольник: m = 1 — плоский, m = 2 | (место << 2) — из сетки (3D), m = 3 — сетка с видеокарты (tb3.vert)
        if (!edge(vq.xy, vq.zw, p) || !edge(vq.zw, vo, p) || !edge(vo, vq.xy, p)) discard;
        if ((m & 2u) != 0u) {   // глубина: та же целая формула, что tb_tri3_depth в runtime.h
            uvec4 P = m == 3u ? vp : planes.pl[m >> 2];
            uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);
            depth = float(clamp(int(kk) >> 6, 0, 16777215)) / 16777216.0;
        }
    } else if (k == 1u) {
        int r = int(m & 0x3FFFFFu), i = p.x - vo.x, j = p.y - vo.y;
        if (i * i + j * j > r * r + r) discard;
    } else if (k == 2u) {
        int s = int(m & 0x3FFFFFu), cx = (p.x - vo.x) / s, ry = (p.y - vo.y) / s;
        uint g = (m >> 22) & 255u;
        uint b = (font.f[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;
        if ((b & (0x80u >> uint(cx))) == 0u) discard;
    }
    uint c = vcm.x, a = 255u;
    if (k == 3u) {
        uint v = img.px[vcm.x + uint(p.y - vo.y) * (m & 0x3FFFFFu) + uint(p.x - vo.x)];
        a = v >> 24;
        if ((m & (1u << 29)) != 0u) { if (a == 0u) discard; }   // буква шрифта: смешать с фоном по прозрачности a
        else { if (a < 128u) discard; a = 255u; }
        c = v;
    }
    gl_FragDepth = depth;
    // смешивание ONE, ONE_MINUS_SRC_ALPHA: свою часть цвета (c * a / 255) считаем здесь целыми числами,
    // тогда на любой видеокарте точка = своя часть + округлённая часть фона, как в tb_draw_image_a
    uvec3 kk = (uvec3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) * a + 127u) / 255u;
    col = vec4(vec3(kk), float(a)) / 255.0;
}
