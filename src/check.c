// Слой 3. ПРОВЕРКА: ищет ошибки до запуска и узнаёт тип каждого значения.
// Таблица типов, таблица функций и obj, таблица переменных (со слоями-областями).
#include <stdlib.h>
#include <string.h>
#include "ast.h"

// ---------- типы ----------
// Каждый тип хранится один раз: два "list int" — это один и тот же Type.
// Поэтому типы можно сравнивать простым ==.

Type *make_type_key(int kind, Type *key, Type *elem, Node *obj, Node *func);

Type *all_types[1000];
int type_count = 0;

static Type *make_type(int kind, Type *elem, Node *obj, Node *func) {
    return make_type_key(kind, 0, elem, obj, func);
}

Type *make_type_key(int kind, Type *key, Type *elem, Node *obj, Node *func) {
    for (int i = 0; i < type_count; i++) {
        Type *t = all_types[i];
        if (t->kind == kind && t->key == key && t->elem == elem && t->obj == obj && t->func == func) {
            return t;
        }
    }
    if (type_count == 1000) {
        fail(0, "слишком много разных типов", 0);
    }
    Type *t = arena_alloc(sizeof(Type));
    t->kind = kind;
    t->key = key;
    t->elem = elem;
    t->obj = obj;
    t->func = func;
    t->id = type_count;
    all_types[type_count] = t;
    type_count += 1;
    return t;
}

static Type *t_none;
static Type *t_int;
static Type *t_dec;
static Type *t_bool;
static Type *t_text;

static Type *list_of(Type *elem) {
    return make_type(TY_LIST, elem, 0, 0);
}

static Type *dict_of(Type *key, Type *value) {
    return make_type_key(TY_DICT, key, value, 0, 0);
}

char *type_name(Type *t) {
    if (t == 0) {
        return "?";
    }
    if (t->kind == TY_LIST) {
        char *inner = type_name(t->elem);
        char *s = arena_alloc(strlen(inner) + 6);
        strcpy(s, "list ");
        strcat(s, inner);
        return s;
    }
    if (t->kind == TY_DICT) {
        char *k = type_name(t->key);
        char *v = type_name(t->elem);
        char *s = arena_alloc(strlen(k) + strlen(v) + 7);
        strcpy(s, "dict ");
        strcat(s, k);
        strcat(s, " ");
        strcat(s, v);
        return s;
    }
    if (t->kind == TY_OBJ) {
        return t->obj->name;
    }
    if (t->kind == TY_MULTI) {
        return "несколько выходов";
    }
    char *names[] = {"?", "ничего", "int", "dec", "bool", "text"};
    return names[t->kind];
}

static char *join2(char *a, char *b) {
    char *s = arena_alloc(strlen(a) + strlen(b) + 1);
    strcpy(s, a);
    strcat(s, b);
    return s;
}

static int is_number(Type *t) {
    return t == t_int || t == t_dec;
}

static int is_list(Type *t) {
    return t != 0 && t->kind == TY_LIST;
}

static int is_scalar(Type *t) {
    return t == t_int || t == t_dec || t == t_bool || t == t_text;
}

// можно ли положить значение типа from туда, где ждут to
static int fits(Type *to, Type *from) {
    if (to == from) {
        return 1;
    }
    if (to == t_dec && from == t_int) {
        return 1;
    }
    return 0;
}

// ---------- функции и obj ----------

typedef struct {
    char *name;
    char *mod;
    Node *node;
    int state;   // 0 — ещё не проверена, 1 — проверяется сейчас, 2 — готова
} Func;

static Func funcs[1000];
static int func_count = 0;
static Node *objs[500];
static int obj_count = 0;

static char *builtins[] = {
    "print", "len", "round", "code", "char", "cut", "to_int", "to_dec",
    "read_file", "write_file", "input", "args",
    "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with",
    "sort", "abs", "min", "max", "sqrt"
};
static int builtin_count = 26;

// встроенные функции, которые могут закончиться ошибкой
static int builtin_can_fail(char *name) {
    return same(name, "to_int") || same(name, "to_dec") || same(name, "read_file") ||
           same(name, "write_file") || same(name, "input");
}

// ---------- модули ----------
// use money: модуль main видит modpub-функции модуля money

typedef struct {
    char *mod;
    char *used;
} Use;

static Use uses[500];
static int use_count = 0;
static char *cur_mod = "";
static char *all_mods[200];
static int mod_count = 0;

static int is_module(char *name) {
    for (int i = 0; i < mod_count; i++) {
        if (same(all_mods[i], name)) {
            return 1;
        }
    }
    return 0;
}

static int module_uses(char *mod, char *other) {
    if (same(mod, other)) {
        return 1;
    }
    for (int i = 0; i < use_count; i++) {
        if (same(uses[i].mod, mod) && same(uses[i].used, other)) {
            return 1;
        }
    }
    return 0;
}

static int is_builtin(char *name) {
    for (int i = 0; i < builtin_count; i++) {
        if (same(builtins[i], name)) {
            return 1;
        }
    }
    return 0;
}

static Func *find_func_in(char *name, char *mod) {
    for (int i = 0; i < func_count; i++) {
        if (same(funcs[i].name, name) && same(funcs[i].mod, mod)) {
            return &funcs[i];
        }
    }
    return 0;
}

// функция, которую видно из текущего модуля: своя или modpub из модуля после use
static Func *find_func(char *name) {
    Func *own = find_func_in(name, cur_mod);
    if (own != 0) {
        return own;
    }
    Func *found = 0;
    for (int i = 0; i < func_count; i++) {
        Func *f = &funcs[i];
        if (same(f->name, name) && f->node->is_pub && module_uses(cur_mod, f->mod)) {
            if (found != 0) {
                fail(f->node->line, "функция с таким именем есть в двух модулях, напиши модуль: money.total(...)", name);
            }
            found = f;
        }
    }
    return found;
}

// почему функцию не видно: подсказка для ошибки
static void explain_missing_func(Node *n) {
    for (int i = 0; i < func_count; i++) {
        Func *f = &funcs[i];
        if (same(f->name, n->name)) {
            if (!f->node->is_pub) {
                fail(n->line, "эта функция спрятана в своём модуле: поставь перед ней modpub", f->mod);
            }
            fail(n->line, "функция лежит в другом модуле: добавь в начало файла  use", f->mod);
        }
    }
    fail(n->line, "нет такой функции", n->name);
}

static Node *find_obj(char *name) {
    for (int i = 0; i < obj_count; i++) {
        if (same(objs[i]->name, name)) {
            return objs[i];
        }
    }
    return 0;
}

static Node *find_field(Node *obj, char *name) {
    for (int i = 0; i < obj->nkids; i++) {
        if (same(obj->kids[i]->name, name)) {
            return obj->kids[i];
        }
    }
    return 0;
}

static Type *resolve_type(Node *t) {
    if (same(t->name, "int")) {
        return t_int;
    }
    if (same(t->name, "dec")) {
        return t_dec;
    }
    if (same(t->name, "text")) {
        return t_text;
    }
    if (same(t->name, "bool")) {
        return t_bool;
    }
    if (same(t->name, "list")) {
        return list_of(resolve_type(t->kids[0]));
    }
    if (same(t->name, "dict")) {
        return dict_of(resolve_type(t->kids[0]), resolve_type(t->kids[1]));
    }
    Node *obj = find_obj(t->name);
    if (obj == 0) {
        fail(t->line, "нет такого типа и такого obj", t->name);
    }
    return make_type(TY_OBJ, 0, obj, 0);
}

// ---------- переменные ----------

typedef struct {
    char *name;
    Type *type;
    int is_const;
    int is_link;
    int depth;
} Var;

static Var vars[2000];
static int var_count = 0;
static int depth = 0;
static int var_base = 0;   // переменные ниже этой границы принадлежат другой функции

static Func *current = 0;
static int loop_depth = 0;

static Var *find_var(char *name) {
    for (int i = var_count - 1; i >= var_base; i--) {
        if (same(vars[i].name, name)) {
            return &vars[i];
        }
    }
    return 0;
}

static void name_is_free(char *name, int line) {
    if (find_var(name) != 0) {
        fail(line, "такое имя уже есть, возьми другое", name);
    }
    if (find_func(name) != 0 || is_builtin(name)) {
        fail(line, "так уже называется функция", name);
    }
    if (find_obj(name) != 0) {
        fail(line, "так уже называется obj", name);
    }
}

static void add_var(char *name, Type *type, int is_const, int is_link, int line) {
    name_is_free(name, line);
    if (var_count == 2000) {
        fail(line, "слишком много переменных", 0);
    }
    vars[var_count].name = name;
    vars[var_count].type = type;
    vars[var_count].is_const = is_const;
    vars[var_count].is_link = is_link;
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

// ---------- выражения ----------

static Type *check_expr(Node *n);
static void check_func(Func *f);

static void need_value(Node *n, Type *t) {
    if (t == t_none) {
        fail(n->line, "это ничего не возвращает, значение нельзя использовать", n->name);
    }
    if (t->kind == TY_MULTI) {
        fail(n->line, "у функции несколько выходов: пиши  a, b = ...", n->name);
    }
}

static Type *check_value(Node *n) {
    Type *t = check_expr(n);
    need_value(n, t);
    return t;
}

static int can_fail_call(Node *n);
static Type *check_dict(Node *n);
static Type *check_handled(Node *n, char *how);

static Type *check_binop(Node *n) {
    char *op = n->value;
    if (same(op, "or") && can_fail_call(n->kids[0])) {
        // f() or запасное: если f сломалась, берём запасное
        Type *a = check_handled(n->kids[0], "or");
        Type *b = check_value(n->kids[1]);
        if (!fits(a, b)) {
            fail(n->line, "запасное значение после or другого типа", type_name(a));
        }
        if (a == t_none) {
            fail(n->line, "у функции нет значения, тут подходит  try  или  else err", 0);
        }
        if (temp_count == 2000) {
            fail(n->line, "слишком много or", 0);
        }
        temp_count += 1;
        temp_types[temp_count] = a;
        n->temp = temp_count;
        n->value = "orelse";
        return a;
    }
    Type *a = check_value(n->kids[0]);
    Type *b = check_value(n->kids[1]);
    if (same(op, "and") || same(op, "or")) {
        if (a != t_bool || b != t_bool) {
            fail(n->line, "and / or работают только с true и false", 0);
        }
        return t_bool;
    }
    if (same(op, "==") || same(op, "!=")) {
        if (!(is_number(a) && is_number(b)) && a != b) {
            fail(n->line, "нельзя сравнить разные типы", 0);
        }
        if (!is_scalar(a)) {
            fail(n->line, "списки и объекты пока нельзя сравнивать", 0);
        }
        return t_bool;
    }
    if (same(op, "<") || same(op, ">") || same(op, "<=") || same(op, ">=")) {
        if (a == t_text && b == t_text) {
            return t_bool;
        }
        if (!is_number(a) || !is_number(b)) {
            fail(n->line, "больше / меньше работают с числами или с двумя текстами", op);
        }
        return t_bool;
    }
    if (same(op, "+") && (a == t_text || b == t_text)) {
        if (!is_scalar(a) || !is_scalar(b)) {
            fail(n->line, "список или объект нельзя сложить с текстом", 0);
        }
        return t_text;
    }
    if (!is_number(a) || !is_number(b)) {
        fail(n->line, "эта операция работает только с числами", op);
    }
    if (same(op, "%") && (a == t_dec || b == t_dec)) {
        fail(n->line, "остаток % считается только для int", 0);
    }
    if (a == t_dec || b == t_dec) {
        return t_dec;
    }
    return t_int;
}

static Type *check_list(Node *n) {
    if (n->nkids == 0) {
        fail(n->line, "пустой список пишется с типом: x = list int", 0);
    }
    Type *elem = check_value(n->kids[0]);
    for (int i = 1; i < n->nkids; i++) {
        Type *t = check_value(n->kids[i]);
        if (fits(elem, t)) {
            continue;
        }
        if (elem == t_int && t == t_dec) {
            elem = t_dec;
            continue;
        }
        fail(n->kids[i]->line, "в одном списке все значения одного типа", type_name(elem));
    }
    return list_of(elem);
}

// значения builtins: только по порядку, без имён
static Type *builtin_arg(Node *n, int i) {
    if (n->kids[i]->name != 0) {
        fail(n->line, "у встроенных функций аргументы пишутся без имён", n->kids[i]->name);
    }
    Type *t = check_value(n->kids[i]->kids[0]);
    n->kids[i]->ty = t;
    return t;
}

static int is_dict(Type *t) {
    return t != 0 && t->kind == TY_DICT;
}

// проверка аргументов встроенной функции по строке-шаблону:
// "t" text, "i" int, "n" число, "l" список, "L" список text, "s" список для sort, "x" список/текст/dict
static void builtin_args(Node *n, char *pattern, char *usage) {
    int count = (int)strlen(pattern);
    if (n->nkids != count) {
        fail(n->line, "не то количество значений, правильно так", usage);
    }
    for (int i = 0; i < count; i++) {
        Type *t = builtin_arg(n, i);
        char c = pattern[i];
        int ok = 1;
        if (c == 't') {
            ok = t == t_text;
        } else if (c == 'i') {
            ok = t == t_int;
        } else if (c == 'n') {
            ok = is_number(t);
        } else if (c == 'L') {
            ok = is_list(t) && t->elem == t_text;
        } else if (c == 's') {
            ok = is_list(t) && (t->elem == t_int || t->elem == t_dec || t->elem == t_text);
        } else if (c == 'x') {
            ok = is_list(t) || is_dict(t) || t == t_text;
        }
        if (!ok) {
            fail(n->line, "не тот тип значения, правильно так", usage);
        }
    }
}

static Type *arg_type(Node *n, int i) {
    return n->kids[i]->ty;
}

static Type *check_builtin(Node *n) {
    char *name = n->name;
    if (same(name, "print")) {
        for (int i = 0; i < n->nkids; i++) {
            builtin_arg(n, i);
        }
        return t_none;
    }
    if (same(name, "len")) {
        builtin_args(n, "x", "len(список, текст или dict)");
        return t_int;
    }
    if (same(name, "round")) {
        builtin_args(n, "n", "round(число)");
        return t_int;
    }
    if (same(name, "code")) {
        builtin_args(n, "t", "code(\"A\")  →  65");
        return t_int;
    }
    if (same(name, "char")) {
        builtin_args(n, "i", "char(66)  →  \"B\"");
        return t_text;
    }
    if (same(name, "cut")) {
        builtin_args(n, "tii", "cut(текст, с какого номера, по какой номер)");
        return t_text;
    }
    if (same(name, "to_int")) {
        builtin_args(n, "t", "to_int(\"42\")");
        return t_int;
    }
    if (same(name, "to_dec")) {
        builtin_args(n, "t", "to_dec(\"9.5\")");
        return t_dec;
    }
    if (same(name, "read_file")) {
        builtin_args(n, "t", "read_file(\"путь\")");
        return t_text;
    }
    if (same(name, "write_file")) {
        builtin_args(n, "tt", "write_file(\"путь\", текст)");
        return t_none;
    }
    if (same(name, "input")) {
        builtin_args(n, "", "input()");
        return t_text;
    }
    if (same(name, "args")) {
        builtin_args(n, "", "args()");
        return list_of(t_text);
    }
    if (same(name, "split")) {
        builtin_args(n, "tt", "split(текст, разделитель)");
        return list_of(t_text);
    }
    if (same(name, "join")) {
        builtin_args(n, "Lt", "join(список text, разделитель)");
        return t_text;
    }
    if (same(name, "find")) {
        builtin_args(n, "tt", "find(текст, что ищем)  →  номер или -1");
        return t_int;
    }
    if (same(name, "replace")) {
        builtin_args(n, "ttt", "replace(текст, что, на что)");
        return t_text;
    }
    if (same(name, "upper") || same(name, "lower") || same(name, "trim")) {
        builtin_args(n, "t", "upper(текст)");
        return t_text;
    }
    if (same(name, "starts_with") || same(name, "ends_with")) {
        builtin_args(n, "tt", "starts_with(текст, начало)");
        return t_bool;
    }
    if (same(name, "sort")) {
        builtin_args(n, "s", "sort(список int, dec или text)");
        return t_none;
    }
    if (same(name, "abs")) {
        builtin_args(n, "n", "abs(число)");
        return arg_type(n, 0);
    }
    if (same(name, "sqrt")) {
        builtin_args(n, "n", "sqrt(число)");
        return t_dec;
    }
    // min и max
    builtin_args(n, "nn", "min(a, b)");
    if (arg_type(n, 0) == t_int && arg_type(n, 1) == t_int) {
        return t_int;
    }
    return t_dec;
}

// приводит аргументы к порядку параметров:  add(b: 3, a: 2)  →  add(2, 3)
// allow_missing: для создания obj можно пропустить качества
static void order_args(Node *call, Node *params, int allow_missing) {
    Node **ordered = arena_alloc(sizeof(Node *) * (params->nkids + 1));
    int named = 0;
    if (call->nkids > params->nkids || (!allow_missing && call->nkids != params->nkids)) {
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
                fail(arg->line, "нет параметра с таким именем", arg->name);
            }
        } else if (named) {
            fail(arg->line, "после именованных аргументов обычные писать нельзя", 0);
        }
        if (ordered[slot] != 0) {
            fail(arg->line, "параметр указан два раза", params->kids[slot]->name);
        }
        ordered[slot] = arg;
    }
    // теперь kids[i] — значение для параметра i, или 0, если пропущено
    call->kids = ordered;
    call->nkids = params->nkids;
    call->cap = params->nkids + 1;
}

// то, на что можно повесить ярлык или что можно менять: x, u.age, nums[2], d[k]
static void need_place_for(Node *n, char *why, int for_link);

static void need_place(Node *n, char *why) {
    need_place_for(n, why, 0);
}

static void need_place_for(Node *n, char *why, int for_link) {
    if (n->kind == N_INDEX && is_dict(n->kids[0]->ty)) {
        if (for_link) {
            fail(n->line, "ярлык на значение в dict нельзя: dict может переложить значения", 0);
        }
        return;
    }
    if (n->kind == N_NAME) {
        Var *v = find_var(n->name);
        if (v != 0 && v->is_const) {
            fail(n->line, "это const или переменная цикла, её нельзя менять", n->name);
        }
        return;
    }
    if (n->kind == N_FIELD) {
        return;
    }
    if (n->kind == N_INDEX && is_list(n->kids[0]->ty)) {
        return;
    }
    if (n->kind == N_INDEX) {
        fail(n->line, "буквы в тексте менять нельзя, собери новый текст", 0);
    }
    fail(n->line, why, 0);
}

static Type *check_new_obj(Node *n, Node *obj) {
    order_args(n, obj, 1);
    for (int i = 0; i < n->nkids; i++) {
        if (n->kids[i] == 0) {
            continue;
        }
        Node *value = n->kids[i]->kids[0];
        Type *t = check_value(value);
        if (!fits(obj->kids[i]->ty, t)) {
            fail(value->line, "качество другого типа", obj->kids[i]->name);
        }
        n->kids[i]->ty = obj->kids[i]->ty;
    }
    n->ref = obj;
    return make_type(TY_OBJ, 0, obj, 0);
}

static Type *check_user_call(Node *n, Func *f);

static Type *check_call(Node *n) {
    if (is_builtin(n->name)) {
        if (builtin_can_fail(n->name) && !n->creates) {
            fail(n->line, "это может закончиться ошибкой: напиши  try ...,  ... or запасное  или  ... else err", n->name);
        }
        return check_builtin(n);
    }
    if (find_var(n->name) != 0) {
        fail(n->line, "это переменная, а не функция", n->name);
    }
    Node *obj = find_obj(n->name);
    if (obj != 0) {
        return check_new_obj(n, obj);
    }
    Func *f = find_func(n->name);
    if (f == 0) {
        explain_missing_func(n);
    }
    return check_user_call(n, f);
}

// вызов функции программиста (f уже найдена: по имени или как money.total)
static Type *check_user_call(Node *n, Func *f) {
    if (same(n->name, "main")) {
        fail(n->line, "main запускается сама, её нельзя вызывать", 0);
    }
    if (f->node->can_fail && !n->creates) {
        fail(n->line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", n->name);
    }
    n->ref = f->node;
    Node *params = f->node->kids[0];
    order_args(n, params, 0);
    for (int i = 0; i < n->nkids; i++) {
        Node *p = params->kids[i];
        Node *value = n->kids[i]->kids[0];
        Type *t = check_value(value);
        if (p->is_link) {
            need_place_for(value, "в ярлык link можно передать только переменную, u.age или nums[i]", 1);
            if (p->ty == 0) {
                p->ty = t;   // тип ярлыка узнаём по первому вызову
            } else if (p->ty != t) {
                fail(value->line, "этот ярлык уже был другого типа", type_name(p->ty));
            }
        } else if (!fits(p->ty, t)) {
            fail(value->line, "параметр ждёт другой тип", p->name);
        }
        n->kids[i]->ty = p->ty;
    }
    if (f->state == 0) {
        // проверяем вызванную функцию сейчас, чтобы узнать, что она возвращает
        Func *saved_current = current;
        int saved_loops = loop_depth;
        int saved_vars = var_count;
        int saved_depth = depth;
        int saved_base = var_base;
        char *saved_mod = cur_mod;
        char *saved_file = file_name;
        var_base = var_count;
        check_func(f);
        var_base = saved_base;
        cur_mod = saved_mod;
        file_name = saved_file;
        current = saved_current;
        loop_depth = saved_loops;
        var_count = saved_vars;
        depth = saved_depth;
    }
    if (f->node->ty == 0) {
        fail(n->line, "функция вызывает сама себя раньше первого return со значением: поставь простой return выше", n->name);
    }
    return f->node->ty;
}

// money.total(...)  —  вызов функции из модуля по имени модуля
static Type *check_module_call(Node *n) {
    char *mod = n->kids[0]->name;
    if (!module_uses(cur_mod, mod)) {
        fail(n->line, "этот модуль не подключён: добавь в начало файла  use", mod);
    }
    Func *f = find_func_in(n->name, mod);
    if (f == 0) {
        fail(n->line, "в этом модуле нет такой функции", n->name);
    }
    if (!f->node->is_pub && !same(mod, cur_mod)) {
        fail(n->line, "эта функция спрятана в своём модуле: поставь перед ней modpub", n->name);
    }
    // превращаем в обычный вызов: аргументы — kids после модуля
    n->kind = N_CALL;
    for (int i = 1; i < n->nkids; i++) {
        n->kids[i - 1] = n->kids[i];
    }
    n->nkids -= 1;
    return check_user_call(n, f);
}

static Type *check_dict_method(Node *n, Type *t) {
    int args = n->nkids - 1;
    if (same(n->name, "has") || same(n->name, "remove")) {
        if (args != 1) {
            fail(n->line, "нужен ровно один ключ", n->name);
        }
        Type *k = check_value(n->kids[1]->kids[0]);
        if (k != t->key) {
            fail(n->line, "ключ у этого dict другого типа", type_name(t->key));
        }
        n->kids[1]->ty = t->key;
        if (same(n->name, "has")) {
            return t_bool;
        }
        return t_none;
    }
    if (same(n->name, "keys") || same(n->name, "values")) {
        if (args != 0) {
            fail(n->line, "пишется без значений", n->name);
        }
        if (same(n->name, "keys")) {
            return list_of(t->key);
        }
        return list_of(t->elem);
    }
    fail(n->line, "у dict нет такого метода (есть .has .remove .keys .values)", n->name);
    return 0;
}

static Type *check_method(Node *n) {
    Node *base = n->kids[0];
    if (base->kind == N_NAME && find_var(base->name) == 0 && is_module(base->name)) {
        return check_module_call(n);
    }
    Type *t = check_value(base);
    int args = n->nkids - 1;
    for (int i = 1; i < n->nkids; i++) {
        if (n->kids[i]->name != 0) {
            fail(n->line, "у методов аргументы пишутся без имён", 0);
        }
    }
    if (is_dict(t)) {
        return check_dict_method(n, t);
    }
    if (!is_list(t)) {
        fail(n->line, "у этого значения нет методов: они есть у списков (.add .pop) и dict (.has .remove .keys .values)", n->name);
    }
    if (same(n->name, "add")) {
        if (args != 1) {
            fail(n->line, ".add кладёт ровно одно значение", 0);
        }
        Type *v = check_value(n->kids[1]->kids[0]);
        if (!fits(t->elem, v)) {
            fail(n->line, "в этот список кладут другой тип", type_name(t->elem));
        }
        n->kids[1]->ty = t->elem;
        return t_none;
    }
    if (same(n->name, "pop")) {
        if (args != 0) {
            fail(n->line, ".pop() пишется без значений", 0);
        }
        return t->elem;
    }
    fail(n->line, "у списка нет такого метода (есть .add и .pop)", n->name);
    return 0;
}

static Type *check_dict(Node *n) {
    if (n->nkids == 0) {
        fail(n->line, "пустой dict пишется с типом: d = dict text int", 0);
    }
    Type *key = check_value(n->kids[0]);
    Type *value = check_value(n->kids[1]);
    if (key != t_text && key != t_int) {
        fail(n->line, "ключ у dict бывает text или int", 0);
    }
    for (int i = 2; i + 1 < n->nkids; i += 2) {
        if (check_value(n->kids[i]) != key) {
            fail(n->kids[i]->line, "все ключи одного типа", type_name(key));
        }
        Type *v = check_value(n->kids[i + 1]);
        if (fits(value, v)) {
            continue;
        }
        if (value == t_int && v == t_dec) {
            value = t_dec;
            continue;
        }
        fail(n->kids[i + 1]->line, "все значения одного типа", type_name(value));
    }
    return dict_of(key, value);
}

// вызов, который может закончиться ошибкой (для  f() or 0,  try f(),  f() else err)
static int can_fail_call(Node *n) {
    if (n->kind == N_CALL) {
        if (is_builtin(n->name)) {
            return builtin_can_fail(n->name);
        }
        if (find_var(n->name) != 0 || find_obj(n->name) != 0) {
            return 0;
        }
        Func *f = find_func(n->name);
        return f != 0 && f->node->can_fail;
    }
    if (n->kind == N_METHOD && n->kids[0]->kind == N_NAME && find_var(n->kids[0]->name) == 0 && is_module(n->kids[0]->name)) {
        Func *f = find_func_in(n->name, n->kids[0]->name);
        return f != 0 && f->node->can_fail;
    }
    return 0;
}

// скрытые переменные для  f() or 0  (вывод C объявляет их заранее)
Type *temp_types[2000];
int temp_count = 0;

static void check_block(Node *block);

// вызов, ошибку которого кто-то ловит (or / try / else err)
static Type *check_handled(Node *n, char *how) {
    if (!can_fail_call(n)) {
        fail(n->line, "этот вызов не может закончиться ошибкой, здесь не нужно", how);
    }
    n->creates = 1;
    return check_expr(n);
}

static Type *check_expr(Node *n) {
    Type *t = 0;
    int k = n->kind;
    if (k == N_NUMBER) {
        t = t_int;
    } else if (k == N_DECIMAL) {
        t = t_dec;
    } else if (k == N_STRING) {
        t = t_text;
    } else if (k == N_BOOL) {
        t = t_bool;
    } else if (k == N_NAME) {
        Var *v = find_var(n->name);
        if (v == 0) {
            if (find_func(n->name) != 0) {
                fail(n->line, "у функции нужны скобки", n->name);
            }
            if (find_obj(n->name) != 0) {
                fail(n->line, "чтобы создать obj, нужны скобки", join2(n->name, "(...)"));
            }
            fail(n->line, "нет такой переменной", n->name);
        }
        n->is_link = v->is_link;
        t = v->type;
    } else if (k == N_LIST) {
        t = check_list(n);
    } else if (k == N_DICT) {
        t = check_dict(n);
    } else if (k == N_TRY) {
        t = check_handled(n->kids[0], "try");
    } else if (k == N_ELSE_ERR) {
        t = check_handled(n->kids[0], "else");
        open_scope();
        add_var(n->name, t_text, 1, 0, n->line);
        check_block(n->kids[1]);
        close_scope();
    } else if (k == N_CALL) {
        t = check_call(n);
    } else if (k == N_METHOD) {
        t = check_method(n);
    } else if (k == N_BINOP) {
        t = check_binop(n);
    } else if (k == N_UNARY) {
        Type *inner = check_value(n->kids[0]);
        if (same(n->value, "not")) {
            if (inner != t_bool) {
                fail(n->line, "not работает только с true и false", 0);
            }
            t = t_bool;
        } else {
            if (!is_number(inner)) {
                fail(n->line, "минус ставится только перед числом", 0);
            }
            t = inner;
        }
    } else if (k == N_INDEX && is_dict(check_value(n->kids[0]))) {
        Type *base = n->kids[0]->ty;
        if (check_value(n->kids[1]) != base->key) {
            fail(n->line, "ключ у этого dict другого типа", type_name(base->key));
        }
        t = base->elem;
    } else if (k == N_INDEX) {
        Type *base = check_value(n->kids[0]);
        Type *index = check_value(n->kids[1]);
        if (index != t_int) {
            fail(n->line, "номер в списке должен быть int", 0);
        }
        if (base == t_text) {
            t = t_text;
        } else if (is_list(base)) {
            t = base->elem;
        } else {
            fail(n->line, "[номер] берут у списка или текста", 0);
        }
    } else if (k == N_FIELD) {
        Type *base = check_value(n->kids[0]);
        if (base->kind != TY_OBJ) {
            fail(n->line, "точка с качеством бывает только у obj", n->name);
        }
        Node *field = find_field(base->obj, n->name);
        if (field == 0) {
            fail(n->line, "у этого obj нет такого качества", n->name);
        }
        t = field->ty;
    }
    n->ty = t;
    return t;
}

static void need_bool(Node *n) {
    if (check_value(n) != t_bool) {
        fail(n->line, "условие должно быть true или false (например a > 3)", 0);
    }
}

static void need_int(Node *n, char *what) {
    if (check_value(n) != t_int) {
        fail(n->line, "здесь нужно целое число int", what);
    }
}

// ---------- команды ----------

static void check_block(Node *block);

static void check_assign(Node *n) {
    Type *declared = 0;
    if (n->tsyn != 0) {
        declared = resolve_type(n->tsyn);
    }
    Type *t = 0;
    if (n->nkids == 1) {
        t = check_value(n->kids[0]);
    }
    Var *v = find_var(n->name);
    if (v == 0) {
        Type *type = t;
        if (declared != 0) {
            if (t != 0 && !fits(declared, t)) {
                fail(n->line, "значение не того типа, что написан", type_name(declared));
            }
            type = declared;
        }
        n->creates = 1;
        n->ty = type;
        add_var(n->name, type, n->is_const, 0, n->line);
        return;
    }
    if (n->is_const || declared != 0) {
        fail(n->line, "переменная уже есть: тип и const пишутся только в первый раз", n->name);
    }
    if (v->is_const) {
        fail(n->line, "это const или переменная цикла, её нельзя менять", n->name);
    }
    if (!fits(v->type, t)) {
        fail(n->line, "переменная другого типа", type_name(v->type));
    }
    n->is_link = v->is_link;
    n->ty = v->type;
}

static void check_set(Node *n) {
    Node *target = n->kids[0];
    Type *to = check_value(target);
    need_place(target, "менять можно только переменную, u.age или nums[i]");
    Type *t = check_value(n->kids[1]);
    n->ty = to;
    char *op = n->value;
    if (same(op, "=")) {
        if (!fits(to, t)) {
            fail(n->line, "здесь лежит другой тип", type_name(to));
        }
        return;
    }
    if (to == t_text && same(op, "+=")) {
        if (!is_scalar(t)) {
            fail(n->line, "к тексту можно приписать только текст, число или bool", 0);
        }
        return;
    }
    if (!is_number(to) || !is_number(t)) {
        fail(n->line, "эта операция работает только с числами", op);
    }
    if (to == t_int && t == t_dec) {
        fail(n->line, "в int нельзя добавить дробь: сделай его dec", 0);
    }
}

static void check_linkdecl(Node *n) {
    Node *target = n->kids[0];
    Type *t = check_value(target);
    need_place(target, "ярлык link ведёт к переменной, u.age или nums[i]");
    n->ty = t;
    add_var(n->name, t, 0, 1, n->line);
}

static void check_multi(Node *n) {
    Node *value = n->kids[n->nkids - 1];
    Node *call = value;
    if (value->kind == N_TRY || value->kind == N_ELSE_ERR) {
        call = value->kids[0];
    }
    if (call->kind != N_CALL && call->kind != N_METHOD) {
        fail(n->line, "справа должен быть вызов функции с gives", 0);
    }
    Type *t = check_expr(value);
    int names = n->nkids - 1;
    if (t->kind != TY_MULTI) {
        fail(n->line, "у этой функции один выход, пиши  x = ...", call->name);
    }
    Node *outputs = t->func->kids[2];
    if (outputs->nkids != names) {
        fail(n->line, "у функции другое число выходов", call->name);
    }
    for (int i = 0; i < names; i++) {
        Node *name = n->kids[i];
        Type *out = outputs->kids[i]->ty;
        Var *v = find_var(name->name);
        if (v == 0) {
            name->creates = 1;
            add_var(name->name, out, 0, 0, name->line);
        } else {
            if (v->is_const) {
                fail(name->line, "это const или переменная цикла, её нельзя менять", name->name);
            }
            if (!fits(v->type, out)) {
                fail(name->line, "переменная другого типа", type_name(v->type));
            }
            name->is_link = v->is_link;
        }
        name->ty = out;
    }
}

static void check_return(Node *n) {
    Node *f = current->node;
    if (f->kids[2]->nkids > 0) {
        if (n->nkids != 0) {
            fail(n->line, "у функции с gives return пишется без значения: он отдаёт выходы", 0);
        }
        return;
    }
    Type *t = t_none;
    if (n->nkids == 1) {
        t = check_value(n->kids[0]);
    }
    if (same(f->name, "main") && t != t_none) {
        fail(n->line, "main ничего не возвращает: пиши просто return", 0);
    }
    if (f->ty == 0) {
        f->ty = t;
    } else if (f->ty == t_int && t == t_dec) {
        fail(n->line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", 0);
    } else if (!fits(f->ty, t)) {
        fail(n->line, "раньше эта функция вернула другой тип", type_name(f->ty));
    }
}

static void check_statement(Node *n) {
    int k = n->kind;
    if (k == N_ASSIGN) {
        check_assign(n);
    } else if (k == N_SET) {
        check_set(n);
    } else if (k == N_LINKDECL) {
        check_linkdecl(n);
    } else if (k == N_MULTI) {
        check_multi(n);
    } else if (k == N_IF) {
        for (int i = 0; i + 1 < n->nkids; i += 2) {
            need_bool(n->kids[i]);
            check_block(n->kids[i + 1]);
        }
        if (n->nkids % 2 == 1) {
            check_block(n->kids[n->nkids - 1]);
        }
    } else if (k == N_EACH) {
        Type *t = check_value(n->kids[0]);
        Type *elem = t_text;
        if (is_list(t)) {
            elem = t->elem;
        } else if (is_dict(t)) {
            elem = t->key;
        } else if (t != t_text) {
            fail(n->line, "each ходит по списку, по ключам dict или по буквам текста", 0);
        }
        open_scope();
        add_var(n->name, elem, 1, 0, n->line);
        loop_depth += 1;
        check_block(n->kids[1]);
        loop_depth -= 1;
        close_scope();
    } else if (k == N_COUNT) {
        need_int(n->kids[0], "from");
        need_int(n->kids[1], "to");
        open_scope();
        add_var(n->name, t_int, 1, 0, n->line);
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
    } else if (k == N_FAIL) {
        if (check_value(n->kids[0]) != t_text) {
            fail(n->line, "после fail пишется текст ошибки: fail \"нет файла\"", 0);
        }
    } else if (k == N_EXPR_STMT) {
        Node *e = n->kids[0];
        Node *inner = e;
        if (e->kind == N_TRY || e->kind == N_ELSE_ERR) {
            inner = e->kids[0];
        }
        if (inner->kind != N_CALL && inner->kind != N_METHOD) {
            fail(n->line, "строка ничего не делает", 0);
        }
        Type *t = check_expr(e);
        if (t != t_none) {
            fail(n->line, "результат вызова пропадает: сохрани его, например x = ...", e->name);
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
    cur_mod = f->mod;
    file_name = f->node->file;
    f->state = 1;
    loop_depth = 0;
    open_scope();
    Node *params = f->node->kids[0];
    Node *outputs = f->node->kids[2];
    for (int i = 0; i < params->nkids; i++) {
        Node *p = params->kids[i];
        add_var(p->name, p->ty, 0, p->is_link, p->line);
    }
    for (int i = 0; i < outputs->nkids; i++) {
        Node *o = outputs->kids[i];
        Var *same_param = find_var(o->name);
        if (same_param != 0) {
            // выход с именем параметра: это та же переменная
            if (same_param->type != o->ty) {
                fail(o->line, "выход и параметр с одним именем должны быть одного типа", o->name);
            }
            o->is_const = 1;   // для вывода C: отдельно не объявлять
        } else {
            add_var(o->name, o->ty, 0, 0, o->line);
        }
    }
    check_block(f->node->kids[1]);
    close_scope();
    if (f->node->ty == 0) {
        f->node->ty = t_none;
    }
    f->state = 2;
}

static void register_obj(Node *o) {
    if (find_obj(o->name) != 0 || is_builtin(o->name)) {
        fail(o->line, "это имя уже занято", o->name);
    }
    if (obj_count == 500) {
        fail(o->line, "слишком много obj", 0);
    }
    objs[obj_count] = o;
    obj_count += 1;
}

static void register_func(Node *fn) {
    if (find_func_in(fn->name, fn->mod) != 0 || find_obj(fn->name) != 0 || is_builtin(fn->name)) {
        fail(fn->line, "это имя уже занято", fn->name);
    }

    if (func_count == 1000) {
        fail(fn->line, "слишком много функций", 0);
    }
    funcs[func_count].name = fn->name;
    funcs[func_count].mod = fn->mod;
    funcs[func_count].node = fn;
    funcs[func_count].state = 0;
    func_count += 1;
}

// есть ли внутри fail или try: тогда функция может закончиться ошибкой
static int has_fail(Node *n) {
    if (n == 0) {
        return 0;
    }
    if (n->kind == N_FAIL || n->kind == N_TRY) {
        return 1;
    }
    for (int i = 0; i < n->nkids; i++) {
        if (has_fail(n->kids[i])) {
            return 1;
        }
    }
    return 0;
}

static int links_known(Node *fn) {
    Node *params = fn->kids[0];
    for (int i = 0; i < params->nkids; i++) {
        if (params->kids[i]->ty == 0) {
            return 0;
        }
    }
    return 1;
}

static void resolve_params(Node *list) {
    for (int i = 0; i < list->nkids; i++) {
        Node *p = list->kids[i];
        for (int j = 0; j < i; j++) {
            if (same(list->kids[j]->name, p->name)) {
                fail(p->line, "это имя уже есть в списке", p->name);
            }
        }
        if (p->tsyn != 0) {
            p->ty = resolve_type(p->tsyn);
        }
    }
}

void check(Node *program) {
    t_none = make_type(TY_NONE, 0, 0, 0);
    t_int = make_type(TY_INT, 0, 0, 0);
    t_dec = make_type(TY_DEC, 0, 0, 0);
    t_bool = make_type(TY_BOOL, 0, 0, 0);
    t_text = make_type(TY_TEXT, 0, 0, 0);

    // 0) модули и их use
    char *main_mod = "";
    if (program->nkids > 0) {
        main_mod = program->kids[0]->mod;
    }
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        if (!is_module(top->mod)) {
            all_mods[mod_count] = top->mod;
            mod_count += 1;
        }
        if (top->kind == N_USE) {
            if (use_count == 500) {
                fail(top->line, "слишком много use", 0);
            }
            uses[use_count].mod = top->mod;
            uses[use_count].used = top->name;
            use_count += 1;
        }
    }
    // 1) имена всех obj и функций — чтобы их можно было использовать до объявления
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        file_name = top->file;
        if (top->kind == N_OBJ) {
            register_obj(top);
        } else if (top->kind == N_FUNC) {
            register_func(top);
            top->can_fail = has_fail(top->kids[1]);
            if (same(top->name, "main") && !same(top->mod, main_mod)) {
                fail(top->line, "main бывает только в главном файле, в модуле её быть не должно", 0);
            }
        }
    }
    // 2) типы качеств, параметров и выходов
    for (int i = 0; i < program->nkids; i++) {
        Node *top = program->kids[i];
        file_name = top->file;
        if (top->kind == N_OBJ) {
            resolve_params(top);
        } else if (top->kind == N_FUNC) {
            resolve_params(top->kids[0]);
            resolve_params(top->kids[2]);
            Node *outputs = top->kids[2];
            if (outputs->nkids == 1) {
                top->ty = outputs->kids[0]->ty;
            } else if (outputs->nkids > 1) {
                top->ty = make_type(TY_MULTI, 0, 0, top);
            }
        }
    }
    Func *main_func = find_func_in("main", main_mod);
    if (program->nkids > 0) {
        file_name = program->kids[0]->file;
    }
    if (main_func == 0) {
        fail(0, "нет функции main: программа начинается с func main()", 0);
    }
    if (main_func->node->kids[0]->nkids != 0 || main_func->node->kids[2]->nkids != 0) {
        fail(main_func->node->line, "у main не бывает параметров и gives", 0);
    }
    // 3) сначала main: так ярлыки link узнают свои типы по вызовам
    check_func(main_func);
    // 4) остальные функции; та, у которой ярлык ещё без типа, ждёт, пока её вызовет другая
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < func_count; i++) {
            if (funcs[i].state != 0 || !links_known(funcs[i].node)) {
                continue;
            }
            var_count = 0;
            depth = 0;
            var_base = 0;
            check_func(&funcs[i]);
            progress = 1;
        }
    }
    for (int i = 0; i < func_count; i++) {
        if (funcs[i].state == 0) {
            file_name = funcs[i].node->file;
            fail(funcs[i].node->line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", funcs[i].name);
        }
    }
}
