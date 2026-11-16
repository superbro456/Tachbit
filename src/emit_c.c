// Слой 4. ВЫВОД C: проверенное дерево → текст программы на C.
// Дальше обычный компилятор C (clang / gcc) делает из него машинный код.
// Имена программиста получают приставки: v_ у переменных, f_ у функций,
// чтобы не столкнуться со словами C (int, main, printf ...).
#include <stdlib.h>
#include "ast.h"

static FILE *out = 0;
static int label = 0;   // счётчик для скрытых имён циклов: tb_i1, tb_i2 ...

static char *runtime =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "\n"
    "typedef struct { long long *data; long long len; } tb_ilist;\n"
    "typedef struct { double *data; long long len; } tb_dlist;\n"
    "\n"
    "static void tb_crash(int line, const char *msg) {\n"
    "    fprintf(stderr, \"строка %d: %s\\n\", line, msg);\n"
    "    exit(1);\n"
    "}\n"
    "static const char *tb_cat(const char *a, const char *b) {\n"
    "    size_t la = strlen(a), lb = strlen(b);\n"
    "    char *s = malloc(la + lb + 1);\n"
    "    memcpy(s, a, la);\n"
    "    memcpy(s + la, b, lb + 1);\n"
    "    return s;\n"
    "}\n"
    "static const char *tb_itos(long long x) { char *s = malloc(32); snprintf(s, 32, \"%lld\", x); return s; }\n"
    "static const char *tb_dtos(double x) { char *s = malloc(40); snprintf(s, 40, \"%g\", x); return s; }\n"
    "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"
    "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"
    "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"
    "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"
    "static tb_ilist tb_ilist_make(long long n, const long long *src) {\n"
    "    tb_ilist l; l.len = n; l.data = malloc(sizeof(long long) * n);\n"
    "    memcpy(l.data, src, sizeof(long long) * n); return l;\n"
    "}\n"
    "static tb_dlist tb_dlist_make(long long n, const double *src) {\n"
    "    tb_dlist l; l.len = n; l.data = malloc(sizeof(double) * n);\n"
    "    memcpy(l.data, src, sizeof(double) * n); return l;\n"
    "}\n"
    "static tb_dlist tb_to_dlist(tb_ilist a) {\n"
    "    tb_dlist l; l.len = a.len; l.data = malloc(sizeof(double) * a.len);\n"
    "    for (long long i = 0; i < a.len; i++) l.data[i] = (double)a.data[i];\n"
    "    return l;\n"
    "}\n"
    "static long long tb_iat(tb_ilist l, long long i, int line) {\n"
    "    if (i < 0 || i >= l.len) tb_crash(line, \"номер за пределами списка\");\n"
    "    return l.data[i];\n"
    "}\n"
    "static double tb_dat(tb_dlist l, long long i, int line) {\n"
    "    if (i < 0 || i >= l.len) tb_crash(line, \"номер за пределами списка\");\n"
    "    return l.data[i];\n"
    "}\n"
    "static void tb_pi(long long x) { printf(\"%lld\", x); }\n"
    "static void tb_pd(double x) { printf(\"%g\", x); }\n"
    "static void tb_pb(int x) { printf(\"%s\", x ? \"true\" : \"false\"); }\n"
    "static void tb_ps(const char *s) { printf(\"%s\", s); }\n"
    "static void tb_pil(tb_ilist l) {\n"
    "    printf(\"[\"); for (long long i = 0; i < l.len; i++) printf(i ? \", %lld\" : \"%lld\", l.data[i]); printf(\"]\");\n"
    "}\n"
    "static void tb_pdl(tb_dlist l) {\n"
    "    printf(\"[\"); for (long long i = 0; i < l.len; i++) printf(i ? \", %g\" : \"%g\", l.data[i]); printf(\"]\");\n"
    "}\n"
    "\n";

static char *c_type(int t) {
    if (t == TY_INT) {
        return "long long";
    }
    if (t == TY_DEC) {
        return "double";
    }
    if (t == TY_BOOL) {
        return "int";
    }
    if (t == TY_TEXT) {
        return "const char *";
    }
    if (t == TY_LIST_INT) {
        return "tb_ilist";
    }
    if (t == TY_LIST_DEC) {
        return "tb_dlist";
    }
    return "void";
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) {
        fprintf(out, "    ");
    }
}

// ---------- выражения ----------

static void emit_expr(Node *n);

// значение типа from там, где нужен тип to (int → dec, список int → список dec)
static void emit_as(Node *n, int to) {
    if (to == TY_LIST_DEC && n->type == TY_LIST_INT) {
        fprintf(out, "tb_to_dlist(");
        emit_expr(n);
        fprintf(out, ")");
        return;
    }
    if (to == TY_DEC && n->type == TY_INT) {
        fprintf(out, "(double)(");
        emit_expr(n);
        fprintf(out, ")");
        return;
    }
    emit_expr(n);
}

// любое значение как текст (для  "итог: " + x)
static void emit_as_text(Node *n) {
    if (n->type == TY_TEXT) {
        emit_expr(n);
        return;
    }
    if (n->type == TY_INT) {
        fprintf(out, "tb_itos(");
    } else if (n->type == TY_DEC) {
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
        int t = value->type;
        if (i > 0) {
            fprintf(out, ", tb_ps(\" \"), ");
        }
        if (t == TY_INT) {
            fprintf(out, "tb_pi(");
        } else if (t == TY_DEC) {
            fprintf(out, "tb_pd(");
        } else if (t == TY_BOOL) {
            fprintf(out, "tb_pb(");
        } else if (t == TY_TEXT) {
            fprintf(out, "tb_ps(");
        } else if (t == TY_LIST_INT) {
            fprintf(out, "tb_pil(");
        } else {
            fprintf(out, "tb_pdl(");
        }
        emit_expr(value);
        fprintf(out, ")");
    }
    if (n->nkids > 0) {
        fprintf(out, ", ");
    }
    fprintf(out, "tb_ps(\"\\n\"))");
}

static void emit_call(Node *n) {
    if (same(n->name, "print")) {
        emit_print(n);
        return;
    }
    if (same(n->name, "len")) {
        Node *value = n->kids[0]->kids[0];
        if (value->type == TY_TEXT) {
            fprintf(out, "(long long)strlen(");
            emit_expr(value);
            fprintf(out, ")");
        } else {
            fprintf(out, "(");
            emit_expr(value);
            fprintf(out, ").len");
        }
        return;
    }
    if (same(n->name, "round")) {
        fprintf(out, "tb_round(");
        emit_expr(n->kids[0]->kids[0]);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "f_%s(", n->name);
    for (int i = 0; i < n->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        emit_as(n->kids[i]->kids[0], n->kids[i]->type);
    }
    fprintf(out, ")");
}

static void emit_list(Node *n) {
    char *maker = "tb_ilist_make";
    char *elem = "long long";
    int elem_type = TY_INT;
    if (n->type == TY_LIST_DEC) {
        maker = "tb_dlist_make";
        elem = "double";
        elem_type = TY_DEC;
    }
    fprintf(out, "%s(%d, (%s[]){", maker, n->nkids, elem);
    for (int i = 0; i < n->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        emit_as(n->kids[i], elem_type);
    }
    fprintf(out, "})");
}

static void emit_binop(Node *n) {
    char *op = n->value;
    Node *a = n->kids[0];
    Node *b = n->kids[1];
    if (n->type == TY_TEXT) {
        fprintf(out, "tb_cat(");
        emit_as_text(a);
        fprintf(out, ", ");
        emit_as_text(b);
        fprintf(out, ")");
        return;
    }
    if (a->type == TY_TEXT && (same(op, "==") || same(op, "!="))) {
        fprintf(out, "(strcmp(");
        emit_expr(a);
        fprintf(out, ", ");
        emit_expr(b);
        fprintf(out, ") %s 0)", op);
        return;
    }
    if (n->type == TY_INT && (same(op, "/") || same(op, "%"))) {
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
    if (n->type == TY_DEC && same(op, "/")) {
        emit_as(a, TY_DEC);
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
        if (same(n->value, "true")) {
            fprintf(out, "1");
        } else {
            fprintf(out, "0");
        }
    } else if (k == N_NAME) {
        fprintf(out, "v_%s", n->name);
    } else if (k == N_LIST) {
        emit_list(n);
    } else if (k == N_CALL) {
        emit_call(n);
    } else if (k == N_BINOP) {
        emit_binop(n);
    } else if (k == N_UNARY) {
        if (same(n->value, "not")) {
            fprintf(out, "(!");
        } else {
            fprintf(out, "(-");
        }
        emit_expr(n->kids[0]);
        fprintf(out, ")");
    } else if (k == N_INDEX) {
        if (n->type == TY_DEC) {
            fprintf(out, "tb_dat(");
        } else {
            fprintf(out, "tb_iat(");
        }
        emit_expr(n->kids[0]);
        fprintf(out, ", ");
        emit_expr(n->kids[1]);
        fprintf(out, ", %d)", n->line);
    }
}

// ---------- команды ----------

static void emit_block(Node *block, int depth);

static void emit_assign(Node *n, int depth) {
    indent(depth);
    if (n->creates) {
        fprintf(out, "%s ", c_type(n->type));
        if (n->is_const) {
            fprintf(out, "const ");
        }
        fprintf(out, "v_%s = ", n->name);
    } else {
        fprintf(out, "v_%s = ", n->name);
    }
    emit_as(n->kids[0], n->type);
    fprintf(out, ";\n");
}

static void emit_opassign(Node *n, int depth) {
    Node *value = n->kids[0];
    indent(depth);
    if (n->type == TY_TEXT) {
        fprintf(out, "v_%s = tb_cat(v_%s, ", n->name, n->name);
        emit_as_text(value);
        fprintf(out, ");\n");
        return;
    }
    if (n->type == TY_INT && same(n->value, "/=")) {
        fprintf(out, "v_%s = tb_div(v_%s, ", n->name, n->name);
        emit_expr(value);
        fprintf(out, ", %d);\n", n->line);
        return;
    }
    fprintf(out, "v_%s %s ", n->name, n->value);
    emit_expr(value);
    fprintf(out, ";\n");
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
    int list_type = n->kids[0]->type;
    char *elem = "long long";
    if (list_type == TY_LIST_DEC) {
        elem = "double";
    }
    indent(depth);
    fprintf(out, "{\n");
    indent(depth + 1);
    fprintf(out, "%s tb_l%d = ", c_type(list_type), label);
    emit_expr(n->kids[0]);
    fprintf(out, ";\n");
    indent(depth + 1);
    fprintf(out, "for (long long tb_i%d = 0; tb_i%d < tb_l%d.len; tb_i%d++) {\n", label, label, label, label);
    indent(depth + 2);
    fprintf(out, "const %s v_%s = tb_l%d.data[tb_i%d];\n", elem, n->name, label, label);
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

static void emit_statement(Node *n, int depth) {
    int k = n->kind;
    if (k == N_ASSIGN) {
        emit_assign(n, depth);
    } else if (k == N_OPASSIGN) {
        emit_opassign(n, depth);
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
        indent(depth);
        if (n->nkids == 1) {
            fprintf(out, "return ");
            emit_expr(n->kids[0]);
            fprintf(out, ";\n");
        } else {
            fprintf(out, "return;\n");
        }
    } else if (k == N_EXPR_STMT) {
        indent(depth);
        emit_expr(n->kids[0]);
        fprintf(out, ";\n");
    }
}

static void emit_block(Node *block, int depth) {
    for (int i = 0; i < block->nkids; i++) {
        emit_statement(block->kids[i], depth);
    }
}

static void emit_signature(Node *f) {
    Node *params = f->kids[0];
    fprintf(out, "static %s f_%s(", c_type(f->type), f->name);
    if (params->nkids == 0) {
        fprintf(out, "void");
    }
    for (int i = 0; i < params->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        fprintf(out, "%s v_%s", c_type(params->kids[i]->declared), params->kids[i]->name);
    }
    fprintf(out, ")");
}

void emit_c(Node *program, FILE *file) {
    out = file;
    label = 0;
    fprintf(out, "// Сделано компилятором Tachbit 0.1. Не правь руками: правь файл .tb\n");
    fprintf(out, "%s", runtime);
    for (int i = 0; i < program->nkids; i++) {
        emit_signature(program->kids[i]);
        fprintf(out, ";\n");
    }
    for (int i = 0; i < program->nkids; i++) {
        Node *f = program->kids[i];
        fprintf(out, "\n");
        emit_signature(f);
        fprintf(out, " {\n");
        emit_block(f->kids[1], 1);
        fprintf(out, "}\n");
    }
    fprintf(out, "\nint main(void) {\n    f_main();\n    return 0;\n}\n");
}
