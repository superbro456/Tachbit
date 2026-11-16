// Память компилятора: одна арена. Берём большой блок и раздаём из него кусками.
// Ничего не освобождаем по одному: всё уходит разом, когда компилятор закончил.
#include <stdlib.h>
#include <string.h>
#include "ast.h"

static char *block = 0;
static long used = 0;
static long block_size = 0;

void *arena_alloc(long size) {
    size = (size + 7) / 8 * 8;
    if (block == 0 || used + size > block_size) {
        block_size = 1024 * 1024;
        if (size > block_size) {
            block_size = size;
        }
        block = calloc(1, block_size);
        if (block == 0) {
            printf("ошибка: не хватило памяти\n");
            exit(1);
        }
        used = 0;
    }
    void *p = block + used;
    used += size;
    return p;
}

char *arena_copy(char *src, int len) {
    char *s = arena_alloc(len + 1);
    memcpy(s, src, len);
    s[len] = 0;
    return s;
}
