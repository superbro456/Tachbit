// То же на обычном C: malloc на каждую строку и free в конце раунда.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    long long total = 0;
    for (int r = 0; r < 100; r++) {
        long long n = 100000;
        char **lines = malloc(sizeof(char *) * n);
        for (long long i = 0; i < n; i++) {
            char buf[64];
            int len = snprintf(buf, sizeof buf, "строка %lld", i + 1);
            lines[i] = malloc(len + 1);
            memcpy(lines[i], buf, len + 1);
        }
        for (long long i = 0; i < n; i++) total += (long long)strlen(lines[i]);
        for (long long i = 0; i < n; i++) free(lines[i]);
        free(lines);
    }
    printf("%lld\n", total);
    return 0;
}
