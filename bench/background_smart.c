// То же на C с той же хитростью, что background: две работы в своих потоках (pthread).
#include <stdio.h>
#include <pthread.h>
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
typedef struct { long long lo, hi, res; } Job;
static void *run(void *p) { Job *j = p; j->res = primes(j->lo, j->hi); return 0; }
int main(void) {
    Job a = {1, 2000000, 0}, b = {2000001, 3300000, 0};
    pthread_t ta, tb;
    pthread_create(&ta, 0, run, &a);
    pthread_create(&tb, 0, run, &b);
    long long c = primes(3300001, 4500000);
    pthread_join(ta, 0); pthread_join(tb, 0);
    printf("%lld\n", a.res + b.res + c);
    return 0;
}
