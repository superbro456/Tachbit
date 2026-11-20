// То же на C: 10 000 шариков, 200 кадров, сетка и столкновения
#include <stdio.h>
#include <stdlib.h>
#define N 10000
#define G 125
static double xs[N], ys[N], vxs[N], vys[N];
static long long counts[G * G], starts[G * G + 1], order[N], cells[N];
static long long rnd(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }
static long long cell_of(double v) { long long c = rnd(v / 8.0 - 0.5); return c < 0 ? 0 : c > 124 ? 124 : c; }
int main(void) {
    long long seed = 42;
    for (int i = 0; i < N; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL; xs[i] = seed % 1000;
        seed = (seed * 1103515245 + 12345) % 2147483648LL; ys[i] = seed % 1000;
        seed = (seed * 1103515245 + 12345) % 2147483648LL; vxs[i] = (seed % 200 - 100) / 50.0;
        seed = (seed * 1103515245 + 12345) % 2147483648LL; vys[i] = (seed % 200 - 100) / 50.0;
    }
    long long hits = 0;
    for (int f = 0; f < 200; f++) {
        for (int i = 0; i < N; i++) {
            vys[i] += 0.05; xs[i] += vxs[i]; ys[i] += vys[i];
            if (xs[i] < 0.0) { xs[i] = -xs[i]; vxs[i] = -vxs[i]; }
            if (xs[i] > 999.0) { xs[i] = 1998.0 - xs[i]; vxs[i] = -vxs[i]; }
            if (ys[i] < 0.0) { ys[i] = -ys[i]; vys[i] = -vys[i]; }
            if (ys[i] > 999.0) { ys[i] = 1998.0 - ys[i]; vys[i] = -vys[i] * 0.9; }
        }
        for (int k = 0; k < G * G; k++) counts[k] = 0;
        for (int i = 0; i < N; i++) { long long c = cell_of(xs[i]) * G + cell_of(ys[i]); cells[i] = c; counts[c]++; }
        starts[0] = 0;
        for (int k = 0; k < G * G; k++) { starts[k + 1] = starts[k] + counts[k]; counts[k] = starts[k]; }
        for (int i = 0; i < N; i++) order[counts[cells[i]]++] = i;
        for (int i = 0; i < N; i++) {
            long long cx = cells[i] / G, cy = cells[i] % G;
            for (long long ax = cx > 0 ? cx - 1 : 0; ax <= (cx + 1 < G - 1 ? cx + 1 : G - 1); ax++)
                for (long long ay = cy > 0 ? cy - 1 : 0; ay <= (cy + 1 < G - 1 ? cy + 1 : G - 1); ay++) {
                    long long k = ax * G + ay;
                    for (long long p = starts[k]; p < starts[k + 1]; p++) {
                        long long j = order[p];
                        if (j > i) {
                            double dx = xs[i] - xs[j], dy = ys[i] - ys[j];
                            if (dx * dx + dy * dy < 16.0) {
                                hits++;
                                double t = vxs[i]; vxs[i] = vxs[j]; vxs[j] = t;
                                t = vys[i]; vys[i] = vys[j]; vys[j] = t;
                            }
                        }
                    }
                }
        }
    }
    double sum = 0;
    for (int i = 0; i < N; i++) sum += xs[i] + ys[i];
    printf("столкновений: %lld  сумма: %lld\n", hits, rnd(sum));
    return 0;
}
