// решето битами: то же на C, но list bool руками сложен в биты (64 в одном слове)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    long long limit = 20000000, found = 0;
    unsigned long long *b = malloc((limit / 64 + 1) * 8);
    memset(b, 0xff, (limit / 64 + 1) * 8);
    for (long long i = 2; i <= limit; i++) {
        if (b[i >> 6] >> (i & 63) & 1) {
            found++;
            for (long long j = i * i; j <= limit; j += i) b[j >> 6] &= ~(1ULL << (j & 63));
        }
    }
    printf("%lld\n", found);
}
