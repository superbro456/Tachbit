#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long long limit = 20000000, found = 0;
    char *sieve = malloc(limit + 1);
    for (long long i = 0; i <= limit; i++) sieve[i] = 1;
    for (long long i = 2; i <= limit; i++) {
        if (sieve[i]) {
            found++;
            for (long long j = i * i; j <= limit; j += i) sieve[j] = 0;
        }
    }
    printf("%lld\n", found);
    return 0;
}
