// Tachbit 0.1 — точка входа компилятора.
//
//   tachbit tokens файл.tb   — показать токены (слой 1)
//   tachbit tree   файл.tb   — показать дерево (слой 2)
//   tachbit check  файл.tb   — только найти ошибки (слой 3)
//   tachbit c      файл.tb   — показать готовый C (слой 4)
//   tachbit build  файл.tb   — сделать программу рядом с файлом
//   tachbit run    файл.tb   — сделать и сразу запустить
#include <stdlib.h>
#include <string.h>
#include "ast.h"

static char *file_name = "";

int same(char *a, char *b) {
    return strcmp(a, b) == 0;
}

void fail(int line, char *message, char *detail) {
    if (line > 0) {
        fprintf(stderr, "%s, строка %d: %s", file_name, line, message);
    } else {
        fprintf(stderr, "%s: %s", file_name, message);
    }
    if (detail != 0) {
        fprintf(stderr, ": %s", detail);
    }
    fprintf(stderr, "\n");
    exit(1);
}

static char *read_file(char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (f == 0) {
        fprintf(stderr, "не могу открыть файл: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    // +2 нуля в конце: лексер иногда заглядывает на символ вперёд
    char *text = arena_alloc(n + 2);
    if (fread(text, 1, n, f) != (size_t)n) {
        fprintf(stderr, "не могу прочитать файл: %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (int)n;
    return text;
}

// examples/hello.tb  →  examples/hello
static char *without_tb(char *path) {
    int len = (int)strlen(path);
    if (len > 3 && same(path + len - 3, ".tb")) {
        return arena_copy(path, len - 3);
    }
    return arena_copy(path, len);
}

static int has_slash(char *path) {
    for (int i = 0; path[i] != 0; i++) {
        if (path[i] == '/') {
            return 1;
        }
    }
    return 0;
}

static void usage(void) {
    printf("Tachbit 0.1\n\n");
    printf("  tachbit run    файл.tb   сделать программу и запустить\n");
    printf("  tachbit build  файл.tb   сделать программу рядом с файлом\n");
    printf("  tachbit check  файл.tb   только найти ошибки\n");
    printf("  tachbit c      файл.tb   показать, какой C получился\n");
    printf("  tachbit tree   файл.tb   показать дерево\n");
    printf("  tachbit tokens файл.tb   показать токены\n");
}

static int build(char *path) {
    char *exe = without_tb(path);
    char *c_path = arena_alloc(strlen(exe) + 3);
    sprintf(c_path, "%s.c", exe);
    FILE *f = fopen(c_path, "w");
    if (f == 0) {
        fprintf(stderr, "не могу записать файл: %s\n", c_path);
        return 1;
    }
    Node *program = parse();
    check(program);
    emit_c(program, f);
    fclose(f);

    char *cc = getenv("CC");
    if (cc == 0 || cc[0] == 0) {
        cc = "cc";
    }
    char *command = arena_alloc(strlen(cc) + strlen(exe) + strlen(c_path) + 64);
    sprintf(command, "%s -O2 -w -o '%s' '%s'", cc, exe, c_path);
    if (system(command) != 0) {
        fprintf(stderr, "компилятор C не справился (это ошибка Tachbit, а не твоя): %s\n", c_path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage();
        return 1;
    }
    char *mode = argv[1];
    file_name = argv[2];
    int size = 0;
    char *source = read_file(file_name, &size);
    lex(source, size);

    if (same(mode, "tokens")) {
        print_tokens();
        return 0;
    }
    if (same(mode, "tree")) {
        print_tree(parse(), 0);
        return 0;
    }
    if (same(mode, "check")) {
        check(parse());
        printf("ошибок нет\n");
        return 0;
    }
    if (same(mode, "c")) {
        Node *program = parse();
        check(program);
        emit_c(program, stdout);
        return 0;
    }
    if (same(mode, "build")) {
        return build(file_name);
    }
    if (same(mode, "run")) {
        if (build(file_name) != 0) {
            return 1;
        }
        char *exe = without_tb(file_name);
        char *command = arena_alloc(strlen(exe) + 8);
        if (has_slash(exe)) {
            sprintf(command, "'%s'", exe);
        } else {
            sprintf(command, "'./%s'", exe);
        }
        int code = system(command);
        return code == 0 ? 0 : 1;
    }
    usage();
    return 1;
}
