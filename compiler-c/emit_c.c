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

extern char *runtime_text;   // src/runtime_text.c (из src/runtime.h)

// ---------- типы C ----------

static char *join(char *a, char *b, char *c) {
    char *s = arena_alloc(strlen(a) + strlen(b) + strlen(c) + 1);
    strcpy(s, a);
    strcat(s, b);
    strcat(s, c);
    return s;
}

// имя функции в C: f_<модуль>__<имя>, чтобы две modpri-функции из разных файлов не столкнулись
static char *fname(Node *f) {
    char *mod = arena_copy(f->mod, (int)strlen(f->mod));
    for (int i = 0; mod[i] != 0; i++) {
        char c = mod[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) {
            mod[i] = '_';
        }
    }
    return join(join("f_", mod, "__"), f->name, "");
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
        return "_Bool";
    }
    if (k == TY_TEXT) {
        return "const char *";
    }
    if (k == TY_LIST) {
        return "tb_list *";
    }
    if (k == TY_DICT) {
        return "tb_dict *";
    }
    if (k == TY_OBJ) {
        return join("tb_o_", t->obj->name, " *");
    }
    if (k == TY_MULTI) {
        return join("tb_r_", fname(t->func), "");
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
    } else if (k == TY_DICT) {
        fprintf(out, "tb_dict_new(%d, sizeof(%s))", t->key->kind == TY_TEXT, c_type(t->elem));
    } else {
        fprintf(out, "0");
    }
}

static void emit_expr(Node *n);

static int is_simple(Type *t) {
    int k = t->kind;
    return k == TY_INT || k == TY_DEC || k == TY_BOOL || k == TY_TEXT;
}

// &(K){ключ} — ключ dict как адрес
static void emit_key(Type *dict, Node *key) {
    fprintf(out, "&(%s){", c_type(dict->key));
    emit_expr(key);
    fprintf(out, "}");
}

// ---------- выражения ----------

static void emit_expr(Node *n);
static void emit_as(Node *n, Type *to);

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

// встроенные функции: имя → функция из runtime.h и её аргументы
static void emit_args(Node *n, int with_line) {
    for (int i = 0; i < n->nkids; i++) {
        if (i > 0) {
            fprintf(out, ", ");
        }
        emit_expr(n->kids[i]->kids[0]);
    }
    if (with_line) {
        fprintf(out, "%s%d", n->nkids > 0 ? ", " : "", n->line);
    }
    fprintf(out, ")");
}

static void emit_builtin(Node *n) {
    char *name = n->name;
    if (same(name, "print")) {
        emit_print(n);
        return;
    }
    if (same(name, "args")) {
        fprintf(out, "tb_args");
        return;
    }
    if (same(name, "input")) {
        fprintf(out, "tb_input()");
        return;
    }
    Node *a = n->kids[0]->kids[0];
    if (same(name, "len")) {
        if (a->ty->kind == TY_TEXT) {
            fprintf(out, "(long long)strlen(");
            emit_expr(a);
            fprintf(out, ")");
        } else if (a->ty->kind == TY_DICT) {
            fprintf(out, "(");
            emit_expr(a);
            fprintf(out, ")->keys->len");
        } else {
            fprintf(out, "(");
            emit_expr(a);
            fprintf(out, ")->len");
        }
        return;
    }
    if (same(name, "sort")) {
        int k = a->ty->elem->kind;
        fprintf(out, "tb_sort_list(");
        emit_expr(a);
        fprintf(out, ", %s)", k == TY_INT ? "tb_cmp_int" : (k == TY_DEC ? "tb_cmp_dec" : "tb_cmp_text"));
        return;
    }
    if (same(name, "abs")) {
        fprintf(out, a->ty->kind == TY_INT ? "tb_absi(" : "fabs(");
        emit_expr(a);
        fprintf(out, ")");
        return;
    }
    if (same(name, "sqrt")) {
        fprintf(out, "tb_sqrt((double)(");
        emit_expr(a);
        fprintf(out, "), %d)", n->line);
        return;
    }
    if (same(name, "min") || same(name, "max")) {
        int ints = n->ty->kind == TY_INT;
        fprintf(out, "tb_%s%s(", name, ints ? "i" : "d");
        if (ints) {
            emit_expr(a);
            fprintf(out, ", ");
            emit_expr(n->kids[1]->kids[0]);
        } else {
            emit_as(a, n->ty);
            fprintf(out, ", ");
            emit_as(n->kids[1]->kids[0], n->ty);
        }
        fprintf(out, ")");
        return;
    }
    // остальные: просто имя функции из runtime.h; с номером строки — те, что могут упасть
    char *c_names[] = {
        "round", "tb_round", "0", "char", "tb_chr", "0", "cut", "tb_cut", "1", "code", "tb_code", "1",
        "to_int", "tb_to_int", "0", "to_dec", "tb_to_dec", "0", "read_file", "tb_read_file", "0",
        "write_file", "tb_write_file", "0", "split", "tb_split", "1", "join", "tb_join", "0",
        "find", "tb_find", "0", "replace", "tb_replace", "1", "upper", "tb_upper", "0",
        "lower", "tb_lower", "0", "trim", "tb_trim", "0", "starts_with", "tb_starts", "0",
        "ends_with", "tb_ends", "0", "run", "tb_run", "0", "exit", "tb_exit", "0",
        "file_exists", "tb_file_exists", "0"
    };
    for (int i = 0; i < 60; i += 3) {
        if (same(c_names[i], name)) {
            fprintf(out, "%s(", c_names[i + 1]);
            emit_args(n, same(c_names[i + 2], "1"));
            return;
        }
    }
    fprintf(out, "/* неизвестная встроенная %s */0", name);
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
    fprintf(out, "%s(", fname(n->ref));
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

static void emit_method(Node *n);

static void emit_dict(Node *n) {
    Type *t = n->ty;
    char *kt = c_type(t->key);
    char *vt = c_type(t->elem);
    fprintf(out, "tb_dict_from(%d, sizeof(%s), %d, (%s[]){", t->key->kind == TY_TEXT, vt, n->nkids / 2, kt);
    for (int i = 0; i < n->nkids; i += 2) {
        fprintf(out, "%s", i > 0 ? ", " : "");
        emit_expr(n->kids[i]);
    }
    fprintf(out, "}, (%s[]){", vt);
    for (int i = 1; i < n->nkids; i += 2) {
        fprintf(out, "%s", i > 1 ? ", " : "");
        emit_as(n->kids[i], t->elem);
    }
    fprintf(out, "})");
}

// .pop()  и методы dict; .add пишется отдельной строкой (emit_statement)
static void emit_method(Node *n) {
    Node *base = n->kids[0];
    if (base->ty->kind == TY_DICT) {
        if (same(n->name, "keys") || same(n->name, "values")) {
            fprintf(out, "tb_copy((");
            emit_expr(base);
            fprintf(out, ")->%s)", same(n->name, "keys") ? "keys" : "vals");
            return;
        }
        fprintf(out, "%s(", same(n->name, "has") ? "tb_dhas" : "tb_dremove");
        emit_expr(base);
        fprintf(out, ", ");
        emit_key(base->ty, n->kids[1]->kids[0]);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "(*(%s *)tb_pop(", c_type(n->ty));
    emit_expr(base);
    fprintf(out, ", %d))", n->line);
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
    if (same(op, "orelse")) {
        // f() or запасное: (tb_t = f(), tb_err ? (tb_err = 0, запасное) : tb_t)
        fprintf(out, "(tb_t%d = ", n->temp);
        emit_expr(a);
        fprintf(out, ", tb_err ? (tb_err = 0, ");
        emit_as(b, n->ty);
        fprintf(out, ") : tb_t%d)", n->temp);
        return;
    }
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
    } else if (k == N_DICT) {
        emit_dict(n);
    } else if (k == N_TRY || k == N_ELSE_ERR) {
        // значение уже посчитано строкой выше (emit_pre)
        fprintf(out, "tb_v%d", n->creates);
    } else if (k == N_CALL) {
        emit_call(n);
    } else if (k == N_METHOD) {
        emit_method(n);
    } else if (k == N_BINOP) {
        emit_binop(n);
    } else if (k == N_UNARY) {
        fprintf(out, "(%s", same(n->value, "not") ? "!" : "-");
        emit_expr(n->kids[0]);
        fprintf(out, ")");
    } else if (k == N_INDEX && n->kids[0]->ty->kind == TY_DICT) {
        fprintf(out, "(*(%s *)tb_dget(", c_type(n->ty));
        emit_expr(n->kids[0]);
        fprintf(out, ", ");
        emit_key(n->kids[0]->ty, n->kids[1]);
        fprintf(out, ", %d))", n->line);
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

// что отдать, если функция ломается: обычный return с пустым значением
static void emit_bail(void) {
    Node *outputs = cur_func->kids[2];
    if (same(cur_func->name, "main")) {
        fprintf(out, "tb_die(tb_err);");
    } else if (outputs->nkids == 1) {
        fprintf(out, "return v_%s;", outputs->kids[0]->name);
    } else if (outputs->nkids > 1) {
        fprintf(out, "return (%s){", c_type(cur_func->ty));
        for (int i = 0; i < outputs->nkids; i++) {
            fprintf(out, "%sv_%s", i > 0 ? ", " : "", outputs->kids[i]->name);
        }
        fprintf(out, "};");
    } else if (cur_func->ty->kind == TY_NONE) {
        fprintf(out, "return;");
    } else {
        fprintf(out, "return ");
        emit_default(cur_func->ty);
        fprintf(out, ";");
    }
}

// try f()  и  f() else err: вызов отдельной строкой, потом проверка tb_err
static void emit_pre(Node *v, int depth) {
    if (v == 0 || (v->kind != N_TRY && v->kind != N_ELSE_ERR)) {
        return;
    }
    Node *call = v->kids[0];
    label += 1;
    v->creates = label;
    indent(depth);
    if (call->ty->kind != TY_NONE) {
        fprintf(out, "%s tb_v%d = ", c_type(call->ty), label);
    }
    emit_expr(call);
    fprintf(out, ";\n");
    indent(depth);
    if (v->kind == N_TRY) {
        fprintf(out, "if (TB_RARE(tb_err != 0)) { ");
        emit_bail();
        fprintf(out, " }\n");
        return;
    }
    fprintf(out, "if (tb_err != 0) {\n");
    indent(depth + 1);
    fprintf(out, "const char * const v_%s = tb_err;\n", v->name);
    indent(depth + 1);
    fprintf(out, "tb_err = 0;\n");
    emit_block(v->kids[1], depth + 1);
    indent(depth);
    fprintf(out, "}\n");
}

// цель для  =  и  +=: у dict ключ создаётся, если его нет
static void emit_target(Node *t) {
    if (t->kind == N_INDEX && t->kids[0]->ty->kind == TY_DICT) {
        Type *d = t->kids[0]->ty;
        char *vt = c_type(d->elem);
        fprintf(out, "(*(%s *)tb_dput(", vt);
        emit_expr(t->kids[0]);
        fprintf(out, ", ");
        emit_key(d, t->kids[1]);
        if (is_simple(d->elem)) {
            fprintf(out, ", &(%s){", vt);
            emit_default(d->elem);
            fprintf(out, "}))");
        } else {
            fprintf(out, ", 0))");
        }
        return;
    }
    emit_expr(t);
}

static void emit_assign(Node *n, int depth) {
    if (n->nkids == 1) {
        emit_pre(n->kids[0], depth);
    }
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

// цель = значение: сначала значение, потом адрес цели (значение может менять список)
static void emit_set(Node *n, int depth) {
    Node *target = n->kids[0];
    Node *value = n->kids[1];
    char *op = n->value;
    char *t = c_type(n->ty);
    emit_pre(value, depth);
    label += 1;
    int id = label;
    indent(depth);
    fprintf(out, "{ ");
    if (n->ty->kind == TY_TEXT && !same(op, "=")) {
        fprintf(out, "const char *tb_v%d = ", id);
        emit_as_text(value);
    } else {
        fprintf(out, "%s tb_v%d = ", same(op, "=") ? t : c_type(value->ty), id);
        if (same(op, "=")) {
            emit_as(value, n->ty);
        } else {
            emit_expr(value);
        }
    }
    fprintf(out, "; %s *tb_p%d = &(", t, id);
    emit_target(target);
    fprintf(out, "); ");
    if (same(op, "=")) {
        fprintf(out, "*tb_p%d = tb_v%d;", id, id);
    } else if (n->ty->kind == TY_TEXT) {
        fprintf(out, "*tb_p%d = tb_cat(*tb_p%d, tb_v%d);", id, id, id);
    } else if (n->ty->kind == TY_INT && same(op, "/=")) {
        fprintf(out, "*tb_p%d = tb_div(*tb_p%d, tb_v%d, %d);", id, id, id, n->line);
    } else {
        fprintf(out, "*tb_p%d %s tb_v%d;", id, op, id);
    }
    fprintf(out, " }\n");
}

static void emit_multi(Node *n, int depth) {
    int names = n->nkids - 1;
    Node *call = n->kids[names];
    emit_pre(call, depth);
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
    if (t->kind == TY_DICT) {
        fprintf(out, "tb_list * tb_l%d = (", id);
        emit_expr(n->kids[0]);
        fprintf(out, ")->keys;\n");
        t = make_type_key(TY_LIST, 0, t->key, 0, 0);
    } else {
        fprintf(out, "%s tb_l%d = ", c_type(t), id);
        emit_expr(n->kids[0]);
        fprintf(out, ";\n");
    }
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
    } else if (k == N_FAIL) {
        indent(depth);
        fprintf(out, "{ tb_err = ");
        emit_expr(n->kids[0]);
        fprintf(out, "; ");
        emit_bail();
        fprintf(out, " }\n");
    } else if (k == N_RETURN) {
        if (n->nkids == 1) {
            emit_pre(n->kids[0], depth);
        }
        if (cur_func->kids[2]->nkids > 0 && n->nkids == 0) {
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
        if (e->kind == N_TRY || e->kind == N_ELSE_ERR) {
            emit_pre(e, depth);
            return;
        }
        indent(depth);
        if (e->kind == N_METHOD && same(e->name, "add")) {
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
    fprintf(out, "static %s %s(", c_type(f->ty), fname(f));
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
    } else if (k == TY_DICT) {
        char *kt = c_type(t->key);
        char *vt = c_type(t->elem);
        fprintf(out, "    tb_ps(\"{\");\n");
        fprintf(out, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        fprintf(out, "        if (i > 0) tb_ps(\", \");\n");
        fprintf(out, "        tb_show_%d(((%s *)x->keys->data)[i]);\n", t->key->id, kt);
        fprintf(out, "        tb_ps(\": \");\n");
        fprintf(out, "        tb_show_%d(((%s *)x->vals->data)[i]);\n", t->elem->id, vt);
        fprintf(out, "    }\n");
        fprintf(out, "    tb_ps(\"}\");\n");
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
    fprintf(out, "// Сделано компилятором Tachbit 0.3. Не правь руками: правь файл .tb\n");
    fprintf(out, "%s", runtime_text);

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
            fprintf(out, " } tb_r_%s;\n", fname(top));
        }
    }
    for (int i = 0; i < type_count; i++) {
        int k = all_types[i]->kind;
        if (k != TY_NONE && k != TY_MULTI && k != TY_UNKNOWN) {
            fprintf(out, "static void tb_show_%d(%s x);\n", all_types[i]->id, c_type(all_types[i]));
        }
    }
    for (int i = 1; i <= temp_count; i++) {
        fprintf(out, "static %s tb_t%d;   // для  f() or запасное\n", c_type(temp_types[i]), i);
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
    Node *main_func = 0;
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        if (top->kind == N_FUNC && same(top->name, "main") && same(top->mod, program->kids[0]->mod)) {
            main_func = top;
        }
    }
    fprintf(out, "\nint main(int argc, char **argv) {\n");
    fprintf(out, "    tb_args = tb_list_new(sizeof(const char *));\n");
    fprintf(out, "    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];\n");
    fprintf(out, "    %s();\n    return 0;\n}\n", fname(main_func));
}
