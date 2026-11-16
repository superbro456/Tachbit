// Tachbit 0.2 — общие типы для всех слоёв компилятора.
// Код написан простым C (функции, if, циклы, struct, массивы), чтобы потом
// переписать его строчка в строчку на сам Tachbit (самосборка).
#ifndef TACHBIT_AST_H
#define TACHBIT_AST_H

#include <stdio.h>

// ---------- память (arena.c) ----------
void *arena_alloc(long size);
char *arena_copy(char *src, int len);

// ---------- токены (lexer.c) ----------
enum {
    T_NAME,      // имя, которое придумал программист: total, rock
    T_COMMAND,   // слово языка: func, each, in, int ...
    T_NUMBER,    // 42
    T_DECIMAL,   // 9.5
    T_STRING,    // "текст"
    T_SIGN,      // ( ) [ ] , : = += == < ...
    T_NEWLINE,   // конец строки
    T_INDENT,    // отступ стал больше: начался блок
    T_DEDENT,    // отступ стал меньше: блок кончился
    T_END        // конец файла
};

typedef struct {
    int kind;
    char *text;
    int line;
} Token;

extern Token *tokens;
extern int token_count;

void lex(char *source, int size);
void print_tokens(void);
char *token_kind_name(int kind);

// ---------- типы языка (check.c) ----------
enum {
    TY_UNKNOWN, TY_NONE, TY_INT, TY_DEC, TY_BOOL, TY_TEXT,
    TY_LIST,    // список: elem — тип элементов
    TY_OBJ,     // объект: obj — его объявление (N_OBJ)
    TY_MULTI    // несколько выходов gives: func — функция
};

typedef struct Node Node;
typedef struct Type Type;
struct Type {
    int kind;
    Type *elem;
    Node *obj;
    Node *func;
    int id;      // номер типа: из него делаются имена в C (tb_show_3 ...)
};

// ---------- дерево (parser.c) ----------
enum {
    N_PROGRAM,
    N_FUNC,         // kids: параметры (N_BLOCK), тело (N_BLOCK), выходы gives (N_BLOCK)
    N_PARAM,        // name, tsyn = тип; is_link — параметр-ярлык (тип узнаётся по вызову)
    N_BLOCK,
    N_OBJ,          // obj Name: kids = N_PARAM (качества)
    N_TYPE,         // запись типа: name = int / dec / text / bool / list / имя obj; kids[0] = тип элементов
    N_ASSIGN,       // x = ...   (name, tsyn = тип, если написан; kids[0] = значение, может не быть)
    N_SET,          // цель = / += ... значение: value = знак, kids[0] = цель, kids[1] = значение
    N_LINKDECL,     // hp: link u.age   (name, kids[0] = на что ведёт)
    N_MULTI,        // q, r = divide(...)   kids: N_NAME..., последний — вызов
    N_IF,           // kids: условие, блок, [условие, блок]..., [блок else]
    N_EACH,         // each name in kids[0]: kids[1] = блок
    N_COUNT,        // count name from kids[0] to kids[1]: kids[2] = блок
    N_REPEAT_TIMES, // repeat kids[0] times: kids[1] = блок
    N_REPEAT_WHILE, // repeat while kids[0]: kids[1] = блок
    N_SKIP, N_STOP,
    N_RETURN,       // [kids[0]]
    N_EXPR_STMT,    // вызов как отдельная строка
    N_NUMBER, N_DECIMAL, N_STRING, N_BOOL, N_NAME,
    N_LIST,         // [a, b, c]
    N_CALL,         // name(kids = N_ARG)
    N_ARG,          // name (может быть 0) : kids[0]
    N_BINOP,        // value = "+", kids[0], kids[1]
    N_UNARY,        // value = "-" или "not", kids[0]
    N_INDEX,        // kids[0][kids[1]]  —  nums[2]
    N_FIELD,        // kids[0].name  —  u.age
    N_METHOD        // kids[0].name(аргументы kids[1..])  —  nums.add(5)
};

struct Node {
    int kind;
    int line;
    char *name;      // имя переменной / функции / параметра
    char *value;     // текст числа, строки или знак операции
    Node *tsyn;      // тип, как его написал программист (N_TYPE)
    Type *ty;        // тип, который выяснила проверка
    int is_const;    // const x = ...
    int is_link;     // параметр link hp  /  имя, которое является ярлыком
    int creates;     // проверка: это присваивание создаёт новую переменную
    Node *ref;       // проверка: для вызова — функция или obj, для имени — объявление
    Node **kids;
    int nkids;
    int cap;
};

Node *new_node(int kind, int line);
void add_kid(Node *parent, Node *kid);
Node *parse(void);
void print_tree(Node *n, int depth);
char *node_kind_name(int kind);

// ---------- проверка (check.c) ----------
void check(Node *program);
char *type_name(Type *t);
extern Type *all_types[];
extern int type_count;

// ---------- вывод C (emit_c.c) ----------
void emit_c(Node *program, FILE *out);

// ---------- общее ----------
int same(char *a, char *b);
void fail(int line, char *message, char *detail);

#endif
