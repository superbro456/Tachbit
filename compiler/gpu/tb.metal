#include <metal_stdlib>
using namespace metal;
// то же, что tb.vert и tb.frag (Vulkan), только для Metal на Mac
struct TbInst { int4 q; int2 o; uint2 cm; };
struct TbV { float4 pos [[position]]; int2 o [[flat]]; uint2 cm [[flat]]; int4 q [[flat]]; };
vertex TbV tb_vert(uint vid [[vertex_id]], uint iid [[instance_id]],
                   const device TbInst *ins [[buffer(0)]], constant float2 &size [[buffer(1)]]) {
    TbInst t = ins[iid];
    int4 b = t.q;
    if (t.cm.y == 1u) {   // треугольник: квадрат вокруг трёх углов, внутри решает tb_frag
        b.xy = min(min(t.q.xy, t.q.zw), t.o);
        b.zw = max(max(t.q.xy, t.q.zw), t.o) + 1;
        b = clamp(b, int4(0), int4(int(size.x), int(size.y), int(size.x), int(size.y)));
    }
    float2 p = float2((vid & 1) ? b.z : b.x, (vid & 2) ? b.w : b.y);
    TbV out;
    out.pos = float4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
    out.o = t.o;
    out.cm = t.cm;
    out.q = t.q;
    return out;
}
// сторона a→b треугольника: то же правило, что tb_tri_edge в runtime.h
static bool tb_edge(int2 a, int2 b, int2 p) {
    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
fragment float4 tb_frag(TbV v [[stage_in]], constant uint4 *font [[buffer(0)]], const device uint *img [[buffer(1)]]) {
    int2 p = int2(v.pos.xy);
    uint m = v.cm.y, k = m >> 30;
    if (m == 1u) {
        if (!tb_edge(v.q.xy, v.q.zw, p) || !tb_edge(v.q.zw, v.o, p) || !tb_edge(v.o, v.q.xy, p)) discard_fragment();
    } else if (k == 1u) {
        int r = int(m & 0x3FFFFFu), i = p.x - v.o.x, j = p.y - v.o.y;
        if (i * i + j * j > r * r + r) discard_fragment();
    } else if (k == 2u) {
        int s = int(m & 0x3FFFFFu), cx = (p.x - v.o.x) / s, ry = (p.y - v.o.y) / s;
        uint g = (m >> 22) & 255u;
        uint b = (font[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;
        if ((b & (0x80u >> uint(cx))) == 0u) discard_fragment();
    }
    uint c = v.cm.x;
    if (k == 3u) {
        uint px = img[v.cm.x + uint(p.y - v.o.y) * (m & 0x3FFFFFu) + uint(p.x - v.o.x)];
        if ((px >> 24) < 128u) discard_fragment();
        c = px;
    }
    return float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
}
