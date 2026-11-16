// Слой 3. ПРОВЕРКА: ищет ошибки до запуска и узнаёт тип каждого значения.
// Таблица функций, таблица переменных (со слоями-областями), правила типов.
#include <stdlib.h>
#include "ast.h"

// ---------- функции ----------

typedef struct {
    char *name;
    Node *node;
    int state;   // 0 — ещё не проверена, 1 — проверяется сейчас, 2 — готова
} Func;

static Func funcs[1000];
static int func_count = 0;

// ---------- переменные ----------

typedef struct {
    char *name;
    int type;
    int is_const;
    int depth;
} Var;

static Var vars[2000];
static int var_count = 0;
static int depth = 0;
static int var_base = 0;   // переменные ниже этой границы принадлежат другой функции

static Func *current = 0;
static int loop_depth = 0;

char *type_name(int type) {
    char *names[] = {"?", "ничего", "int", "dec", "bool", "text", "список int", "список dec"};
    return names[type];
}

static int is_number(int t) {
    return t == TY_INT || t == TY_DEC;
}

static int is_list(int t) {
    return t == TY_LIST_INT || t == TY_LIST_DEC;
}

static int is_builtin(char *name) {
    return same(name, "print") || same(name, "len") || same(name, "round");
}

static Func *find_func(char *name) {
    for (int i = 0; i < func_count; i++) {
        if (same(funcs[i].name, name)) {
            return &funcs[i];
        }
    }
    return 0;
}

static Var *find_var(char *name) {
    for (int i = var_count - 1; i >= var_base; i--) {
        if (same(vars[i].name, name)) {
            return &vars[i];
        }
    }
    return 0;
}

static void add_var(char *name, int type, int is_const, int line) {
    if (find_var(name) != 0) {
        fail(line, "такое имя уже есть, возьми другое", name);
    }
    if (find_func(name) != 0 || is_builtin(name)) {
        fail(line, "так уже называется функция", name);
    }
    if (var_count == 2000) {
        fail(line, "слишком много переменных", 0);
    }
    vars[var_count].name = name;
    vars[var_count].type = type;
    vars[var_count].is_const = is_const;
    vars[var_count].depth = depth;
    var_count += 1;
}

static void open_scope(void) {
    depth += 1;
}

static void close_scope(void) {
    while (var_count > 0 && vars[var_count - 1].depth == depth) {
        var_count -= 1;
    }
    depth -= 1;
}

// можно ли положить значение типа from туда, где ждут to
static int fits(int to, int from) {
    if (to == from) {
        return 1;
    }
    if (to == TY_DEC && from == TY_INT) {
        return 1;
    }
    if (to == TY_LIST_DEC && from == TY_LIST_INT) {
        return 1;
    }
    return 0;
}

// ---------- выражения ----------

static int check_expr(Node *n);
static void check_func(Func *f);

static void need_value(Node *n, int t) {
    if (t == TY_NONE) {
        fail(n->line, "эта функция ничего не возвращает, её значение нельзя использовать", n->name);
    }
}

static int check_binop(Node *n) {
    char *op = n->value;
    int a = check_expr(n->kids[0]);
    int b = check_expr(n->kids[1]);
    need_value(n->kids[0], a);
    need_value(n->kids[1], b);
    if (same(op, "and") || same(op, "or")) {
        if (a != TY_BOOL || b != TY_BOOL) {
            fail(n->line, "and / or работают только с true и false", 0);
        }
        return TY_BOOL;
    }
    if (same(op, "==") || same(op, "!=")) {
        if (!(is_number(a) && is_number(b)) && a != b) {
            fail(n->line, "нельзя сравнить разные типы", 0);
        }
        if (is_list(a)) {
            fail(n->line, "списки пока нельзя сравнивать", 0);
        }
        return TY_BOOL;
    }
    if (same(op, "<") || same(op, ">") || same(op, "<=") || same(op, ">=")) {
        if (!is_number(a) || !is_number(b)) {
            fail(n->line, "больше / меньше работают только с числами", op);
        }
        return TY_BOOL;
    }
    if (same(op, "+") && (a == TY_TEXT || b == TY_TEXT)) {
        if (is_list(a) || is_list(b)) {
            fail(n->line, "список нельзя сложить с текстом", 0);
        }
        return TY_TEXT;
    }
    if (!is_number(a) || !is_number(b)) {
        fail(n->line, "эта операция работает только с числами", op);
    }
    if (same(op, "%") && (a == TY_DEC || b == TY_DEC)) {
        fail(n->line, "остаток % считается только для int", 0);
    }
    if (a == TY_DEC || b == TY_DEC) {
        return TY_DEC;
    }
    return TY_INT;
}

static int check_list(Node *n) {
    if (n->nkids == 0) {
        fail(n->line, "пустой список [] пока нельзя: положи хотя бы одно число", 0);
    }
    int result = TY_INT;
    for (int i = 0; i < n->nkids; i++) {
        int t = check_expr(n->kids[i]);
        if (t == TY_DEC) {
            result = TY_DEC;
        } else if (t != TY_INT) {
            fail(n->line, "в версии 0.1 списки бывают только из чисел", 0);
        }
    }
    if (result == TY_DEC) {
        return TY_LIST_DEC;
    }
    return TY_LIST_INT;
}

static int check_builtin(Node *n) {
    if (same(n->name, "print")) {
        for (int i = 0; i < n->nkids; i++) {
            if (n->kids[i]->name != 0) {
                fail(n->line, "у print нет именованных аргументов", n->kids[i]->name);
            }
            Node *value = n->kids[i]->kids[0];
            int t = check_expr(value);
            need_value(value, t);
            n->kids[i]->type = t;
        }
        return TY_NONE;
    }
    if (n->nkids != 1 || n->kids[0]->name != 0) {
        fail(n->line, "этой функции нужно ровно одно значение", n->name);
    }
    Node *value = n->kids[0]->kids[0];
    int t = check_expr(value);
    n->kids[0]->type = t;
    if (same(n->name, "len")) {
        if (!is_list(t) && t != TY_TEXT) {
            fail(n->line, "len считает длину списка или текста", 0);
        }
        return TY_INT;
    }
    if (same(n->name, "round")) {
        if (!is_number(t)) {
            fail(n->line, "round округляет только число", 0);
        }
        return TY_INT;
    }
    fail(n->line, "неизвестная функция", n->name);
    return 0;
}

// приводит аргументы к порядку параметров:  add(b: 3, a: 2)  →  add(2, 3)
static void order_args(Node *call, Node *params) {
    Node **ordered = arena_alloc(sizeof(Node *) * (params->nkids + 1));
    int named = 0;
    if (call->nkids != params->nkids) {
        fail(call->line, "не то количество значений в вызове", call->name);
    }
    for (int i = 0; i < call->nkids; i++) {
        Node *arg = call->kids[i];
        int slot = i;
        if (arg->name != 0) {
            named = 1;
            slot = -1;
            for (int j = 0; j < params->nkids; j++) {
                if (same(params->kids[j]->name, arg->name)) {
                    slot = j;
                }
            }
            if (slot < 0) {
                fail(arg->line, "у функции нет параметра с таким именем", arg->name);
            }
        } else if (named) {
            fail(arg->line, "после именованных аргументов обычные писать нельзя", 0);
        }
        if (ordered[slot] != 0) {
            fail(arg->line, "параметр указан два раза", params->kids[slot]->name);
        }
        ordered[slot] = arg;
    }
    for (int i = 0; i < call->nkids; i++) {
        call->kids[i] = ordered[i];
    }
}

static int check_call(Node *n) {
    if (is_builtin(n->name)) {
        return check_builtin(n);
    }
    if (find_var(n->name) != 0) {
        fail(n->line, "это переменная, а не функция", n->name);
    }
    Func *f = find_func(n->name);
    if (f == 0) {
        fail(n->line, "нет такой функции", n->name);
    }
    if (same(n->name, "main")) {
        fail(n->line, "main запускается сама, её нельзя вызывать", 0);
    }
    Node *params = f->node->kids[0];
    order_args(n, params);
    for (int i = 0; i < n->nkids; i++) {
        Node *value = n->kids[i]->kids[0];
        int t = check_expr(value);
        need_value(value, t);
        int want = params->kids[i]->declared;
        if (!fits(want, t)) {
            fail(value->line, "параметр ждёт другой тип", params->kids[i]->name);
        }
        n->kids[i]->type = want;
    }
    if (f->state == 0) {
        // проверяем вызванную функцию сейчас, чтобы узнать, что она возвращает
        Func *saved_current = current;
        int saved_loops = loop_depth;
        int saved_vars = var_count;
        int saved_depth = depth;
        int saved_base = var_base;
        var_base = var_count;
        check_func(f);
        var_base = saved_base;
        current = saved_current;
        loop_depth = saved_loops;
        var_count = saved_vars;
        depth = saved_depth;
    }
    if (f->node->type == TY_UNKNOWN) {
        fail(n->line, "функция вызывает сама себя раньше первого return с числом: поставь простой return выше", n->name);
    }
    return f->node->type;
}

static int check_expr(Node *n) {
    int t = TY_UNKNOWN;
    if (n->kind == N_NUMBER) {
        t = TY_INT;
    } else if (n->kind == N_DECIMAL) {
        t = TY_DEC;
    } else if (n->kind == N_STRING) {
        t = TY_TEXT;
    } else if (n->kind == N_BOOL) {
        t = TY_BOOL;
    } else if (n->kind == N_NAME) {
        Var *v = find_var(n->name);
        if (v == 0) {
            if (find_func(n->name) != 0) {
                fail(n->line, "у функции нужны скобки", n->name);
            }
            fail(n->line, "нет такой переменной", n->name);
        }
        t = v->type;
    } else if (n->kind == N_LIST) {
        t = check_list(n);
    } else if (n->kind == N_CALL) {
        t = check_call(n);
    } else if (n->kind == N_BINOP) {
        t = check_binop(n);
    } else if (n->kind == N_UNARY) {
        int inner = check_expr(n->kids[0]);
        if (same(n->value, "not")) {
            if (inner != TY_BOOL) {
                fail(n->line, "not работает только с true и false", 0);
            }
            t = TY_BOOL;
        } else {
            if (!is_number(inner)) {
                fail(n->line, "минус ставится только перед числом", 0);
            }
            t = inner;
        }
    } else if (n->kind == N_INDEX) {
        int list = check_expr(n->kids[0]);
        int index = check_expr(n->kids[1]);
        if (!is_list(list)) {
            fail(n->line, "[номер] можно брать только у списка", 0);
        }
        if (index != TY_INT) {
            fail(n->line, "номер в списке должен быть int", 0);
        }
        t = TY_INT;
        if (list == TY_LIST_DEC) {
            t = TY_DEC;
        }
    }
    n->type = t;
    return t;
}

static void need_bool(Node *n) {
    int t = check_expr(n);
    if (t != TY_BOOL) {
        fail(n->line, "условие должно быть true или false (например a > 3)", 0);
    }
}

static void need_int(Node *n, char *what) {
    int t = check_expr(n);
    if (t != TY_INT) {
        fail(n->line, "здесь нужно целое число int", what);
    }
}

// ---------- команды ----------

static void check_block(Node *block);

static void check_assign(Node *n) {
    int t = check_expr(n->kids[0]);
    need_value(n->kids[0], t);
    Var *v = find_var(n->name);
    if (v == 0) {
        if (find_func(n->name) != 0 || is_builtin(n->name)) {
            fail(n->line, "так уже называется функция", n->name);
        }
        int type = t;
        if (n->declared != 0) {
            if (!fits(n->declared, t)) {
                fail(n->line, "значение не того типа, что написан", type_name(n->declared));
            }
            type = n->declared;
        }
        n->creates = 1;
        n->type = type;
        add_var(n->name, type, n->is_const, n->line);
        return;
    }
    if (n->is_const || n->declared != 0) {
        fail(n->line, "переменная уже есть: тип и const пишутся только в первый раз", n->name);
    }
    if (v->is_const) {
        fail(n->line, "это const, его нельзя менять", n->name);
    }
    if (!fits(v->type, t)) {
        fail(n->line, "переменная другого типа", type_name(v->type));
    }
    n->type = v->type;
}

static void check_opassign(Node *n) {
    Var *v = find_var(n->name);
    if (v == 0) {
        fail(n->line, "нет такой переменной", n->name);
    }
    if (v->is_const) {
        fail(n->line, "это const, его нельзя менять", n->name);
    }
    int t = check_expr(n->kids[0]);
    need_value(n->kids[0], t);
    n->type = v->type;
    if (v->type == TY_TEXT && same(n->value, "+=")) {
        if (is_list(t)) {
            fail(n->line, "список нельзя приписать к тексту", 0);
        }
        return;
    }
    if (!is_number(v->type) || !is_number(t)) {
        fail(n->line, "эта операция работает только с числами", n->value);
    }
    if (v->type == TY_INT && t == TY_DEC) {
        fail(n->line, "в int нельзя добавить дробь: сделай переменную dec", n->name);
    }
}

static void check_return(Node *n) {
    int t = TY_NONE;
    if (n->nkids == 1) {
        t = check_expr(n->kids[0]);
        need_value(n->kids[0], t);
    }
    if (is_list(t)) {
        fail(n->line, "в версии 0.1 функция не может вернуть список", 0);
    }
    Node *f = current->node;
    if (same(f->name, "main") && t != TY_NONE) {
        fail(n->line, "main ничего не возвращает: пиши просто return", 0);
    }
    if (f->type == TY_UNKNOWN) {
        f->type = t;
    } else if (f->type == TY_INT && t == TY_DEC) {
        fail(n->line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", 0);
    } else if (!fits(f->type, t)) {
        fail(n->line, "раньше эта функция вернула другой тип", type_name(f->type));
    }
}

static void check_statement(Node *n) {
    int k = n->kind;
    if (k == N_ASSIGN) {
        check_assign(n);
    } else if (k == N_OPASSIGN) {
        check_opassign(n);
    } else if (k == N_IF) {
        for (int i = 0; i + 1 < n->nkids; i += 2) {
            need_bool(n->kids[i]);
            check_block(n->kids[i + 1]);
        }
        if (n->nkids % 2 == 1) {
            check_block(n->kids[n->nkids - 1]);
        }
    } else if (k == N_EACH) {
        int t = check_expr(n->kids[0]);
        if (!is_list(t)) {
            fail(n->line, "each ходит только по списку", 0);
        }
        open_scope();
        add_var(n->name, t == TY_LIST_DEC ? TY_DEC : TY_INT, 1, n->line);
        loop_depth += 1;
        check_block(n->kids[1]);
        loop_depth -= 1;
        close_scope();
    } else if (k == N_COUNT) {
        need_int(n->kids[0], "from");
        need_int(n->kids[1], "to");
        open_scope();
        add_var(n->name, TY_INT, 1, n->line);
        loop_depth += 1;
        check_block(n->kids[2]);
        loop_depth -= 1;
        close_scope();
    } else if (k == N_REPEAT_TIMES || k == N_REPEAT_WHILE) {
        if (k == N_REPEAT_TIMES) {
            need_int(n->kids[0], "times");
        } else {
            need_bool(n->kids[0]);
        }
        loop_depth += 1;
        check_block(n->kids[1]);
        loop_depth -= 1;
    } else if (k == N_SKIP || k == N_STOP) {
        if (loop_depth == 0) {
            fail(n->line, "skip и stop пишутся только внутри цикла", 0);
        }
    } else if (k == N_RETURN) {
        check_return(n);
    } else if (k == N_EXPR_STMT) {
        int t = check_expr(n->kids[0]);
        if (t != TY_NONE) {
            fail(n->line, "результат вызова пропадает: сохрани его, например x = ...", n->kids[0]->name);
        }
    }
}

static void check_block(Node *block) {
    open_scope();
    for (int i = 0; i < block->nkids; i++) {
        check_statement(block->kids[i]);
    }
    close_scope();
}

static void check_func(Func *f) {
    current = f;
    f->state = 1;
    loop_depth = 0;
    open_scope();
    Node *params = f->node->kids[0];
    for (int i = 0; i < params->nkids; i++) {
        add_var(params->kids[i]->name, params->kids[i]->declared, 0, params->kids[i]->line);
    }
    check_block(f->node->kids[1]);
    close_scope();
    if (f->node->type == TY_UNKNOWN) {
        f->node->type = TY_NONE;
    }
    f->state = 2;
}

void check(Node *program) {
    for (int i = 0; i < program->nkids; i++) {
        Node *fn = program->kids[i];
        if (find_func(fn->name) != 0) {
            fail(fn->line, "функция с таким именем уже есть", fn->name);
        }
        if (is_builtin(fn->name)) {
            fail(fn->line, "это имя занято встроенной функцией", fn->name);
        }
        if (func_count == 1000) {
            fail(fn->line, "слишком много функций", 0);
        }
        funcs[func_count].name = fn->name;
        funcs[func_count].node = fn;
        funcs[func_count].state = 0;
        func_count += 1;
    }
    Func *main_func = find_func("main");
    if (main_func == 0) {
        fail(0, "нет функции main: программа начинается с func main()", 0);
    }
    if (main_func->node->kids[0]->nkids != 0) {
        fail(main_func->node->line, "у main не бывает параметров", 0);
    }
    for (int i = 0; i < func_count; i++) {
        if (funcs[i].state == 0) {
            var_count = 0;
            depth = 0;
            check_func(&funcs[i]);
        }
    }
}
