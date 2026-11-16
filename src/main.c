// Tachbit 0.3 — точка входа компилятора.
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

char *file_name = "";

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
    printf("Tachbit 0.3\n\n");
    printf("  tachbit run    файл.tb [слова]   сделать программу и запустить\n");
    printf("  tachbit build  файл.tb   сделать программу рядом с файлом\n");
    printf("  tachbit check  файл.tb   только найти ошибки\n");
    printf("  tachbit c      файл.tb   показать, какой C получился\n");
    printf("  tachbit tree   файл.tb   показать дерево\n");
    printf("  tachbit tokens файл.tb   показать токены\n");
}

// ---------- модули: use money  →  файл money.tb в той же папке ----------

static char *loaded[200];
static int loaded_count = 0;

// "examples/shop.tb" → папка "examples/", имя модуля "shop"
static char *folder_of(char *path) {
    int len = (int)strlen(path);
    int cut = 0;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') {
            cut = i + 1;
        }
    }
    return arena_copy(path, cut);
}

static char *module_of(char *path) {
    char *base = path + strlen(folder_of(path));
    return without_tb(base);
}

static void load_module(Node *program, char *path, int line_of_use, char *used_from) {
    char *name = module_of(path);
    for (int i = 0; i < loaded_count; i++) {
        if (same(loaded[i], name)) {
            return;
        }
    }
    if (loaded_count == 200) {
        fail(0, "слишком много модулей", 0);
    }
    loaded[loaded_count] = name;
    loaded_count += 1;

    FILE *test = fopen(path, "rb");
    if (test == 0) {
        file_name = used_from;
        fail(line_of_use, "нет файла для use", path);
    }
    fclose(test);
    int size = 0;
    char *source = read_file(path, &size);
    file_name = path;
    lex(source, size);
    Node *part = parse();
    for (int i = 0; i < part->nkids; i++) {
        part->kids[i]->mod = name;
        part->kids[i]->file = path;
        add_kid(program, part->kids[i]);
    }
    // теперь файлы, которые этот файл подключает через use
    for (int i = 0; i < part->nkids; i++) {
        Node *u = part->kids[i];
        if (u->kind == N_USE) {
            char *folder = folder_of(path);
            char *next_path = arena_alloc(strlen(folder) + strlen(u->name) + 4);
            sprintf(next_path, "%s%s.tb", folder, u->name);
            load_module(program, next_path, u->line, path);
        }
    }
}

static Node *load_program(char *path) {
    Node *program = new_node(N_PROGRAM, 1);
    load_module(program, path, 0, path);
    file_name = path;
    return program;
}

static int build(char *path) {
    char *exe = without_tb(path);
    char *c_path = arena_alloc(strlen(exe) + 3);
    sprintf(c_path, "%s.c", exe);
    Node *program = load_program(path);
    check(program);
    FILE *f = fopen(c_path, "w");
    if (f == 0) {
        fprintf(stderr, "не могу записать файл: %s\n", c_path);
        return 1;
    }
    emit_c(program, f);
    fclose(f);

    char *cc = getenv("CC");
    if (cc == 0 || cc[0] == 0) {
        cc = "cc";
    }
    char *command = arena_alloc(strlen(cc) + strlen(exe) + strlen(c_path) + 64);
    sprintf(command, "%s -O2 -w -o '%s' '%s' -lm", cc, exe, c_path);
    if (system(command) != 0) {
        fprintf(stderr, "компилятор C не справился (это ошибка Tachbit, а не твоя): %s\n", c_path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    char *mode = argv[1];
    file_name = argv[2];

    if (same(mode, "tokens")) {
        int size = 0;
        char *source = read_file(file_name, &size);
        lex(source, size);
        print_tokens();
        return 0;
    }
    if (same(mode, "tree")) {
        print_tree(load_program(file_name), 0);
        return 0;
    }
    if (same(mode, "check")) {
        check(load_program(file_name));
        printf("ошибок нет\n");
        return 0;
    }
    if (same(mode, "c")) {
        Node *program = load_program(file_name);
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
        // tachbit run файл.tb a b c  →  программа получит a b c в args()
        char *exe = without_tb(file_name);
        long length = strlen(exe) + 8;
        for (int i = 3; i < argc; i++) {
            length += strlen(argv[i]) * 4 + 3;
        }
        char *command = arena_alloc(length);
        if (has_slash(exe)) {
            sprintf(command, "'%s'", exe);
        } else {
            sprintf(command, "'./%s'", exe);
        }
        for (int i = 3; i < argc; i++) {
            strcat(command, " '");
            for (int j = 0; argv[i][j] != 0; j++) {
                if (argv[i][j] == '\'') {
                    strcat(command, "'\\''");
                } else {
                    int end = (int)strlen(command);
                    command[end] = argv[i][j];
                    command[end + 1] = 0;
                }
            }
            strcat(command, "'");
        }
        int code = system(command);
        return code == 0 ? 0 : 1;
    }
    usage();
    return 1;
}
