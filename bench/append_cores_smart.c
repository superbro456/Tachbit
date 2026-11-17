// То же на C с той же хитростью: потоки на все ядра, у каждого свой буфер,
// в конце буферы кладутся в общий по очереди.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#define N 4000000LL
#define CHUNK 4096
typedef struct { char *buf; long long len, cap; char pad[40]; } Buf;   // pad: у каждого потока своя линия кэша
static long long next_i = 0;
static void app(Buf *b, const char *s, long long n) {
    if (b->len + n + 1 > b->cap) { b->cap = 2 * (b->len + n + 1); b->buf = realloc(b->buf, b->cap); }
    memcpy(b->buf + b->len, s, n); b->len += n;
}
// числа в текст своей функцией (snprintf медленный и мешает потокам)
static int itoa10(long long x, char *out) {
    char t[24]; int n = 0;
    if (x == 0) t[n++] = '0';
    while (x > 0) { t[n++] = '0' + x % 10; x /= 10; }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    return n;
}
static void *work(void *arg) {
    Buf *b = arg; char num[32];
    for (;;) {
        long long lo = __atomic_fetch_add(&next_i, CHUNK, __ATOMIC_RELAXED);
        if (lo >= N) break;
        long long hi = lo + CHUNK < N ? lo + CHUNK : N;
        for (long long i = lo; i < hi; i++) {
            long long x = i * i % 1000003;
            app(b, "line ", 5); app(b, num, itoa10(i, num));
            app(b, ",value", 6); app(b, num, itoa10(x, num)); app(b, "\n", 1);
        }
    }
    return 0;
}
int main(void) {
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    if (cores > 64) cores = 64;
    pthread_t th[64]; Buf b[64]; memset(b, 0, sizeof b);
    for (int i = 0; i < cores; i++) pthread_create(&th[i], 0, work, &b[i]);
    Buf out = {0};
    for (int i = 0; i < cores; i++) { pthread_join(th[i], 0); app(&out, b[i].buf ? b[i].buf : "", b[i].len); }
    printf("%lld\n", out.len);
    return 0;
}
