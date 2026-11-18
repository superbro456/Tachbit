#version 450
// Tachbit: одна фигура кадра = один «экземпляр» (instance). Вершины — 4 угла прямоугольника.
// q = x0, y0, x1, y1 (уже обрезан окном, в точках), o = начало фигуры, cm = цвет и вид
layout(location = 0) in ivec4 q;
layout(location = 1) in ivec2 o;
layout(location = 2) in uvec2 cm;
layout(push_constant) uniform P { vec2 size; } pc;
layout(location = 0) flat out ivec2 vo;
layout(location = 1) flat out uvec2 vcm;
void main() {
    int v = gl_VertexIndex;
    vec2 p = vec2((v & 1) != 0 ? q.z : q.x, (v & 2) != 0 ? q.w : q.y);
    gl_Position = vec4(p / pc.size * 2.0 - 1.0, 0.0, 1.0);
    vo = o;
    vcm = cm;
}
