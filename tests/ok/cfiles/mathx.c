#include <stdint.h>
#include <string.h>
#include <stdlib.h>
int32_t mx_add32(int32_t a, int32_t b) { return a + b; }
uint8_t mx_low(uint8_t x) { return (uint8_t)(x + 1); }
int8_t mx_neg(int8_t x) { return (int8_t)-x; }
double mx_half(double x) { return x / 2; }
float mx_f(float x) { return x * 2.0f; }
_Bool mx_even(long long x) { return x % 2 == 0; }
static char buf[64];
const char *mx_hello(const char *name) { strcpy(buf, "hello "); strncat(buf, name, 50); return buf; }
long long mx_len(const char *s) { return (long long)strlen(s); }
static long long total;
void mx_store(long long x) { total += x; }
long long mx_total(void) { return total; }
void mx_put(long long addr, uint32_t v) { *(uint32_t *)(intptr_t)addr = v; }
