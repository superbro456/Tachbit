#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// C с той же хитростью: у функции своя арена — память подряд, отдаётся разом при выходе
static char arena[4096]; static long long used;
static void *take(long long n) { n = (n + 15) & ~15LL; void *p = arena + used; used += n; return p; }
static long long score(long long i) {
    long long mark = used;
    char *line = take(64);
    snprintf(line, 64, "user%lld,city%lld,45.10", i, i % 100);
    char *parts[8]; int n = 0; char *p = line;
    for (;;) {
        char *c = strchr(p, ',');
        long long len = c ? c - p : (long long)strlen(p);
        parts[n] = take(len + 1); memcpy(parts[n], p, len); parts[n][len] = 0; n++;
        if (!c) break;
        p = c + 1;
    }
    long long l0 = strlen(parts[0]);
    char *up = take(l0 + 1);
    for (long long k = 0; k <= l0; k++) up[k] = toupper((unsigned char)parts[0][k]);
    long long r = strlen(up) + n;
    used = mark;
    return r;
}
int main(void) {
    long long cap = 1024, len = 0, *all = malloc(cap * 8);
    for (long long i = 0; i < 3000000; i++) {
        if (len == cap) { cap *= 2; all = realloc(all, cap * 8); }
        all[len++] = score(i);
    }
    long long total = 0;
    for (long long i = 0; i < len; i++) total += all[i];
    printf("%lld [n0, n1000000, n2000000]\n", total);
    return 0;
}
