// То же на C с той же хитростью, что on all cores: потоки на все ядра,
// числа раздаются кусками через общий счётчик (atomic), у каждого потока своя сумма.
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#define N 5000000LL
#define CHUNK 4096
static long long next_n = 1;
static int is_prime(long long n) {
    if (n < 2) return 0;
    for (long long d = 2; d * d <= n; d++) {
        if (n % d == 0) return 0;
    }
    return 1;
}
static void *work(void *arg) {
    long long *found = arg;
    for (;;) {
        long long lo = __atomic_fetch_add(&next_n, CHUNK, __ATOMIC_RELAXED);
        if (lo > N) break;
        long long hi = lo + CHUNK - 1 < N ? lo + CHUNK - 1 : N;
        for (long long n = lo; n <= hi; n++) if (is_prime(n)) (*found)++;
    }
    return 0;
}
int main(void) {
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    if (cores > 64) cores = 64;
    pthread_t th[64]; long long found[64] = {0};
    for (int i = 0; i < cores; i++) pthread_create(&th[i], 0, work, &found[i]);
    long long total = 0;
    for (int i = 0; i < cores; i++) { pthread_join(th[i], 0); total += found[i]; }
    printf("%lld\n", total);
    return 0;
}
