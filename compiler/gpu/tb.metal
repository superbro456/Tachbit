#include <metal_stdlib>
using namespace metal;
// то же, что tb.vert и tb.frag (Vulkan), только для Metal на Mac
struct TbInst { int4 q; int2 o; uint2 cm; };
struct TbV { float4 pos [[position]]; int2 o [[flat]]; uint2 cm [[flat]]; };
vertex TbV tb_vert(uint vid [[vertex_id]], uint iid [[instance_id]],
                   const device TbInst *ins [[buffer(0)]], constant float2 &size [[buffer(1)]]) {
    TbInst t = ins[iid];
    float2 p = float2((vid & 1) ? t.q.z : t.q.x, (vid & 2) ? t.q.w : t.q.y);
    TbV out;
    out.pos = float4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
    out.o = t.o;
    out.cm = t.cm;
    return out;
}
fragment float4 tb_frag(TbV v [[stage_in]], constant uint4 *font [[buffer(0)]]) {
    int2 p = int2(v.pos.xy);
    uint m = v.cm.y, k = m >> 30;
    if (k == 1u) {
        int r = int(m & 0x3FFFFFu), i = p.x - v.o.x, j = p.y - v.o.y;
        if (i * i + j * j > r * r + r) discard_fragment();
    } else if (k == 2u) {
        int s = int(m & 0x3FFFFFu), cx = (p.x - v.o.x) / s, ry = (p.y - v.o.y) / s;
        uint g = (m >> 22) & 255u;
        uint b = (font[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;
        if ((b & (0x80u >> uint(cx))) == 0u) discard_fragment();
    }
    uint c = v.cm.x;
    return float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
}
