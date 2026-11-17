#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// C с той же хитростью: своя арена на шаг — память берётся подряд, в конце шага отдаётся разом
static char arena[4096]; static long long used;
static void *take(long long n) { n = (n + 15) & ~15LL; void *p = arena + used; used += n; return p; }
int main(void) {
    long long total = 0;
    for (long long i = 0; i < 3000000; i++) {
        used = 0;
        char *line = take(64);
        snprintf(line, 64, "user%lld,city%lld,45.10", i, i % 100);
        char *parts[8]; int n = 0;
        char *p = line;
        for (;;) {
            char *c = strchr(p, ',');
            long long len = c ? c - p : (long long)strlen(p);
            parts[n] = take(len + 1); memcpy(parts[n], p, len); parts[n][len] = 0; n++;
            if (!c) break;
            p = c + 1;
        }
        long long l0 = strlen(parts[0]);
        char *up = take(l0 + 2);
        for (long long k = 0; k < l0; k++) up[k] = toupper((unsigned char)parts[0][k]);
        up[l0] = '!'; up[l0 + 1] = 0;
        total += strlen(up) + n;
    }
    printf("%lld\n", total);
    return 0;
}
