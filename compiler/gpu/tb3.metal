// 3D на видеокарте (Mac): то же, что tb3.vert — целые числа, та же формула, что tb_m3_* в runtime.h.
// Собирается отдельно, вместе с tb.metal: не собралось — сетки считает процессор, как раньше
struct TbL3 { long x, y, z; };   // три целых по 64 бита
struct TbPC { float2 size; int tbase, ntri, rbase, f, gw, gh, cull; };
static long tb_fdiv(long a, long b) {   // деление с округлением вниз (b > 0)
    if (a >= 0) return long(ulong(a) / ulong(b));
    return -long((ulong(-a) + ulong(b) - 1ul) / ulong(b));
}
static TbL3 tb_corner(int4 p, const device int4 *rec, int ri, constant TbPC &pc) {
    long c[3];
    for (int k = 0; k < 3; k++) {
        int4 m = rec[ri + k];
        c[k] = ((long(m.x) * long(p.x) + long(m.y) * long(p.y) + long(m.z) * long(p.z)) >> 30) + long(m.w);
    }
    long f = long(pc.f);
    TbL3 o;
    o.x = (long(pc.gw) * 128 + tb_fdiv(f * c[0], c[2]) + 128) >> 8;
    o.y = (long(pc.gh) * 128 - tb_fdiv(f * c[1], c[2]) + 128) >> 8;
    o.z = 16777216 - tb_fdiv(long(205) << 24, c[2]);
    return o;
}
vertex TbV tb_vert3(uint vid [[vertex_id]], uint iid [[instance_id]], constant TbPC &pc [[buffer(1)]],
                    const device int4 *mesh [[buffer(3)]], const device int4 *rec [[buffer(4)]]) {
    int ii = int(iid), d = ii / pc.ntri, t = ii - d * pc.ntri;
    int4 tr = mesh[pc.tbase + 2 * t], nr = mesh[pc.tbase + 2 * t + 1];
    int ri = pc.rbase + d * 4;
    TbL3 a = tb_corner(mesh[tr.x], rec, ri, pc), b = tb_corner(mesh[tr.y], rec, ri, pc), c = tb_corner(mesh[tr.z], rec, ri, pc);
    long area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    bool skip = area == 0 || (pc.cull > 0 && area > 0) || (pc.cull < 0 && area < 0);
    // свет (tb_shade): грань к солнцу ярче; со спины — от обратной стороны
    uint col = uint(tr.w) & 0xFFFFFFu;
    int4 L = rec[ri + 3];
    if (L.w >= 0) {
        int s = nr.x * L.x + nr.y * L.y + nr.z * L.z;
        if (area < 0) s = -s;
        if (s < 0) s = 0;
        uint k = uint(L.w + (((256 - L.w) * (s >> 14)) >> 14));
        col = ((((col >> 16) & 255u) * k + 128u) >> 8) << 16 | ((((col >> 8) & 255u) * k + 128u) >> 8) << 8 | (((col & 255u) * k + 128u) >> 8);
    }
    if (area < 0) { TbL3 k = b; b = c; c = k; area = -area; }
    if (skip) area = 1;
    long e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y, d1 = b.z - a.z, d2 = c.z - a.z;
    long A = tb_fdiv(64 * (d1 * e2y - d2 * e1y) + area, 2 * area), B = tb_fdiv(64 * (d2 * e1x - d1 * e2x) + area, 2 * area);
    long C = 64 * a.z - 2 * A * a.x - 2 * B * a.y;
    int4 q = int4(int(a.x), int(a.y), int(b.x), int(b.y));
    int2 o = int2(int(c.x), int(c.y));
    int4 bb;
    bb.xy = min(min(q.xy, q.zw), o);
    bb.zw = max(max(q.xy, q.zw), o) + 1;
    bb = clamp(bb, int4(0), int4(int(pc.size.x), int(pc.size.y), int(pc.size.x), int(pc.size.y)));
    if (skip) bb = int4(0);
    float2 p = float2((vid & 1) ? bb.z : bb.x, (vid & 2) ? bb.w : bb.y);
    TbV out;
    out.pos = float4(p.x / pc.size.x * 2.0 - 1.0, 1.0 - p.y / pc.size.y * 2.0, 0.0, 1.0);
    out.o = o;
    out.cm = uint2(col, 3u);
    out.q = q;
    out.p = uint4(uint(ulong(A) & 0xFFFFFFFFul), uint(ulong(B) & 0xFFFFFFFFul), uint(ulong(C) & 0xFFFFFFFFul), 0u);
    return out;
}
// треугольник сетки с картинкой (mesh_image): то же, что tbt.frag — W = (2^24 - глубина) >> 4, u = (плоскость u·W) / W
static long tb_i64(int lo, int hi) { return (long(hi) << 32) | long(uint(lo)); }
fragment TbOut tb_fragt(TbV v [[stage_in]], const device uint *img [[buffer(1)]], const device uint4 *pl [[buffer(2)]],
                        const device int4 *tex [[buffer(5)]]) {
    int2 p = int2(v.pos.xy);
    if (!tb_edge(v.q.xy, v.q.zw, p) || !tb_edge(v.q.zw, v.o, p) || !tb_edge(v.o, v.q.xy, p)) discard_fragment();
    uint4 P = pl[(v.cm.y & ~(1u << 29)) >> 2];
    uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);
    int d = clamp(as_type<int>(kk) >> 6, 0, 16777215);
    int r = int(P.w) * 4;
    int4 t0 = tex[r], t1 = tex[r + 1], t2 = tex[r + 2], t3 = tex[r + 3];
    long au = tb_i64(t0.x, t0.y), bu = tb_i64(t0.z, t0.w), cu = tb_i64(t1.x, t1.y), av = tb_i64(t1.z, t1.w), bv = tb_i64(t2.x, t2.y), cv = tb_i64(t2.z, t2.w);
    long W = long((16777216 - d) >> 4);
    if (W < 1) W = 1;
    long X = long(2 * p.x + 1), Y = long(2 * p.y + 1);
    long su = (cu + au * X + bu * Y) >> 6, sv = (cv + av * X + bv * Y) >> 6;
    long w = long(uint(t3.y)), h = long(uint(t3.z));
    long u = tb_fdiv(tb_fdiv(su, W), 16), vv = tb_fdiv(tb_fdiv(sv, W), 16);
    u -= tb_fdiv(u, w) * w;
    vv -= tb_fdiv(vv, h) * h;
    uint tp = img[uint(t3.x) + uint(vv) * uint(w) + uint(u)];
    if ((tp >> 24) < 128u) discard_fragment();
    uint c = v.cm.x;
    uint3 k = (uint3((tp >> 16) & 255u, (tp >> 8) & 255u, tp & 255u) * uint3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) + 127u) / 255u;
    TbOut o;
    o.c = float4(float3(k), 255.0) / 255.0;
    o.d = float(d) / 16777216.0;
    return o;
}
