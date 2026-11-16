// Tachbit 0.3 — точка входа компилятора.
//
//   tb new   имя             — новый проект: папка имя/ с main.tb
//   tb start имя [слова]     — собрать проект (или файл) и запустить
//   (имя — это папка проекта, файл.tb или файл без .tb; без имени — текущая папка)
//
//   tachbit tokens файл.tb   — показать токены (слой 1)
//   tachbit tree   файл.tb   — показать дерево (слой 2)
//   tachbit check  файл.tb   — только найти ошибки (слой 3)
//   tachbit c      файл.tb   — показать готовый C (слой 4)
//   tachbit build  файл.tb   — сделать программу рядом с файлом
//   tachbit run    файл.tb   — сделать и сразу запустить
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
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
    printf("  tb new   имя             новый проект: папка имя/ с main.tb\n");
    printf("  tb start имя [слова]     собрать проект и запустить (без имени — текущая папка)\n\n");
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

static int is_folder(char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// что запускать: файл .tb и куда класть готовую программу
//   shop      (папка)  → shop/main.tb,  программа shop/build/shop
//   shop.tb / shop     → shop.tb,       программа shop
//   ничего             → ./main.tb,     программа build/<имя папки>
static char *exe_path = 0;

static char *project_main(char *arg) {
    if (arg == 0 || arg[0] == 0) {
        arg = ".";
    }
    int len = (int)strlen(arg);
    while (len > 1 && arg[len - 1] == '/') {
        len -= 1;
    }
    char *name = arena_copy(arg, len);
    if (is_folder(name)) {
        char *main_tb = arena_alloc(len + 16);
        sprintf(main_tb, "%s/main.tb", name);
        if (!is_file(main_tb)) {
            fprintf(stderr, "в папке %s нет main.tb — с него начинается проект\n", name);
            exit(1);
        }
        char *base = name + strlen(folder_of(name));
        if (same(name, ".")) {
            char here[4096];
            if (getcwd(here, sizeof here) != 0) {
                base = arena_copy(here + strlen(folder_of(here)), (int)strlen(here + strlen(folder_of(here))));
            }
        }
        if (base[0] == 0 || same(base, ".") || same(base, "..")) {
            base = "main";
        }
        char *build_dir = arena_alloc(len + 16);
        sprintf(build_dir, "%s/build", name);
        mkdir(build_dir, 0755);
        exe_path = arena_alloc(strlen(build_dir) + strlen(base) + 4);
        sprintf(exe_path, "%s/%s", build_dir, base);
        return main_tb;
    }
    if (is_file(name)) {
        return name;
    }
    char *with_tb = arena_alloc(len + 4);
    sprintf(with_tb, "%s.tb", name);
    if (is_file(with_tb)) {
        return with_tb;
    }
    fprintf(stderr, "нет проекта или файла: %s\n", name);
    fprintf(stderr, "новый проект делается так:  tb new %s\n", name);
    exit(1);
}

static int new_project(char *name) {
    if (name == 0 || name[0] == 0) {
        fprintf(stderr, "напиши имя проекта:  tb new имя\n");
        return 1;
    }
    struct stat st;
    if (stat(name, &st) == 0) {
        fprintf(stderr, "уже есть папка или файл с именем %s — возьми другое имя\n", name);
        return 1;
    }
    if (mkdir(name, 0755) != 0) {
        fprintf(stderr, "не могу сделать папку: %s\n", name);
        return 1;
    }
    char *path = arena_alloc(strlen(name) + 16);
    sprintf(path, "%s/main.tb", name);
    FILE *f = fopen(path, "w");
    if (f == 0) {
        fprintf(stderr, "не могу записать файл: %s\n", path);
        return 1;
    }
    fprintf(f, "func main()\n    print(\"Привет из проекта %s!\")\n", name);
    fclose(f);
    sprintf(path, "%s/.gitignore", name);
    f = fopen(path, "w");
    if (f != 0) {
        fprintf(f, "build/\n");
        fclose(f);
    }
    printf("готов проект %s:\n", name);
    printf("  %s/main.tb   — здесь пишешь программу\n", name);
    printf("  %s/build/    — сюда кладётся готовая программа\n\n", name);
    printf("запуск:  tb start %s\n", name);
    return 0;
}

static int build(char *path) {
    char *exe = exe_path != 0 ? exe_path : without_tb(path);
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
    if (argc < 2) {
        usage();
        return 1;
    }
    char *mode = argv[1];
    if (same(mode, "new")) {
        return new_project(argc > 2 ? argv[2] : 0);
    }
    if (same(mode, "help") || same(mode, "-h") || same(mode, "--help")) {
        usage();
        return 0;
    }
    if (same(mode, "version") || same(mode, "--version")) {
        printf("Tachbit 0.3\n");
        return 0;
    }
    if (same(mode, "start")) {
        mode = "run";
    }
    // имя проекта можно не писать: тогда берётся текущая папка
    if (argc < 3 && !(same(mode, "run") || same(mode, "build") || same(mode, "check") || same(mode, "c"))) {
        usage();
        return 1;
    }
    file_name = project_main(argc > 2 ? argv[2] : 0);

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
        char *exe = exe_path != 0 ? exe_path : without_tb(file_name);
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
        // код выхода программы (exit(3) → 3); system отдаёт его умноженным на 256
        if (code > 255) {
            code = code / 256;
        }
        return code == 0 ? 0 : (code < 0 ? 1 : code);
    }
    usage();
    return 1;
}
