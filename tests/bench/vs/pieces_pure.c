// C, как пишут обычно: 4 прохода по всему массиву (160 МБ), 5 раз
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long n = 20000000;
    double *p = malloc(sizeof(double) * n);
    for (long i = 0; i < n; i++) p[i] = (i % 1000) / 1000.0;
    double total = 0;
    for (int r = 0; r < 5; r++) {
        for (long i = 0; i < n; i++) p[i] = p[i] * 0.5 + 1.0;
        for (long i = 0; i < n; i++) p[i] = p[i] * p[i] - 0.25;
        for (long i = 0; i < n; i++) p[i] = p[i] / 3.0 + 0.1;
        double s = 0;
        for (long i = 0; i < n; i++) s += p[i];
        total += s;
    }
    printf("%.6f\n", total);
    return 0;
}
