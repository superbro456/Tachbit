// То же на C с той же хитростью, что by vec: 8 сумм сразу, чтобы процессор
// складывал по несколько чисел одной командой (SIMD), в конце суммы складываются.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long long n = 10000000;
    double *nums = malloc(sizeof(double) * n);
    for (long long i = 0; i < n; i++) nums[i] = (i % 1000) / 1000.0;
    double total = 0.0;
    for (int r = 0; r < 20; r++) {
        double s[8] = {0};
        long long i = 0;
        for (; i + 8 <= n; i += 8)
            for (int k = 0; k < 8; k++) s[k] += nums[i + k] * nums[i + k];
        for (; i < n; i++) s[0] += nums[i] * nums[i];
        for (int k = 0; k < 8; k++) total += s[k];
    }
    printf("%g\n", total);
    return 0;
}
