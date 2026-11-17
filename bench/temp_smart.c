// То же на C с той же хитростью, что temp: своя арена — строки берутся подряд
// из одного куска, в конце раунда кусок отдаётся разом (без free на каждую строку).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *arena; static long long used, cap;
static void *take(long long n) {
    n = (n + 15) & ~15LL;
    if (used + n > cap) { fprintf(stderr, "арена мала\n"); exit(1); }
    void *p = arena + used; used += n; return p;
}
int main(void) {
    cap = 16 << 20; arena = malloc(cap);
    long long total = 0;
    for (int r = 0; r < 100; r++) {
        used = 0;
        long long n = 100000;
        char **lines = take(sizeof(char *) * n);
        for (long long i = 0; i < n; i++) {
            char buf[64];
            int len = snprintf(buf, sizeof buf, "строка %lld", i + 1);
            lines[i] = take(len + 1);
            memcpy(lines[i], buf, len + 1);
        }
        for (long long i = 0; i < n; i++) total += (long long)strlen(lines[i]);
    }
    printf("%lld\n", total);
    return 0;
}
