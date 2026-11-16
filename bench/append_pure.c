#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// то же на C: свой буфер с запасом, realloc при нехватке
static char *buf; static long long len, cap;
static void app(const char *s) {
    long long n = strlen(s);
    if (len + n + 1 > cap) { cap = 2 * (len + n + 1); buf = realloc(buf, cap); }
    memcpy(buf + len, s, n + 1); len += n;
}
int main(void) {
    char num[32];
    buf = malloc(64); cap = 64; buf[0] = 0;
    for (long long i = 0; i < 2000000; i++) {
        app("line "); snprintf(num, 32, "%lld", i); app(num);
        app(",client"); snprintf(num, 32, "%lld", i % 997); app(num); app("\n");
    }
    printf("%lld\n", len);
    return 0;
}
