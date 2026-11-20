// То же на C с той же хитростью, что by columns: каждое качество — свой массив (колонка).
#include <stdio.h>
#include <stdlib.h>
typedef struct { double *x, *y, *z, *mass; long long *id; _Bool *alive; } Particles;
int main(void) {
    long long n = 20000000;
    Particles p = { malloc(8 * n), malloc(8 * n), malloc(8 * n), malloc(8 * n), malloc(8 * n), malloc(n) };
    for (long long i = 0; i < n; i++) {
        p.x[i] = 1.0; p.y[i] = 2.0; p.z[i] = 3.0; p.mass[i] = (i % 100) / 10.0; p.id[i] = i; p.alive[i] = 1;
    }
    double total = 0.0;
    for (int r = 0; r < 10; r++) {
        for (long long i = 0; i < n; i++) total += p.mass[i];
    }
    printf("%g\n", total);
    return 0;
}
