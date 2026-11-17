// C с той же хитростью: заказ наперёд вручную — просим data[idx[i + 16]] заранее.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long long n = 16000000;
    long long *data = malloc(8 * n), *idx = malloc(8 * n);
    for (long long i = 0; i < n; i++) data[i] = i % 1000;
    long long seed = 12345;
    for (long long i = 0; i < n; i++) { seed = (seed * 1103515245 + 12345) % 2147483648LL; idx[i] = seed % n; }
    long long total = 0;
    for (int r = 0; r < 3; r++)
        for (long long i = 0; i < n; i++) {
            if (i + 16 < n) __builtin_prefetch(&data[idx[i + 16]]);
            total += data[idx[i]];
        }
    printf("%lld\n", total);
    return 0;
}
