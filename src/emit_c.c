// Слой 4. ВЫВОД C: проверенное дерево → текст программы на C.
// Дальше обычный компилятор C (clang / gcc) делает из него машинный код.
// Имена программиста получают приставки: v_ у переменных, f_ у функций,
// tb_o_ у obj, q_ у качеств obj, чтобы не столкнуться со словами C (int, main, printf ...).
#include <stdlib.h>
#include <string.h>
#include "ast.h"

static FILE *out = 0;
static int label = 0;        // счётчик для скрытых имён: tb_i1, tb_i2 ...
static Node *cur_func = 0;

static char *runtime =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "\n"
    "// список: кусок памяти, len элементов по size байт, место на cap\n"
    "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"
    "\n"
    "static void tb_crash(int line, const char *msg) {\n"
    "    fflush(stdout);\n"
    "    fprintf(stderr, \"строка %d: %s\\n\", line, msg);\n"
    "    exit(1);\n"
    "}\n"
    "static void *tb_mem(long long n) {\n"
    "    void *p = malloc(n > 0 ? n : 1);\n"
    "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"
    "    return p;\n"
    "}\n"
    "static tb_list *tb_list_new(long long size) {\n"
    "    tb_list *l = tb_mem(sizeof(tb_list));\n"
    "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"
    "    return l;\n"
    "}\n"
    "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"
    "    tb_list *l = tb_mem(sizeof(tb_list));\n"
    "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"
    "    memcpy(l->data, src, size * n);\n"
    "    return l;\n"
    "}\n"
    "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"
    "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"
    "    if (i < 0 || i >= l->len) tb_crash(line, \"номер за пределами списка\");\n"
    "    return l->data + i * size;\n"
    "}\n"
    "static void *tb_grow(tb_list *l) {\n"
    "    if (l->len == l->cap) {\n"
    "        l->cap = l->cap * 2 + 4;\n"
    "        char *bigger = tb_mem(l->cap * l->size);\n"
    "        memcpy(bigger, l->data, l->len * l->size);\n"
    "        free(l->data);\n"
    "        l->data = bigger;\n"
    "    }\n"
    "    l->len += 1;\n"
    "    return l->data + (l->len - 1) * l->size;\n"
    "}\n"
    "static void *tb_pop(tb_list *l, int line) {\n"
    "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"
    "    l->len -= 1;\n"
    "    return l->data + l->len * l->size;\n"
    "}\n"
    "static void *tb_nn(void *p, int line) {\n"
    "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"
    "    return p;\n"
    "}\n"
    "static const char *tb_cat(const char *a, const char *b) {\n"
    "    size_t la = strlen(a), lb = strlen(b);\n"
    "    char *s = tb_mem(la + lb + 1);\n"
    "    memcpy(s, a, la);\n"
    "    memcpy(s + la, b, lb + 1);\n"
    "    return s;\n"
    "}\n"
    "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"
    "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"
    "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"
    "static const char *tb_chr(long long c) { char *s = tb_mem(2); s[0] = (char)c; s[1] = 0; return s; }\n"
    "static const char *tb_char_at(const char *s, long long i, int line) {\n"
    "    if (i < 0 || i >= (long long)strlen(s)) tb_crash(line, \"номер за пределами текста\");\n"
    "    return tb_chr((unsigned char)s[i]);\n"
    "}\n"
    "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"
    "    long long n = (long long)strlen(s);\n"
    "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"
    "    char *r = tb_mem(to - from + 2);\n"
    "    memcpy(r, s + from, to - from + 1);\n"
    "    r[to - from + 1] = 0;\n"
    "    return r;\n"
    "}\n"
    "static long long tb_code(const char *s, int line) {\n"
    "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"
    "    return (unsigned char)s[0];\n"
    "}\n"
    "static long long tb_to_int(const char *s, int line) {\n"
    "    long long v = 0; int i = 0, minus = 0;\n"
    "    if (s[0] == '-') { minus = 1; i = 1; }\n"
    "    if (s[i] == 0) tb_crash(line, \"to_int: это не число\");\n"
    "    for (; s[i]; i++) {\n"
    "        if (s[i] < '0' || s[i] > '9') tb_crash(line, \"to_int: это не число\");\n"
    "        v = v * 10 + (s[i] - '0');\n"
    "    }\n"
    "    return minus ? -v : v;\n"
    "}\n"
    "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"
    "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"
    "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"
    "static void tb_ps(const char *s) { fputs(s, stdout); }\n"
    "\n";

// ---------- типы C ----------

static char *join(char *a, char *b, char *c) {
    char *s = arena_alloc(strlen(a) + strlen(b) + strlen(c) + 1);
    strcpy(s, a);
    strcat(s, b);
    strcat(s, c);
    return s;
}

static char *c_type(Type *t) {
    int k = t->kind;
    if (k == TY_INT) {
        return "long long";
    }
    if (k == TY_DEC) {
        return "double";
    }
    if (k == TY_BOOL) {
        return "unsigned char";
    }
    if (k == TY_TEXT) {
        return "const char *";
    }
    if (k == TY_LIST) {
        return "tb_list *";
    }
    if (k == TY_OBJ) {
        return join("tb_o_", t->obj->name, " *");
    }
    if (k == TY_MULTI) {
        return join("tb_r_", t->func->name, "");
    }
    return "void";
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) {
        fprintf(out, "    ");
    }
}

static void emit_default(Type *t) {
    int k = t->kind;
    if (k == TY_INT) {
        fprintf(out, "0LL");
    } else if (k == TY_DEC) {
        fprintf(out, "0.0");
    } else if (k == TY_TEXT) {
        fprintf(out, "\"\"");
    } else if (k == TY_LIST) {
        fprintf(out, "tb_list_new(sizeof(%s))", c_type(t->elem));
    } else {
        fprintf(out, "0");
    }
}

// ---------- выражения ----------

static void emit_expr(Node *n);

// значение там, где нужен тип to (int → dec)
static void emit_as(Node *n, Type *to) {
    if (to->kind == TY_DEC && n->ty->kind == TY_INT) {
        fprintf(out, "(double)(");
        emit_expr(n);
        fprintf(out, ")");
        return;
    }
    emit_expr(n);
}

// любое простое значение как текст (для  "итог: " + x)
static void emit_as_text(Node *n) {
    int k = n->ty->kind;
    if (k == TY_TEXT) {
        emit_expr(n);
        return;
    }
    if (k == TY_INT) {
        fprintf(out, "tb_itos(");
    } else if (k == TY_DEC) {
        fprintf(out, "tb_dtos(");
    } else {
        fprintf(out, "tb_btos(");
    }
    emit_expr(n);
    fprintf(out, ")");
}

static void emit_print(Node *n) {
    fprintf(out, "(");
    for (int i = 0; i < n->nkids; i++) {
        Node *value = n->kids[i]->kids[0];
        if (i > 0) {
            fprintf(out, ", tb_ps(\" \"), ");
        }
        fprintf(out, "tb_show_%d(", value->ty->id);
        emit_expr(value);
        fprintf(out, ")");
    }
    if (n->nkids > 0) {
        fprintf(out, ", ");
    }
    fprintf(out, "tb_ps(\"\\n\"))");
}

static void emit_builtin(Node *n) {
    char *name = n->name;
    if (same(name, "print")) {
        emit_print(n);
        return;
    }
    Node *a = n->kids[0]->kids[0];
    if (same(name, "len")) {
        if (a->ty->kind == TY_TEXT) {
            fprintf(out, "(long long)strlen(");
            emit_expr(a);
            fprintf(out, ")");
        } else {
            fprintf(out, "(");
            emit_expr(a);
            fprintf(out, ")->len");
        }
    } else if (same(name, "round")) {
        fprintf(out, "tb_round(");
        emit_expr(a);
        fprintf(out, ")");
    } else if (same(name, "char")) {
        fprintf(out, "tb_chr(");
        emit_expr(a);
        fprintf(out, ")");
    } else if (same(name, "cut")) {
        fprintf(out, "tb_cut(");
        emit_expr(a);
        fprintf(out, ", ");
        emit_expr(n->kids[1]->kids[0]);
        fprintf(out, ", ");
        emit_expr(n->kids[2]->kids[0]);
        fprintf(out, ", %d)", n->line);
    } else {
        // code и to_int
        if (same(name, "code")) {
            fprintf(out, "tb_code(");
        } else {
            fprintf(out, "tb_to_int(");
        }
        emit_expr(a);
        fprintf(out, ", %d)", n->line);
    }
}

static void emit_call(Node *n) {
    if (n->ref == 0) {
        emit_builtin(n);
        return;
    }
    if (n->ref->kind == N_OBJ) {
        Node *obj = n->ref;
        fprintf(out, "tb_new_%s(", obj->name);
        for (int i = 0; i < obj->nkids; i++) {
            if (i > 0) {
                fprintf(out, ", ");
            }
            if (n->kids[i] == 0) {
                emit_default(obj->kids[i]->ty);
            } else {
                emit_as(n->kids[i]->kids[0], obj->kids[i]->ty);
            }
        }
        fprintf(out, ")");
        return;
    }
    Node *params = n->ref->kids[0];
    fprintf(out, "f_%s(", n->name);
    for (int i = 0; i < n->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        if (params->kids[i]->is_link) {
            fprintf(out, "&(");
            emit_expr(n->kids[i]->kids[0]);
            fprintf(out, ")");
        } else {
            emit_as(n->kids[i]->kids[0], params->kids[i]->ty);
        }
    }
    fprintf(out, ")");
}

static void emit_list(Node *n) {
    char *elem = c_type(n->ty->elem);
    fprintf(out, "tb_list_from(%d, sizeof(%s), (%s[]){", n->nkids, elem, elem);
    for (int i = 0; i < n->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        emit_as(n->kids[i], n->ty->elem);
    }
    fprintf(out, "})");
}

static void emit_binop(Node *n) {
    char *op = n->value;
    Node *a = n->kids[0];
    Node *b = n->kids[1];
    if (n->ty->kind == TY_TEXT) {
        fprintf(out, "tb_cat(");
        emit_as_text(a);
        fprintf(out, ", ");
        emit_as_text(b);
        fprintf(out, ")");
        return;
    }
    if (a->ty->kind == TY_TEXT) {
        fprintf(out, "(strcmp(");
        emit_expr(a);
        fprintf(out, ", ");
        emit_expr(b);
        fprintf(out, ") %s 0)", op);
        return;
    }
    if (n->ty->kind == TY_INT && (same(op, "/") || same(op, "%"))) {
        if (same(op, "/")) {
            fprintf(out, "tb_div(");
        } else {
            fprintf(out, "tb_mod(");
        }
        emit_expr(a);
        fprintf(out, ", ");
        emit_expr(b);
        fprintf(out, ", %d)", n->line);
        return;
    }
    char *c_op = op;
    if (same(op, "and")) {
        c_op = "&&";
    } else if (same(op, "or")) {
        c_op = "||";
    }
    fprintf(out, "(");
    if (n->ty->kind == TY_DEC && same(op, "/")) {
        emit_as(a, n->ty);
    } else {
        emit_expr(a);
    }
    fprintf(out, " %s ", c_op);
    emit_expr(b);
    fprintf(out, ")");
}

static void emit_expr(Node *n) {
    int k = n->kind;
    if (k == N_NUMBER) {
        fprintf(out, "%sLL", n->value);
    } else if (k == N_DECIMAL || k == N_STRING) {
        fprintf(out, "%s", n->value);
    } else if (k == N_BOOL) {
        fprintf(out, "%d", same(n->value, "true"));
    } else if (k == N_NAME) {
        if (n->is_link) {
            fprintf(out, "(*v_%s)", n->name);
        } else {
            fprintf(out, "v_%s", n->name);
        }
    } else if (k == N_LIST) {
        emit_list(n);
    } else if (k == N_CALL) {
        emit_call(n);
    } else if (k == N_METHOD) {
        // .pop(); .add пишется отдельной строкой (emit_statement)
        fprintf(out, "(*(%s *)tb_pop(", c_type(n->ty));
        emit_expr(n->kids[0]);
        fprintf(out, ", %d))", n->line);
    } else if (k == N_BINOP) {
        emit_binop(n);
    } else if (k == N_UNARY) {
        fprintf(out, "(%s", same(n->value, "not") ? "!" : "-");
        emit_expr(n->kids[0]);
        fprintf(out, ")");
    } else if (k == N_INDEX) {
        if (n->kids[0]->ty->kind == TY_TEXT) {
            fprintf(out, "tb_char_at(");
            emit_expr(n->kids[0]);
            fprintf(out, ", ");
            emit_expr(n->kids[1]);
            fprintf(out, ", %d)", n->line);
        } else {
            char *t = c_type(n->ty);
            fprintf(out, "(*(%s *)tb_at(", t);
            emit_expr(n->kids[0]);
            fprintf(out, ", ");
            emit_expr(n->kids[1]);
            fprintf(out, ", sizeof(%s), %d))", t, n->line);
        }
    } else if (k == N_FIELD) {
        fprintf(out, "((%s)tb_nn(", c_type(n->kids[0]->ty));
        emit_expr(n->kids[0]);
        fprintf(out, ", %d))->q_%s", n->line, n->name);
    }
}

// ---------- команды ----------

static void emit_block(Node *block, int depth);

static void emit_assign(Node *n, int depth) {
    indent(depth);
    if (n->creates) {
        fprintf(out, "%s ", c_type(n->ty));
        if (n->is_const) {
            fprintf(out, "const ");
        }
        fprintf(out, "v_%s = ", n->name);
    } else if (n->is_link) {
        fprintf(out, "(*v_%s) = ", n->name);
    } else {
        fprintf(out, "v_%s = ", n->name);
    }
    if (n->nkids == 1) {
        emit_as(n->kids[0], n->ty);
    } else {
        emit_default(n->ty);
    }
    fprintf(out, ";\n");
}

static void emit_set(Node *n, int depth) {
    Node *target = n->kids[0];
    Node *value = n->kids[1];
    char *op = n->value;
    indent(depth);
    emit_expr(target);
    if (same(op, "=")) {
        fprintf(out, " = ");
        emit_as(value, n->ty);
    } else if (n->ty->kind == TY_TEXT) {
        fprintf(out, " = tb_cat(");
        emit_expr(target);
        fprintf(out, ", ");
        emit_as_text(value);
        fprintf(out, ")");
    } else if (n->ty->kind == TY_INT && same(op, "/=")) {
        fprintf(out, " = tb_div(");
        emit_expr(target);
        fprintf(out, ", ");
        emit_expr(value);
        fprintf(out, ", %d)", n->line);
    } else {
        fprintf(out, " %s ", op);
        emit_expr(value);
    }
    fprintf(out, ";\n");
}

static void emit_multi(Node *n, int depth) {
    int names = n->nkids - 1;
    Node *call = n->kids[names];
    label += 1;
    for (int i = 0; i < names; i++) {
        if (n->kids[i]->creates) {
            indent(depth);
            fprintf(out, "%s v_%s;\n", c_type(n->kids[i]->ty), n->kids[i]->name);
        }
    }
    indent(depth);
    fprintf(out, "{\n");
    indent(depth + 1);
    fprintf(out, "%s tb_m%d = ", c_type(call->ty), label);
    emit_expr(call);
    fprintf(out, ";\n");
    for (int i = 0; i < names; i++) {
        indent(depth + 1);
        emit_expr(n->kids[i]);
        fprintf(out, " = tb_m%d.o%d;\n", label, i);
    }
    indent(depth);
    fprintf(out, "}\n");
}

static void emit_if(Node *n, int depth) {
    indent(depth);
    for (int i = 0; i + 1 < n->nkids; i += 2) {
        if (i > 0) {
            fprintf(out, " else ");
        }
        fprintf(out, "if (");
        emit_expr(n->kids[i]);
        fprintf(out, ") {\n");
        emit_block(n->kids[i + 1], depth + 1);
        indent(depth);
        fprintf(out, "}");
    }
    if (n->nkids % 2 == 1) {
        fprintf(out, " else {\n");
        emit_block(n->kids[n->nkids - 1], depth + 1);
        indent(depth);
        fprintf(out, "}");
    }
    fprintf(out, "\n");
}

static void emit_each(Node *n, int depth) {
    label += 1;
    int id = label;
    Type *t = n->kids[0]->ty;
    indent(depth);
    fprintf(out, "{\n");
    indent(depth + 1);
    fprintf(out, "%s tb_l%d = ", c_type(t), id);
    emit_expr(n->kids[0]);
    fprintf(out, ";\n");
    indent(depth + 1);
    if (t->kind == TY_TEXT) {
        fprintf(out, "for (long long tb_i%d = 0; tb_l%d[tb_i%d] != 0; tb_i%d++) {\n", id, id, id, id);
        indent(depth + 2);
        fprintf(out, "const char * const v_%s = tb_chr((unsigned char)tb_l%d[tb_i%d]);\n", n->name, id, id);
    } else {
        char *elem = c_type(t->elem);
        fprintf(out, "for (long long tb_i%d = 0; tb_i%d < tb_l%d->len; tb_i%d++) {\n", id, id, id, id);
        indent(depth + 2);
        fprintf(out, "%s const v_%s = ((%s *)tb_l%d->data)[tb_i%d];\n", elem, n->name, elem, id, id);
    }
    emit_block(n->kids[1], depth + 2);
    indent(depth + 1);
    fprintf(out, "}\n");
    indent(depth);
    fprintf(out, "}\n");
}

static void emit_count(Node *n, int depth) {
    label += 1;
    indent(depth);
    fprintf(out, "{\n");
    indent(depth + 1);
    fprintf(out, "long long tb_end%d = ", label);
    emit_expr(n->kids[1]);
    fprintf(out, ";\n");
    indent(depth + 1);
    fprintf(out, "for (long long v_%s = ", n->name);
    emit_expr(n->kids[0]);
    fprintf(out, "; v_%s <= tb_end%d; v_%s++) {\n", n->name, label, n->name);
    emit_block(n->kids[2], depth + 2);
    indent(depth + 1);
    fprintf(out, "}\n");
    indent(depth);
    fprintf(out, "}\n");
}

// return у функции с gives: отдать выходы
static void emit_gives_return(int depth) {
    Node *outputs = cur_func->kids[2];
    indent(depth);
    if (outputs->nkids == 1) {
        fprintf(out, "return v_%s;\n", outputs->kids[0]->name);
        return;
    }
    fprintf(out, "return (%s){", c_type(cur_func->ty));
    for (int i = 0; i < outputs->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        fprintf(out, "v_%s", outputs->kids[i]->name);
    }
    fprintf(out, "};\n");
}

static void emit_statement(Node *n, int depth) {
    int k = n->kind;
    if (k == N_ASSIGN) {
        emit_assign(n, depth);
    } else if (k == N_SET) {
        emit_set(n, depth);
    } else if (k == N_LINKDECL) {
        indent(depth);
        fprintf(out, "%s *v_%s = &(", c_type(n->ty), n->name);
        emit_expr(n->kids[0]);
        fprintf(out, ");\n");
    } else if (k == N_MULTI) {
        emit_multi(n, depth);
    } else if (k == N_IF) {
        emit_if(n, depth);
    } else if (k == N_EACH) {
        emit_each(n, depth);
    } else if (k == N_COUNT) {
        emit_count(n, depth);
    } else if (k == N_REPEAT_TIMES) {
        label += 1;
        indent(depth);
        fprintf(out, "for (long long tb_r%d = 0, tb_n%d = ", label, label);
        emit_expr(n->kids[0]);
        fprintf(out, "; tb_r%d < tb_n%d; tb_r%d++) {\n", label, label, label);
        emit_block(n->kids[1], depth + 1);
        indent(depth);
        fprintf(out, "}\n");
    } else if (k == N_REPEAT_WHILE) {
        indent(depth);
        fprintf(out, "while (");
        emit_expr(n->kids[0]);
        fprintf(out, ") {\n");
        emit_block(n->kids[1], depth + 1);
        indent(depth);
        fprintf(out, "}\n");
    } else if (k == N_SKIP) {
        indent(depth);
        fprintf(out, "continue;\n");
    } else if (k == N_STOP) {
        indent(depth);
        fprintf(out, "break;\n");
    } else if (k == N_RETURN) {
        if (cur_func->kids[2]->nkids > 0) {
            emit_gives_return(depth);
        } else if (n->nkids == 1) {
            indent(depth);
            fprintf(out, "return ");
            emit_as(n->kids[0], cur_func->ty);
            fprintf(out, ";\n");
        } else {
            indent(depth);
            fprintf(out, "return;\n");
        }
    } else if (k == N_EXPR_STMT) {
        Node *e = n->kids[0];
        indent(depth);
        if (e->kind == N_METHOD) {
            // list.add(x): сначала значение, потом место под него
            Type *elem = e->kids[0]->ty->elem;
            label += 1;
            fprintf(out, "{ %s tb_v%d = ", c_type(elem), label);
            emit_as(e->kids[1]->kids[0], elem);
            fprintf(out, "; *(%s *)tb_grow(", c_type(elem));
            emit_expr(e->kids[0]);
            fprintf(out, ") = tb_v%d; }\n", label);
        } else {
            emit_expr(e);
            fprintf(out, ";\n");
        }
    }
}

static void emit_block(Node *block, int depth) {
    for (int i = 0; i < block->nkids; i++) {
        emit_statement(block->kids[i], depth);
    }
}

// ---------- объявления верхнего уровня ----------

static void emit_signature(Node *f) {
    Node *params = f->kids[0];
    fprintf(out, "static %s f_%s(", c_type(f->ty), f->name);
    if (params->nkids == 0) {
        fprintf(out, "void");
    }
    for (int i = 0; i < params->nkids; i++) {
        Node *p = params->kids[i];
        if (i > 0) {
            fprintf(out, ", ");
        }
        if (p->is_link) {
            fprintf(out, "%s *v_%s", c_type(p->ty), p->name);
        } else {
            fprintf(out, "%s v_%s", c_type(p->ty), p->name);
        }
    }
    fprintf(out, ")");
}

static void emit_func(Node *f) {
    cur_func = f;
    Node *outputs = f->kids[2];
    fprintf(out, "\n");
    emit_signature(f);
    fprintf(out, " {\n");
    for (int i = 0; i < outputs->nkids; i++) {
        Node *o = outputs->kids[i];
        if (!o->is_const) {
            fprintf(out, "    %s v_%s = ", c_type(o->ty), o->name);
            emit_default(o->ty);
            fprintf(out, ";\n");
        }
    }
    emit_block(f->kids[1], 1);
    if (outputs->nkids > 0) {
        emit_gives_return(1);
    } else if (f->ty->kind != TY_NONE) {
        fprintf(out, "    tb_crash(%d, \"функция %s дошла до конца без return\");\n", f->line, f->name);
        fprintf(out, "    return ");
        emit_default(f->ty);
        fprintf(out, ";\n");
    }
    fprintf(out, "}\n");
}

// tb_show_N: печать значения типа N (для print)
static void emit_show(Type *t) {
    int k = t->kind;
    if (k == TY_NONE || k == TY_MULTI || k == TY_UNKNOWN) {
        return;
    }
    fprintf(out, "static void tb_show_%d(%s x) {\n", t->id, c_type(t));
    if (k == TY_INT) {
        fprintf(out, "    printf(\"%%lld\", x);\n");
    } else if (k == TY_DEC) {
        fprintf(out, "    printf(\"%%g\", x);\n");
    } else if (k == TY_BOOL) {
        fprintf(out, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if (k == TY_TEXT) {
        fprintf(out, "    tb_ps(x);\n");
    } else if (k == TY_LIST) {
        char *elem = c_type(t->elem);
        fprintf(out, "    tb_ps(\"[\");\n");
        fprintf(out, "    for (long long i = 0; i < x->len; i++) {\n");
        fprintf(out, "        if (i > 0) tb_ps(\", \");\n");
        fprintf(out, "        tb_show_%d(((%s *)x->data)[i]);\n", t->elem->id, elem);
        fprintf(out, "    }\n");
        fprintf(out, "    tb_ps(\"]\");\n");
    } else if (k == TY_OBJ) {
        Node *obj = t->obj;
        fprintf(out, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        fprintf(out, "    tb_ps(\"%s(\");\n", obj->name);
        for (int i = 0; i < obj->nkids; i++) {
            fprintf(out, "    tb_ps(\"%s%s: \");\n", i > 0 ? ", " : "", obj->kids[i]->name);
            fprintf(out, "    tb_show_%d(x->q_%s);\n", obj->kids[i]->ty->id, obj->kids[i]->name);
        }
        fprintf(out, "    tb_ps(\")\");\n");
    }
    fprintf(out, "}\n");
}

static void emit_obj(Node *obj) {
    fprintf(out, "struct tb_o_%s {\n", obj->name);
    for (int i = 0; i < obj->nkids; i++) {
        fprintf(out, "    %s q_%s;\n", c_type(obj->kids[i]->ty), obj->kids[i]->name);
    }
    if (obj->nkids == 0) {
        fprintf(out, "    char empty;\n");
    }
    fprintf(out, "};\n");
}

static void emit_constructor(Node *obj) {
    fprintf(out, "static tb_o_%s *tb_new_%s(", obj->name, obj->name);
    if (obj->nkids == 0) {
        fprintf(out, "void");
    }
    for (int i = 0; i < obj->nkids; i++) {
        fprintf(out, "%s%s a%d", i > 0 ? ", " : "", c_type(obj->kids[i]->ty), i);
    }
    fprintf(out, ") {\n");
    fprintf(out, "    tb_o_%s *o = tb_mem(sizeof(tb_o_%s));\n", obj->name, obj->name);
    for (int i = 0; i < obj->nkids; i++) {
        fprintf(out, "    o->q_%s = a%d;\n", obj->kids[i]->name, i);
    }
    fprintf(out, "    return o;\n}\n");
}

void emit_c(Node *program, FILE *file) {
    out = file;
    label = 0;
    fprintf(out, "// Сделано компилятором Tachbit 0.2. Не правь руками: правь файл .tb\n");
    fprintf(out, "%s", runtime);

    // obj: сначала имена, потом устройство (obj может хранить другой obj)
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        if (top->kind == N_OBJ) {
            fprintf(out, "typedef struct tb_o_%s tb_o_%s;\n", top->name, top->name);
        }
    }
    for (int i = 0; i < program->nkids; i++) {
        if (program->kids[i]->kind == N_OBJ) {
            emit_obj(program->kids[i]);
        }
    }
    // выходы gives: структура на функцию
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        if (top->kind == N_FUNC && top->kids[2]->nkids > 1) {
            fprintf(out, "typedef struct {");
            for (int j = 0; j < top->kids[2]->nkids; j++) {
                fprintf(out, " %s o%d;", c_type(top->kids[2]->kids[j]->ty), j);
            }
            fprintf(out, " } tb_r_%s;\n", top->name);
        }
    }
    for (int i = 0; i < type_count; i++) {
        int k = all_types[i]->kind;
        if (k != TY_NONE && k != TY_MULTI && k != TY_UNKNOWN) {
            fprintf(out, "static void tb_show_%d(%s x);\n", all_types[i]->id, c_type(all_types[i]));
        }
    }
    for (int i = 0; i < program->nkids; i++) {
        if (program->kids[i]->kind == N_OBJ) {
            emit_constructor(program->kids[i]);
        }
    }
    for (int i = 0; i < program->nkids; i++) {
        if (program->kids[i]->kind == N_FUNC) {
            emit_signature(program->kids[i]);
            fprintf(out, ";\n");
        }
    }
    fprintf(out, "\n");
    for (int i = 0; i < type_count; i++) {
        emit_show(all_types[i]);
    }
    for (int i = 0; i < program->nkids; i++) {
        if (program->kids[i]->kind == N_FUNC) {
            emit_func(program->kids[i]);
        }
    }
    fprintf(out, "\nint main(void) {\n    f_main();\n    return 0;\n}\n");
}
