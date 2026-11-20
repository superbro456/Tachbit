// То же на обычном C: три работы по очереди, одно ядро.
#include <stdio.h>
static int is_prime(long long n) {
    if (n < 2) return 0;
    for (long long d = 2; d * d <= n; d++) if (n % d == 0) return 0;
    return 1;
}
static long long primes(long long lo, long long hi) {
    long long found = 0;
    for (long long n = lo; n <= hi; n++) if (is_prime(n)) found++;
    return found;
}
int main(void) {
    long long a = primes(1, 2000000), b = primes(2000001, 3300000), c = primes(3300001, 4500000);
    printf("%lld\n", a + b + c);
    return 0;
}
