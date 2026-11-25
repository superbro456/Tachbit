// /bin/cc для FuturaOS (его зовёт tb run).
// Есть файлы .c → tcc + tb_futura_sdk.h (поправки под его libc).
// Только готовые .o (путь через LLVM) → ld.lld, как tools/futura/build.sh.
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ends(const char *s, const char *e) {
    size_t a = strlen(s), b = strlen(e);
    return a >= b && strcmp(s + a - b, e) == 0;
}
int main(int argc, char **argv) {
    char **a = malloc(sizeof(char *) * (argc + 32));
    int n = 0, has_c = 0;
    for (int i = 1; i < argc; i++) if (ends(argv[i], ".c")) has_c = 1;
    if (has_c || access("/bin/ld.lld", X_OK) != 0) {
        a[n++] = "tcc";
        a[n++] = "-include";
        a[n++] = "tb_futura_sdk.h";
        for (int i = 1; i < argc; i++) a[n++] = argv[i];
        a[n] = 0;
        execv("/bin/tcc", a);
        perror("cc: не могу запустить /bin/tcc");
        return 127;
    }
    const char *pre[] = { "ld.lld", "--image-base=0x400000", "-z", "noseparate-code", "--no-rosegment",
        "-z", "norelro", "-dynamic-linker", "/lib/ld.so", "--hash-style=sysv", "-rpath", "/lib",
        // запись готового файла через mmap в FuturaOS не доходит до файла — пишем обычным write
        "--no-mmap-output-file", "/lib/crt0.o", "/lib/crti.o", 0 };
    for (int i = 0; pre[i]; i++) a[n++] = (char *)pre[i];
    for (int i = 1; i < argc; i++) {
        // -O2, -w и т.п. — для компилятора, не для линкера; -lpthread: потоки в его libc
        if (strcmp(argv[i], "-lpthread") == 0 || (argv[i][0] == '-' && argv[i][1] != 'o' && argv[i][1] != 'l' && argv[i][1] != 'L')) continue;
        a[n++] = argv[i];
    }
    a[n++] = "-L/lib";
    a[n++] = "-lm";
    a[n++] = "-lc";
    a[n++] = "/usr/lib/tcc/libtcc1.a";   // __atomic_* и т.п., если понадобятся
    a[n++] = "/lib/crtn.o";
    a[n] = 0;
    execv("/bin/ld.lld", a);
    perror("cc: не могу запустить /bin/ld.lld");
    return 127;
}
