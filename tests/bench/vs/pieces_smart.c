// C с той же хитростью, что by 4096: все 4 прохода по куску в 4096 чисел, пока он в кэше.
#include <stdio.h>
#include <stdlib.h>
#define PIECE 4096
int main(void) {
    long n = 20000000;
    double *p = malloc(sizeof(double) * n);
    for (long i = 0; i < n; i++) p[i] = (i % 1000) / 1000.0;
    double total = 0;
    for (int r = 0; r < 5; r++) {
        for (long a = 0; a < n; a += PIECE) {
            long b = a + PIECE < n ? a + PIECE : n;
            for (long i = a; i < b; i++) p[i] = p[i] * 0.5 + 1.0;
            for (long i = a; i < b; i++) p[i] = p[i] * p[i] - 0.25;
            for (long i = a; i < b; i++) p[i] = p[i] / 3.0 + 0.1;
            double s = 0;
            for (long i = a; i < b; i++) s += p[i];
            total += s;
        }
    }
    printf("%.6f\n", total);
    return 0;
}
