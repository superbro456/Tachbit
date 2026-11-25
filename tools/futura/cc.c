// /bin/cc для FuturaOS: зовёт tcc и сразу подключает tb_futura_sdk.h
// (поправки под его libc). Так `tb run файл.tb` работает прямо внутри FuturaOS.
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    char **a = malloc(sizeof(char *) * (argc + 4));
    int n = 0;
    a[n++] = "tcc";
    a[n++] = "-include";
    a[n++] = "tb_futura_sdk.h";
    for (int i = 1; i < argc; i++) a[n++] = argv[i];
    a[n] = 0;
    execv("/bin/tcc", a);
    perror("cc: не могу запустить /bin/tcc");
    return 127;
}
