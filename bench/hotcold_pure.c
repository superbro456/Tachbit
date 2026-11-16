// C, как пишут обычно: массив структур, все поля вместе
#include <stdio.h>
#include <stdlib.h>
typedef struct { long long id; double balance; const char *name, *city, *phone, *notes; long long created; _Bool vip; } User;
int main(void) {
    long long n = 5000000;
    User *u = malloc(sizeof(User) * n);
    for (long long i = 0; i < n; i++) u[i] = (User){i, (i % 1000) / 10.0, "u", "Бишкек", "+996", "", 2026, i % 50 == 0};
    double total = 0;
    for (int r = 0; r < 20; r++)
        for (long long i = 0; i < n; i++) total += u[i].balance;
    printf("%g\n", total);
    return 0;
}
