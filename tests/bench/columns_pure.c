// То же на обычном C: массив структур (так пишут чаще всего).
#include <stdio.h>
#include <stdlib.h>
typedef struct { double x, y, z, mass; long long id; _Bool alive; } Particle;
int main(void) {
    long long n = 20000000;
    Particle *parts = malloc(sizeof(Particle) * n);
    for (long long i = 0; i < n; i++) {
        parts[i] = (Particle){1.0, 2.0, 3.0, (i % 100) / 10.0, i, 1};
    }
    double total = 0.0;
    for (int r = 0; r < 10; r++) {
        for (long long i = 0; i < n; i++) total += parts[i].mass;
    }
    printf("%g\n", total);
    return 0;
}
