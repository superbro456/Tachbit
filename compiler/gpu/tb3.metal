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
// «новый» треугольник 3D (свет по углам, тени, туман, прозрачность): то же, что tbl.frag и tb_lit_px в runtime.h
static long tb_ledge(int2 a, int2 b, int2 p) {
    return long(b.x - a.x) * long(2 * p.y + 1 - 2 * a.y) - long(b.y - a.y) * long(2 * p.x + 1 - 2 * a.x);
}
fragment TbOut tb_fragl(TbV v [[stage_in]], const device uint *img [[buffer(1)]], const device uint4 *pl [[buffer(2)]],
                        const device int4 *tex [[buffer(5)]], const device int *lit [[buffer(6)]], const device uint *sm [[buffer(7)]]) {
    int2 p = int2(v.pos.xy);
    int2 A = v.q.xy, B = v.q.zw, C = v.o;
    if (!tb_edge(A, B, p) || !tb_edge(B, C, p) || !tb_edge(C, A, p)) discard_fragment();
    uint4 P = pl[(v.cm.y & 0x07FFFFFFu) >> 2];
    uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);
    int d = clamp(as_type<int>(kk) >> 6, 0, 16777215);
    int R = int(P.w) * 48;
    int flags = lit[R + 3];
    uint base = v.cm.x;
    int ti = lit[R + 43];
    if (ti >= 0) {
        int r = ti * 4;
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
        base = ((((tp >> 16) & 255u) * ((base >> 16) & 255u) + 127u) / 255u) << 16 | ((((tp >> 8) & 255u) * ((base >> 8) & 255u) + 127u) / 255u) << 8
             | (((tp & 255u) * (base & 255u) + 127u) / 255u);
    }
    long e0 = tb_ledge(B, C, p), e1 = tb_ledge(C, A, p), e2 = tb_ledge(A, B, p);
    long dn = e0 * long(lit[R]) + e1 * long(lit[R + 1]) + e2 * long(lit[R + 2]);
    if (dn < 1) dn = 1;
    long val[11];
    int nv = (flags & 1) != 0 ? 11 : 8;
    for (int k = 0; k < nv; k++)
        val[k] = tb_fdiv(e0 * long(lit[R + 4 + k]) + e1 * long(lit[R + 16 + k]) + e2 * long(lit[R + 28 + k]), dn);
    long sh = 256;
    if ((flags & 1) != 0) {
        long N = long(lit[R + 44]), lx = val[8] - 8, ly = val[9] - 8, ld = val[10];
        long ix = lx >> 4, fx = lx & 15, iy = ly >> 4, fy = ly & 15;
        sh = 0;
        for (int k = 0; k < 4; k++) {
            long i = ix + long(k & 1), j = iy + long(k >> 1);
            long w = ((k & 1) != 0 ? fx : 16 - fx) * ((k >> 1) != 0 ? fy : 16 - fy);
            if (i < 0 || j < 0 || i >= N || j >= N || ld <= long(sm[uint(long(lit[R + 45]) + j * N + i)])) sh += w;
        }
    }
    long ft = 0;
    if ((flags & 2) != 0) {
        long zq = tb_fdiv(long(205) << 24, 16777216 - long(d)), fn = long(lit[R + 41]), ff = long(lit[R + 42]);
        ft = zq <= fn ? 0 : zq >= ff ? 256 : (zq - fn) * 256 / (ff - fn);
    }
    uint a = uint(flags >> 8) & 255u, fc = uint(lit[R + 40]);
    uint3 o;
    for (int k = 0; k < 3; k++) {
        int s = 16 - 8 * k;
        long cb = long((base >> uint(s)) & 255u), M = val[k] + ((val[6] * sh) >> 8), Ad = val[3 + k] + ((val[7] * sh) >> 8);
        long r = ((cb * M + 128) >> 8) + ((Ad + 8) >> 4);
        if (r > 255) r = 255;
        if ((flags & 2) != 0) r = (r * (256 - ft) + long((fc >> uint(s)) & 255u) * ft + 128) >> 8;
        o[k] = uint(r);
    }
    uint3 k3 = (o * a + 127u) / 255u;
    TbOut out;
    out.c = float4(float3(k3), float(a)) / 255.0;
    out.d = float(d) / 16777216.0;
    return out;
}
