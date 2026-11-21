#include <stdio.h>
#include "mylib.h"
int main(void) {
    printf("%lld\n", add(40, 2));
    printf("%g\n", average("1, 2, 3, 4"));
    printf("%s\n", greet("C"));
    printf("%d %d\n", is_big(150.5), is_big(3));
    ping();
    printf("%lld\n", times_called());
    return 0;
}
