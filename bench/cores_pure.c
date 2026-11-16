// То же на обычном C: одно ядро.
#include <stdio.h>
static int is_prime(long long n) {
    if (n < 2) return 0;
    for (long long d = 2; d * d <= n; d++) {
        if (n % d == 0) return 0;
    }
    return 1;
}
int main(void) {
    long long found = 0;
    for (long long n = 1; n <= 5000000; n++) {
        if (is_prime(n)) found++;
    }
    printf("%lld\n", found);
    return 0;
}
