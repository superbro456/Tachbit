// C с той же хитростью, что together: сначала просим память принести ячейки всех 8 ключей
// (__builtin_prefetch), потом ищем — пока ждём один, остальные уже в пути.
#include <stdio.h>
#include <stdlib.h>
static long long *keys, *vals, *slots; static unsigned long long mask;
static unsigned long long h(long long k) { unsigned long long x = 1469598103934665603ULL; x ^= (unsigned long long)k; x *= 0x9E3779B97F4A7C15ULL; return x ^ (x >> 32); }
static long long get(long long k) {
    unsigned long long i = h(k) & mask;
    while (slots[i]) { long long at = slots[i] - 1; if (keys[at] == k) return vals[at]; i = (i + 1) & mask; }
    return 0;
}
int main(void) {
    long long n = 4000000, ns = 8;
    while (ns < n * 2) ns *= 2;
    mask = ns - 1;
    keys = malloc(8 * n); vals = malloc(8 * n); slots = calloc(ns, 8);
    for (long long i = 0; i < n; i++) {
        keys[i] = i; vals[i] = i % 1000;
        unsigned long long j = h(i) & mask;
        while (slots[j]) j = (j + 1) & mask;
        slots[j] = i + 1;
    }
    long long seed = 12345, total = 0;
    for (long long r = 0; r < 2000000; r++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        long long k[8] = { seed % n, (seed / 7) % n, (seed / 13) % n, (seed / 29) % n,
                           (seed / 61) % n, (seed / 127) % n, (seed / 251) % n, (seed / 509) % n };
        for (int t = 0; t < 8; t++) __builtin_prefetch(&slots[h(k[t]) & mask]);
        for (int t = 0; t < 8; t++) __builtin_prefetch(&keys[k[t]]), __builtin_prefetch(&vals[k[t]]);
        for (int t = 0; t < 8; t++) total += get(k[t]);
    }
    printf("%lld\n", total);
    return 0;
}
