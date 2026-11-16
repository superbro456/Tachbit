#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// то же на C: каждый шаг malloc для строки, кусков и большой копии, в конце шага free
int main(void) {
    long long total = 0;
    for (long long i = 0; i < 3000000; i++) {
        char *line = malloc(64);
        snprintf(line, 64, "user%lld,city%lld,45.10", i, i % 100);
        char *parts[8]; int n = 0;
        char *p = line;
        for (;;) {
            char *c = strchr(p, ',');
            long long len = c ? c - p : (long long)strlen(p);
            parts[n] = malloc(len + 1); memcpy(parts[n], p, len); parts[n][len] = 0; n++;
            if (!c) break;
            p = c + 1;
        }
        long long l0 = strlen(parts[0]);
        char *up = malloc(l0 + 2);
        for (long long k = 0; k < l0; k++) up[k] = toupper((unsigned char)parts[0][k]);
        up[l0] = '!'; up[l0 + 1] = 0;
        total += strlen(up) + n;
        free(up);
        for (int k = 0; k < n; k++) free(parts[k]);
        free(line);
    }
    printf("%lld\n", total);
    return 0;
}
