// То же на обычном C (как пишут обычно: одна сумма в цикле).
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long long n = 10000000;
    double *nums = malloc(sizeof(double) * n);
    for (long long i = 0; i < n; i++) nums[i] = (i % 1000) / 1000.0;
    double total = 0.0;
    for (int r = 0; r < 20; r++) {
        for (long long i = 0; i < n; i++) total += nums[i] * nums[i];
    }
    printf("%g\n", total);
    return 0;
}
