// C: тот же текст, обычный цикл по байтам
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    long long cap = 1 << 26, len = 0; char *s = malloc(cap);
    for (long long i = 0; i < 1000000; i++) {
        if (len + 64 > cap) { cap *= 2; s = realloc(s, cap); }
        len += sprintf(s + len, "%s%lld,client%lld,45.10,%lld,Бишкек", i ? "\n" : "", i, i % 99999, i % 30);
    }
    long long n = 0;
    for (int r = 0; r < 10; r++) for (char *p = s; *p; p++) if (*p == ',') n++;
    printf("%lld\n", n);
    return 0;
}
