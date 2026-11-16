// Слой 2. ПАРСЕР: токены → дерево (AST).
// Рекурсивный спуск: на каждое правило языка своя маленькая функция.
#include <stdlib.h>
#include "ast.h"

static int pos = 0;

// ---------- узлы ----------

Node *new_node(int kind, int line) {
    Node *n = arena_alloc(sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}

void add_kid(Node *parent, Node *kid) {
    if (parent->nkids == parent->cap) {
        int new_cap = parent->cap * 2 + 4;
        Node **bigger = arena_alloc(sizeof(Node *) * new_cap);
        for (int i = 0; i < parent->nkids; i++) {
            bigger[i] = parent->kids[i];
        }
        parent->kids = bigger;
        parent->cap = new_cap;
    }
    parent->kids[parent->nkids] = kid;
    parent->nkids += 1;
}

// ---------- чтение токенов ----------

static Token *now(void) {
    return &tokens[pos];
}

static int line_now(void) {
    return tokens[pos].line;
}

static void next(void) {
    if (tokens[pos].kind != T_END) {
        pos += 1;
    }
}

static int is_sign(char *s) {
    return now()->kind == T_SIGN && same(now()->text, s);
}

static int is_cmd(char *s) {
    return now()->kind == T_COMMAND && same(now()->text, s);
}

static char *what_is_here(void) {
    if (now()->kind == T_NEWLINE) {
        return "конец строки";
    }
    if (now()->kind == T_INDENT) {
        return "лишний отступ";
    }
    if (now()->kind == T_DEDENT) {
        return "конец блока";
    }
    if (now()->kind == T_END) {
        return "конец файла";
    }
    return now()->text;
}

static void expect_sign(char *s) {
    if (!is_sign(s)) {
        fail(line_now(), "ожидался знак", s);
    }
    next();
}

static void expect_cmd(char *s) {
    if (!is_cmd(s)) {
        fail(line_now(), "ожидалась команда", s);
    }
    next();
}

static char *expect_name(void) {
    if (now()->kind != T_NAME) {
        if (now()->kind == T_COMMAND) {
            fail(line_now(), "это слово занято языком, возьми другое имя", now()->text);
        }
        fail(line_now(), "ожидалось имя, а тут", what_is_here());
    }
    char *name = now()->text;
    next();
    return name;
}

static void expect_newline(void) {
    if (now()->kind != T_NEWLINE) {
        fail(line_now(), "лишнее в конце строки", what_is_here());
    }
    next();
}

// int / dec / text / bool  →  TY_...;  не тип  →  0
static int type_here(void) {
    if (is_cmd("int")) {
        return TY_INT;
    }
    if (is_cmd("dec")) {
        return TY_DEC;
    }
    if (is_cmd("text")) {
        return TY_TEXT;
    }
    if (is_cmd("bool")) {
        return TY_BOOL;
    }
    return 0;
}

// ---------- выражения (от слабых операций к сильным) ----------

static Node *parse_expr(void);

static Node *binop(char *sign, Node *left, Node *right, int line) {
    Node *n = new_node(N_BINOP, line);
    n->value = sign;
    add_kid(n, left);
    add_kid(n, right);
    return n;
}

static Node *parse_call(char *name, int line) {
    Node *call = new_node(N_CALL, line);
    call->name = name;
    expect_sign("(");
    while (!is_sign(")")) {
        Node *arg = new_node(N_ARG, line_now());
        // именованный аргумент:  add(a: 2)
        if (now()->kind == T_NAME && tokens[pos + 1].kind == T_SIGN && same(tokens[pos + 1].text, ":")) {
            arg->name = now()->text;
            next();
            next();
        }
        add_kid(arg, parse_expr());
        add_kid(call, arg);
        if (!is_sign(",")) {
            break;
        }
        next();
    }
    expect_sign(")");
    return call;
}

static Node *parse_primary(void) {
    Token *t = now();
    int line = t->line;
    Node *n = 0;
    if (t->kind == T_NUMBER) {
        n = new_node(N_NUMBER, line);
        n->value = t->text;
        next();
    } else if (t->kind == T_DECIMAL) {
        n = new_node(N_DECIMAL, line);
        n->value = t->text;
        next();
    } else if (t->kind == T_STRING) {
        n = new_node(N_STRING, line);
        n->value = t->text;
        next();
    } else if (is_cmd("true") || is_cmd("false")) {
        n = new_node(N_BOOL, line);
        n->value = t->text;
        next();
    } else if (t->kind == T_NAME) {
        char *name = t->text;
        next();
        if (is_sign("(")) {
            n = parse_call(name, line);
        } else {
            n = new_node(N_NAME, line);
            n->name = name;
        }
    } else if (is_sign("(")) {
        next();
        n = parse_expr();
        expect_sign(")");
    } else if (is_sign("[")) {
        next();
        n = new_node(N_LIST, line);
        while (!is_sign("]")) {
            add_kid(n, parse_expr());
            if (!is_sign(",")) {
                break;
            }
            next();
        }
        expect_sign("]");
    } else {
        fail(line, "ожидалось значение, а тут", what_is_here());
    }
    // nums[2]
    while (is_sign("[")) {
        Node *index = new_node(N_INDEX, line_now());
        next();
        add_kid(index, n);
        add_kid(index, parse_expr());
        expect_sign("]");
        n = index;
    }
    return n;
}

static Node *parse_unary(void) {
    if (is_sign("-")) {
        Node *n = new_node(N_UNARY, line_now());
        n->value = "-";
        next();
        add_kid(n, parse_unary());
        return n;
    }
    return parse_primary();
}

static Node *parse_mul(void) {
    Node *left = parse_unary();
    while (is_sign("*") || is_sign("/") || is_sign("%")) {
        char *sign = now()->text;
        int line = line_now();
        next();
        left = binop(sign, left, parse_unary(), line);
    }
    return left;
}

static Node *parse_add(void) {
    Node *left = parse_mul();
    while (is_sign("+") || is_sign("-")) {
        char *sign = now()->text;
        int line = line_now();
        next();
        left = binop(sign, left, parse_mul(), line);
    }
    return left;
}

static Node *parse_compare(void) {
    Node *left = parse_add();
    if (is_sign("==") || is_sign("!=") || is_sign("<") || is_sign(">") || is_sign("<=") || is_sign(">=")) {
        char *sign = now()->text;
        int line = line_now();
        next();
        left = binop(sign, left, parse_add(), line);
    }
    if (is_sign("=")) {
        fail(line_now(), "в условии сравнение пишется двумя знаками", "==");
    }
    return left;
}

static Node *parse_not(void) {
    if (is_cmd("not")) {
        Node *n = new_node(N_UNARY, line_now());
        n->value = "not";
        next();
        add_kid(n, parse_not());
        return n;
    }
    return parse_compare();
}

static Node *parse_and(void) {
    Node *left = parse_not();
    while (is_cmd("and")) {
        int line = line_now();
        next();
        left = binop("and", left, parse_not(), line);
    }
    return left;
}

static Node *parse_expr(void) {
    Node *left = parse_and();
    while (is_cmd("or")) {
        int line = line_now();
        next();
        left = binop("or", left, parse_and(), line);
    }
    return left;
}

// ---------- команды ----------

static Node *parse_block(void);

static Node *parse_if(void) {
    Node *n = new_node(N_IF, line_now());
    expect_cmd("if");
    add_kid(n, parse_expr());
    expect_newline();
    add_kid(n, parse_block());
    while (is_cmd("else")) {
        next();
        if (is_cmd("if")) {
            next();
            add_kid(n, parse_expr());
            expect_newline();
            add_kid(n, parse_block());
        } else {
            expect_newline();
            add_kid(n, parse_block());
            break;
        }
    }
    return n;
}

static Node *parse_each(void) {
    Node *n = new_node(N_EACH, line_now());
    expect_cmd("each");
    n->name = expect_name();
    expect_cmd("in");
    add_kid(n, parse_expr());
    expect_newline();
    add_kid(n, parse_block());
    return n;
}

static Node *parse_count(void) {
    Node *n = new_node(N_COUNT, line_now());
    expect_cmd("count");
    n->name = expect_name();
    expect_cmd("from");
    add_kid(n, parse_expr());
    expect_cmd("to");
    add_kid(n, parse_expr());
    expect_newline();
    add_kid(n, parse_block());
    return n;
}

static Node *parse_repeat(void) {
    int line = line_now();
    expect_cmd("repeat");
    Node *n;
    if (is_cmd("while")) {
        next();
        n = new_node(N_REPEAT_WHILE, line);
        add_kid(n, parse_expr());
    } else {
        n = new_node(N_REPEAT_TIMES, line);
        add_kid(n, parse_expr());
        expect_cmd("times");
    }
    expect_newline();
    add_kid(n, parse_block());
    return n;
}

// x = 5   /   x = int 5   /   const x = 5   /   x += 1
static Node *parse_assign(int is_const) {
    int line = line_now();
    char *name = expect_name();
    Node *n;
    if (is_sign("=")) {
        next();
        n = new_node(N_ASSIGN, line);
        n->declared = type_here();
        if (n->declared != 0) {
            next();
        }
    } else if (is_sign("+=") || is_sign("-=") || is_sign("*=") || is_sign("/=")) {
        if (is_const) {
            fail(line, "после const нужен знак", "=");
        }
        n = new_node(N_OPASSIGN, line);
        n->value = now()->text;
        next();
    } else {
        fail(line, "после имени ожидался знак = или вызов (...)", what_is_here());
        return 0;
    }
    n->name = name;
    n->is_const = is_const;
    add_kid(n, parse_expr());
    expect_newline();
    return n;
}

static int later_command(void) {
    char *later[] = {"gives", "shared", "obj", "link", "use", "modpub", "modpri", "temp", "together", "danger", "by"};
    for (int i = 0; i < 11; i++) {
        if (is_cmd(later[i])) {
            return 1;
        }
    }
    return 0;
}

static Node *parse_statement(void) {
    int line = line_now();
    if (is_cmd("if")) {
        return parse_if();
    }
    if (is_cmd("each")) {
        return parse_each();
    }
    if (is_cmd("count")) {
        return parse_count();
    }
    if (is_cmd("repeat")) {
        return parse_repeat();
    }
    if (is_cmd("skip") || is_cmd("stop")) {
        Node *n = new_node(is_cmd("skip") ? N_SKIP : N_STOP, line);
        next();
        expect_newline();
        return n;
    }
    if (is_cmd("return")) {
        Node *n = new_node(N_RETURN, line);
        next();
        if (now()->kind != T_NEWLINE) {
            add_kid(n, parse_expr());
        }
        expect_newline();
        return n;
    }
    if (is_cmd("const")) {
        next();
        return parse_assign(1);
    }
    if (is_cmd("else")) {
        fail(line, "else без if над ним (проверь отступ)", 0);
    }
    if (is_cmd("func")) {
        fail(line, "функцию нельзя писать внутри другой функции", 0);
    }
    if (later_command()) {
        fail(line, "эта команда будет в следующей версии", now()->text);
    }
    if (now()->kind == T_NAME && tokens[pos + 1].kind == T_SIGN && same(tokens[pos + 1].text, "(")) {
        Node *n = new_node(N_EXPR_STMT, line);
        add_kid(n, parse_expr());
        expect_newline();
        return n;
    }
    if (now()->kind == T_NAME) {
        return parse_assign(0);
    }
    fail(line, "не понимаю, что делает строка, начиная с", what_is_here());
    return 0;
}

static Node *parse_block(void) {
    Node *block = new_node(N_BLOCK, line_now());
    if (now()->kind != T_INDENT) {
        fail(line_now(), "после этой строки нужен блок с отступом", 0);
    }
    next();
    while (now()->kind != T_DEDENT && now()->kind != T_END) {
        if (now()->kind == T_INDENT) {
            fail(line_now(), "лишний отступ", 0);
        }
        add_kid(block, parse_statement());
    }
    next();
    return block;
}

// func name(a: int, b: dec)
static Node *parse_func(void) {
    Node *f = new_node(N_FUNC, line_now());
    expect_cmd("func");
    f->name = expect_name();
    expect_sign("(");
    Node *params = new_node(N_BLOCK, line_now());
    while (!is_sign(")")) {
        Node *p = new_node(N_PARAM, line_now());
        p->name = expect_name();
        expect_sign(":");
        p->declared = type_here();
        if (p->declared == 0) {
            fail(line_now(), "после двоеточия нужен тип: int, dec, text или bool", what_is_here());
        }
        next();
        add_kid(params, p);
        if (!is_sign(",")) {
            break;
        }
        next();
    }
    expect_sign(")");
    if (is_cmd("gives")) {
        fail(line_now(), "gives будет в следующей версии, пока пиши просто return", 0);
    }
    expect_newline();
    add_kid(f, params);
    add_kid(f, parse_block());
    return f;
}

Node *parse(void) {
    pos = 0;
    Node *program = new_node(N_PROGRAM, 1);
    while (now()->kind != T_END) {
        if (is_cmd("func")) {
            add_kid(program, parse_func());
        } else if (now()->kind == T_INDENT) {
            fail(line_now(), "лишний отступ", 0);
        } else {
            fail(line_now(), "снаружи функций пока можно писать только func", what_is_here());
        }
    }
    return program;
}

// ---------- печать дерева (команда tree) ----------

char *node_kind_name(int kind) {
    char *names[] = {
        "ПРОГРАММА", "ФУНКЦИЯ", "ПАРАМЕТР", "БЛОК", "ПРИСВОИТЬ", "ИЗМЕНИТЬ", "ЕСЛИ",
        "EACH", "COUNT", "REPEAT TIMES", "REPEAT WHILE", "SKIP", "STOP", "RETURN",
        "ВЫЗОВ-СТРОКА", "ЧИСЛО", "ДРОБЬ", "ТЕКСТ", "ДА/НЕТ", "ИМЯ", "СПИСОК", "ВЫЗОВ",
        "АРГУМЕНТ", "ОПЕРАЦИЯ", "ОДИНОЧНАЯ", "ИНДЕКС"
    };
    return names[kind];
}

void print_tree(Node *n, int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
    printf("%s", node_kind_name(n->kind));
    if (n->name != 0) {
        printf(" %s", n->name);
    }
    if (n->value != 0) {
        printf(" %s", n->value);
    }
    if (n->declared != 0) {
        printf(" : %s", type_name(n->declared));
    }
    if (n->is_const) {
        printf(" (const)");
    }
    printf("\n");
    for (int i = 0; i < n->nkids; i++) {
        print_tree(n->kids[i], depth + 1);
    }
}
