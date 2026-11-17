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

static int at_type_word(void) {
    return is_cmd("int") || is_cmd("dec") || is_cmd("text") || is_cmd("bool") || is_cmd("list") || is_cmd("dict");
}

// int / dec / text / bool / list int / list list text / User
static Node *parse_type(void) {
    Node *t = new_node(N_TYPE, line_now());
    if (is_cmd("dict")) {
        // dict text int: ключ text, значение int
        t->name = "dict";
        next();
        Node *key = parse_type();
        if (!same(key->name, "text") && !same(key->name, "int")) {
            fail(key->line, "ключ у dict бывает text или int", key->name);
        }
        add_kid(t, key);
        if (now()->kind == T_NEWLINE) {
            fail(line_now(), "после ключа напиши тип значения: dict text int", 0);
        }
        add_kid(t, parse_type());
        return t;
    }
    if (at_type_word()) {
        t->name = now()->text;
        next();
        if (same(t->name, "list")) {
            if (!at_type_word() && now()->kind != T_NAME) {
                fail(line_now(), "после list напиши, что в нём лежит: list int, list text ...", 0);
            }
            add_kid(t, parse_type());
        }
        return t;
    }
    if (now()->kind == T_NAME) {
        t->name = now()->text;
        next();
        return t;
    }
    fail(line_now(), "ожидался тип: int, dec, text, bool, list ... или имя obj, а тут", what_is_here());
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
    } else if (is_sign("{")) {
        // {"Ali": 20, "Vali": 30}
        next();
        n = new_node(N_DICT, line);
        while (!is_sign("}")) {
            add_kid(n, parse_expr());
            expect_sign(":");
            add_kid(n, parse_expr());
            if (!is_sign(",")) {
                break;
            }
            next();
        }
        expect_sign("}");
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
    // nums[2]   u.age   nums.add(5)
    while (is_sign("[") || is_sign(".")) {
        if (is_sign("[")) {
            Node *index = new_node(N_INDEX, line_now());
            next();
            add_kid(index, n);
            add_kid(index, parse_expr());
            expect_sign("]");
            n = index;
        } else {
            int dot_line = line_now();
            next();
            char *name = expect_name();
            if (is_sign("(")) {
                Node *call = parse_call(name, dot_line);
                Node *m = new_node(N_METHOD, dot_line);
                m->name = name;
                add_kid(m, n);
                for (int i = 0; i < call->nkids; i++) {
                    add_kid(m, call->kids[i]);
                }
                n = m;
            } else {
                Node *f = new_node(N_FIELD, dot_line);
                f->name = name;
                add_kid(f, n);
                n = f;
            }
        }
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

// значение справа: обычное, или  try f(),  или  f() else err  + блок
static Node *parse_value(void) {
    if (is_cmd("try")) {
        Node *t = new_node(N_TRY, line_now());
        next();
        add_kid(t, parse_expr());
        return t;
    }
    Node *v = parse_expr();
    if (is_cmd("else")) {
        Node *e = new_node(N_ELSE_ERR, line_now());
        next();
        e->name = expect_name();
        expect_newline();
        add_kid(e, v);
        add_kid(e, parse_block());
        return e;
    }
    return v;
}

// после  else err  блок уже съел конец строки
static void end_statement(Node *value) {
    if (value->kind != N_ELSE_ERR) {
        expect_newline();
    }
}

static int is_change_sign(void) {
    return is_sign("=") || is_sign("+=") || is_sign("-=") || is_sign("*=") || is_sign("/=");
}

// x = 5   /   x = int 5   /   names = list text   /   const x = 5
static Node *parse_assign(int is_const) {
    int line = line_now();
    Node *n = new_node(N_ASSIGN, line);
    n->name = expect_name();
    n->is_const = is_const;
    expect_sign("=");
    if (at_type_word() || is_cmd("dict")) {
        n->tsyn = parse_type();
    }
    if (now()->kind != T_NEWLINE) {
        add_kid(n, parse_value());
    } else if (n->tsyn == 0) {
        fail(line, "после = нужно значение", 0);
    }
    if (is_const && n->nkids == 0) {
        fail(line, "const нужно сразу дать значение", n->name);
    }
    if (n->nkids == 1) {
        end_statement(n->kids[0]);
    } else {
        expect_newline();
    }
    return n;
}

// hp: link u.age
static Node *parse_linkdecl(void) {
    Node *n = new_node(N_LINKDECL, line_now());
    n->name = expect_name();
    expect_sign(":");
    if (!is_cmd("link")) {
        fail(line_now(), "после двоеточия в строке ожидалось link (ярлык)", what_is_here());
    }
    next();
    add_kid(n, parse_expr());
    expect_newline();
    return n;
}

// q, r = divide(a: 17, b: 5)
static Node *parse_multi(void) {
    Node *n = new_node(N_MULTI, line_now());
    while (1) {
        Node *name = new_node(N_NAME, line_now());
        name->name = expect_name();
        add_kid(n, name);
        if (!is_sign(",")) {
            break;
        }
        next();
    }
    expect_sign("=");
    Node *value = parse_value();
    add_kid(n, value);
    end_statement(value);
    return n;
}

// строка, которая начинается с имени
static Node *parse_name_statement(void) {
    int line = line_now();
    Token *after = &tokens[pos + 1];
    if (after->kind == T_SIGN && same(after->text, "=")) {
        return parse_assign(0);
    }
    if (after->kind == T_SIGN && same(after->text, ":")) {
        return parse_linkdecl();
    }
    if (after->kind == T_SIGN && same(after->text, ",")) {
        return parse_multi();
    }
    Node *target = parse_primary();
    if (is_change_sign()) {
        int k = target->kind;
        if (k != N_NAME && k != N_INDEX && k != N_FIELD) {
            fail(line, "слева от знака должно быть имя, nums[i] или u.age", 0);
        }
        Node *n = new_node(N_SET, line);
        n->value = now()->text;
        next();
        add_kid(n, target);
        Node *value = parse_value();
        add_kid(n, value);
        end_statement(value);
        return n;
    }
    Node *n = new_node(N_EXPR_STMT, line);
    if (is_cmd("else")) {
        Node *e = new_node(N_ELSE_ERR, line_now());
        next();
        e->name = expect_name();
        expect_newline();
        add_kid(e, target);
        add_kid(e, parse_block());
        add_kid(n, e);
        return n;
    }
    add_kid(n, target);
    expect_newline();
    return n;
}

static int later_command(void) {
    char *later[] = {"gives", "shared", "obj", "temp", "together", "danger", "by"};
    for (int i = 0; i < 7; i++) {
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
            Node *value = parse_value();
            add_kid(n, value);
            end_statement(value);
        } else {
            expect_newline();
        }
        return n;
    }
    if (is_cmd("fail")) {
        Node *n = new_node(N_FAIL, line);
        next();
        add_kid(n, parse_expr());
        expect_newline();
        return n;
    }
    if (is_cmd("try")) {
        Node *n = new_node(N_EXPR_STMT, line);
        add_kid(n, parse_value());
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
    if (is_cmd("link")) {
        fail(line, "link в начале строки подключает файл: это будет в версии 0.3", 0);
    }
    if (is_cmd("use") || is_cmd("modpub") || is_cmd("modpri")) {
        fail(line, "use, modpub и modpri пишутся снаружи функций, в начале строки", 0);
    }
    if (later_command()) {
        fail(line, "эта команда будет в следующей версии", now()->text);
    }
    if (now()->kind == T_NAME) {
        return parse_name_statement();
    }
    if (now()->kind == T_COMMAND && tokens[pos + 1].kind == T_SIGN && same(tokens[pos + 1].text, "=")) {
        fail(line, "это слово занято языком, возьми другое имя", now()->text);
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

// name: type   или   link name
static Node *parse_param(int allow_link) {
    Node *p = new_node(N_PARAM, line_now());
    if (is_cmd("link")) {
        if (!allow_link) {
            fail(line_now(), "link здесь нельзя", 0);
        }
        next();
        p->is_link = 1;
        p->name = expect_name();
        return p;
    }
    p->name = expect_name();
    expect_sign(":");
    p->tsyn = parse_type();
    return p;
}

// func name(a: int, link hp) gives result: int, rest: int
static Node *parse_func(void) {
    Node *f = new_node(N_FUNC, line_now());
    expect_cmd("func");
    f->name = expect_name();
    expect_sign("(");
    Node *params = new_node(N_BLOCK, line_now());
    while (!is_sign(")")) {
        add_kid(params, parse_param(1));
        if (!is_sign(",")) {
            break;
        }
        next();
    }
    expect_sign(")");
    Node *outputs = new_node(N_BLOCK, line_now());
    if (is_cmd("gives")) {
        next();
        while (1) {
            add_kid(outputs, parse_param(0));
            if (!is_sign(",")) {
                break;
            }
            next();
        }
    }
    expect_newline();
    add_kid(f, params);
    add_kid(f, parse_block());
    add_kid(f, outputs);
    return f;
}

// obj User
//     name: text
//     age: int
static Node *parse_obj(void) {
    Node *o = new_node(N_OBJ, line_now());
    expect_cmd("obj");
    o->name = expect_name();
    if (is_cmd("by")) {
        fail(line_now(), "by columns будет в версии 0.7", 0);
    }
    expect_newline();
    if (now()->kind != T_INDENT) {
        fail(line_now(), "у obj ниже с отступом пишутся качества: name: text", 0);
    }
    next();
    while (now()->kind != T_DEDENT && now()->kind != T_END) {
        add_kid(o, parse_param(0));
        expect_newline();
    }
    next();
    return o;
}

Node *parse(void) {
    pos = 0;
    Node *program = new_node(N_PROGRAM, 1);
    while (now()->kind != T_END) {
        if (is_cmd("use")) {
            // use money  —  подключить файл money.tb рядом
            Node *u = new_node(N_USE, line_now());
            next();
            u->name = expect_name();
            expect_newline();
            add_kid(program, u);
        } else if (is_cmd("modpub") || is_cmd("modpri")) {
            int is_pub = is_cmd("modpub");
            next();
            if (!is_cmd("func")) {
                fail(line_now(), "modpub и modpri ставятся перед func", 0);
            }
            Node *f = parse_func();
            f->is_pub = is_pub;
            add_kid(program, f);
        } else if (is_cmd("func")) {
            add_kid(program, parse_func());
        } else if (is_cmd("obj")) {
            add_kid(program, parse_obj());
        } else if (is_cmd("link")) {
            fail(line_now(), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", 0);
        } else if (now()->kind == T_INDENT) {
            fail(line_now(), "лишний отступ", 0);
        } else {
            fail(line_now(), "снаружи функций можно писать только use, func и obj", what_is_here());
        }
    }
    return program;
}

// ---------- печать дерева (команда tree) ----------

char *node_kind_name(int kind) {
    char *names[] = {
        "ПРОГРАММА", "ФУНКЦИЯ", "ПАРАМЕТР", "БЛОК", "OBJ", "ТИП", "ПРИСВОИТЬ", "ИЗМЕНИТЬ",
        "ЯРЛЫК", "НЕСКОЛЬКО", "ЕСЛИ", "EACH", "COUNT", "REPEAT TIMES", "REPEAT WHILE",
        "SKIP", "STOP", "RETURN", "ВЫЗОВ-СТРОКА", "ЧИСЛО", "ДРОБЬ", "ТЕКСТ", "ДА/НЕТ", "ИМЯ",
        "СПИСОК", "ВЫЗОВ", "АРГУМЕНТ", "ОПЕРАЦИЯ", "ОДИНОЧНАЯ", "ИНДЕКС", "КАЧЕСТВО", "МЕТОД",
        "СЛОВАРЬ", "USE", "FAIL", "TRY", "ELSE ERR"
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
    if (n->is_const) {
        printf(" (const)");
    }
    if (n->is_link) {
        printf(" (link)");
    }
    printf("\n");
    if (n->tsyn != 0) {
        print_tree(n->tsyn, depth + 1);
    }
    for (int i = 0; i < n->nkids; i++) {
        print_tree(n->kids[i], depth + 1);
    }
}
