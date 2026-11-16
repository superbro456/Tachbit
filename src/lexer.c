// Слой 1. ЛЕКСЕР: текст → токены.
// Пробелы в начале строки превращаются в T_INDENT / T_DEDENT, как в Python.
#include <stdlib.h>
#include <string.h>
#include "ast.h"

Token *tokens = 0;
int token_count = 0;
static int token_cap = 0;

static char *commands[] = {
    "func", "return", "gives", "if", "else", "each", "in", "count", "from", "to",
    "repeat", "times", "while", "skip", "stop", "const", "shared", "obj", "link",
    "int", "dec", "text", "bool", "and", "or", "not", "true", "false", "use",
    "modpub", "modpri", "temp", "together", "danger", "by"
};
static int command_count = 35;

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_command(char *word) {
    for (int i = 0; i < command_count; i++) {
        if (same(commands[i], word)) {
            return 1;
        }
    }
    return 0;
}

static void add_token(int kind, char *start, int len, int line) {
    if (token_count == token_cap) {
        int new_cap = token_cap * 2 + 256;
        Token *bigger = arena_alloc(sizeof(Token) * new_cap);
        for (int i = 0; i < token_count; i++) {
            bigger[i] = tokens[i];
        }
        tokens = bigger;
        token_cap = new_cap;
    }
    tokens[token_count].kind = kind;
    tokens[token_count].text = arena_copy(start, len);
    tokens[token_count].line = line;
    token_count += 1;
}

// знаки из двух символов
static int two_char_sign(char a, char b) {
    if (b == '=' && (a == '+' || a == '-' || a == '*' || a == '/' || a == '=' || a == '!' || a == '<' || a == '>')) {
        return 1;
    }
    if (a == '-' && b == '>') {
        return 1;
    }
    if (a == '.' && b == '.') {
        return 1;
    }
    return 0;
}

static int one_char_sign(char c) {
    char *signs = "()[],:=+-*/%<>.";
    for (int i = 0; signs[i] != 0; i++) {
        if (signs[i] == c) {
            return 1;
        }
    }
    return 0;
}

void lex(char *src, int size) {
    int indents[200];
    int depth = 0;
    indents[0] = 0;
    int pos = 0;
    int line = 1;

    while (pos < size) {
        // 1) отступ в начале строки
        int spaces = 0;
        while (pos < size && (src[pos] == ' ' || src[pos] == '\t')) {
            if (src[pos] == '\t') {
                spaces += 4;
            } else {
                spaces += 1;
            }
            pos += 1;
        }
        if (pos >= size) {
            break;
        }
        // пустая строка или строка-комментарий: пропускаем целиком
        if (src[pos] == '\n' || src[pos] == '\r' || (src[pos] == '/' && src[pos + 1] == '/')) {
            while (pos < size && src[pos] != '\n') {
                pos += 1;
            }
            pos += 1;
            line += 1;
            continue;
        }
        if (spaces > indents[depth]) {
            if (depth == 199) {
                fail(line, "слишком глубокая вложенность", 0);
            }
            depth += 1;
            indents[depth] = spaces;
            add_token(T_INDENT, "", 0, line);
        } else {
            while (spaces < indents[depth]) {
                depth -= 1;
                add_token(T_DEDENT, "", 0, line);
            }
            if (spaces != indents[depth]) {
                fail(line, "отступ не совпадает ни с одним уровнем выше", 0);
            }
        }

        // 2) токены внутри строки
        while (pos < size && src[pos] != '\n') {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r') {
                pos += 1;
                continue;
            }
            if (c == '/' && src[pos + 1] == '/') {
                while (pos < size && src[pos] != '\n') {
                    pos += 1;
                }
                break;
            }
            char *start = src + pos;
            if (is_letter(c)) {
                int len = 0;
                while (is_letter(src[pos]) || is_digit(src[pos])) {
                    pos += 1;
                    len += 1;
                }
                char *word = arena_copy(start, len);
                if (is_command(word)) {
                    add_token(T_COMMAND, start, len, line);
                } else {
                    add_token(T_NAME, start, len, line);
                }
            } else if (is_digit(c)) {
                int len = 0;
                int kind = T_NUMBER;
                while (is_digit(src[pos])) {
                    pos += 1;
                    len += 1;
                }
                if (src[pos] == '.' && is_digit(src[pos + 1])) {
                    kind = T_DECIMAL;
                    pos += 1;
                    len += 1;
                    while (is_digit(src[pos])) {
                        pos += 1;
                        len += 1;
                    }
                }
                add_token(kind, start, len, line);
            } else if (c == '"') {
                int len = 1;
                pos += 1;
                while (pos < size && src[pos] != '"' && src[pos] != '\n') {
                    if (src[pos] == '\\') {
                        pos += 1;
                        len += 1;
                    }
                    pos += 1;
                    len += 1;
                }
                if (src[pos] != '"') {
                    fail(line, "текст в кавычках не закрыт", 0);
                }
                pos += 1;
                len += 1;
                add_token(T_STRING, start, len, line);
            } else if (two_char_sign(c, src[pos + 1])) {
                add_token(T_SIGN, start, 2, line);
                pos += 2;
            } else if (one_char_sign(c)) {
                add_token(T_SIGN, start, 1, line);
                pos += 1;
            } else {
                char bad[2];
                bad[0] = c;
                bad[1] = 0;
                fail(line, "непонятный символ", bad);
            }
        }
        add_token(T_NEWLINE, "", 0, line);
        pos += 1;
        line += 1;
    }
    while (depth > 0) {
        depth -= 1;
        add_token(T_DEDENT, "", 0, line);
    }
    add_token(T_END, "", 0, line);
}

char *token_kind_name(int kind) {
    char *names[] = {"ИМЯ", "КОМАНДА", "ЧИСЛО", "ДРОБЬ", "ТЕКСТ", "ЗНАК",
                     "КОНЕЦ СТРОКИ", "ОТСТУП", "ВЫСТУП", "КОНЕЦ ФАЙЛА"};
    return names[kind];
}

void print_tokens(void) {
    for (int i = 0; i < token_count; i++) {
        printf("%3d: %-13s %s\n", tokens[i].line, token_kind_name(tokens[i].kind), tokens[i].text);
    }
}
