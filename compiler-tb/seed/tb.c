// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb
// Библиотека, которую Tachbit кладёт в начало каждой программы на C.
// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,
// который делает tools/embed_runtime.py. Поменял этот файл — запусти:
//     python3 tools/embed_runtime.py
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается
#if defined(__GNUC__)
#define TB_RARE(x) __builtin_expect(!!(x), 0)
#define TB_COLD __attribute__((cold, noreturn, noinline))
#else
#define TB_RARE(x) (x)
#define TB_COLD
#endif

// список: кусок памяти, len элементов по size байт, место на cap
typedef struct { char *data; long long len; long long cap; long long size; } tb_list;

TB_COLD static void tb_crash(int line, const char *msg) {
    fflush(stdout);
    // в терминале — красным (если не задан NO_COLOR)
    int color = isatty(2) && getenv("NO_COLOR") == 0;
    if (color) fputs("\033[31m", stderr);
    if (line > 0) fprintf(stderr, "строка %d: %s", line, msg);
    else fprintf(stderr, "%s", msg);
    fputs(color ? "\033[0m\n" : "\n", stderr);
    exit(1);
}
// ---------- temp: временная память ----------
// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),
// а в конце блока все куски отдаются разом. У каждого ядра свои куски.
typedef struct { char *mem; long long size, used; } tb_chunk;
typedef struct { int n; long long used; } tb_mark;
static _Thread_local tb_chunk *tb_chunks = 0;
static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;
static int tb_in_temp(const void *p) {
    for (int i = tb_nchunks - 1; i >= 0; i--) {
        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;
    }
    return 0;
}
static void *tb_temp_alloc(long long n) {
    n = n > 0 ? (n + 15) & ~15LL : 16;
    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;
    if (c == 0 || c->used + n > c->size) {
        long long size = c != 0 ? c->size * 2 : (1LL << 20);
        if (size < n) size = n;
        if (tb_nchunks == tb_capchunks) {
            tb_capchunks = tb_capchunks * 2 + 8;
            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);
            if (tb_chunks == 0) tb_crash(0, "не хватило памяти");
        }
        char *mem = malloc(size);
        if (mem == 0) tb_crash(0, "не хватило памяти");
        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};
        c = &tb_chunks[tb_nchunks];
        tb_nchunks += 1;
    }
    void *p = c->mem + c->used;
    c->used += n;
    return p;
}
// метки открытых temp (и арен шага / функции): самая последняя — текущая область
static _Thread_local tb_mark *tb_marks = 0;
static _Thread_local int tb_capmarks = 0;
static inline tb_mark tb_temp_begin(void) {
    tb_mark m = {tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};
    if (tb_temp_depth == tb_capmarks) {
        tb_capmarks = tb_capmarks * 2 + 16;
        tb_marks = realloc(tb_marks, sizeof(tb_mark) * tb_capmarks);
        if (tb_marks == 0) tb_crash(0, "не хватило памяти");
    }
    tb_marks[tb_temp_depth] = m;
    tb_temp_depth += 1;
    return m;
}
// p во временной памяти текущей (самой внутренней) области, а не во внешней
static int tb_in_cur_temp(const void *p) {
    if (tb_temp_depth == 0) return 0;
    tb_mark t = tb_marks[tb_temp_depth - 1];
    for (int i = tb_nchunks - 1; i >= 0; i--) {
        const char *m = tb_chunks[i].mem;
        if ((const char *)p >= m && (const char *)p < m + tb_chunks[i].size)
            return i > t.n - 1 || (i == t.n - 1 && (const char *)p - m >= t.used);
    }
    return 0;
}
// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз
static _Thread_local const char *tb_lc_p[2];
static _Thread_local long long tb_lc_n[2];
static _Thread_local int tb_lc_next;
static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }
static inline void tb_temp_end(tb_mark m) {
    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc
    while (tb_nchunks > m.n && tb_nchunks > 1) {
        tb_nchunks -= 1;
        free(tb_chunks[tb_nchunks].mem);
    }
    if (tb_nchunks > m.n) tb_chunks[0].used = 0;
    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;
    tb_temp_depth -= 1;
    tb_lc_clear();
}
static void *tb_mem(long long n) {
    if (tb_temp_depth > 0) return tb_temp_alloc(n);
    void *p = malloc(n > 0 ? n : 1);
    if (p == 0) tb_crash(0, "не хватило памяти");
    return p;
}
// вырасти кусок: временный текущей области растёт там же, обычный — через realloc.
// Кусок из внешней области (список создан снаружи, растёт внутри temp или шага цикла)
// переезжает в обычную память: внутренняя область скоро отдаётся, а список живёт дольше.
static void *tb_resize(void *p, long long old_n, long long new_n) {
    tb_lc_clear();
    if (tb_nchunks > 0 && tb_in_temp(p)) {
        void *q = tb_in_cur_temp(p) ? tb_temp_alloc(new_n) : malloc(new_n > 0 ? new_n : 1);
        if (q == 0) tb_crash(0, "не хватило памяти");
        memcpy(q, p, old_n);
        return q;
    }
    p = realloc(p, new_n > 0 ? new_n : 1);
    if (p == 0) tb_crash(0, "не хватило памяти");
    return p;
}
static tb_list *tb_list_new(long long size) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);
    return l;
}
static tb_list *tb_list_from(long long n, long long size, const void *src) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);
    memcpy(l->data, src, size * n);
    return l;
}
// size передаётся как sizeof(T): после встраивания C знает его заранее
static inline void *tb_at(tb_list *l, long long i, long long size, int line) {
    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    return l->data + i * size;
}
static inline void *tb_grow(tb_list *l) {
    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски
    if (TB_RARE(l->len >= l->cap)) {
        if (l->cap < 0) tb_crash(0, "список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)");
        long long old = l->cap * l->size;
        l->cap = l->cap * 2 + 4;
        // realloc часто растит большой кусок на месте, без копирования
        l->data = tb_resize(l->data, old, l->cap * l->size);
    }
    l->len += 1;
    return l->data + (l->len - 1) * l->size;
}
static void *tb_pop(tb_list *l, int line) {
    if (l->len == 0) tb_crash(line, ".pop() у пустого списка");
    l->len -= 1;
    return l->data + l->len * l->size;
}
// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------
// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита
static tb_list *tb_bits_new(void) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem(8);
    memset(l->data, 0, 8);
    return l;
}
static tb_list *tb_bits_from(long long n, const unsigned char *src) {
    tb_list *l = tb_mem(sizeof(tb_list));
    long long words = (n + 63) / 64 + (n == 0);
    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem(words * 8);
    memset(l->data, 0, words * 8);
    unsigned long long *w = (unsigned long long *)l->data;
    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);
    return l;
}
// бит без проверки номера (для each — номер всегда внутри)
#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))
static inline _Bool tb_bget(tb_list *l, long long i, int line) {
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    return TB_BIT(l, i);
}
static inline void tb_bset(tb_list *l, long long i, _Bool v, int line) {
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);
    *w = v ? (*w | m) : (*w & ~m);
}
static inline void tb_badd(tb_list *l, _Bool v) {
    if (TB_RARE(l->len >= l->cap)) {
        if (l->cap < 0) tb_crash(0, "список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)");
        long long old = l->cap / 8;
        l->cap = l->cap * 2;
        l->data = tb_resize(l->data, old, l->cap / 8);
        memset(l->data + old, 0, l->cap / 8 - old);
    }
    long long i = l->len++;
    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);
    *w = v ? (*w | m) : (*w & ~m);
}
static inline _Bool tb_bpop(tb_list *l, int line) {
    if (l->len == 0) tb_crash(line, ".pop() у пустого списка");
    l->len -= 1;
    return TB_BIT(l, l->len);
}
// N одинаковых битов в конец (repeat N times: flags.add(true))
static void tb_bfill(tb_list *l, long long n, _Bool v) {
    if (n <= 0) return;
    if (l->cap < 0) tb_crash(0, "список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)");
    long long need = l->len + n;
    if (need > l->cap) {
        long long old = l->cap / 8, cap = l->cap;
        while (cap < need) cap *= 2;
        l->data = tb_resize(l->data, old, cap / 8);
        memset(l->data + old, 0, cap / 8 - old);
        l->cap = cap;
    }
    unsigned long long *w = (unsigned long long *)l->data;
    long long i = l->len;
    for (; i < need && (i & 63); i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));
    if (need - i >= 64) { memset(w + (i >> 6), v ? 0xff : 0, ((need - i) >> 6) * 8); i += (need - i) & ~63LL; }
    for (; i < need; i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));
    l->len = need;
}
// значения dict bool лежат байтами — в list bool их надо переложить в биты
static tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }
// N одинаковых элементов в конец (repeat N times: nums.add(0))
static void tb_fill(tb_list *l, long long n, const void *v) {
    if (n <= 0) return;
    if (l->len + n > l->cap) {
        if (l->cap < 0) tb_crash(0, "список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)");
        long long cap = l->cap * 2 + 4;
        if (cap < l->len + n) cap = l->len + n;
        l->data = tb_resize(l->data, l->cap * l->size, cap * l->size);
        l->cap = cap;
    }
    char *p = l->data + l->len * l->size;
    int zero = 1;
    for (long long b = 0; b < l->size; b++) if (((const char *)v)[b]) zero = 0;
    if (zero) memset(p, 0, n * l->size);
    else for (long long i = 0; i < n; i++) memcpy(p + i * l->size, v, l->size);
    l->len += n;
}
static void *tb_nn(void *p, int line) {
    if (p == 0) tb_crash(line, "obj пустой: его ещё не создали");
    return p;
}
static const char *tb_cat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = tb_mem(la + lb + 1);
    memcpy(s, a, la);
    memcpy(s + la, b, lb + 1);
    return s;
}
static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, "%lld", x); return s; }
static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, "%g", x); return s; }
static const char *tb_btos(int x) { return x ? "true" : "false"; }
// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву
// таблица постоянная: компилятор C видит, что второй байт всегда 0, и  ch == ","  становится одним сравнением байта
static const char tb_chars[512] = {
    0,0,1,0,2,0,3,0,4,0,5,0,6,0,7,0,8,0,9,0,10,0,11,0,12,0,13,0,14,0,15,0,
    16,0,17,0,18,0,19,0,20,0,21,0,22,0,23,0,24,0,25,0,26,0,27,0,28,0,29,0,30,0,31,0,
    32,0,33,0,34,0,35,0,36,0,37,0,38,0,39,0,40,0,41,0,42,0,43,0,44,0,45,0,46,0,47,0,
    48,0,49,0,50,0,51,0,52,0,53,0,54,0,55,0,56,0,57,0,58,0,59,0,60,0,61,0,62,0,63,0,
    64,0,65,0,66,0,67,0,68,0,69,0,70,0,71,0,72,0,73,0,74,0,75,0,76,0,77,0,78,0,79,0,
    80,0,81,0,82,0,83,0,84,0,85,0,86,0,87,0,88,0,89,0,90,0,91,0,92,0,93,0,94,0,95,0,
    96,0,97,0,98,0,99,0,100,0,101,0,102,0,103,0,104,0,105,0,106,0,107,0,108,0,109,0,110,0,111,0,
    112,0,113,0,114,0,115,0,116,0,117,0,118,0,119,0,120,0,121,0,122,0,123,0,124,0,125,0,126,0,127,0,
    128,0,129,0,130,0,131,0,132,0,133,0,134,0,135,0,136,0,137,0,138,0,139,0,140,0,141,0,142,0,143,0,
    144,0,145,0,146,0,147,0,148,0,149,0,150,0,151,0,152,0,153,0,154,0,155,0,156,0,157,0,158,0,159,0,
    160,0,161,0,162,0,163,0,164,0,165,0,166,0,167,0,168,0,169,0,170,0,171,0,172,0,173,0,174,0,175,0,
    176,0,177,0,178,0,179,0,180,0,181,0,182,0,183,0,184,0,185,0,186,0,187,0,188,0,189,0,190,0,191,0,
    192,0,193,0,194,0,195,0,196,0,197,0,198,0,199,0,200,0,201,0,202,0,203,0,204,0,205,0,206,0,207,0,
    208,0,209,0,210,0,211,0,212,0,213,0,214,0,215,0,216,0,217,0,218,0,219,0,220,0,221,0,222,0,223,0,
    224,0,225,0,226,0,227,0,228,0,229,0,230,0,231,0,232,0,233,0,234,0,235,0,236,0,237,0,238,0,239,0,
    240,0,241,0,242,0,243,0,244,0,245,0,246,0,247,0,248,0,249,0,250,0,251,0,252,0,253,0,254,0,255,0,
};
static inline const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }
static inline long long tb_tlen(const char *s) {
    if (tb_lc_p[0] == s) return tb_lc_n[0];
    if (tb_lc_p[1] == s) return tb_lc_n[1];
    long long n = (long long)strlen(s);
    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;
    return n;
}
// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1
static inline const char *tb_char_at(const char *s, long long i, long long n, int line) {
    if (n < 0) n = tb_tlen(s);
    if (i < 0 || i >= n) tb_crash(line, "номер за пределами текста");
    return tb_chr((unsigned char)s[i]);
}
// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.
// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),
// *rc — сколько там места.
static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);
// число цифрами в конец буфера d[24]: вернёт начало цифр, длину — в *n
static inline const char *tb_digits(long long x, char *d, long long *n) {
    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;
    char *p = d + 24;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (x < 0) *--p = '-';
    *n = d + 24 - p;
    return p;
}
static inline void tb_app(const char **p, long long *rl, long long *rc, const char *b) {
    tb_app_n(p, rl, rc, b, (long long)strlen(b));
}
// дописать число: цифры пишутся прямо в конец, без отдельного текста
static inline void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {
    char d[24]; int n = 0;
    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;
    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (x < 0) d[23 - n++] = '-';
    tb_app_n(p, rl, rc, d + 24 - n, n);
}
static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);
// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии
static void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {
    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }
    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));
}
static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {
    char *s = (char *)*p;
    if (*rl < 0) {
        long long la = (long long)strlen(s);
        long long cap = 2 * (la + lb) + 1;
        if (cap < 64) cap = 64;
        char *r = tb_mem(cap);
        memcpy(r, s, la);
        memcpy(r + la, b, lb);
        r[la + lb] = 0;
        *p = r; *rl = la + lb; *rc = cap;
        return;
    }
    long long need = *rl + lb + 1;
    if (need > *rc) {
        long long cap = 2 * need;
        long long off = (b >= s && b < s + *rc) ? b - s : -1;
        s = tb_resize(s, *rl + 1, cap);
        if (off >= 0) b = s + off;
        *p = s; *rc = cap;
    }
    memmove(s + *rl, b, lb);
    *rl += lb;
    s[*rl] = 0;
}
static const char *tb_cut(const char *s, long long from, long long to, int line) {
    long long n = (long long)strlen(s);
    if (from < 0 || to >= n || from > to + 1) tb_crash(line, "cut: номера за пределами текста");
    char *r = tb_mem(to - from + 2);
    memcpy(r, s + from, to - from + 1);
    r[to - from + 1] = 0;
    return r;
}
static long long tb_code(const char *s, int line) {
    if (s[0] == 0) tb_crash(line, "code: текст пустой");
    return (unsigned char)s[0];
}
static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, "деление на ноль"); return a / b; }
static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, "остаток от деления на ноль"); return a % b; }
static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }
static void tb_ps(const char *s) { fputs(s, stdout); }



// ---------- ошибки fail / try / or / else err ----------
// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.
// Тот, кто её вызвал, смотрит в tb_err.
static _Thread_local const char *tb_err = 0;   // у каждого ядра своя

TB_COLD static void tb_die(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "ошибка: %s\n", msg);
    exit(1);
}

// ---------- текст ----------
static const char *tb_upper(const char *s) {
    size_t n = strlen(s); char *r = tb_mem(n + 1);
    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    return r;
}
static const char *tb_lower(const char *s) {
    size_t n = strlen(s); char *r = tb_mem(n + 1);
    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    return r;
}
static int tb_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static const char *tb_trim(const char *s) {
    size_t a = 0, b = strlen(s);
    while (a < b && tb_space(s[a])) a++;
    while (b > a && tb_space(s[b - 1])) b--;
    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;
    return r;
}
static long long tb_find(const char *s, const char *part) {
    const char *p = strstr(s, part);
    return p ? (long long)(p - s) : -1;
}
static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int tb_ends(const char *s, const char *p) {
    size_t a = strlen(s), b = strlen(p);
    return b <= a && strcmp(s + a - b, p) == 0;
}
static const char *tb_replace(const char *s, const char *from, const char *to, int line) {
    size_t lf = strlen(from), lt = strlen(to), count = 0;
    if (lf == 0) tb_crash(line, "replace: что заменять — пустой текст");
    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;
    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;
    const char *p = s, *q;
    while ((q = strstr(p, from)) != 0) {
        memcpy(w, p, q - p); w += q - p;
        memcpy(w, to, lt); w += lt;
        p = q + lf;
    }
    strcpy(w, p);
    return r;
}
static tb_list *tb_split(const char *s, const char *sep, int line) {
    size_t ls = strlen(sep);
    if (ls == 0) tb_crash(line, "split: разделитель — пустой текст");
    tb_list *l = tb_list_new(sizeof(const char *));
    const char *p = s, *q;
    while (1) {
        q = strstr(p, sep);
        size_t n = q ? (size_t)(q - p) : strlen(p);
        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;
        *(const char **)tb_grow(l) = part;
        if (!q) break;
        p = q + ls;
    }
    return l;
}
static const char *tb_join(tb_list *l, const char *sep) {
    size_t n = 0, ls = strlen(sep);
    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;
    char *r = tb_mem(n + 1), *w = r;
    for (long long i = 0; i < l->len; i++) {
        const char *part = ((const char **)l->data)[i];
        if (i > 0) { memcpy(w, sep, ls); w += ls; }
        memcpy(w, part, strlen(part)); w += strlen(part);
    }
    *w = 0;
    return r;
}
static long long tb_to_int(const char *s) {
    long long v = 0; int i = 0, minus = 0;
    if (s[0] == '-') { minus = 1; i = 1; }
    if (s[i] == 0) { tb_err = tb_cat("это не число: ", s); return 0; }
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat("это не число: ", s); return 0; }
        v = v * 10 + (s[i] - '0');
    }
    return minus ? -v : v;
}
static double tb_to_dec(const char *s) {
    char *end = 0;
    double v = strtod(s, &end);
    if (s[0] == 0 || *end != 0) { tb_err = tb_cat("это не дробное число: ", s); return 0; }
    return v;
}

// ---------- файлы, ввод, слова запуска ----------
static const char *tb_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == 0) { tb_err = tb_cat("не могу открыть файл: ", path); return ""; }
    size_t cap = 4096, n = 0, got;
    char *buf = tb_mem(cap);
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }
    }
    fclose(f);
    buf[n] = 0;
    return buf;
}
static void tb_write_file(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (f == 0) { tb_err = tb_cat("не могу записать файл: ", path); return; }
    fputs(s, f);
    fclose(f);
}
static const char *tb_input(void) {
    fflush(stdout);
    size_t cap = 256, n = 0;
    char *buf = tb_mem(cap);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }
        buf[n++] = (char)c;
    }
    if (c == EOF && n == 0) { tb_err = "ввод кончился"; return ""; }
    if (n > 0 && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    return buf;
}
static tb_list *tb_args;
static long long tb_run(const char *command) {
    fflush(stdout);
    int code = system(command);
    if (code == -1) return -1;
    return code == 0 ? 0 : (code > 255 ? code / 256 : code);
}
static void tb_exit(long long code) { fflush(stdout); exit((int)code); }
static int tb_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == 0) return 0;
    fclose(f);
    return 1;
}

// ---------- числа ----------
static long long tb_absi(long long x) { return x < 0 ? -x : x; }
static long long tb_mini(long long a, long long b) { return a < b ? a : b; }
static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }
static double tb_mind(double a, double b) { return a < b ? a : b; }
static double tb_maxd(double a, double b) { return a > b ? a : b; }
static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, "sqrt: корень из отрицательного числа"); return sqrt(x); }

// ---------- sort ----------
static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }
static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }
static tb_list *tb_copy(tb_list *l) {
    if (l->size == 0) {
        tb_list *r = tb_mem(sizeof(tb_list));
        long long bytes = l->cap / 8;
        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem(bytes);
        memcpy(r->data, l->data, bytes);
        return r;
    }
    return tb_list_from(l->len, l->size, l->data);
}
static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }

// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,
// а найти номер по ключу можно тремя способами (mode):
//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;
//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;
//   0 — таблица slots с хешем (текст или разбросанные числа) ----------
typedef struct {
    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;
    int mode; long long base; long long *pos; long long npos;
} tb_dict;

static unsigned long long tb_hash(tb_dict *d, const void *k) {
    unsigned long long h = 1469598103934665603ULL;
    if (d->text) {
        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    } else {
        h ^= (unsigned long long)*(const long long *)k;
        h *= 0x9E3779B97F4A7C15ULL;
        h ^= h >> 32;
    }
    return h;
}
static int tb_keq(tb_dict *d, const void *a, const void *b) {
    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;
    return *(const long long *)a == *(const long long *)b;
}
static inline long long tb_dfind(tb_dict *d, const void *k) {
    if (d->mode == 1) {
        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);
        return at < (unsigned long long)d->keys->len ? (long long)at : -1;
    }
    if (d->mode == 2) {
        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);
        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;
    }
    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;
    while (d->slots[i] != 0) {
        long long at = d->slots[i] - 1;
        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;
        i = (i + 1) & mask;
    }
    return -1;
}
// память для таблиц dict — там же, где лежат его ключи (временная или обычная)
static void *tb_dalloc(tb_dict *d, void *old, long long bytes) {
    if (old && !(tb_nchunks > 0 && tb_in_temp(old))) free(old);
    void *p = tb_in_cur_temp(d->keys->data) ? tb_temp_alloc(bytes) : malloc(bytes);
    if (!p) tb_crash(0, "не хватило памяти");
    return p;
}
static void tb_dreindex(tb_dict *d, long long nslots) {
    d->mode = 0;
    if (d->pos) {
        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);
        d->pos = 0; d->npos = 0;
    }
    d->nslots = nslots;
    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));
    memset(d->slots, 0, nslots * sizeof(long long));
    unsigned long long mask = nslots - 1;
    for (long long at = 0; at < d->keys->len; at++) {
        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;
        while (d->slots[i] != 0) i = (i + 1) & mask;
        d->slots[i] = at + 1;
    }
}
// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)
static void tb_dto_hash(tb_dict *d) {
    long long n = 8;
    while (n < d->keys->len * 2 + 2) n *= 2;
    tb_dreindex(d, n);
}
// mode 2: таблица pos на ключи base .. base+npos-1
static void tb_dto_pos(tb_dict *d, long long npos) {
    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));
    memset(p, 0, npos * sizeof(long long));
    const long long *ks = (const long long *)d->keys->data;
    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;
    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);
    d->pos = p;
    d->npos = npos;
    d->mode = 2;
}
static tb_dict *tb_dict_new(int text, long long vsize) {
    tb_dict *d = tb_mem(sizeof(tb_dict));
    d->text = text;
    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));
    d->vals = tb_list_new(vsize);
    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;
    if (text) tb_dreindex(d, 8);
    else { d->mode = 1; d->nslots = 0; }
    return d;
}
// .clear(): список снова пустой; свой кусок памяти отдаётся (если он не временный).
// Тексты и obj внутри не удаляются: на них могут смотреть другие переменные.
static void tb_lclear(tb_list *l, int line) {
    if (l->cap < 0) tb_crash(line, ".clear() нельзя, пока по списку идёт each ... by N");
    int temp = tb_nchunks > 0 && tb_in_temp(l->data);
    if (!temp) free(l->data);
    long long cap = l->size == 0 ? 64 : 4;
    long long bytes = l->size == 0 ? 8 : l->size * 4;
    l->data = tb_in_cur_temp(l->data) ? tb_temp_alloc(bytes) : malloc(bytes);
    if (l->data == 0) tb_crash(line, "не хватило памяти");
    memset(l->data, 0, bytes);
    l->len = 0; l->cap = cap;
}
static void tb_dclear(tb_dict *d, int line) {
    tb_lclear(d->keys, line);
    tb_lclear(d->vals, line);
    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);
    if (d->slots && !(tb_nchunks > 0 && tb_in_temp(d->slots))) free(d->slots);
    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;
    if (d->text) tb_dreindex(d, 8);
    else { d->mode = 1; d->nslots = 0; }
}
// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)
static void tb_dplace_int(tb_dict *d, long long k) {
    long long n = d->keys->len;          // уже вместе с новым ключом
    if (d->mode == 1) {
        if (n == 1) { d->base = k; return; }
        if (k == d->base + n - 1) return;  // подряд — остаёмся в mode 1
    }
    if (d->mode == 1 || d->mode == 2) {
        // плотно ли: ключи не дальше 4 ячеек на один ключ
        long long o = k - d->base;
        if (o >= 0 && o < 4 * n + 64) {
            if (d->mode == 2 && o < d->npos) { d->pos[o] = n; return; }
            long long want = d->npos * 2 > o + 1 ? d->npos * 2 : o + 1 + n;
            if (want < 16) want = 16;
            tb_dto_pos(d, want);
            return;
        }
        tb_dto_hash(d);
        return;
    }
}
// место под значение ключа k; если ключа нет — добавить со значением *def
static void *tb_dput(tb_dict *d, const void *k, const void *def) {
    long long at = tb_dfind(d, k);
    if (at >= 0) return d->vals->data + at * d->vals->size;
    memcpy(tb_grow(d->keys), k, d->keys->size);
    void *v = tb_grow(d->vals);
    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);
    if (d->mode != 0) {
        // без поиска; если ключ лёг далеко, tb_dplace_int сам перейдёт на хеш и впишет все ключи
        tb_dplace_int(d, *(const long long *)k);
        return v;
    }
    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);
    else {
        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;
        while (d->slots[i] != 0) i = (i + 1) & mask;
        d->slots[i] = d->keys->len;
    }
    return v;
}
static void *tb_dget(tb_dict *d, const void *k, int line) {
    long long at = tb_dfind(d, k);
    if (TB_RARE(at < 0)) {
        if (d->text) tb_crash(line, tb_cat("в dict нет ключа: ", *(const char * const *)k));
        tb_crash(line, tb_cat("в dict нет ключа: ", tb_itos(*(const long long *)k)));
    }
    return d->vals->data + at * d->vals->size;
}
// together: шаг 1 — просим память принести место ключа заранее
static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {
    if (d->mode == 1) {
        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);
        if (at < (unsigned long long)d->vals->len) __builtin_prefetch(d->vals->data + at * d->vals->size);
        return ~0ULL;
    }
    if (d->mode == 2) {
        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);
        if (o < (unsigned long long)d->npos) { __builtin_prefetch(&d->pos[o]); return o; }
        return ~0ULL;
    }
    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);
    __builtin_prefetch(&d->slots[i]);
    return i;
}
// шаг 2 — место приехало: просим принести ключ и значение
static inline void tb_dpre2(tb_dict *d, unsigned long long i) {
    if (i == ~0ULL || d->mode == 1) return;
    long long s = d->mode == 2 ? d->pos[i] : d->slots[i];
    if (s != 0) {
        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);
        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);
    }
}
// элемент списка заранее (prefetch за пределами списка ничего не ломает)
static inline void tb_lpre(tb_list *l, long long i) {
    __builtin_prefetch(l->data + i * l->size);
}
// ---------- холодные качества obj: своя память, подряд, отдельно от горячих ----------
static void *tb_cold_mem(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    static _Thread_local char *p = 0;
    static _Thread_local long long left = 0;
    n = (n + 15) & ~15LL;
    if (left < n) {
        long long size = n > 65536 ? n : 65536;
        p = malloc(size);
        if (!p) tb_crash(0, "не хватило памяти");
        left = size;
    }
    void *r = p;
    p += n;
    left -= n;
    memset(r, 0, n);
    return r;
}
// ---------- background: работа в своём потоке ----------
typedef struct { pthread_t th; int done; void *res; } tb_task;
static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {
    t->done = 0;
    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, "не получилось запустить background");
}
// a.wait: дождаться потока (один раз) и отдать адрес ответа
static void *tb_wait(tb_task *t) {
    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }
    return t->res;
}
static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }
static void tb_dremove(tb_dict *d, const void *k) {
    long long at = tb_dfind(d, k);
    if (at < 0) return;
    if (d->mode != 0) tb_dto_hash(d);
    long long last = d->keys->len - 1;
    // на место удалённого ставим последний: порядок меняется только у него
    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);
    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);
    d->keys->len -= 1;
    d->vals->len -= 1;
    tb_dreindex(d, d->nslots);
}
static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {
    tb_dict *d = tb_dict_new(text, vsize);
    long long ksize = d->keys->size;
    for (long long i = 0; i < n; i++) {
        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);
    }
    return d;
}

// ---------- on all cores: цикл на всех ядрах ----------
// Работа делится на куски; каждое ядро берёт следующий свободный кусок,
// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).
// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.
static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;
static int tb_cores(void) {
    const char *want = getenv("TB_CORES");   // TB_CORES=1 — проверить на одном ядре
    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);
}
static void tb_par_run(void *(*work)(void *), void *job, long long total) {
    int n = tb_cores();
    if (n > total) n = (int)total;
    if (n < 1) n = 1;
    pthread_t t[256];
    for (int i = 1; i < n; i++) {
        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, "не получилось запустить ядро");
    }
    work(job);
    for (int i = 1; i < n; i++) pthread_join(t[i], 0);
}
static long long tb_par_step(long long total) {
    long long s = total / ((long long)tb_cores() * 16);
    return s < 1 ? 1 : s;
}
typedef struct tb_o_Token tb_o_Token;
typedef struct tb_o_Node tb_o_Node;
typedef struct tb_o_Type tb_o_Type;
typedef struct tb_o_Func tb_o_Func;
typedef struct tb_o_Use tb_o_Use;
typedef struct tb_o_Var tb_o_Var;
typedef struct tb_o_Comp tb_o_Comp;
struct tb_o_Token {
    const char * q_kind;
    const char * q_word;
    long long q_line;
};
struct tb_o_Node {
    const char * q_kind;
    long long q_line;
    const char * q_name;
    const char * q_value;
    tb_o_Node * q_tsyn;
    _Bool q_has_tsyn;
    long long q_ty;
    _Bool q_is_const;
    _Bool q_is_link;
    _Bool q_creates;
    tb_o_Node * q_ref;
    _Bool q_has_ref;
    const char * q_mod;
    const char * q_file;
    _Bool q_is_pub;
    _Bool q_can_fail;
    long long q_temp_no;
    tb_list * q_kids;
    _Bool q_is_par;
    _Bool q_is_shared;
    _Bool q_by_cols;
    _Bool q_by_vec;
    _Bool q_by_piece;
    tb_o_Node * q_piece_size;
    tb_list * q_pre_bases;
    tb_list * q_pre_idx;
    _Bool q_hot;
    _Bool q_is_cold;
    tb_list * q_caps;
};
struct tb_o_Type {
    const char * q_kind;
    long long q_key;
    long long q_elem;
    tb_o_Node * q_obj_node;
    const char * q_obj_name;
    tb_o_Node * q_func_node;
    const char * q_func_key;
    long long q_id;
};
struct tb_o_Func {
    const char * q_name;
    const char * q_mod;
    tb_o_Node * q_node;
    long long q_state;
};
struct tb_o_Use {
    const char * q_mod;
    const char * q_used;
};
struct tb_o_Var {
    const char * q_name;
    long long q_type;
    _Bool q_is_const;
    _Bool q_is_link;
    long long q_depth;
    _Bool q_is_shared;
    const char * q_op;
};
struct tb_o_Comp {
    const char * q_file_name;
    tb_list * q_tokens;
    long long q_pos;
    tb_list * q_types;
    tb_list * q_funcs;
    tb_list * q_objs;
    tb_list * q_uses;
    tb_list * q_all_mods;
    const char * q_cur_mod;
    tb_list * q_vars;
    long long q_depth;
    long long q_var_base;
    long long q_current;
    long long q_loop_depth;
    tb_list * q_temp_types;
    long long q_par_base;
    tb_o_Node * q_par_node;
    long long q_temp_base;
    long long q_temp_loops;
    _Bool q_shared_ok;
    _Bool q_par_vec;
    tb_list * q_out;
    long long q_label;
    tb_o_Node * q_cur_func;
    tb_list * q_workers;
    tb_list * q_vec_names;
    tb_list * q_vec_repl;
    long long q_piece_depth;
    tb_list * q_pieces;
    tb_list * q_safe_keys;
    tb_list * q_safe_vals;
    long long q_safe_busy;
    tb_list * q_loops;
    tb_list * q_acc_field;
    tb_list * q_acc_func;
    tb_list * q_acc_loop;
    tb_list * q_call_from;
    tb_list * q_call_to;
    tb_list * q_call_loop;
    tb_list * q_builders;
    const char * q_fmark;
    _Bool q_at_ret_ok;
};
typedef struct { const char * o0; const char * o1; } tb_r_f_main__project_main;
static void tb_show_1(long long x);
static void tb_show_2(double x);
static void tb_show_3(_Bool x);
static void tb_show_4(const char * x);
static void tb_show_5(tb_o_Comp * x);
static void tb_show_6(tb_o_Node * x);
static void tb_show_7(tb_list * x);
static void tb_show_9(tb_list * x);
static void tb_show_10(tb_o_Var * x);
static void tb_show_11(tb_list * x);
static void tb_show_12(tb_o_Token * x);
static void tb_show_13(tb_list * x);
static void tb_show_14(tb_o_Type * x);
static void tb_show_15(tb_list * x);
static void tb_show_16(tb_o_Func * x);
static void tb_show_17(tb_list * x);
static void tb_show_18(tb_o_Use * x);
static void tb_show_19(tb_list * x);
static void tb_show_20(tb_list * x);
static void tb_show_21(tb_list * x);
static void tb_show_22(tb_dict * x);
static _Thread_local const char * tb_t1;   // для  f() or запасное
static tb_o_Token *tb_new_Token(const char * a0, const char * a1, long long a2) {
    tb_o_Token *o = tb_mem(sizeof(tb_o_Token));
    o->q_kind = a0;
    o->q_word = a1;
    o->q_line = a2;
    return o;
}
static tb_o_Node *tb_new_Node(const char * a0, long long a1, const char * a2, const char * a3, tb_o_Node * a4, _Bool a5, long long a6, _Bool a7, _Bool a8, _Bool a9, tb_o_Node * a10, _Bool a11, const char * a12, const char * a13, _Bool a14, _Bool a15, long long a16, tb_list * a17, _Bool a18, _Bool a19, _Bool a20, _Bool a21, _Bool a22, tb_o_Node * a23, tb_list * a24, tb_list * a25, _Bool a26, _Bool a27, tb_list * a28) {
    tb_o_Node *o = tb_mem(sizeof(tb_o_Node));
    o->q_kind = a0;
    o->q_line = a1;
    o->q_name = a2;
    o->q_value = a3;
    o->q_tsyn = a4;
    o->q_has_tsyn = a5;
    o->q_ty = a6;
    o->q_is_const = a7;
    o->q_is_link = a8;
    o->q_creates = a9;
    o->q_ref = a10;
    o->q_has_ref = a11;
    o->q_mod = a12;
    o->q_file = a13;
    o->q_is_pub = a14;
    o->q_can_fail = a15;
    o->q_temp_no = a16;
    o->q_kids = a17;
    o->q_is_par = a18;
    o->q_is_shared = a19;
    o->q_by_cols = a20;
    o->q_by_vec = a21;
    o->q_by_piece = a22;
    o->q_piece_size = a23;
    o->q_pre_bases = a24;
    o->q_pre_idx = a25;
    o->q_hot = a26;
    o->q_is_cold = a27;
    o->q_caps = a28;
    return o;
}
static tb_o_Type *tb_new_Type(const char * a0, long long a1, long long a2, tb_o_Node * a3, const char * a4, tb_o_Node * a5, const char * a6, long long a7) {
    tb_o_Type *o = tb_mem(sizeof(tb_o_Type));
    o->q_kind = a0;
    o->q_key = a1;
    o->q_elem = a2;
    o->q_obj_node = a3;
    o->q_obj_name = a4;
    o->q_func_node = a5;
    o->q_func_key = a6;
    o->q_id = a7;
    return o;
}
static tb_o_Func *tb_new_Func(const char * a0, const char * a1, tb_o_Node * a2, long long a3) {
    tb_o_Func *o = tb_mem(sizeof(tb_o_Func));
    o->q_name = a0;
    o->q_mod = a1;
    o->q_node = a2;
    o->q_state = a3;
    return o;
}
static tb_o_Use *tb_new_Use(const char * a0, const char * a1) {
    tb_o_Use *o = tb_mem(sizeof(tb_o_Use));
    o->q_mod = a0;
    o->q_used = a1;
    return o;
}
static tb_o_Var *tb_new_Var(const char * a0, long long a1, _Bool a2, _Bool a3, long long a4, _Bool a5, const char * a6) {
    tb_o_Var *o = tb_mem(sizeof(tb_o_Var));
    o->q_name = a0;
    o->q_type = a1;
    o->q_is_const = a2;
    o->q_is_link = a3;
    o->q_depth = a4;
    o->q_is_shared = a5;
    o->q_op = a6;
    return o;
}
static tb_o_Comp *tb_new_Comp(const char * a0, tb_list * a1, long long a2, tb_list * a3, tb_list * a4, tb_list * a5, tb_list * a6, tb_list * a7, const char * a8, tb_list * a9, long long a10, long long a11, long long a12, long long a13, tb_list * a14, long long a15, tb_o_Node * a16, long long a17, long long a18, _Bool a19, _Bool a20, tb_list * a21, long long a22, tb_o_Node * a23, tb_list * a24, tb_list * a25, tb_list * a26, long long a27, tb_list * a28, tb_list * a29, tb_list * a30, long long a31, tb_list * a32, tb_list * a33, tb_list * a34, tb_list * a35, tb_list * a36, tb_list * a37, tb_list * a38, tb_list * a39, const char * a40, _Bool a41) {
    tb_o_Comp *o = tb_mem(sizeof(tb_o_Comp));
    o->q_file_name = a0;
    o->q_tokens = a1;
    o->q_pos = a2;
    o->q_types = a3;
    o->q_funcs = a4;
    o->q_objs = a5;
    o->q_uses = a6;
    o->q_all_mods = a7;
    o->q_cur_mod = a8;
    o->q_vars = a9;
    o->q_depth = a10;
    o->q_var_base = a11;
    o->q_current = a12;
    o->q_loop_depth = a13;
    o->q_temp_types = a14;
    o->q_par_base = a15;
    o->q_par_node = a16;
    o->q_temp_base = a17;
    o->q_temp_loops = a18;
    o->q_shared_ok = a19;
    o->q_par_vec = a20;
    o->q_out = a21;
    o->q_label = a22;
    o->q_cur_func = a23;
    o->q_workers = a24;
    o->q_vec_names = a25;
    o->q_vec_repl = a26;
    o->q_piece_depth = a27;
    o->q_pieces = a28;
    o->q_safe_keys = a29;
    o->q_safe_vals = a30;
    o->q_safe_busy = a31;
    o->q_loops = a32;
    o->q_acc_field = a33;
    o->q_acc_func = a34;
    o->q_acc_loop = a35;
    o->q_call_from = a36;
    o->q_call_to = a37;
    o->q_call_loop = a38;
    o->q_builders = a39;
    o->q_fmark = a40;
    o->q_at_ret_ok = a41;
    return o;
}
static const char * f_main__folder_of(const char * v_path);
static const char * f_main__without_tb(const char * v_path);
static const char * f_main__module_of(const char * v_path);
static void f_main__load_module(tb_o_Comp * v_c, tb_o_Node * v_program, const char * v_path, long long v_line_of_use, const char * v_used_from, tb_list * v_loaded);
static tb_o_Node * f_main__load_program(tb_o_Comp * v_c, const char * v_path);
static const char * f_main__quote(const char * v_s);
static _Bool f_main__is_folder(const char * v_path);
static _Bool f_main__is_file(const char * v_path);
static void f_main__say_error(const char * v_message);
static tb_r_f_main__project_main f_main__project_main(const char * v_arg);
static void f_main__new_project(const char * v_name);
static long long f_main__build(tb_o_Comp * v_c, const char * v_path, const char * v_exe);
static void f_main__usage(void);
static void f_main__main(void);
static void f_errors__error_at(tb_o_Comp * v_c, long long v_line, const char * v_message, const char * v_detail);
static _Bool f_lexer__is_command(const char * v_word);
static long long f_lexer__at(const char * v_s, long long v_i);
static _Bool f_lexer__is_letter(long long v_c);
static _Bool f_lexer__is_digit(long long v_c);
static void f_lexer__add_token(tb_o_Comp * v_c, const char * v_kind, const char * v_word, long long v_line);
static _Bool f_lexer__two_char_sign(const char * v_a, const char * v_b);
static _Bool f_lexer__one_char_sign(const char * v_ch);
static void f_lexer__lex(tb_o_Comp * v_c, const char * v_src);
static const char * f_lexer__token_kind_name(const char * v_kind);
static void f_lexer__print_tokens(tb_o_Comp * v_c);
static tb_o_Node * f_parser__new_node(const char * v_kind, long long v_line);
static void f_parser__add_kid(tb_o_Node * v_parent, tb_o_Node * v_kid);
static tb_o_Token * f_parser__now(tb_o_Comp * v_c);
static tb_o_Token * f_parser__ahead(tb_o_Comp * v_c);
static long long f_parser__line_now(tb_o_Comp * v_c);
static void f_parser__next(tb_o_Comp * v_c);
static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s);
static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s);
static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s);
static const char * f_parser__what_is_here(tb_o_Comp * v_c);
static void f_parser__expect_sign(tb_o_Comp * v_c, const char * v_s);
static void f_parser__expect_cmd(tb_o_Comp * v_c, const char * v_s);
static const char * f_parser__expect_name(tb_o_Comp * v_c);
static void f_parser__expect_newline(tb_o_Comp * v_c);
static _Bool f_parser__at_type_word(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_type(tb_o_Comp * v_c);
static tb_o_Node * f_parser__binop(const char * v_sign, tb_o_Node * v_left, tb_o_Node * v_right, long long v_line);
static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line);
static tb_o_Node * f_parser__parse_primary(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_unary(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_mul(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_add(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_compare(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_not(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_and(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_expr(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_if(tb_o_Comp * v_c);
static void f_parser__parse_on_all_cores(tb_o_Comp * v_c, tb_o_Node * v_n);
static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_count(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_repeat(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_value(tb_o_Comp * v_c);
static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value);
static _Bool f_parser__is_change_sign(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_assign(tb_o_Comp * v_c, _Bool v_is_const);
static tb_o_Node * f_parser__parse_linkdecl(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_multi(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_name_statement(tb_o_Comp * v_c);
static _Bool f_parser__later_command(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_statement(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_param(tb_o_Comp * v_c, _Bool v_allow_link);
static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse_obj(tb_o_Comp * v_c);
static tb_o_Node * f_parser__parse(tb_o_Comp * v_c);
static const char * f_parser__node_kind_name(const char * v_kind);
static void f_parser__tree_lines(tb_o_Node * v_n, long long v_depth, tb_list * v_out);
static long long f_check__make_type_key(tb_o_Comp * v_c, const char * v_kind, long long v_key, long long v_elem, tb_o_Node * v_onode, _Bool v_has_obj, tb_o_Node * v_fnode, _Bool v_has_func);
static long long f_check__simple_type(tb_o_Comp * v_c, const char * v_kind);
static long long f_check__list_of(tb_o_Comp * v_c, long long v_elem);
static long long f_check__task_of(tb_o_Comp * v_c, long long v_elem);
static long long f_check__dict_of(tb_o_Comp * v_c, long long v_key, long long v_value);
static long long f_check__obj_type(tb_o_Comp * v_c, tb_o_Node * v_o);
static long long f_check__multi_type(tb_o_Comp * v_c, tb_o_Node * v_fnode);
static const char * f_check__kind_of(tb_o_Comp * v_c, long long v_t);
static const char * f_check__type_name(tb_o_Comp * v_c, long long v_t);
static _Bool f_check__is_number(long long v_t);
static _Bool f_check__is_list(tb_o_Comp * v_c, long long v_t);
static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t);
static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t);
static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t);
static _Bool f_check__is_scalar(long long v_t);
static _Bool f_check__fits(long long v_want, long long v_got);
static _Bool f_check__is_builtin(const char * v_name);
static _Bool f_check__builtin_can_fail(const char * v_name);
static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name);
static _Bool f_check__module_uses(tb_o_Comp * v_c, const char * v_mod, const char * v_other);
static long long f_check__find_func_in(tb_o_Comp * v_c, const char * v_name, const char * v_mod);
static long long f_check__find_func(tb_o_Comp * v_c, const char * v_name);
static void f_check__explain_missing_func(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__find_obj(tb_o_Comp * v_c, const char * v_name);
static long long f_check__find_field(tb_o_Node * v_o, const char * v_name);
static long long f_check__resolve_type(tb_o_Comp * v_c, tb_o_Node * v_t);
static long long f_check__find_var(tb_o_Comp * v_c, const char * v_name);
static void f_check__name_is_free(tb_o_Comp * v_c, const char * v_name, long long v_line);
static void f_check__add_var(tb_o_Comp * v_c, const char * v_name, long long v_type, _Bool v_is_const, _Bool v_is_link, long long v_line);
static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left);
static void f_check__open_scope(tb_o_Comp * v_c);
static void f_check__close_scope(tb_o_Comp * v_c);
static const char * f_check__loop_word(tb_o_Comp * v_c);
static _Bool f_check__outside_par(tb_o_Comp * v_c, long long v_v);
static long long f_check__root_var(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__capture(tb_o_Comp * v_c, long long v_v);
static _Bool f_check__outside_temp(tb_o_Comp * v_c, long long v_v);
static _Bool f_check__is_plain(long long v_t);
static _Bool f_check__temp_can_see(tb_o_Comp * v_c, long long v_t);
static void f_check__temp_keep_out(tb_o_Comp * v_c, long long v_line, const char * v_name);
static void f_check__no_outside_change(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__need_value(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t);
static long long f_check__check_value(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_binop(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_list(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__builtin_arg(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_i);
static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage);
static long long f_check__check_builtin(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__order_args(tb_o_Comp * v_c, tb_o_Node * v_call, tb_o_Node * v_params, _Bool v_allow_missing);
static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link);
static void f_check__need_place(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why);
static long long f_check__check_new_obj(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_o);
static long long f_check__check_call(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_user_call(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_fi);
static long long f_check__check_module_call(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t);
static long long f_check__check_method(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_dict(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_handled(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_how);
static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__note_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_check__declared_before(tb_o_Comp * v_c, const char * v_name, const char * v_loop_var);
static void f_check__need_bool(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__need_int(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_what);
static void f_check__check_assign(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_linkdecl(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_return(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n);
static long long f_check__check_background(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_check__check_together(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name);
static void f_check__check_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t);
static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name);
static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name);
static _Bool f_check__param_keeps_piece(tb_o_Comp * v_c, tb_o_Node * v_f, long long v_i);
static void f_check__mark_hot_fields(tb_o_Comp * v_c);
static void f_check__check_piece_leaks(tb_o_Comp * v_c);
static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body);
static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block);
static void f_check__check_func(tb_o_Comp * v_c, long long v_fi);
static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o);
static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn);
static _Bool f_check__has_fail(tb_o_Node * v_n);
static _Bool f_check__has_value_return(tb_o_Node * v_n);
static _Bool f_check__links_known(tb_o_Node * v_fn);
static void f_check__resolve_params(tb_o_Comp * v_c, tb_o_Node * v_params);
static void f_check__check(tb_o_Comp * v_c, tb_o_Node * v_program);
static void f_emit__put(tb_o_Comp * v_c, const char * v_s);
static const char * f_emit__fname(tb_o_Node * v_f);
static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t);
static void f_emit__indent(tb_o_Comp * v_c, long long v_depth);
static _Bool f_emit__is_bits(tb_o_Comp * v_c, long long v_t);
static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t);
static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t);
static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t);
static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key);
static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want);
static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_print(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_args(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_with_line);
static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_dict(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__concat_parts(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_parts);
static void f_emit__emit_concat(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_bail(tb_o_Comp * v_c);
static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth);
static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t);
static void f_emit__emit_assign(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_if(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_par(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name);
static void f_emit__emit_vec(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_each(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_emit__is_pure(tb_o_Node * v_n);
static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth, const char * v_list_c, const char * v_i_c, const char * v_elem);
static _Bool f_emit__short_literal(tb_o_Node * v_n);
static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth);
static _Bool f_emit__fill_form(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth);
static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f);
static _Bool f_emit__bld_has(tb_list * v_names, const char * v_name);
static _Bool f_emit__bld_uses(tb_o_Node * v_n, const char * v_name);
static void f_emit__bld_bad_all(tb_o_Node * v_n, tb_list * v_bad);
static _Bool f_emit__is_text_plus(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_emit__app_form(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_v);
static tb_list * f_emit__app_parts(tb_o_Comp * v_c, tb_o_Node * v_v);
static void f_emit__bld_scan(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_in_par, tb_list * v_cand, tb_list * v_bad);
static void f_emit__find_builders(tb_o_Comp * v_c, tb_o_Node * v_f);
static _Bool f_emit__is_bld(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_q(tb_o_Comp * v_c, tb_o_Node * v_n);
static void f_emit__emit_app(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_part);
static _Bool f_emit__is_plain_ty(long long v_t);
static void f_emit__at_inner(tb_o_Node * v_n, tb_list * v_inner);
static _Bool f_emit__is_box(long long v_t);
static const char * f_emit__root_name(tb_o_Node * v_n);
static _Bool f_emit__at_fresh_expr(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh);
static _Bool f_emit__at_unfresh(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh);
static void f_emit__drop_name(tb_list * v_names, const char * v_name);
static _Bool f_emit__at_own(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh);
static _Bool f_emit__at_bad(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh, long long v_level);
static _Bool f_emit__at_allocates(tb_o_Comp * v_c, tb_o_Node * v_n);
static _Bool f_emit__auto_temp_ok(tb_o_Comp * v_c, tb_o_Node * v_body);
static _Bool f_emit__func_arena_ok(tb_o_Comp * v_c, tb_o_Node * v_f);
static void f_emit__emit_loop_body(tb_o_Comp * v_c, tb_o_Node * v_body, long long v_depth);
static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f);
static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t);
static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o);
static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o);
static _Bool f_emit__has_cold(tb_o_Node * v_o);
static const char * f_emit__field_path(tb_o_Node * v_o, const char * v_name);
static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o);
static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o);
static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program);
static const char * f_runtime_text__runtime_text(void);

static void tb_show_1(long long x) {
    printf("%lld", x);
}
static void tb_show_2(double x) {
    printf("%g", x);
}
static void tb_show_3(_Bool x) {
    tb_ps(x ? "true" : "false");
}
static void tb_show_4(const char * x) {
    tb_ps(x);
}
static void tb_show_5(tb_o_Comp * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Comp(");
    tb_ps("file_name: ");
    tb_show_4(x->q_file_name);
    tb_ps(", tokens: ");
    tb_show_13(x->q_tokens);
    tb_ps(", pos: ");
    tb_show_1(x->q_pos);
    tb_ps(", types: ");
    tb_show_15(x->q_types);
    tb_ps(", funcs: ");
    tb_show_17(x->q_funcs);
    tb_ps(", objs: ");
    tb_show_9(x->q_objs);
    tb_ps(", uses: ");
    tb_show_19(x->q_uses);
    tb_ps(", all_mods: ");
    tb_show_7(x->q_all_mods);
    tb_ps(", cur_mod: ");
    tb_show_4(x->q_cur_mod);
    tb_ps(", vars: ");
    tb_show_11(x->q_vars);
    tb_ps(", depth: ");
    tb_show_1(x->q_depth);
    tb_ps(", var_base: ");
    tb_show_1(x->q_var_base);
    tb_ps(", current: ");
    tb_show_1(x->q_current);
    tb_ps(", loop_depth: ");
    tb_show_1(x->q_loop_depth);
    tb_ps(", temp_types: ");
    tb_show_20(x->q_temp_types);
    tb_ps(", par_base: ");
    tb_show_1(x->q_par_base);
    tb_ps(", par_node: ");
    tb_show_6(x->q_par_node);
    tb_ps(", temp_base: ");
    tb_show_1(x->q_temp_base);
    tb_ps(", temp_loops: ");
    tb_show_1(x->q_temp_loops);
    tb_ps(", shared_ok: ");
    tb_show_3(x->q_shared_ok);
    tb_ps(", par_vec: ");
    tb_show_3(x->q_par_vec);
    tb_ps(", out: ");
    tb_show_7(x->q_out);
    tb_ps(", label: ");
    tb_show_1(x->q_label);
    tb_ps(", cur_func: ");
    tb_show_6(x->q_cur_func);
    tb_ps(", workers: ");
    tb_show_7(x->q_workers);
    tb_ps(", vec_names: ");
    tb_show_7(x->q_vec_names);
    tb_ps(", vec_repl: ");
    tb_show_7(x->q_vec_repl);
    tb_ps(", piece_depth: ");
    tb_show_1(x->q_piece_depth);
    tb_ps(", pieces: ");
    tb_show_9(x->q_pieces);
    tb_ps(", safe_keys: ");
    tb_show_7(x->q_safe_keys);
    tb_ps(", safe_vals: ");
    tb_show_20(x->q_safe_vals);
    tb_ps(", safe_busy: ");
    tb_show_1(x->q_safe_busy);
    tb_ps(", loops: ");
    tb_show_9(x->q_loops);
    tb_ps(", acc_field: ");
    tb_show_9(x->q_acc_field);
    tb_ps(", acc_func: ");
    tb_show_20(x->q_acc_func);
    tb_ps(", acc_loop: ");
    tb_show_21(x->q_acc_loop);
    tb_ps(", call_from: ");
    tb_show_20(x->q_call_from);
    tb_ps(", call_to: ");
    tb_show_20(x->q_call_to);
    tb_ps(", call_loop: ");
    tb_show_21(x->q_call_loop);
    tb_ps(", builders: ");
    tb_show_7(x->q_builders);
    tb_ps(", fmark: ");
    tb_show_4(x->q_fmark);
    tb_ps(", at_ret_ok: ");
    tb_show_3(x->q_at_ret_ok);
    tb_ps(")");
}
static void tb_show_6(tb_o_Node * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Node(");
    tb_ps("kind: ");
    tb_show_4(x->q_kind);
    tb_ps(", line: ");
    tb_show_1(x->q_line);
    tb_ps(", name: ");
    tb_show_4(x->q_name);
    tb_ps(", value: ");
    tb_show_4(x->q_value);
    tb_ps(", tsyn: ");
    tb_show_6(x->q_tsyn);
    tb_ps(", has_tsyn: ");
    tb_show_3(x->q_has_tsyn);
    tb_ps(", ty: ");
    tb_show_1(x->q_ty);
    tb_ps(", is_const: ");
    tb_show_3(x->q_is_const);
    tb_ps(", is_link: ");
    tb_show_3(x->q_is_link);
    tb_ps(", creates: ");
    tb_show_3(x->q_creates);
    tb_ps(", ref: ");
    tb_show_6(x->q_ref);
    tb_ps(", has_ref: ");
    tb_show_3(x->q_has_ref);
    tb_ps(", mod: ");
    tb_show_4(x->q_mod);
    tb_ps(", file: ");
    tb_show_4(x->q_file);
    tb_ps(", is_pub: ");
    tb_show_3(x->q_is_pub);
    tb_ps(", can_fail: ");
    tb_show_3(x->q_can_fail);
    tb_ps(", temp_no: ");
    tb_show_1(x->q_temp_no);
    tb_ps(", kids: ");
    tb_show_9(x->q_kids);
    tb_ps(", is_par: ");
    tb_show_3(x->q_is_par);
    tb_ps(", is_shared: ");
    tb_show_3(x->q_is_shared);
    tb_ps(", by_cols: ");
    tb_show_3(x->q_by_cols);
    tb_ps(", by_vec: ");
    tb_show_3(x->q_by_vec);
    tb_ps(", by_piece: ");
    tb_show_3(x->q_by_piece);
    tb_ps(", piece_size: ");
    tb_show_6(x->q_piece_size);
    tb_ps(", pre_bases: ");
    tb_show_9(x->q_pre_bases);
    tb_ps(", pre_idx: ");
    tb_show_9(x->q_pre_idx);
    tb_ps(", hot: ");
    tb_show_3(x->q_hot);
    tb_ps(", is_cold: ");
    tb_show_3(x->q_is_cold);
    tb_ps(", caps: ");
    tb_show_11(x->q_caps);
    tb_ps(")");
}
static void tb_show_7(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_4(((const char * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_9(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_6(((tb_o_Node * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_10(tb_o_Var * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Var(");
    tb_ps("name: ");
    tb_show_4(x->q_name);
    tb_ps(", type: ");
    tb_show_1(x->q_type);
    tb_ps(", is_const: ");
    tb_show_3(x->q_is_const);
    tb_ps(", is_link: ");
    tb_show_3(x->q_is_link);
    tb_ps(", depth: ");
    tb_show_1(x->q_depth);
    tb_ps(", is_shared: ");
    tb_show_3(x->q_is_shared);
    tb_ps(", op: ");
    tb_show_4(x->q_op);
    tb_ps(")");
}
static void tb_show_11(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_10(((tb_o_Var * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_12(tb_o_Token * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Token(");
    tb_ps("kind: ");
    tb_show_4(x->q_kind);
    tb_ps(", word: ");
    tb_show_4(x->q_word);
    tb_ps(", line: ");
    tb_show_1(x->q_line);
    tb_ps(")");
}
static void tb_show_13(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_12(((tb_o_Token * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_14(tb_o_Type * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Type(");
    tb_ps("kind: ");
    tb_show_4(x->q_kind);
    tb_ps(", key: ");
    tb_show_1(x->q_key);
    tb_ps(", elem: ");
    tb_show_1(x->q_elem);
    tb_ps(", obj_node: ");
    tb_show_6(x->q_obj_node);
    tb_ps(", obj_name: ");
    tb_show_4(x->q_obj_name);
    tb_ps(", func_node: ");
    tb_show_6(x->q_func_node);
    tb_ps(", func_key: ");
    tb_show_4(x->q_func_key);
    tb_ps(", id: ");
    tb_show_1(x->q_id);
    tb_ps(")");
}
static void tb_show_15(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_14(((tb_o_Type * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_16(tb_o_Func * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Func(");
    tb_ps("name: ");
    tb_show_4(x->q_name);
    tb_ps(", mod: ");
    tb_show_4(x->q_mod);
    tb_ps(", node: ");
    tb_show_6(x->q_node);
    tb_ps(", state: ");
    tb_show_1(x->q_state);
    tb_ps(")");
}
static void tb_show_17(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_16(((tb_o_Func * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_18(tb_o_Use * x) {
    if (x == 0) { tb_ps("пусто"); return; }
    tb_ps("Use(");
    tb_ps("mod: ");
    tb_show_4(x->q_mod);
    tb_ps(", used: ");
    tb_show_4(x->q_used);
    tb_ps(")");
}
static void tb_show_19(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_18(((tb_o_Use * *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_20(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_1(((long long *)x->data)[i]);
    }
    tb_ps("]");
}
static void tb_show_21(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_3(TB_BIT(x, i));
    }
    tb_ps("]");
}
static void tb_show_22(tb_dict * x) {
    tb_ps("{");
    for (long long i = 0; i < x->keys->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_4(((const char * *)x->keys->data)[i]);
        tb_ps(": ");
        tb_show_4(((const char * *)x->vals->data)[i]);
    }
    tb_ps("}");
}

static const char * f_main__folder_of(const char * v_path) {
    long long v_cut_at = 0LL;
    {
        long long tb_end1 = (tb_tlen(v_path) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1; v_i++) {
            if (({ const char *tb_s2 = tb_char_at(v_path, v_i, -1, 14); ((unsigned char)tb_s2[0] == 47 && tb_s2[1] == 0); })) {
                v_cut_at = (v_i + 1LL);
            }
        }
    }
    if ((v_cut_at == 0LL)) {
        return "";
    }
    return tb_cut(v_path, 0LL, (v_cut_at - 1LL), 18);
    tb_crash(11, "функция folder_of дошла до конца без return");
    return "";
}

static const char * f_main__without_tb(const char * v_path) {
    if (((tb_tlen(v_path) > 3LL) && tb_ends(v_path, ".tb"))) {
        return tb_cut(v_path, 0LL, (tb_tlen(v_path) - 4LL), 22);
    }
    return v_path;
    tb_crash(20, "функция without_tb дошла до конца без return");
    return "";
}

static const char * f_main__module_of(const char * v_path) {
    const char * v_folder = f_main__folder_of(v_path);
    return f_main__without_tb(tb_cut(v_path, tb_tlen(v_folder), (tb_tlen(v_path) - 1LL), 27));
    tb_crash(25, "функция module_of дошла до конца без return");
    return "";
}

static void f_main__load_module(tb_o_Comp * v_c, tb_o_Node * v_program, const char * v_path, long long v_line_of_use, const char * v_used_from, tb_list * v_loaded) {
    const char * v_name = f_main__module_of(v_path);
    {
        tb_list * tb_l3 = v_loaded;
        for (long long tb_i3 = 0; tb_i3 < tb_l3->len; tb_i3++) {
            const char * const v_m = ((const char * *)tb_l3->data)[tb_i3];
            if ((strcmp(v_m, v_name) == 0)) {
                return;
            }
        }
    }
    if (((v_loaded)->len == 200LL)) {
        f_errors__error_at(v_c, 0LL, "слишком много модулей", "");
    }
    { const char * tb_v4 = v_name; *(const char * *)tb_grow(v_loaded) = tb_v4; }
    if ((!tb_file_exists(v_path))) {
        { const char * tb_v5 = v_used_from; const char * *tb_p5 = &(((tb_o_Comp *)tb_nn(v_c, 38))->q_file_name); *tb_p5 = tb_v5; }
        f_errors__error_at(v_c, v_line_of_use, "нет файла для use", v_path);
    }
    const char * tb_v6 = tb_read_file(v_path);
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(({ const char *tb_c7_0 = "не могу открыть файл: "; long long tb_c7_0n = sizeof("не могу открыть файл: ") - 1; const char *tb_c7_1 = v_path; long long tb_c7_1n = (long long)strlen(tb_c7_1); char *tb_c7r = tb_mem(tb_c7_0n + tb_c7_1n + 1), *tb_c7rw = tb_c7r; memcpy(tb_c7rw, tb_c7_0, tb_c7_0n); tb_c7rw += tb_c7_0n; memcpy(tb_c7rw, tb_c7_1, tb_c7_1n); tb_c7rw += tb_c7_1n; *tb_c7rw = 0; (const char *)tb_c7r; })), tb_ps("\n"));
        tb_exit(1LL);
    }
    const char * v_source = tb_v6;
    { const char * tb_v8 = v_path; const char * *tb_p8 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_file_name); *tb_p8 = tb_v8; }
    f_lexer__lex(v_c, v_source);
    tb_o_Node * v_part = f_parser__parse(v_c);
    {
        tb_list * tb_l9 = ((tb_o_Node *)tb_nn(v_part, 46))->q_kids;
        for (long long tb_i9 = 0; tb_i9 < tb_l9->len; tb_i9++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l9->data)[tb_i9];
            { const char * tb_v10 = v_name; const char * *tb_p10 = &(((tb_o_Node *)tb_nn(v_k, 47))->q_mod); *tb_p10 = tb_v10; }
            { const char * tb_v11 = v_path; const char * *tb_p11 = &(((tb_o_Node *)tb_nn(v_k, 48))->q_file); *tb_p11 = tb_v11; }
            f_parser__add_kid(v_program, v_k);
        }
    }
    {
        tb_list * tb_l12 = ((tb_o_Node *)tb_nn(v_part, 51))->q_kids;
        for (long long tb_i12 = 0; tb_i12 < tb_l12->len; tb_i12++) {
            tb_o_Node * const v_u = ((tb_o_Node * *)tb_l12->data)[tb_i12];
            if (({ const char *tb_s13 = ((tb_o_Node *)tb_nn(v_u, 52))->q_kind; ((unsigned char)tb_s13[0] == 117 && (unsigned char)tb_s13[1] == 115 && (unsigned char)tb_s13[2] == 101 && tb_s13[3] == 0); })) {
                const char * v_next_path = ({ const char *tb_c14_0 = f_main__folder_of(v_path); long long tb_c14_0n = (long long)strlen(tb_c14_0); const char *tb_c14_1 = ((tb_o_Node *)tb_nn(v_u, 53))->q_name; long long tb_c14_1n = (long long)strlen(tb_c14_1); const char *tb_c14_2 = ".tb"; long long tb_c14_2n = sizeof(".tb") - 1; char *tb_c14r = tb_mem(tb_c14_0n + tb_c14_1n + tb_c14_2n + 1), *tb_c14rw = tb_c14r; memcpy(tb_c14rw, tb_c14_0, tb_c14_0n); tb_c14rw += tb_c14_0n; memcpy(tb_c14rw, tb_c14_1, tb_c14_1n); tb_c14rw += tb_c14_1n; memcpy(tb_c14rw, tb_c14_2, tb_c14_2n); tb_c14rw += tb_c14_2n; *tb_c14rw = 0; (const char *)tb_c14r; });
                f_main__load_module(v_c, v_program, v_next_path, ((tb_o_Node *)tb_nn(v_u, 54))->q_line, v_path, v_loaded);
            }
        }
    }
}

static tb_o_Node * f_main__load_program(tb_o_Comp * v_c, const char * v_path) {
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    tb_list * v_loaded = tb_list_new(sizeof(const char *));
    f_main__load_module(v_c, v_program, v_path, 0LL, v_path, v_loaded);
    { const char * tb_v15 = v_path; const char * *tb_p15 = &(((tb_o_Comp *)tb_nn(v_c, 60))->q_file_name); *tb_p15 = tb_v15; }
    return v_program;
    tb_crash(56, "функция load_program дошла до конца без return");
    return 0;
}

static const char * f_main__quote(const char * v_s) {
    return ({ const char *tb_c16_0 = "'"; long long tb_c16_0n = sizeof("'") - 1; const char *tb_c16_1 = tb_replace(v_s, "'", "'\\''", 65); long long tb_c16_1n = (long long)strlen(tb_c16_1); const char *tb_c16_2 = "'"; long long tb_c16_2n = sizeof("'") - 1; char *tb_c16r = tb_mem(tb_c16_0n + tb_c16_1n + tb_c16_2n + 1), *tb_c16rw = tb_c16r; memcpy(tb_c16rw, tb_c16_0, tb_c16_0n); tb_c16rw += tb_c16_0n; memcpy(tb_c16rw, tb_c16_1, tb_c16_1n); tb_c16rw += tb_c16_1n; memcpy(tb_c16rw, tb_c16_2, tb_c16_2n); tb_c16rw += tb_c16_2n; *tb_c16rw = 0; (const char *)tb_c16r; });
    tb_crash(64, "функция quote дошла до конца без return");
    return "";
}

static _Bool f_main__is_folder(const char * v_path) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    { _Bool tb_rv = (tb_run(({ const char *tb_c17_0 = "test -d "; long long tb_c17_0n = sizeof("test -d ") - 1; const char *tb_c17_1 = f_main__quote(v_path); long long tb_c17_1n = (long long)strlen(tb_c17_1); char *tb_c17r = tb_mem(tb_c17_0n + tb_c17_1n + 1), *tb_c17rw = tb_c17r; memcpy(tb_c17rw, tb_c17_0, tb_c17_0n); tb_c17rw += tb_c17_0n; memcpy(tb_c17rw, tb_c17_1, tb_c17_1n); tb_c17rw += tb_c17_1n; *tb_c17rw = 0; (const char *)tb_c17r; })) == 0LL); if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
    if (tb_fa) tb_temp_end(tb_fm);
    tb_crash(67, "функция is_folder дошла до конца без return");
    return 0;
}

static _Bool f_main__is_file(const char * v_path) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    { _Bool tb_rv = (tb_run(({ const char *tb_c18_0 = "test -f "; long long tb_c18_0n = sizeof("test -f ") - 1; const char *tb_c18_1 = f_main__quote(v_path); long long tb_c18_1n = (long long)strlen(tb_c18_1); char *tb_c18r = tb_mem(tb_c18_0n + tb_c18_1n + 1), *tb_c18rw = tb_c18r; memcpy(tb_c18rw, tb_c18_0, tb_c18_0n); tb_c18rw += tb_c18_0n; memcpy(tb_c18rw, tb_c18_1, tb_c18_1n); tb_c18rw += tb_c18_1n; *tb_c18rw = 0; (const char *)tb_c18r; })) == 0LL); if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
    if (tb_fa) tb_temp_end(tb_fm);
    tb_crash(70, "функция is_file дошла до конца без return");
    return 0;
}

static void f_main__say_error(const char * v_message) {
    (tb_show_4(v_message), tb_ps("\n"));
    tb_exit(1LL);
}

static tb_r_f_main__project_main f_main__project_main(const char * v_arg) {
    const char * v_main_tb = "";
    const char * v_exe = "";
    const char * v_name = v_arg;
    if (({ const char *tb_s19 = v_name; (tb_s19[0] == 0); })) {
        v_name = ".";
    }
    while (((tb_tlen(v_name) > 1LL) && tb_ends(v_name, "/"))) {
        v_name = tb_cut(v_name, 0LL, (tb_tlen(v_name) - 2LL), 86);
    }
    if (f_main__is_folder(v_name)) {
        v_main_tb = ({ const char *tb_c20_0 = v_name; long long tb_c20_0n = (long long)strlen(tb_c20_0); const char *tb_c20_1 = "/main.tb"; long long tb_c20_1n = sizeof("/main.tb") - 1; char *tb_c20r = tb_mem(tb_c20_0n + tb_c20_1n + 1), *tb_c20rw = tb_c20r; memcpy(tb_c20rw, tb_c20_0, tb_c20_0n); tb_c20rw += tb_c20_0n; memcpy(tb_c20rw, tb_c20_1, tb_c20_1n); tb_c20rw += tb_c20_1n; *tb_c20rw = 0; (const char *)tb_c20r; });
        if ((!f_main__is_file(v_main_tb))) {
            f_main__say_error(({ const char *tb_c21_0 = "в папке "; long long tb_c21_0n = sizeof("в папке ") - 1; const char *tb_c21_1 = v_name; long long tb_c21_1n = (long long)strlen(tb_c21_1); const char *tb_c21_2 = " нет main.tb — с него начинается проект"; long long tb_c21_2n = sizeof(" нет main.tb — с него начинается проект") - 1; char *tb_c21r = tb_mem(tb_c21_0n + tb_c21_1n + tb_c21_2n + 1), *tb_c21rw = tb_c21r; memcpy(tb_c21rw, tb_c21_0, tb_c21_0n); tb_c21rw += tb_c21_0n; memcpy(tb_c21rw, tb_c21_1, tb_c21_1n); tb_c21rw += tb_c21_1n; memcpy(tb_c21rw, tb_c21_2, tb_c21_2n); tb_c21rw += tb_c21_2n; *tb_c21rw = 0; (const char *)tb_c21r; }));
        }
        const char * v_base = tb_cut(v_name, tb_tlen(f_main__folder_of(v_name)), (tb_tlen(v_name) - 1LL), 91);
        const char * v_build_dir = ({ const char *tb_c22_0 = v_name; long long tb_c22_0n = (long long)strlen(tb_c22_0); const char *tb_c22_1 = "/build"; long long tb_c22_1n = sizeof("/build") - 1; char *tb_c22r = tb_mem(tb_c22_0n + tb_c22_1n + 1), *tb_c22rw = tb_c22r; memcpy(tb_c22rw, tb_c22_0, tb_c22_0n); tb_c22rw += tb_c22_0n; memcpy(tb_c22rw, tb_c22_1, tb_c22_1n); tb_c22rw += tb_c22_1n; *tb_c22rw = 0; (const char *)tb_c22r; });
        long long v_code = tb_run(({ const char *tb_c23_0 = "mkdir -p "; long long tb_c23_0n = sizeof("mkdir -p ") - 1; const char *tb_c23_1 = f_main__quote(v_build_dir); long long tb_c23_1n = (long long)strlen(tb_c23_1); char *tb_c23r = tb_mem(tb_c23_0n + tb_c23_1n + 1), *tb_c23rw = tb_c23r; memcpy(tb_c23rw, tb_c23_0, tb_c23_0n); tb_c23rw += tb_c23_0n; memcpy(tb_c23rw, tb_c23_1, tb_c23_1n); tb_c23rw += tb_c23_1n; *tb_c23rw = 0; (const char *)tb_c23r; }));
        if (({ const char *tb_s24 = v_name; ((unsigned char)tb_s24[0] == 46 && tb_s24[1] == 0); })) {
            v_code = tb_run(({ const char *tb_c25_0 = "basename \"$PWD\" > "; long long tb_c25_0n = sizeof("basename \"$PWD\" > ") - 1; const char *tb_c25_1 = f_main__quote(({ const char *tb_c26_0 = v_build_dir; long long tb_c26_0n = (long long)strlen(tb_c26_0); const char *tb_c26_1 = "/.name"; long long tb_c26_1n = sizeof("/.name") - 1; char *tb_c26r = tb_mem(tb_c26_0n + tb_c26_1n + 1), *tb_c26rw = tb_c26r; memcpy(tb_c26rw, tb_c26_0, tb_c26_0n); tb_c26rw += tb_c26_0n; memcpy(tb_c26rw, tb_c26_1, tb_c26_1n); tb_c26rw += tb_c26_1n; *tb_c26rw = 0; (const char *)tb_c26r; })); long long tb_c25_1n = (long long)strlen(tb_c25_1); char *tb_c25r = tb_mem(tb_c25_0n + tb_c25_1n + 1), *tb_c25rw = tb_c25r; memcpy(tb_c25rw, tb_c25_0, tb_c25_0n); tb_c25rw += tb_c25_0n; memcpy(tb_c25rw, tb_c25_1, tb_c25_1n); tb_c25rw += tb_c25_1n; *tb_c25rw = 0; (const char *)tb_c25r; }));
            v_base = tb_trim((tb_t1 = tb_read_file(({ const char *tb_c27_0 = v_build_dir; long long tb_c27_0n = (long long)strlen(tb_c27_0); const char *tb_c27_1 = "/.name"; long long tb_c27_1n = sizeof("/.name") - 1; char *tb_c27r = tb_mem(tb_c27_0n + tb_c27_1n + 1), *tb_c27rw = tb_c27r; memcpy(tb_c27rw, tb_c27_0, tb_c27_0n); tb_c27rw += tb_c27_0n; memcpy(tb_c27rw, tb_c27_1, tb_c27_1n); tb_c27rw += tb_c27_1n; *tb_c27rw = 0; (const char *)tb_c27r; })), tb_err ? (tb_err = 0, "") : tb_t1));
        }
        if (((({ const char *tb_s28 = v_base; (tb_s28[0] == 0); }) || ({ const char *tb_s29 = v_base; ((unsigned char)tb_s29[0] == 46 && tb_s29[1] == 0); })) || ({ const char *tb_s30 = v_base; ((unsigned char)tb_s30[0] == 46 && (unsigned char)tb_s30[1] == 46 && tb_s30[2] == 0); }))) {
            v_base = "main";
        }
        v_exe = ({ const char *tb_c31_0 = v_build_dir; long long tb_c31_0n = (long long)strlen(tb_c31_0); const char *tb_c31_1 = "/"; long long tb_c31_1n = sizeof("/") - 1; const char *tb_c31_2 = v_base; long long tb_c31_2n = (long long)strlen(tb_c31_2); char *tb_c31r = tb_mem(tb_c31_0n + tb_c31_1n + tb_c31_2n + 1), *tb_c31rw = tb_c31r; memcpy(tb_c31rw, tb_c31_0, tb_c31_0n); tb_c31rw += tb_c31_0n; memcpy(tb_c31rw, tb_c31_1, tb_c31_1n); tb_c31rw += tb_c31_1n; memcpy(tb_c31rw, tb_c31_2, tb_c31_2n); tb_c31rw += tb_c31_2n; *tb_c31rw = 0; (const char *)tb_c31r; });
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    if (f_main__is_file(v_name)) {
        v_main_tb = v_name;
        v_exe = f_main__without_tb(v_name);
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    if (f_main__is_file(({ const char *tb_c32_0 = v_name; long long tb_c32_0n = (long long)strlen(tb_c32_0); const char *tb_c32_1 = ".tb"; long long tb_c32_1n = sizeof(".tb") - 1; char *tb_c32r = tb_mem(tb_c32_0n + tb_c32_1n + 1), *tb_c32rw = tb_c32r; memcpy(tb_c32rw, tb_c32_0, tb_c32_0n); tb_c32rw += tb_c32_0n; memcpy(tb_c32rw, tb_c32_1, tb_c32_1n); tb_c32rw += tb_c32_1n; *tb_c32rw = 0; (const char *)tb_c32r; }))) {
        v_main_tb = ({ const char *tb_c33_0 = v_name; long long tb_c33_0n = (long long)strlen(tb_c33_0); const char *tb_c33_1 = ".tb"; long long tb_c33_1n = sizeof(".tb") - 1; char *tb_c33r = tb_mem(tb_c33_0n + tb_c33_1n + 1), *tb_c33rw = tb_c33r; memcpy(tb_c33rw, tb_c33_0, tb_c33_0n); tb_c33rw += tb_c33_0n; memcpy(tb_c33rw, tb_c33_1, tb_c33_1n); tb_c33rw += tb_c33_1n; *tb_c33rw = 0; (const char *)tb_c33r; });
        v_exe = v_name;
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    (tb_show_4(({ const char *tb_c34_0 = "нет проекта или файла: "; long long tb_c34_0n = sizeof("нет проекта или файла: ") - 1; const char *tb_c34_1 = v_name; long long tb_c34_1n = (long long)strlen(tb_c34_1); char *tb_c34r = tb_mem(tb_c34_0n + tb_c34_1n + 1), *tb_c34rw = tb_c34r; memcpy(tb_c34rw, tb_c34_0, tb_c34_0n); tb_c34rw += tb_c34_0n; memcpy(tb_c34rw, tb_c34_1, tb_c34_1n); tb_c34rw += tb_c34_1n; *tb_c34rw = 0; (const char *)tb_c34r; })), tb_ps("\n"));
    f_main__say_error(({ const char *tb_c35_0 = "новый проект делается так:  tb new "; long long tb_c35_0n = sizeof("новый проект делается так:  tb new ") - 1; const char *tb_c35_1 = v_name; long long tb_c35_1n = (long long)strlen(tb_c35_1); char *tb_c35r = tb_mem(tb_c35_0n + tb_c35_1n + 1), *tb_c35rw = tb_c35r; memcpy(tb_c35rw, tb_c35_0, tb_c35_0n); tb_c35rw += tb_c35_0n; memcpy(tb_c35rw, tb_c35_1, tb_c35_1n); tb_c35rw += tb_c35_1n; *tb_c35rw = 0; (const char *)tb_c35r; }));
    return (tb_r_f_main__project_main){v_main_tb, v_exe};
}

static void f_main__new_project(const char * v_name) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    if (({ const char *tb_s36 = v_name; (tb_s36[0] == 0); })) {
        f_main__say_error("напиши имя проекта:  tb new имя");
    }
    if ((tb_run(({ const char *tb_c37_0 = "test -e "; long long tb_c37_0n = sizeof("test -e ") - 1; const char *tb_c37_1 = f_main__quote(v_name); long long tb_c37_1n = (long long)strlen(tb_c37_1); char *tb_c37r = tb_mem(tb_c37_0n + tb_c37_1n + 1), *tb_c37rw = tb_c37r; memcpy(tb_c37rw, tb_c37_0, tb_c37_0n); tb_c37rw += tb_c37_0n; memcpy(tb_c37rw, tb_c37_1, tb_c37_1n); tb_c37rw += tb_c37_1n; *tb_c37rw = 0; (const char *)tb_c37r; })) == 0LL)) {
        f_main__say_error(({ const char *tb_c38_0 = "уже есть папка или файл с именем "; long long tb_c38_0n = sizeof("уже есть папка или файл с именем ") - 1; const char *tb_c38_1 = v_name; long long tb_c38_1n = (long long)strlen(tb_c38_1); const char *tb_c38_2 = " — возьми другое имя"; long long tb_c38_2n = sizeof(" — возьми другое имя") - 1; char *tb_c38r = tb_mem(tb_c38_0n + tb_c38_1n + tb_c38_2n + 1), *tb_c38rw = tb_c38r; memcpy(tb_c38rw, tb_c38_0, tb_c38_0n); tb_c38rw += tb_c38_0n; memcpy(tb_c38rw, tb_c38_1, tb_c38_1n); tb_c38rw += tb_c38_1n; memcpy(tb_c38rw, tb_c38_2, tb_c38_2n); tb_c38rw += tb_c38_2n; *tb_c38rw = 0; (const char *)tb_c38r; }));
    }
    if ((tb_run(({ const char *tb_c39_0 = "mkdir "; long long tb_c39_0n = sizeof("mkdir ") - 1; const char *tb_c39_1 = f_main__quote(v_name); long long tb_c39_1n = (long long)strlen(tb_c39_1); char *tb_c39r = tb_mem(tb_c39_0n + tb_c39_1n + 1), *tb_c39rw = tb_c39r; memcpy(tb_c39rw, tb_c39_0, tb_c39_0n); tb_c39rw += tb_c39_0n; memcpy(tb_c39rw, tb_c39_1, tb_c39_1n); tb_c39rw += tb_c39_1n; *tb_c39rw = 0; (const char *)tb_c39r; })) != 0LL)) {
        f_main__say_error(({ const char *tb_c40_0 = "не могу сделать папку: "; long long tb_c40_0n = sizeof("не могу сделать папку: ") - 1; const char *tb_c40_1 = v_name; long long tb_c40_1n = (long long)strlen(tb_c40_1); char *tb_c40r = tb_mem(tb_c40_0n + tb_c40_1n + 1), *tb_c40rw = tb_c40r; memcpy(tb_c40rw, tb_c40_0, tb_c40_0n); tb_c40rw += tb_c40_0n; memcpy(tb_c40rw, tb_c40_1, tb_c40_1n); tb_c40rw += tb_c40_1n; *tb_c40rw = 0; (const char *)tb_c40r; }));
    }
    tb_write_file(({ const char *tb_c42_0 = v_name; long long tb_c42_0n = (long long)strlen(tb_c42_0); const char *tb_c42_1 = "/main.tb"; long long tb_c42_1n = sizeof("/main.tb") - 1; char *tb_c42r = tb_mem(tb_c42_0n + tb_c42_1n + 1), *tb_c42rw = tb_c42r; memcpy(tb_c42rw, tb_c42_0, tb_c42_0n); tb_c42rw += tb_c42_0n; memcpy(tb_c42rw, tb_c42_1, tb_c42_1n); tb_c42rw += tb_c42_1n; *tb_c42rw = 0; (const char *)tb_c42r; }), ({ const char *tb_c43_0 = "func main()\n    print(\"Привет из проекта "; long long tb_c43_0n = sizeof("func main()\n    print(\"Привет из проекта ") - 1; const char *tb_c43_1 = v_name; long long tb_c43_1n = (long long)strlen(tb_c43_1); const char *tb_c43_2 = "!\")\n"; long long tb_c43_2n = sizeof("!\")\n") - 1; char *tb_c43r = tb_mem(tb_c43_0n + tb_c43_1n + tb_c43_2n + 1), *tb_c43rw = tb_c43r; memcpy(tb_c43rw, tb_c43_0, tb_c43_0n); tb_c43rw += tb_c43_0n; memcpy(tb_c43rw, tb_c43_1, tb_c43_1n); tb_c43rw += tb_c43_1n; memcpy(tb_c43rw, tb_c43_2, tb_c43_2n); tb_c43rw += tb_c43_2n; *tb_c43rw = 0; (const char *)tb_c43r; }));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        f_main__say_error(({ const char *tb_c44_0 = "не могу записать файл: "; long long tb_c44_0n = sizeof("не могу записать файл: ") - 1; const char *tb_c44_1 = v_name; long long tb_c44_1n = (long long)strlen(tb_c44_1); const char *tb_c44_2 = "/main.tb"; long long tb_c44_2n = sizeof("/main.tb") - 1; char *tb_c44r = tb_mem(tb_c44_0n + tb_c44_1n + tb_c44_2n + 1), *tb_c44rw = tb_c44r; memcpy(tb_c44rw, tb_c44_0, tb_c44_0n); tb_c44rw += tb_c44_0n; memcpy(tb_c44rw, tb_c44_1, tb_c44_1n); tb_c44rw += tb_c44_1n; memcpy(tb_c44rw, tb_c44_2, tb_c44_2n); tb_c44rw += tb_c44_2n; *tb_c44rw = 0; (const char *)tb_c44r; }));
    }
    tb_write_file(({ const char *tb_c46_0 = v_name; long long tb_c46_0n = (long long)strlen(tb_c46_0); const char *tb_c46_1 = "/.gitignore"; long long tb_c46_1n = sizeof("/.gitignore") - 1; char *tb_c46r = tb_mem(tb_c46_0n + tb_c46_1n + 1), *tb_c46rw = tb_c46r; memcpy(tb_c46rw, tb_c46_0, tb_c46_0n); tb_c46rw += tb_c46_0n; memcpy(tb_c46rw, tb_c46_1, tb_c46_1n); tb_c46rw += tb_c46_1n; *tb_c46rw = 0; (const char *)tb_c46r; }), "build/\n");
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        const char * v_ignored = v_err;
    }
    (tb_show_4(({ const char *tb_c47_0 = "готов проект "; long long tb_c47_0n = sizeof("готов проект ") - 1; const char *tb_c47_1 = v_name; long long tb_c47_1n = (long long)strlen(tb_c47_1); const char *tb_c47_2 = ":"; long long tb_c47_2n = sizeof(":") - 1; char *tb_c47r = tb_mem(tb_c47_0n + tb_c47_1n + tb_c47_2n + 1), *tb_c47rw = tb_c47r; memcpy(tb_c47rw, tb_c47_0, tb_c47_0n); tb_c47rw += tb_c47_0n; memcpy(tb_c47rw, tb_c47_1, tb_c47_1n); tb_c47rw += tb_c47_1n; memcpy(tb_c47rw, tb_c47_2, tb_c47_2n); tb_c47rw += tb_c47_2n; *tb_c47rw = 0; (const char *)tb_c47r; })), tb_ps("\n"));
    (tb_show_4(({ const char *tb_c48_0 = "  "; long long tb_c48_0n = sizeof("  ") - 1; const char *tb_c48_1 = v_name; long long tb_c48_1n = (long long)strlen(tb_c48_1); const char *tb_c48_2 = "/main.tb   — здесь пишешь программу"; long long tb_c48_2n = sizeof("/main.tb   — здесь пишешь программу") - 1; char *tb_c48r = tb_mem(tb_c48_0n + tb_c48_1n + tb_c48_2n + 1), *tb_c48rw = tb_c48r; memcpy(tb_c48rw, tb_c48_0, tb_c48_0n); tb_c48rw += tb_c48_0n; memcpy(tb_c48rw, tb_c48_1, tb_c48_1n); tb_c48rw += tb_c48_1n; memcpy(tb_c48rw, tb_c48_2, tb_c48_2n); tb_c48rw += tb_c48_2n; *tb_c48rw = 0; (const char *)tb_c48r; })), tb_ps("\n"));
    (tb_show_4(({ const char *tb_c49_0 = "  "; long long tb_c49_0n = sizeof("  ") - 1; const char *tb_c49_1 = v_name; long long tb_c49_1n = (long long)strlen(tb_c49_1); const char *tb_c49_2 = "/build/    — сюда кладётся готовая программа\n"; long long tb_c49_2n = sizeof("/build/    — сюда кладётся готовая программа\n") - 1; char *tb_c49r = tb_mem(tb_c49_0n + tb_c49_1n + tb_c49_2n + 1), *tb_c49rw = tb_c49r; memcpy(tb_c49rw, tb_c49_0, tb_c49_0n); tb_c49rw += tb_c49_0n; memcpy(tb_c49rw, tb_c49_1, tb_c49_1n); tb_c49rw += tb_c49_1n; memcpy(tb_c49rw, tb_c49_2, tb_c49_2n); tb_c49rw += tb_c49_2n; *tb_c49rw = 0; (const char *)tb_c49r; })), tb_ps("\n"));
    (tb_show_4(({ const char *tb_c50_0 = "запуск:  tb start "; long long tb_c50_0n = sizeof("запуск:  tb start ") - 1; const char *tb_c50_1 = v_name; long long tb_c50_1n = (long long)strlen(tb_c50_1); char *tb_c50r = tb_mem(tb_c50_0n + tb_c50_1n + 1), *tb_c50rw = tb_c50r; memcpy(tb_c50rw, tb_c50_0, tb_c50_0n); tb_c50rw += tb_c50_0n; memcpy(tb_c50rw, tb_c50_1, tb_c50_1n); tb_c50rw += tb_c50_1n; *tb_c50rw = 0; (const char *)tb_c50r; })), tb_ps("\n"));
    if (tb_fa) tb_temp_end(tb_fm);
}

static long long f_main__build(tb_o_Comp * v_c, const char * v_path, const char * v_exe) {
    const char * v_c_path = ({ const char *tb_c51_0 = v_exe; long long tb_c51_0n = (long long)strlen(tb_c51_0); const char *tb_c51_1 = ".c"; long long tb_c51_1n = sizeof(".c") - 1; char *tb_c51r = tb_mem(tb_c51_0n + tb_c51_1n + 1), *tb_c51rw = tb_c51r; memcpy(tb_c51rw, tb_c51_0, tb_c51_0n); tb_c51rw += tb_c51_0n; memcpy(tb_c51rw, tb_c51_1, tb_c51_1n); tb_c51rw += tb_c51_1n; *tb_c51rw = 0; (const char *)tb_c51r; });
    tb_o_Node * v_program = f_main__load_program(v_c, v_path);
    f_check__check(v_c, v_program);
    f_emit__emit_c(v_c, v_program);
    tb_write_file(v_c_path, tb_join(((tb_o_Comp *)tb_nn(v_c, 133))->q_out, ""));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(({ const char *tb_c53_0 = "не могу записать файл: "; long long tb_c53_0n = sizeof("не могу записать файл: ") - 1; const char *tb_c53_1 = v_c_path; long long tb_c53_1n = (long long)strlen(tb_c53_1); char *tb_c53r = tb_mem(tb_c53_0n + tb_c53_1n + 1), *tb_c53rw = tb_c53r; memcpy(tb_c53rw, tb_c53_0, tb_c53_0n); tb_c53rw += tb_c53_0n; memcpy(tb_c53rw, tb_c53_1, tb_c53_1n); tb_c53rw += tb_c53_1n; *tb_c53rw = 0; (const char *)tb_c53r; })), tb_ps("\n"));
        return 1LL;
    }
    const char * v_command = ({ const char *tb_c54_0 = "${CC:-cc} -O2 -w -o "; long long tb_c54_0n = sizeof("${CC:-cc} -O2 -w -o ") - 1; const char *tb_c54_1 = f_main__quote(v_exe); long long tb_c54_1n = (long long)strlen(tb_c54_1); const char *tb_c54_2 = " "; long long tb_c54_2n = sizeof(" ") - 1; const char *tb_c54_3 = f_main__quote(v_c_path); long long tb_c54_3n = (long long)strlen(tb_c54_3); const char *tb_c54_4 = " -lm -lpthread"; long long tb_c54_4n = sizeof(" -lm -lpthread") - 1; char *tb_c54r = tb_mem(tb_c54_0n + tb_c54_1n + tb_c54_2n + tb_c54_3n + tb_c54_4n + 1), *tb_c54rw = tb_c54r; memcpy(tb_c54rw, tb_c54_0, tb_c54_0n); tb_c54rw += tb_c54_0n; memcpy(tb_c54rw, tb_c54_1, tb_c54_1n); tb_c54rw += tb_c54_1n; memcpy(tb_c54rw, tb_c54_2, tb_c54_2n); tb_c54rw += tb_c54_2n; memcpy(tb_c54rw, tb_c54_3, tb_c54_3n); tb_c54rw += tb_c54_3n; memcpy(tb_c54rw, tb_c54_4, tb_c54_4n); tb_c54rw += tb_c54_4n; *tb_c54rw = 0; (const char *)tb_c54r; });
    if ((tb_run(v_command) != 0LL)) {
        (tb_show_4(({ const char *tb_c55_0 = "компилятор C не справился (это ошибка Tachbit, а не твоя): "; long long tb_c55_0n = sizeof("компилятор C не справился (это ошибка Tachbit, а не твоя): ") - 1; const char *tb_c55_1 = v_c_path; long long tb_c55_1n = (long long)strlen(tb_c55_1); char *tb_c55r = tb_mem(tb_c55_0n + tb_c55_1n + 1), *tb_c55rw = tb_c55r; memcpy(tb_c55rw, tb_c55_0, tb_c55_0n); tb_c55rw += tb_c55_0n; memcpy(tb_c55rw, tb_c55_1, tb_c55_1n); tb_c55rw += tb_c55_1n; *tb_c55rw = 0; (const char *)tb_c55r; })), tb_ps("\n"));
        return 1LL;
    }
    return 0LL;
    tb_crash(128, "функция build дошла до конца без return");
    return 0LL;
}

static void f_main__usage(void) {
    (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)\n"), tb_ps("\n"));
    (tb_show_4("  tb new   имя             новый проект: папка имя/ с main.tb"), tb_ps("\n"));
    (tb_show_4("  tb start имя [слова]     собрать проект и запустить (без имени — текущая папка)\n"), tb_ps("\n"));
    (tb_show_4("  tachbit run    файл.tb [слова]   сделать программу и запустить"), tb_ps("\n"));
    (tb_show_4("  tachbit build  файл.tb   сделать программу рядом с файлом"), tb_ps("\n"));
    (tb_show_4("  tachbit check  файл.tb   только найти ошибки"), tb_ps("\n"));
    (tb_show_4("  tachbit c      файл.tb   показать, какой C получился"), tb_ps("\n"));
    (tb_show_4("  tachbit tree   файл.tb   показать дерево"), tb_ps("\n"));
    (tb_show_4("  tachbit tokens файл.tb   показать токены"), tb_ps("\n"));
}

static void f_main__main(void) {
    long long tb_rl_command = -1, tb_rc_command = 0; (void)tb_rl_command; (void)tb_rc_command;
    tb_list * v_a = tb_args;
    if (((v_a)->len < 1LL)) {
        f_main__usage();
        tb_exit(1LL);
    }
    const char * v_mode = (*(const char * *)tb_at(v_a, 0LL, sizeof(const char *), 158));
    if (({ const char *tb_s56 = v_mode; ((unsigned char)tb_s56[0] == 110 && (unsigned char)tb_s56[1] == 101 && (unsigned char)tb_s56[2] == 119 && tb_s56[3] == 0); })) {
        const char * v_name = "";
        if (((v_a)->len > 1LL)) {
            v_name = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 162));
        }
        f_main__new_project(v_name);
        return;
    }
    if (((({ const char *tb_s57 = v_mode; ((unsigned char)tb_s57[0] == 104 && (unsigned char)tb_s57[1] == 101 && (unsigned char)tb_s57[2] == 108 && (unsigned char)tb_s57[3] == 112 && tb_s57[4] == 0); }) || ({ const char *tb_s58 = v_mode; ((unsigned char)tb_s58[0] == 45 && (unsigned char)tb_s58[1] == 104 && tb_s58[2] == 0); })) || ({ const char *tb_s59 = v_mode; ((unsigned char)tb_s59[0] == 45 && (unsigned char)tb_s59[1] == 45 && (unsigned char)tb_s59[2] == 104 && (unsigned char)tb_s59[3] == 101 && (unsigned char)tb_s59[4] == 108 && (unsigned char)tb_s59[5] == 112 && tb_s59[6] == 0); }))) {
        f_main__usage();
        return;
    }
    if ((({ const char *tb_s60 = v_mode; ((unsigned char)tb_s60[0] == 118 && (unsigned char)tb_s60[1] == 101 && (unsigned char)tb_s60[2] == 114 && (unsigned char)tb_s60[3] == 115 && (unsigned char)tb_s60[4] == 105 && (unsigned char)tb_s60[5] == 111 && (unsigned char)tb_s60[6] == 110 && tb_s60[7] == 0); }) || (strcmp(v_mode, "--version") == 0))) {
        (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s61 = v_mode; ((unsigned char)tb_s61[0] == 115 && (unsigned char)tb_s61[1] == 116 && (unsigned char)tb_s61[2] == 97 && (unsigned char)tb_s61[3] == 114 && (unsigned char)tb_s61[4] == 116 && tb_s61[5] == 0); })) {
        v_mode = "run";
    }
    if ((((v_a)->len < 2LL) && (!(((({ const char *tb_s62 = v_mode; ((unsigned char)tb_s62[0] == 114 && (unsigned char)tb_s62[1] == 117 && (unsigned char)tb_s62[2] == 110 && tb_s62[3] == 0); }) || ({ const char *tb_s63 = v_mode; ((unsigned char)tb_s63[0] == 98 && (unsigned char)tb_s63[1] == 117 && (unsigned char)tb_s63[2] == 105 && (unsigned char)tb_s63[3] == 108 && (unsigned char)tb_s63[4] == 100 && tb_s63[5] == 0); })) || ({ const char *tb_s64 = v_mode; ((unsigned char)tb_s64[0] == 99 && (unsigned char)tb_s64[1] == 104 && (unsigned char)tb_s64[2] == 101 && (unsigned char)tb_s64[3] == 99 && (unsigned char)tb_s64[4] == 107 && tb_s64[5] == 0); })) || ({ const char *tb_s65 = v_mode; ((unsigned char)tb_s65[0] == 99 && tb_s65[1] == 0); }))))) {
        f_main__usage();
        tb_exit(1LL);
    }
    const char * v_arg = "";
    if (((v_a)->len > 1LL)) {
        v_arg = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 179));
    }
    const char * v_path;
    const char * v_exe;
    {
        tb_r_f_main__project_main tb_m66 = f_main__project_main(v_arg);
        v_path = tb_m66.o0;
        v_exe = tb_m66.o1;
    }
    tb_o_Comp * v_c = tb_new_Comp(v_path, tb_list_new(sizeof(tb_o_Token *)), 0LL, tb_list_new(sizeof(tb_o_Type *)), tb_list_new(sizeof(tb_o_Func *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Use *)), tb_list_new(sizeof(const char *)), "", tb_list_new(sizeof(tb_o_Var *)), 0LL, 0LL, 0LL, 0LL, tb_list_new(sizeof(long long)), 0LL, 0, 0LL, 0LL, 0, 0, tb_list_new(sizeof(const char *)), 0LL, 0, tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(long long)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(long long)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(const char *)), "", 0);
    if (({ const char *tb_s67 = v_mode; ((unsigned char)tb_s67[0] == 116 && (unsigned char)tb_s67[1] == 111 && (unsigned char)tb_s67[2] == 107 && (unsigned char)tb_s67[3] == 101 && (unsigned char)tb_s67[4] == 110 && (unsigned char)tb_s67[5] == 115 && tb_s67[6] == 0); })) {
        const char * tb_v68 = tb_read_file(v_path);
        if (tb_err != 0) {
            const char * const v_err = tb_err;
            tb_err = 0;
            f_main__say_error(({ const char *tb_c69_0 = "не могу открыть файл: "; long long tb_c69_0n = sizeof("не могу открыть файл: ") - 1; const char *tb_c69_1 = v_path; long long tb_c69_1n = (long long)strlen(tb_c69_1); char *tb_c69r = tb_mem(tb_c69_0n + tb_c69_1n + 1), *tb_c69rw = tb_c69r; memcpy(tb_c69rw, tb_c69_0, tb_c69_0n); tb_c69rw += tb_c69_0n; memcpy(tb_c69rw, tb_c69_1, tb_c69_1n); tb_c69rw += tb_c69_1n; *tb_c69rw = 0; (const char *)tb_c69r; }));
        }
        const char * v_src = tb_v68;
        f_lexer__lex(v_c, v_src);
        f_lexer__print_tokens(v_c);
        return;
    }
    if (({ const char *tb_s70 = v_mode; ((unsigned char)tb_s70[0] == 116 && (unsigned char)tb_s70[1] == 114 && (unsigned char)tb_s70[2] == 101 && (unsigned char)tb_s70[3] == 101 && tb_s70[4] == 0); })) {
        tb_list * v_out = tb_list_new(sizeof(const char *));
        f_parser__tree_lines(f_main__load_program(v_c, v_path), 0LL, v_out);
        (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s71 = v_mode; ((unsigned char)tb_s71[0] == 99 && (unsigned char)tb_s71[1] == 104 && (unsigned char)tb_s71[2] == 101 && (unsigned char)tb_s71[3] == 99 && (unsigned char)tb_s71[4] == 107 && tb_s71[5] == 0); })) {
        f_check__check(v_c, f_main__load_program(v_c, v_path));
        (tb_show_4("ошибок нет"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s72 = v_mode; ((unsigned char)tb_s72[0] == 99 && tb_s72[1] == 0); })) {
        tb_o_Node * v_program = f_main__load_program(v_c, v_path);
        f_check__check(v_c, v_program);
        f_emit__emit_c(v_c, v_program);
        (tb_show_4(tb_cut(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, ""), 0LL, (tb_tlen(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, "")) - 2LL), 201)), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s73 = v_mode; ((unsigned char)tb_s73[0] == 98 && (unsigned char)tb_s73[1] == 117 && (unsigned char)tb_s73[2] == 105 && (unsigned char)tb_s73[3] == 108 && (unsigned char)tb_s73[4] == 100 && tb_s73[5] == 0); })) {
        tb_exit(f_main__build(v_c, v_path, v_exe));
    }
    if (({ const char *tb_s74 = v_mode; ((unsigned char)tb_s74[0] == 114 && (unsigned char)tb_s74[1] == 117 && (unsigned char)tb_s74[2] == 110 && tb_s74[3] == 0); })) {
        if ((f_main__build(v_c, v_path, v_exe) != 0LL)) {
            tb_exit(1LL);
        }
        const char * v_command = f_main__quote(v_exe); tb_rl_command = -1;
        if ((tb_find(v_exe, "/") < 0LL)) {
            v_command = f_main__quote(({ const char *tb_c75_0 = "./"; long long tb_c75_0n = sizeof("./") - 1; const char *tb_c75_1 = v_exe; long long tb_c75_1n = (long long)strlen(tb_c75_1); char *tb_c75r = tb_mem(tb_c75_0n + tb_c75_1n + 1), *tb_c75rw = tb_c75r; memcpy(tb_c75rw, tb_c75_0, tb_c75_0n); tb_c75rw += tb_c75_0n; memcpy(tb_c75rw, tb_c75_1, tb_c75_1n); tb_c75rw += tb_c75_1n; *tb_c75rw = 0; (const char *)tb_c75r; })); tb_rl_command = -1;
        }
        {
            long long tb_end76 = ((v_a)->len - 1LL);
            for (long long v_i = 2LL; v_i <= tb_end76; v_i++) {
                tb_app_n(&v_command, &tb_rl_command, &tb_rc_command, " ", sizeof(" ") - 1); tb_app(&v_command, &tb_rl_command, &tb_rc_command, f_main__quote((*(const char * *)tb_at(v_a, v_i, sizeof(const char *), 213)))); 
            }
        }
        tb_exit(tb_run((tb_rl_command = -1, v_command)));
    }
    f_main__usage();
    tb_exit(1LL);
}

static void f_errors__error_at(tb_o_Comp * v_c, long long v_line, const char * v_message, const char * v_detail) {
    long long tb_rl_s = -1, tb_rc_s = 0; (void)tb_rl_s; (void)tb_rc_s;
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    _Bool v_color = (tb_run("[ -t 1 ] && [ -z \"$NO_COLOR\" ]") == 0LL);
    const char * v_place = ((tb_o_Comp *)tb_nn(v_c, 7))->q_file_name;
    if ((v_line > 0LL)) {
        v_place = ({ const char *tb_c77_0 = ((tb_o_Comp *)tb_nn(v_c, 9))->q_file_name; long long tb_c77_0n = (long long)strlen(tb_c77_0); const char *tb_c77_1 = ", строка "; long long tb_c77_1n = sizeof(", строка ") - 1; char tb_c77_2d[24]; long long tb_c77_2n; const char *tb_c77_2 = tb_digits(v_line, tb_c77_2d, &tb_c77_2n); char *tb_c77r = tb_mem(tb_c77_0n + tb_c77_1n + tb_c77_2n + 1), *tb_c77rw = tb_c77r; memcpy(tb_c77rw, tb_c77_0, tb_c77_0n); tb_c77rw += tb_c77_0n; memcpy(tb_c77rw, tb_c77_1, tb_c77_1n); tb_c77rw += tb_c77_1n; memcpy(tb_c77rw, tb_c77_2, tb_c77_2n); tb_c77rw += tb_c77_2n; *tb_c77rw = 0; (const char *)tb_c77r; });
    }
    const char * v_s = ({ const char *tb_c78_0 = v_place; long long tb_c78_0n = (long long)strlen(tb_c78_0); const char *tb_c78_1 = ": "; long long tb_c78_1n = sizeof(": ") - 1; const char *tb_c78_2 = v_message; long long tb_c78_2n = (long long)strlen(tb_c78_2); char *tb_c78r = tb_mem(tb_c78_0n + tb_c78_1n + tb_c78_2n + 1), *tb_c78rw = tb_c78r; memcpy(tb_c78rw, tb_c78_0, tb_c78_0n); tb_c78rw += tb_c78_0n; memcpy(tb_c78rw, tb_c78_1, tb_c78_1n); tb_c78rw += tb_c78_1n; memcpy(tb_c78rw, tb_c78_2, tb_c78_2n); tb_c78rw += tb_c78_2n; *tb_c78rw = 0; (const char *)tb_c78r; }); tb_rl_s = -1;
    if (({ const char *tb_s79 = v_detail; !(tb_s79[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, ": ", sizeof(": ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_detail); 
    }
    if (v_color) {
        const char * v_esc = tb_chr(27LL);
        v_s = ({ const char *tb_c80_0 = v_esc; long long tb_c80_0n = (long long)strlen(tb_c80_0); const char *tb_c80_1 = "[1m"; long long tb_c80_1n = sizeof("[1m") - 1; const char *tb_c80_2 = v_place; long long tb_c80_2n = (long long)strlen(tb_c80_2); const char *tb_c80_3 = ":"; long long tb_c80_3n = sizeof(":") - 1; const char *tb_c80_4 = v_esc; long long tb_c80_4n = (long long)strlen(tb_c80_4); const char *tb_c80_5 = "[0m "; long long tb_c80_5n = sizeof("[0m ") - 1; const char *tb_c80_6 = v_esc; long long tb_c80_6n = (long long)strlen(tb_c80_6); const char *tb_c80_7 = "[31m"; long long tb_c80_7n = sizeof("[31m") - 1; const char *tb_c80_8 = v_message; long long tb_c80_8n = (long long)strlen(tb_c80_8); const char *tb_c80_9 = v_esc; long long tb_c80_9n = (long long)strlen(tb_c80_9); const char *tb_c80_10 = "[0m"; long long tb_c80_10n = sizeof("[0m") - 1; char *tb_c80r = tb_mem(tb_c80_0n + tb_c80_1n + tb_c80_2n + tb_c80_3n + tb_c80_4n + tb_c80_5n + tb_c80_6n + tb_c80_7n + tb_c80_8n + tb_c80_9n + tb_c80_10n + 1), *tb_c80rw = tb_c80r; memcpy(tb_c80rw, tb_c80_0, tb_c80_0n); tb_c80rw += tb_c80_0n; memcpy(tb_c80rw, tb_c80_1, tb_c80_1n); tb_c80rw += tb_c80_1n; memcpy(tb_c80rw, tb_c80_2, tb_c80_2n); tb_c80rw += tb_c80_2n; memcpy(tb_c80rw, tb_c80_3, tb_c80_3n); tb_c80rw += tb_c80_3n; memcpy(tb_c80rw, tb_c80_4, tb_c80_4n); tb_c80rw += tb_c80_4n; memcpy(tb_c80rw, tb_c80_5, tb_c80_5n); tb_c80rw += tb_c80_5n; memcpy(tb_c80rw, tb_c80_6, tb_c80_6n); tb_c80rw += tb_c80_6n; memcpy(tb_c80rw, tb_c80_7, tb_c80_7n); tb_c80rw += tb_c80_7n; memcpy(tb_c80rw, tb_c80_8, tb_c80_8n); tb_c80rw += tb_c80_8n; memcpy(tb_c80rw, tb_c80_9, tb_c80_9n); tb_c80rw += tb_c80_9n; memcpy(tb_c80rw, tb_c80_10, tb_c80_10n); tb_c80rw += tb_c80_10n; *tb_c80rw = 0; (const char *)tb_c80r; }); tb_rl_s = -1;
        if (({ const char *tb_s81 = v_detail; !(tb_s81[0] == 0); })) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, ": ", sizeof(": ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_esc); tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "[33m", sizeof("[33m") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_detail); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_esc); tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "[0m", sizeof("[0m") - 1); 
        }
    }
    (tb_show_4(v_s), tb_ps("\n"));
    tb_exit(1LL);
    if (tb_fa) tb_temp_end(tb_fm);
}

static _Bool f_lexer__is_command(const char * v_word) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    tb_list * v_commands = tb_list_from(40, sizeof(const char *), (const char *[]){"func", "return", "gives", "if", "else", "each", "in", "count", "from", "to", "repeat", "times", "while", "skip", "stop", "const", "shared", "obj", "link", "int", "dec", "text", "bool", "and", "or", "not", "true", "false", "use", "modpub", "modpri", "temp", "together", "background", "danger", "by", "list", "dict", "fail", "try"});
    {
        tb_list * tb_l82 = v_commands;
        for (long long tb_i82 = 0; tb_i82 < tb_l82->len; tb_i82++) {
            const char * const v_c = ((const char * *)tb_l82->data)[tb_i82];
            if ((strcmp(v_c, v_word) == 0)) {
                { _Bool tb_rv = 1; if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
            }
        }
    }
    { _Bool tb_rv = 0; if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
    if (tb_fa) tb_temp_end(tb_fm);
    tb_crash(6, "функция is_command дошла до конца без return");
    return 0;
}

static long long f_lexer__at(const char * v_s, long long v_i) {
    if ((v_i < tb_tlen(v_s))) {
        return tb_code(tb_char_at(v_s, v_i, -1, 16), 16);
    }
    return 0LL;
    tb_crash(14, "функция at дошла до конца без return");
    return 0LL;
}

static _Bool f_lexer__is_letter(long long v_c) {
    return ((((v_c >= 97LL) && (v_c <= 122LL)) || ((v_c >= 65LL) && (v_c <= 90LL))) || (v_c == 95LL));
    tb_crash(19, "функция is_letter дошла до конца без return");
    return 0;
}

static _Bool f_lexer__is_digit(long long v_c) {
    return ((v_c >= 48LL) && (v_c <= 57LL));
    tb_crash(22, "функция is_digit дошла до конца без return");
    return 0;
}

static void f_lexer__add_token(tb_o_Comp * v_c, const char * v_kind, const char * v_word, long long v_line) {
    { tb_o_Token * tb_v83 = tb_new_Token(v_kind, v_word, v_line); *(tb_o_Token * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 26))->q_tokens) = tb_v83; }
}

static _Bool f_lexer__two_char_sign(const char * v_a, const char * v_b) {
    if ((({ const char *tb_s84 = v_b; ((unsigned char)tb_s84[0] == 61 && tb_s84[1] == 0); }) && (((((((({ const char *tb_s85 = v_a; ((unsigned char)tb_s85[0] == 43 && tb_s85[1] == 0); }) || ({ const char *tb_s86 = v_a; ((unsigned char)tb_s86[0] == 45 && tb_s86[1] == 0); })) || ({ const char *tb_s87 = v_a; ((unsigned char)tb_s87[0] == 42 && tb_s87[1] == 0); })) || ({ const char *tb_s88 = v_a; ((unsigned char)tb_s88[0] == 47 && tb_s88[1] == 0); })) || ({ const char *tb_s89 = v_a; ((unsigned char)tb_s89[0] == 61 && tb_s89[1] == 0); })) || ({ const char *tb_s90 = v_a; ((unsigned char)tb_s90[0] == 33 && tb_s90[1] == 0); })) || ({ const char *tb_s91 = v_a; ((unsigned char)tb_s91[0] == 60 && tb_s91[1] == 0); })) || ({ const char *tb_s92 = v_a; ((unsigned char)tb_s92[0] == 62 && tb_s92[1] == 0); })))) {
        return 1;
    }
    if ((({ const char *tb_s93 = v_a; ((unsigned char)tb_s93[0] == 45 && tb_s93[1] == 0); }) && ({ const char *tb_s94 = v_b; ((unsigned char)tb_s94[0] == 62 && tb_s94[1] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s95 = v_a; ((unsigned char)tb_s95[0] == 46 && tb_s95[1] == 0); }) && ({ const char *tb_s96 = v_b; ((unsigned char)tb_s96[0] == 46 && tb_s96[1] == 0); }))) {
        return 1;
    }
    return 0;
    tb_crash(29, "функция two_char_sign дошла до конца без return");
    return 0;
}

static _Bool f_lexer__one_char_sign(const char * v_ch) {
    return (tb_find("()[]{},:=+-*/%<>.", v_ch) >= 0LL);
    tb_crash(38, "функция one_char_sign дошла до конца без return");
    return 0;
}

static void f_lexer__lex(tb_o_Comp * v_c, const char * v_src) {
    tb_list * v_fresh = tb_list_new(sizeof(tb_o_Token *));
    { tb_list * tb_v97 = v_fresh; tb_list * *tb_p97 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_tokens); *tb_p97 = tb_v97; }
    tb_list * v_indents = tb_list_from(1, sizeof(long long), (long long[]){0LL});
    tb_list * v_lines = tb_split(v_src, "\n", 45);
    long long v_line = 0LL;
    {
        tb_list * tb_l98 = v_lines;
        for (long long tb_i98 = 0; tb_i98 < tb_l98->len; tb_i98++) {
            const char * const v_s = ((const char * *)tb_l98->data)[tb_i98];
            { long long tb_v99 = 1LL; long long *tb_p99 = &(v_line); *tb_p99 += tb_v99; }
            long long v_n = tb_tlen(v_s);
            long long v_spaces = 0LL;
            long long v_pos = 0LL;
            while (((v_pos < v_n) && ((f_lexer__at(v_s, v_pos) == 32LL) || (f_lexer__at(v_s, v_pos) == 9LL)))) {
                tb_mark tb_am100 = tb_temp_begin();
                do {
                    if ((f_lexer__at(v_s, v_pos) == 9LL)) {
                        { long long tb_v101 = 4LL; long long *tb_p101 = &(v_spaces); *tb_p101 += tb_v101; }
                    } else {
                        { long long tb_v102 = 1LL; long long *tb_p102 = &(v_spaces); *tb_p102 += tb_v102; }
                    }
                    { long long tb_v103 = 1LL; long long *tb_p103 = &(v_pos); *tb_p103 += tb_v103; }
                } while (0);
                tb_temp_end(tb_am100);
            }
            if ((((v_pos >= v_n) || (f_lexer__at(v_s, v_pos) == 13LL)) || ((f_lexer__at(v_s, v_pos) == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL)))) {
                continue;
            }
            long long v_top = (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 62));
            if ((v_spaces > v_top)) {
                if (((v_indents)->len == 200LL)) {
                    f_errors__error_at(v_c, v_line, "слишком глубокая вложенность", "");
                }
                { long long tb_v104 = v_spaces; *(long long *)tb_grow(v_indents) = tb_v104; }
                f_lexer__add_token(v_c, "indent", "", v_line);
            } else {
                while ((v_spaces < (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 69)))) {
                    long long v_gone = (*(long long *)tb_pop(v_indents, 70));
                    f_lexer__add_token(v_c, "dedent", "", v_line);
                }
                if ((v_spaces != (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 72)))) {
                    f_errors__error_at(v_c, v_line, "отступ не совпадает ни с одним уровнем выше", "");
                }
            }
            while ((v_pos < v_n)) {
                long long v_ch = f_lexer__at(v_s, v_pos);
                if ((((v_ch == 32LL) || (v_ch == 9LL)) || (v_ch == 13LL))) {
                    { long long tb_v105 = 1LL; long long *tb_p105 = &(v_pos); *tb_p105 += tb_v105; }
                    continue;
                }
                if (((v_ch == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL))) {
                    break;
                }
                long long v_start = v_pos;
                if (f_lexer__is_letter(v_ch)) {
                    while ((f_lexer__is_letter(f_lexer__at(v_s, v_pos)) || f_lexer__is_digit(f_lexer__at(v_s, v_pos)))) {
                        { long long tb_v106 = 1LL; long long *tb_p106 = &(v_pos); *tb_p106 += tb_v106; }
                    }
                    const char * v_word = tb_cut(v_s, v_start, (v_pos - 1LL), 87);
                    if (f_lexer__is_command(v_word)) {
                        f_lexer__add_token(v_c, "command", v_word, v_line);
                    } else {
                        f_lexer__add_token(v_c, "name", v_word, v_line);
                    }
                } else if (f_lexer__is_digit(v_ch)) {
                    const char * v_kind = "number";
                    while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                        { long long tb_v107 = 1LL; long long *tb_p107 = &(v_pos); *tb_p107 += tb_v107; }
                    }
                    if (((f_lexer__at(v_s, v_pos) == 46LL) && f_lexer__is_digit(f_lexer__at(v_s, (v_pos + 1LL))))) {
                        v_kind = "decimal";
                        { long long tb_v108 = 1LL; long long *tb_p108 = &(v_pos); *tb_p108 += tb_v108; }
                        while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                            { long long tb_v109 = 1LL; long long *tb_p109 = &(v_pos); *tb_p109 += tb_v109; }
                        }
                    }
                    f_lexer__add_token(v_c, v_kind, tb_cut(v_s, v_start, (v_pos - 1LL), 101), v_line);
                } else if ((v_ch == 34LL)) {
                    { long long tb_v110 = 1LL; long long *tb_p110 = &(v_pos); *tb_p110 += tb_v110; }
                    while (((v_pos < v_n) && (f_lexer__at(v_s, v_pos) != 34LL))) {
                        tb_mark tb_am111 = tb_temp_begin();
                        do {
                            if ((f_lexer__at(v_s, v_pos) == 92LL)) {
                                { long long tb_v112 = 1LL; long long *tb_p112 = &(v_pos); *tb_p112 += tb_v112; }
                            }
                            { long long tb_v113 = 1LL; long long *tb_p113 = &(v_pos); *tb_p113 += tb_v113; }
                        } while (0);
                        tb_temp_end(tb_am111);
                    }
                    if ((f_lexer__at(v_s, v_pos) != 34LL)) {
                        f_errors__error_at(v_c, v_line, "текст в кавычках не закрыт", "");
                    }
                    { long long tb_v114 = 1LL; long long *tb_p114 = &(v_pos); *tb_p114 += tb_v114; }
                    f_lexer__add_token(v_c, "string", tb_cut(v_s, v_start, (v_pos - 1LL), 111), v_line);
                } else {
                    const char * v_a = tb_char_at(v_s, v_pos, -1, 113);
                    const char * v_b = "";
                    if (((v_pos + 1LL) < v_n)) {
                        v_b = tb_char_at(v_s, (v_pos + 1LL), -1, 116);
                    }
                    if (f_lexer__two_char_sign(v_a, v_b)) {
                        f_lexer__add_token(v_c, "sign", ({ const char *tb_c115_0 = v_a; long long tb_c115_0n = (long long)strlen(tb_c115_0); const char *tb_c115_1 = v_b; long long tb_c115_1n = (long long)strlen(tb_c115_1); char *tb_c115r = tb_mem(tb_c115_0n + tb_c115_1n + 1), *tb_c115rw = tb_c115r; memcpy(tb_c115rw, tb_c115_0, tb_c115_0n); tb_c115rw += tb_c115_0n; memcpy(tb_c115rw, tb_c115_1, tb_c115_1n); tb_c115rw += tb_c115_1n; *tb_c115rw = 0; (const char *)tb_c115r; }), v_line);
                        { long long tb_v116 = 2LL; long long *tb_p116 = &(v_pos); *tb_p116 += tb_v116; }
                    } else if (f_lexer__one_char_sign(v_a)) {
                        f_lexer__add_token(v_c, "sign", v_a, v_line);
                        { long long tb_v117 = 1LL; long long *tb_p117 = &(v_pos); *tb_p117 += tb_v117; }
                    } else {
                        f_errors__error_at(v_c, v_line, "непонятный символ", v_a);
                    }
                }
            }
            f_lexer__add_token(v_c, "newline", "", v_line);
        }
    }
    if (((tb_tlen(v_src) > 0LL) && tb_ends(v_src, "\n"))) {
        { long long tb_v118 = 1LL; long long *tb_p118 = &(v_line); *tb_p118 -= tb_v118; }
    }
    while (((v_indents)->len > 1LL)) {
        long long v_gone = (*(long long *)tb_pop(v_indents, 130));
        f_lexer__add_token(v_c, "dedent", "", (v_line + 1LL));
    }
    f_lexer__add_token(v_c, "end", "", (v_line + 1LL));
}

static const char * f_lexer__token_kind_name(const char * v_kind) {
    if (({ const char *tb_s119 = v_kind; ((unsigned char)tb_s119[0] == 110 && (unsigned char)tb_s119[1] == 97 && (unsigned char)tb_s119[2] == 109 && (unsigned char)tb_s119[3] == 101 && tb_s119[4] == 0); })) {
        return "ИМЯ";
    }
    if (({ const char *tb_s120 = v_kind; ((unsigned char)tb_s120[0] == 99 && (unsigned char)tb_s120[1] == 111 && (unsigned char)tb_s120[2] == 109 && (unsigned char)tb_s120[3] == 109 && (unsigned char)tb_s120[4] == 97 && (unsigned char)tb_s120[5] == 110 && (unsigned char)tb_s120[6] == 100 && tb_s120[7] == 0); })) {
        return "КОМАНДА";
    }
    if (({ const char *tb_s121 = v_kind; ((unsigned char)tb_s121[0] == 110 && (unsigned char)tb_s121[1] == 117 && (unsigned char)tb_s121[2] == 109 && (unsigned char)tb_s121[3] == 98 && (unsigned char)tb_s121[4] == 101 && (unsigned char)tb_s121[5] == 114 && tb_s121[6] == 0); })) {
        return "ЧИСЛО";
    }
    if (({ const char *tb_s122 = v_kind; ((unsigned char)tb_s122[0] == 100 && (unsigned char)tb_s122[1] == 101 && (unsigned char)tb_s122[2] == 99 && (unsigned char)tb_s122[3] == 105 && (unsigned char)tb_s122[4] == 109 && (unsigned char)tb_s122[5] == 97 && (unsigned char)tb_s122[6] == 108 && tb_s122[7] == 0); })) {
        return "ДРОБЬ";
    }
    if (({ const char *tb_s123 = v_kind; ((unsigned char)tb_s123[0] == 115 && (unsigned char)tb_s123[1] == 116 && (unsigned char)tb_s123[2] == 114 && (unsigned char)tb_s123[3] == 105 && (unsigned char)tb_s123[4] == 110 && (unsigned char)tb_s123[5] == 103 && tb_s123[6] == 0); })) {
        return "ТЕКСТ";
    }
    if (({ const char *tb_s124 = v_kind; ((unsigned char)tb_s124[0] == 115 && (unsigned char)tb_s124[1] == 105 && (unsigned char)tb_s124[2] == 103 && (unsigned char)tb_s124[3] == 110 && tb_s124[4] == 0); })) {
        return "ЗНАК";
    }
    if (({ const char *tb_s125 = v_kind; ((unsigned char)tb_s125[0] == 110 && (unsigned char)tb_s125[1] == 101 && (unsigned char)tb_s125[2] == 119 && (unsigned char)tb_s125[3] == 108 && (unsigned char)tb_s125[4] == 105 && (unsigned char)tb_s125[5] == 110 && (unsigned char)tb_s125[6] == 101 && tb_s125[7] == 0); })) {
        return "КОНЕЦ СТРОКИ";
    }
    if (({ const char *tb_s126 = v_kind; ((unsigned char)tb_s126[0] == 105 && (unsigned char)tb_s126[1] == 110 && (unsigned char)tb_s126[2] == 100 && (unsigned char)tb_s126[3] == 101 && (unsigned char)tb_s126[4] == 110 && (unsigned char)tb_s126[5] == 116 && tb_s126[6] == 0); })) {
        return "ОТСТУП";
    }
    if (({ const char *tb_s127 = v_kind; ((unsigned char)tb_s127[0] == 100 && (unsigned char)tb_s127[1] == 101 && (unsigned char)tb_s127[2] == 100 && (unsigned char)tb_s127[3] == 101 && (unsigned char)tb_s127[4] == 110 && (unsigned char)tb_s127[5] == 116 && tb_s127[6] == 0); })) {
        return "ВЫСТУП";
    }
    return "КОНЕЦ ФАЙЛА";
    tb_crash(134, "функция token_kind_name дошла до конца без return");
    return "";
}

static void f_lexer__print_tokens(tb_o_Comp * v_c) {
    long long tb_rl_name = -1, tb_rc_name = 0; (void)tb_rl_name; (void)tb_rc_name;
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    tb_list * v_out = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l128 = ((tb_o_Comp *)tb_nn(v_c, 158))->q_tokens;
        for (long long tb_i128 = 0; tb_i128 < tb_l128->len; tb_i128++) {
            tb_o_Token * const v_t = ((tb_o_Token * *)tb_l128->data)[tb_i128];
            const char * v_num = ({ const char *tb_c129_0 = ""; long long tb_c129_0n = sizeof("") - 1; char tb_c129_1d[24]; long long tb_c129_1n; const char *tb_c129_1 = tb_digits(((tb_o_Token *)tb_nn(v_t, 159))->q_line, tb_c129_1d, &tb_c129_1n); char *tb_c129r = tb_mem(tb_c129_0n + tb_c129_1n + 1), *tb_c129rw = tb_c129r; memcpy(tb_c129rw, tb_c129_0, tb_c129_0n); tb_c129rw += tb_c129_0n; memcpy(tb_c129rw, tb_c129_1, tb_c129_1n); tb_c129rw += tb_c129_1n; *tb_c129rw = 0; (const char *)tb_c129r; });
            while ((tb_tlen(v_num) < 3LL)) {
                v_num = ({ const char *tb_c130_0 = " "; long long tb_c130_0n = sizeof(" ") - 1; const char *tb_c130_1 = v_num; long long tb_c130_1n = (long long)strlen(tb_c130_1); char *tb_c130r = tb_mem(tb_c130_0n + tb_c130_1n + 1), *tb_c130rw = tb_c130r; memcpy(tb_c130rw, tb_c130_0, tb_c130_0n); tb_c130rw += tb_c130_0n; memcpy(tb_c130rw, tb_c130_1, tb_c130_1n); tb_c130rw += tb_c130_1n; *tb_c130rw = 0; (const char *)tb_c130r; });
            }
            const char * v_name = f_lexer__token_kind_name(((tb_o_Token *)tb_nn(v_t, 162))->q_kind); tb_rl_name = -1;
            while (((tb_rl_name >= 0 ? tb_rl_name : tb_tlen(v_name)) < 13LL)) {
                tb_app_n(&v_name, &tb_rl_name, &tb_rc_name, " ", sizeof(" ") - 1); 
            }
            { const char * tb_v131 = ({ const char *tb_c132_0 = v_num; long long tb_c132_0n = (long long)strlen(tb_c132_0); const char *tb_c132_1 = ": "; long long tb_c132_1n = sizeof(": ") - 1; const char *tb_c132_2 = v_name; long long tb_c132_2n = (long long)strlen(tb_c132_2); const char *tb_c132_3 = " "; long long tb_c132_3n = sizeof(" ") - 1; const char *tb_c132_4 = ((tb_o_Token *)tb_nn(v_t, 165))->q_word; long long tb_c132_4n = (long long)strlen(tb_c132_4); char *tb_c132r = tb_mem(tb_c132_0n + tb_c132_1n + tb_c132_2n + tb_c132_3n + tb_c132_4n + 1), *tb_c132rw = tb_c132r; memcpy(tb_c132rw, tb_c132_0, tb_c132_0n); tb_c132rw += tb_c132_0n; memcpy(tb_c132rw, tb_c132_1, tb_c132_1n); tb_c132rw += tb_c132_1n; memcpy(tb_c132rw, tb_c132_2, tb_c132_2n); tb_c132rw += tb_c132_2n; memcpy(tb_c132rw, tb_c132_3, tb_c132_3n); tb_c132rw += tb_c132_3n; memcpy(tb_c132rw, tb_c132_4, tb_c132_4n); tb_c132rw += tb_c132_4n; *tb_c132rw = 0; (const char *)tb_c132r; }); *(const char * *)tb_grow(v_out) = tb_v131; }
        }
    }
    (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
    if (tb_fa) tb_temp_end(tb_fm);
}

static tb_o_Node * f_parser__new_node(const char * v_kind, long long v_line) {
    return tb_new_Node(v_kind, v_line, "", "", 0, 0, (-1LL), 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    tb_crash(7, "функция new_node дошла до конца без return");
    return 0;
}

static void f_parser__add_kid(tb_o_Node * v_parent, tb_o_Node * v_kid) {
    { tb_o_Node * tb_v133 = v_kid; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_parent, 11))->q_kids) = tb_v133; }
}

static tb_o_Token * f_parser__now(tb_o_Comp * v_c) {
    return (*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 16))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 16))->q_pos, sizeof(tb_o_Token *), 16));
    tb_crash(15, "функция now дошла до конца без return");
    return 0;
}

static tb_o_Token * f_parser__ahead(tb_o_Comp * v_c) {
    if (((((tb_o_Comp *)tb_nn(v_c, 19))->q_pos + 1LL) < (((tb_o_Comp *)tb_nn(v_c, 19))->q_tokens)->len)) {
        return (*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 20))->q_tokens, (((tb_o_Comp *)tb_nn(v_c, 20))->q_pos + 1LL), sizeof(tb_o_Token *), 20));
    }
    return (*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 21))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 21))->q_pos, sizeof(tb_o_Token *), 21));
    tb_crash(18, "функция ahead дошла до конца без return");
    return 0;
}

static long long f_parser__line_now(tb_o_Comp * v_c) {
    return ((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 24))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 24))->q_pos, sizeof(tb_o_Token *), 24)), 24))->q_line;
    tb_crash(23, "функция line_now дошла до конца без return");
    return 0LL;
}

static void f_parser__next(tb_o_Comp * v_c) {
    if (({ const char *tb_s134 = ((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 27))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 27))->q_pos, sizeof(tb_o_Token *), 27)), 27))->q_kind; !((unsigned char)tb_s134[0] == 101 && (unsigned char)tb_s134[1] == 110 && (unsigned char)tb_s134[2] == 100 && tb_s134[3] == 0); })) {
        { long long tb_v135 = 1LL; long long *tb_p135 = &(((tb_o_Comp *)tb_nn(v_c, 28))->q_pos); *tb_p135 += tb_v135; }
    }
}

static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s136 = ((tb_o_Token *)tb_nn(v_t, 32))->q_kind; ((unsigned char)tb_s136[0] == 115 && (unsigned char)tb_s136[1] == 105 && (unsigned char)tb_s136[2] == 103 && (unsigned char)tb_s136[3] == 110 && tb_s136[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_word, v_s) == 0));
    tb_crash(30, "функция is_sign дошла до конца без return");
    return 0;
}

static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s137 = ((tb_o_Token *)tb_nn(v_t, 36))->q_kind; ((unsigned char)tb_s137[0] == 99 && (unsigned char)tb_s137[1] == 111 && (unsigned char)tb_s137[2] == 109 && (unsigned char)tb_s137[3] == 109 && (unsigned char)tb_s137[4] == 97 && (unsigned char)tb_s137[5] == 110 && (unsigned char)tb_s137[6] == 100 && tb_s137[7] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_word, v_s) == 0));
    tb_crash(34, "функция is_cmd дошла до конца без return");
    return 0;
}

static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__ahead(v_c);
    return (({ const char *tb_s138 = ((tb_o_Token *)tb_nn(v_t, 40))->q_kind; ((unsigned char)tb_s138[0] == 115 && (unsigned char)tb_s138[1] == 105 && (unsigned char)tb_s138[2] == 103 && (unsigned char)tb_s138[3] == 110 && tb_s138[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_word, v_s) == 0));
    tb_crash(38, "функция ahead_is_sign дошла до конца без return");
    return 0;
}

static const char * f_parser__what_is_here(tb_o_Comp * v_c) {
    const char * v_k = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 43))->q_kind;
    if (({ const char *tb_s139 = v_k; ((unsigned char)tb_s139[0] == 110 && (unsigned char)tb_s139[1] == 101 && (unsigned char)tb_s139[2] == 119 && (unsigned char)tb_s139[3] == 108 && (unsigned char)tb_s139[4] == 105 && (unsigned char)tb_s139[5] == 110 && (unsigned char)tb_s139[6] == 101 && tb_s139[7] == 0); })) {
        return "конец строки";
    }
    if (({ const char *tb_s140 = v_k; ((unsigned char)tb_s140[0] == 105 && (unsigned char)tb_s140[1] == 110 && (unsigned char)tb_s140[2] == 100 && (unsigned char)tb_s140[3] == 101 && (unsigned char)tb_s140[4] == 110 && (unsigned char)tb_s140[5] == 116 && tb_s140[6] == 0); })) {
        return "лишний отступ";
    }
    if (({ const char *tb_s141 = v_k; ((unsigned char)tb_s141[0] == 100 && (unsigned char)tb_s141[1] == 101 && (unsigned char)tb_s141[2] == 100 && (unsigned char)tb_s141[3] == 101 && (unsigned char)tb_s141[4] == 110 && (unsigned char)tb_s141[5] == 116 && tb_s141[6] == 0); })) {
        return "конец блока";
    }
    if (({ const char *tb_s142 = v_k; ((unsigned char)tb_s142[0] == 101 && (unsigned char)tb_s142[1] == 110 && (unsigned char)tb_s142[2] == 100 && tb_s142[3] == 0); })) {
        return "конец файла";
    }
    return ((tb_o_Token *)tb_nn(f_parser__now(v_c), 52))->q_word;
    tb_crash(42, "функция what_is_here дошла до конца без return");
    return "";
}

static void f_parser__expect_sign(tb_o_Comp * v_c, const char * v_s) {
    if ((!f_parser__is_sign(v_c, v_s))) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "ожидался знак", v_s);
    }
    f_parser__next(v_c);
}

static void f_parser__expect_cmd(tb_o_Comp * v_c, const char * v_s) {
    if ((!f_parser__is_cmd(v_c, v_s))) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "ожидалась команда", v_s);
    }
    f_parser__next(v_c);
}

static const char * f_parser__expect_name(tb_o_Comp * v_c) {
    tb_o_Token * v_t = f_parser__now(v_c);
    if (({ const char *tb_s143 = ((tb_o_Token *)tb_nn(v_t, 66))->q_kind; !((unsigned char)tb_s143[0] == 110 && (unsigned char)tb_s143[1] == 97 && (unsigned char)tb_s143[2] == 109 && (unsigned char)tb_s143[3] == 101 && tb_s143[4] == 0); })) {
        if (({ const char *tb_s144 = ((tb_o_Token *)tb_nn(v_t, 67))->q_kind; ((unsigned char)tb_s144[0] == 99 && (unsigned char)tb_s144[1] == 111 && (unsigned char)tb_s144[2] == 109 && (unsigned char)tb_s144[3] == 109 && (unsigned char)tb_s144[4] == 97 && (unsigned char)tb_s144[5] == 110 && (unsigned char)tb_s144[6] == 100 && tb_s144[7] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(v_t, 68))->q_word);
        }
        f_errors__error_at(v_c, f_parser__line_now(v_c), "ожидалось имя, а тут", f_parser__what_is_here(v_c));
    }
    f_parser__next(v_c);
    return ((tb_o_Token *)tb_nn(v_t, 71))->q_word;
    tb_crash(64, "функция expect_name дошла до конца без return");
    return "";
}

static void f_parser__expect_newline(tb_o_Comp * v_c) {
    if (({ const char *tb_s145 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 74))->q_kind; !((unsigned char)tb_s145[0] == 110 && (unsigned char)tb_s145[1] == 101 && (unsigned char)tb_s145[2] == 119 && (unsigned char)tb_s145[3] == 108 && (unsigned char)tb_s145[4] == 105 && (unsigned char)tb_s145[5] == 110 && (unsigned char)tb_s145[6] == 101 && tb_s145[7] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "лишнее в конце строки", f_parser__what_is_here(v_c));
    }
    f_parser__next(v_c);
}

static _Bool f_parser__at_type_word(tb_o_Comp * v_c) {
    return (((((f_parser__is_cmd(v_c, "int") || f_parser__is_cmd(v_c, "dec")) || f_parser__is_cmd(v_c, "text")) || f_parser__is_cmd(v_c, "bool")) || f_parser__is_cmd(v_c, "list")) || f_parser__is_cmd(v_c, "dict"));
    tb_crash(78, "функция at_type_word дошла до конца без return");
    return 0;
}

static tb_o_Node * f_parser__parse_type(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_t = f_parser__new_node("type", f_parser__line_now(v_c));
    if (f_parser__is_cmd(v_c, "dict")) {
        { const char * tb_v146 = "dict"; const char * *tb_p146 = &(((tb_o_Node *)tb_nn(v_t, 86))->q_name); *tb_p146 = tb_v146; }
        f_parser__next(v_c);
        tb_o_Node * v_key = f_parser__parse_type(v_c);
        if ((({ const char *tb_s147 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s147[0] == 116 && (unsigned char)tb_s147[1] == 101 && (unsigned char)tb_s147[2] == 120 && (unsigned char)tb_s147[3] == 116 && tb_s147[4] == 0); }) && ({ const char *tb_s148 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s148[0] == 105 && (unsigned char)tb_s148[1] == 110 && (unsigned char)tb_s148[2] == 116 && tb_s148[3] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_key, 90))->q_line, "ключ у dict бывает text или int", ((tb_o_Node *)tb_nn(v_key, 90))->q_name);
        }
        f_parser__add_kid(v_t, v_key);
        if (({ const char *tb_s149 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 92))->q_kind; ((unsigned char)tb_s149[0] == 110 && (unsigned char)tb_s149[1] == 101 && (unsigned char)tb_s149[2] == 119 && (unsigned char)tb_s149[3] == 108 && (unsigned char)tb_s149[4] == 105 && (unsigned char)tb_s149[5] == 110 && (unsigned char)tb_s149[6] == 101 && tb_s149[7] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после ключа напиши тип значения: dict text int", "");
        }
        f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        return v_t;
    }
    if (f_parser__at_type_word(v_c)) {
        { const char * tb_v150 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 97))->q_word; const char * *tb_p150 = &(((tb_o_Node *)tb_nn(v_t, 97))->q_name); *tb_p150 = tb_v150; }
        f_parser__next(v_c);
        if (({ const char *tb_s151 = ((tb_o_Node *)tb_nn(v_t, 99))->q_name; ((unsigned char)tb_s151[0] == 108 && (unsigned char)tb_s151[1] == 105 && (unsigned char)tb_s151[2] == 115 && (unsigned char)tb_s151[3] == 116 && tb_s151[4] == 0); })) {
            if (((!f_parser__at_type_word(v_c)) && ({ const char *tb_s152 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 100))->q_kind; !((unsigned char)tb_s152[0] == 110 && (unsigned char)tb_s152[1] == 97 && (unsigned char)tb_s152[2] == 109 && (unsigned char)tb_s152[3] == 101 && tb_s152[4] == 0); }))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "после list напиши, что в нём лежит: list int, list text ...", "");
            }
            f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        }
        return v_t;
    }
    if (({ const char *tb_s153 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 104))->q_kind; ((unsigned char)tb_s153[0] == 110 && (unsigned char)tb_s153[1] == 97 && (unsigned char)tb_s153[2] == 109 && (unsigned char)tb_s153[3] == 101 && tb_s153[4] == 0); })) {
        { const char * tb_v154 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 105))->q_word; const char * *tb_p154 = &(((tb_o_Node *)tb_nn(v_t, 105))->q_name); *tb_p154 = tb_v154; }
        f_parser__next(v_c);
        return v_t;
    }
    f_errors__error_at(v_c, f_parser__line_now(v_c), "ожидался тип: int, dec, text, bool, list ... или имя obj, а тут", f_parser__what_is_here(v_c));
    return v_t;
    return v_result;
}

static tb_o_Node * f_parser__binop(const char * v_sign, tb_o_Node * v_left, tb_o_Node * v_right, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("binop", v_line);
    { const char * tb_v155 = v_sign; const char * *tb_p155 = &(((tb_o_Node *)tb_nn(v_n, 115))->q_value); *tb_p155 = tb_v155; }
    f_parser__add_kid(v_n, v_left);
    f_parser__add_kid(v_n, v_right);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_call = f_parser__new_node("call", v_line);
    { const char * tb_v156 = v_name; const char * *tb_p156 = &(((tb_o_Node *)tb_nn(v_call, 122))->q_name); *tb_p156 = tb_v156; }
    f_parser__expect_sign(v_c, "(");
    while ((!f_parser__is_sign(v_c, ")"))) {
        tb_o_Node * v_arg = f_parser__new_node("arg", f_parser__line_now(v_c));
        if ((({ const char *tb_s157 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 127))->q_kind; ((unsigned char)tb_s157[0] == 110 && (unsigned char)tb_s157[1] == 97 && (unsigned char)tb_s157[2] == 109 && (unsigned char)tb_s157[3] == 101 && tb_s157[4] == 0); }) && f_parser__ahead_is_sign(v_c, ":"))) {
            { const char * tb_v158 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 128))->q_word; const char * *tb_p158 = &(((tb_o_Node *)tb_nn(v_arg, 128))->q_name); *tb_p158 = tb_v158; }
            f_parser__next(v_c);
            f_parser__next(v_c);
        }
        f_parser__add_kid(v_arg, f_parser__parse_expr(v_c));
        f_parser__add_kid(v_call, v_arg);
        if ((!f_parser__is_sign(v_c, ","))) {
            break;
        }
        f_parser__next(v_c);
    }
    f_parser__expect_sign(v_c, ")");
    return v_call;
    return v_result;
}

static tb_o_Node * f_parser__parse_primary(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Token * v_t = f_parser__now(v_c);
    long long v_line = ((tb_o_Token *)tb_nn(v_t, 141))->q_line;
    tb_o_Node * v_n = f_parser__new_node("missing", v_line);
    if (((({ const char *tb_s159 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s159[0] == 110 && (unsigned char)tb_s159[1] == 117 && (unsigned char)tb_s159[2] == 109 && (unsigned char)tb_s159[3] == 98 && (unsigned char)tb_s159[4] == 101 && (unsigned char)tb_s159[5] == 114 && tb_s159[6] == 0); }) || ({ const char *tb_s160 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s160[0] == 100 && (unsigned char)tb_s160[1] == 101 && (unsigned char)tb_s160[2] == 99 && (unsigned char)tb_s160[3] == 105 && (unsigned char)tb_s160[4] == 109 && (unsigned char)tb_s160[5] == 97 && (unsigned char)tb_s160[6] == 108 && tb_s160[7] == 0); })) || ({ const char *tb_s161 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s161[0] == 115 && (unsigned char)tb_s161[1] == 116 && (unsigned char)tb_s161[2] == 114 && (unsigned char)tb_s161[3] == 105 && (unsigned char)tb_s161[4] == 110 && (unsigned char)tb_s161[5] == 103 && tb_s161[6] == 0); }))) {
        v_n = f_parser__new_node(((tb_o_Token *)tb_nn(v_t, 144))->q_kind, v_line);
        { const char * tb_v162 = ((tb_o_Token *)tb_nn(v_t, 145))->q_word; const char * *tb_p162 = &(((tb_o_Node *)tb_nn(v_n, 145))->q_value); *tb_p162 = tb_v162; }
        f_parser__next(v_c);
    } else if ((f_parser__is_cmd(v_c, "true") || f_parser__is_cmd(v_c, "false"))) {
        v_n = f_parser__new_node("bool", v_line);
        { const char * tb_v163 = ((tb_o_Token *)tb_nn(v_t, 149))->q_word; const char * *tb_p163 = &(((tb_o_Node *)tb_nn(v_n, 149))->q_value); *tb_p163 = tb_v163; }
        f_parser__next(v_c);
    } else if (({ const char *tb_s164 = ((tb_o_Token *)tb_nn(v_t, 151))->q_kind; ((unsigned char)tb_s164[0] == 110 && (unsigned char)tb_s164[1] == 97 && (unsigned char)tb_s164[2] == 109 && (unsigned char)tb_s164[3] == 101 && tb_s164[4] == 0); })) {
        f_parser__next(v_c);
        if (f_parser__is_sign(v_c, "(")) {
            v_n = f_parser__parse_call(v_c, ((tb_o_Token *)tb_nn(v_t, 154))->q_word, v_line);
        } else {
            v_n = f_parser__new_node("name", v_line);
            { const char * tb_v165 = ((tb_o_Token *)tb_nn(v_t, 157))->q_word; const char * *tb_p165 = &(((tb_o_Node *)tb_nn(v_n, 157))->q_name); *tb_p165 = tb_v165; }
        }
    } else if (f_parser__is_sign(v_c, "(")) {
        f_parser__next(v_c);
        v_n = f_parser__parse_expr(v_c);
        f_parser__expect_sign(v_c, ")");
    } else if (f_parser__is_sign(v_c, "{")) {
        f_parser__next(v_c);
        v_n = f_parser__new_node("dict", v_line);
        while ((!f_parser__is_sign(v_c, "}"))) {
            f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
            f_parser__expect_sign(v_c, ":");
            f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
            if ((!f_parser__is_sign(v_c, ","))) {
                break;
            }
            f_parser__next(v_c);
        }
        f_parser__expect_sign(v_c, "}");
    } else if (f_parser__is_sign(v_c, "[")) {
        f_parser__next(v_c);
        v_n = f_parser__new_node("list", v_line);
        while ((!f_parser__is_sign(v_c, "]"))) {
            f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
            if ((!f_parser__is_sign(v_c, ","))) {
                break;
            }
            f_parser__next(v_c);
        }
        f_parser__expect_sign(v_c, "]");
    } else {
        f_errors__error_at(v_c, v_line, "ожидалось значение, а тут", f_parser__what_is_here(v_c));
    }
    while ((f_parser__is_sign(v_c, "[") || f_parser__is_sign(v_c, "."))) {
        if (f_parser__is_sign(v_c, "[")) {
            tb_o_Node * v_index = f_parser__new_node("index", f_parser__line_now(v_c));
            f_parser__next(v_c);
            f_parser__add_kid(v_index, v_n);
            f_parser__add_kid(v_index, f_parser__parse_expr(v_c));
            f_parser__expect_sign(v_c, "]");
            v_n = v_index;
        } else {
            long long v_dot_line = f_parser__line_now(v_c);
            f_parser__next(v_c);
            const char * v_name = f_parser__expect_name(v_c);
            if (f_parser__is_sign(v_c, "(")) {
                tb_o_Node * v_call = f_parser__parse_call(v_c, v_name, v_dot_line);
                tb_o_Node * v_m = f_parser__new_node("method", v_dot_line);
                { const char * tb_v166 = v_name; const char * *tb_p166 = &(((tb_o_Node *)tb_nn(v_m, 201))->q_name); *tb_p166 = tb_v166; }
                f_parser__add_kid(v_m, v_n);
                {
                    tb_list * tb_l167 = ((tb_o_Node *)tb_nn(v_call, 203))->q_kids;
                    for (long long tb_i167 = 0; tb_i167 < tb_l167->len; tb_i167++) {
                        tb_o_Node * const v_k = ((tb_o_Node * *)tb_l167->data)[tb_i167];
                        f_parser__add_kid(v_m, v_k);
                    }
                }
                v_n = v_m;
            } else {
                tb_o_Node * v_f = f_parser__new_node("field", v_dot_line);
                { const char * tb_v168 = v_name; const char * *tb_p168 = &(((tb_o_Node *)tb_nn(v_f, 208))->q_name); *tb_p168 = tb_v168; }
                f_parser__add_kid(v_f, v_n);
                v_n = v_f;
            }
        }
    }
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_unary(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    if (f_parser__is_sign(v_c, "-")) {
        tb_o_Node * v_n = f_parser__new_node("unary", f_parser__line_now(v_c));
        { const char * tb_v169 = "-"; const char * *tb_p169 = &(((tb_o_Node *)tb_nn(v_n, 216))->q_value); *tb_p169 = tb_v169; }
        f_parser__next(v_c);
        f_parser__add_kid(v_n, f_parser__parse_unary(v_c));
        return v_n;
    }
    return f_parser__parse_primary(v_c);
    return v_result;
}

static tb_o_Node * f_parser__parse_mul(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_left = f_parser__parse_unary(v_c);
    while (((f_parser__is_sign(v_c, "*") || f_parser__is_sign(v_c, "/")) || f_parser__is_sign(v_c, "%"))) {
        const char * v_sign = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 225))->q_word;
        long long v_line = f_parser__line_now(v_c);
        f_parser__next(v_c);
        v_left = f_parser__binop(v_sign, v_left, f_parser__parse_unary(v_c), v_line);
    }
    return v_left;
    return v_result;
}

static tb_o_Node * f_parser__parse_add(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_left = f_parser__parse_mul(v_c);
    while ((f_parser__is_sign(v_c, "+") || f_parser__is_sign(v_c, "-"))) {
        const char * v_sign = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 234))->q_word;
        long long v_line = f_parser__line_now(v_c);
        f_parser__next(v_c);
        v_left = f_parser__binop(v_sign, v_left, f_parser__parse_mul(v_c), v_line);
    }
    return v_left;
    return v_result;
}

static tb_o_Node * f_parser__parse_compare(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_left = f_parser__parse_add(v_c);
    if ((((((f_parser__is_sign(v_c, "==") || f_parser__is_sign(v_c, "!=")) || f_parser__is_sign(v_c, "<")) || f_parser__is_sign(v_c, ">")) || f_parser__is_sign(v_c, "<=")) || f_parser__is_sign(v_c, ">="))) {
        const char * v_sign = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 243))->q_word;
        long long v_line = f_parser__line_now(v_c);
        f_parser__next(v_c);
        v_left = f_parser__binop(v_sign, v_left, f_parser__parse_add(v_c), v_line);
    }
    if (f_parser__is_sign(v_c, "=")) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "в условии сравнение пишется двумя знаками", "==");
    }
    return v_left;
    return v_result;
}

static tb_o_Node * f_parser__parse_not(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    if (f_parser__is_cmd(v_c, "not")) {
        tb_o_Node * v_n = f_parser__new_node("unary", f_parser__line_now(v_c));
        { const char * tb_v170 = "not"; const char * *tb_p170 = &(((tb_o_Node *)tb_nn(v_n, 254))->q_value); *tb_p170 = tb_v170; }
        f_parser__next(v_c);
        f_parser__add_kid(v_n, f_parser__parse_not(v_c));
        return v_n;
    }
    return f_parser__parse_compare(v_c);
    return v_result;
}

static tb_o_Node * f_parser__parse_and(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_left = f_parser__parse_not(v_c);
    while (f_parser__is_cmd(v_c, "and")) {
        long long v_line = f_parser__line_now(v_c);
        f_parser__next(v_c);
        v_left = f_parser__binop("and", v_left, f_parser__parse_not(v_c), v_line);
    }
    return v_left;
    return v_result;
}

static tb_o_Node * f_parser__parse_expr(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_left = f_parser__parse_and(v_c);
    while (f_parser__is_cmd(v_c, "or")) {
        long long v_line = f_parser__line_now(v_c);
        f_parser__next(v_c);
        v_left = f_parser__binop("or", v_left, f_parser__parse_and(v_c), v_line);
    }
    return v_left;
    return v_result;
}

static tb_o_Node * f_parser__parse_if(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("if", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "if");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__expect_newline(v_c);
    f_parser__add_kid(v_n, f_parser__parse_block(v_c));
    while (f_parser__is_cmd(v_c, "else")) {
        f_parser__next(v_c);
        if (f_parser__is_cmd(v_c, "if")) {
            f_parser__next(v_c);
            f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_n, f_parser__parse_block(v_c));
        } else {
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_n, f_parser__parse_block(v_c));
            break;
        }
    }
    return v_n;
    return v_result;
}

static void f_parser__parse_on_all_cores(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Token * v_t = f_parser__now(v_c);
    if ((({ const char *tb_s171 = ((tb_o_Token *)tb_nn(v_t, 300))->q_kind; ((unsigned char)tb_s171[0] == 110 && (unsigned char)tb_s171[1] == 97 && (unsigned char)tb_s171[2] == 109 && (unsigned char)tb_s171[3] == 101 && tb_s171[4] == 0); }) && ({ const char *tb_s172 = ((tb_o_Token *)tb_nn(v_t, 300))->q_word; ((unsigned char)tb_s172[0] == 111 && (unsigned char)tb_s172[1] == 110 && tb_s172[2] == 0); }))) {
        f_parser__next(v_c);
        tb_o_Token * v_a = f_parser__now(v_c);
        tb_o_Token * v_b = f_parser__ahead(v_c);
        if ((({ const char *tb_s173 = ((tb_o_Token *)tb_nn(v_a, 304))->q_word; !((unsigned char)tb_s173[0] == 97 && (unsigned char)tb_s173[1] == 108 && (unsigned char)tb_s173[2] == 108 && tb_s173[3] == 0); }) || ({ const char *tb_s174 = ((tb_o_Token *)tb_nn(v_b, 304))->q_word; !((unsigned char)tb_s174[0] == 99 && (unsigned char)tb_s174[1] == 111 && (unsigned char)tb_s174[2] == 114 && (unsigned char)tb_s174[3] == 101 && (unsigned char)tb_s174[4] == 115 && tb_s174[5] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после on пишется all cores: each x in nums on all cores", "");
        }
        f_parser__next(v_c);
        f_parser__next(v_c);
        { _Bool tb_v175 = 1; _Bool *tb_p175 = &(((tb_o_Node *)tb_nn(v_n, 308))->q_is_par); *tb_p175 = tb_v175; }
    }
}

static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("each", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "each");
    { const char * tb_v176 = f_parser__expect_name(v_c); const char * *tb_p176 = &(((tb_o_Node *)tb_nn(v_n, 313))->q_name); *tb_p176 = tb_v176; }
    f_parser__expect_cmd(v_c, "in");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s177 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_kind; ((unsigned char)tb_s177[0] == 110 && (unsigned char)tb_s177[1] == 97 && (unsigned char)tb_s177[2] == 109 && (unsigned char)tb_s177[3] == 101 && tb_s177[4] == 0); }) && ({ const char *tb_s178 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_word; ((unsigned char)tb_s178[0] == 118 && (unsigned char)tb_s178[1] == 101 && (unsigned char)tb_s178[2] == 99 && tb_s178[3] == 0); }))) {
            f_parser__next(v_c);
            { _Bool tb_v179 = 1; _Bool *tb_p179 = &(((tb_o_Node *)tb_nn(v_n, 323))->q_by_vec); *tb_p179 = tb_v179; }
            if (((tb_o_Node *)tb_nn(v_n, 324))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_line, "on all cores и by vec вместе пока нельзя", "");
            }
        } else {
            { tb_o_Node * tb_v180 = f_parser__parse_expr(v_c); tb_o_Node * *tb_p180 = &(((tb_o_Node *)tb_nn(v_n, 328))->q_piece_size); *tb_p180 = tb_v180; }
            { _Bool tb_v181 = 1; _Bool *tb_p181 = &(((tb_o_Node *)tb_nn(v_n, 329))->q_by_piece); *tb_p181 = tb_v181; }
            if (((tb_o_Node *)tb_nn(v_n, 330))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 331))->q_line, "on all cores и by N (куски) вместе пока нельзя", "");
            }
        }
    }
    f_parser__expect_newline(v_c);
    f_parser__add_kid(v_n, f_parser__parse_block(v_c));
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_count(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("count", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "count");
    { const char * tb_v182 = f_parser__expect_name(v_c); const char * *tb_p182 = &(((tb_o_Node *)tb_nn(v_n, 339))->q_name); *tb_p182 = tb_v182; }
    f_parser__expect_cmd(v_c, "from");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__expect_cmd(v_c, "to");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    f_parser__expect_newline(v_c);
    f_parser__add_kid(v_n, f_parser__parse_block(v_c));
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_repeat(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    long long v_line = f_parser__line_now(v_c);
    f_parser__expect_cmd(v_c, "repeat");
    tb_o_Node * v_n = f_parser__new_node("repeat_times", v_line);
    if (f_parser__is_cmd(v_c, "while")) {
        f_parser__next(v_c);
        v_n = f_parser__new_node("repeat_while", v_line);
        f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    } else {
        f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
        f_parser__expect_cmd(v_c, "times");
    }
    f_parser__expect_newline(v_c);
    f_parser__add_kid(v_n, f_parser__parse_block(v_c));
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_value(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    if (f_parser__is_cmd(v_c, "background")) {
        tb_o_Node * v_b = f_parser__new_node("background", f_parser__line_now(v_c));
        f_parser__next(v_c);
        tb_o_Node * v_call = f_parser__parse_expr(v_c);
        if ((({ const char *tb_s183 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s183[0] == 99 && (unsigned char)tb_s183[1] == 97 && (unsigned char)tb_s183[2] == 108 && (unsigned char)tb_s183[3] == 108 && tb_s183[4] == 0); }) && ({ const char *tb_s184 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s184[0] == 109 && (unsigned char)tb_s184[1] == 101 && (unsigned char)tb_s184[2] == 116 && (unsigned char)tb_s184[3] == 104 && (unsigned char)tb_s184[4] == 111 && (unsigned char)tb_s184[5] == 100 && tb_s184[6] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_b, 372))->q_line, "после background пишется вызов функции: a = background load(name)", "");
        }
        f_parser__add_kid(v_b, v_call);
        return v_b;
    }
    if (f_parser__is_cmd(v_c, "try")) {
        tb_o_Node * v_t = f_parser__new_node("try", f_parser__line_now(v_c));
        f_parser__next(v_c);
        f_parser__add_kid(v_t, f_parser__parse_expr(v_c));
        return v_t;
    }
    tb_o_Node * v_v = f_parser__parse_expr(v_c);
    if (f_parser__is_cmd(v_c, "else")) {
        tb_o_Node * v_e = f_parser__new_node("else_err", f_parser__line_now(v_c));
        f_parser__next(v_c);
        { const char * tb_v185 = f_parser__expect_name(v_c); const char * *tb_p185 = &(((tb_o_Node *)tb_nn(v_e, 384))->q_name); *tb_p185 = tb_v185; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_v);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        return v_e;
    }
    return v_v;
    return v_result;
}

static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value) {
    if (({ const char *tb_s186 = ((tb_o_Node *)tb_nn(v_value, 393))->q_kind; !((unsigned char)tb_s186[0] == 101 && (unsigned char)tb_s186[1] == 108 && (unsigned char)tb_s186[2] == 115 && (unsigned char)tb_s186[3] == 101 && (unsigned char)tb_s186[4] == 95 && (unsigned char)tb_s186[5] == 101 && (unsigned char)tb_s186[6] == 114 && (unsigned char)tb_s186[7] == 114 && tb_s186[8] == 0); })) {
        f_parser__expect_newline(v_c);
    }
}

static _Bool f_parser__is_change_sign(tb_o_Comp * v_c) {
    return ((((f_parser__is_sign(v_c, "=") || f_parser__is_sign(v_c, "+=")) || f_parser__is_sign(v_c, "-=")) || f_parser__is_sign(v_c, "*=")) || f_parser__is_sign(v_c, "/="));
    tb_crash(396, "функция is_change_sign дошла до конца без return");
    return 0;
}

static tb_o_Node * f_parser__parse_assign(tb_o_Comp * v_c, _Bool v_is_const) {
    tb_o_Node * v_result = 0;
    long long v_line = f_parser__line_now(v_c);
    tb_o_Node * v_n = f_parser__new_node("assign", v_line);
    { const char * tb_v187 = f_parser__expect_name(v_c); const char * *tb_p187 = &(((tb_o_Node *)tb_nn(v_n, 403))->q_name); *tb_p187 = tb_v187; }
    { _Bool tb_v188 = v_is_const; _Bool *tb_p188 = &(((tb_o_Node *)tb_nn(v_n, 404))->q_is_const); *tb_p188 = tb_v188; }
    f_parser__expect_sign(v_c, "=");
    if ((f_parser__at_type_word(v_c) || f_parser__is_cmd(v_c, "dict"))) {
        { tb_o_Node * tb_v189 = f_parser__parse_type(v_c); tb_o_Node * *tb_p189 = &(((tb_o_Node *)tb_nn(v_n, 407))->q_tsyn); *tb_p189 = tb_v189; }
        { _Bool tb_v190 = 1; _Bool *tb_p190 = &(((tb_o_Node *)tb_nn(v_n, 408))->q_has_tsyn); *tb_p190 = tb_v190; }
    }
    if (({ const char *tb_s191 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 409))->q_kind; !((unsigned char)tb_s191[0] == 110 && (unsigned char)tb_s191[1] == 101 && (unsigned char)tb_s191[2] == 119 && (unsigned char)tb_s191[3] == 108 && (unsigned char)tb_s191[4] == 105 && (unsigned char)tb_s191[5] == 110 && (unsigned char)tb_s191[6] == 101 && tb_s191[7] == 0); })) {
        f_parser__add_kid(v_n, f_parser__parse_value(v_c));
    } else if ((!((tb_o_Node *)tb_nn(v_n, 411))->q_has_tsyn)) {
        f_errors__error_at(v_c, v_line, "после = нужно значение", "");
    }
    if ((v_is_const && ((((tb_o_Node *)tb_nn(v_n, 413))->q_kids)->len == 0LL))) {
        f_errors__error_at(v_c, v_line, "const нужно сразу дать значение", ((tb_o_Node *)tb_nn(v_n, 414))->q_name);
    }
    if (((((tb_o_Node *)tb_nn(v_n, 415))->q_kids)->len == 1LL)) {
        f_parser__end_statement(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 416))->q_kids, 0LL, sizeof(tb_o_Node *), 416)));
    } else {
        f_parser__expect_newline(v_c);
    }
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_linkdecl(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("linkdecl", f_parser__line_now(v_c));
    { const char * tb_v192 = f_parser__expect_name(v_c); const char * *tb_p192 = &(((tb_o_Node *)tb_nn(v_n, 424))->q_name); *tb_p192 = tb_v192; }
    f_parser__expect_sign(v_c, ":");
    if ((!f_parser__is_cmd(v_c, "link"))) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после двоеточия в строке ожидалось link (ярлык)", f_parser__what_is_here(v_c));
    }
    f_parser__next(v_c);
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__expect_newline(v_c);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_multi(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("multi", f_parser__line_now(v_c));
    while (1) {
        tb_o_Node * v_name = f_parser__new_node("name", f_parser__line_now(v_c));
        { const char * tb_v193 = f_parser__expect_name(v_c); const char * *tb_p193 = &(((tb_o_Node *)tb_nn(v_name, 438))->q_name); *tb_p193 = tb_v193; }
        f_parser__add_kid(v_n, v_name);
        if ((!f_parser__is_sign(v_c, ","))) {
            break;
        }
        f_parser__next(v_c);
    }
    f_parser__expect_sign(v_c, "=");
    tb_o_Node * v_value = f_parser__parse_value(v_c);
    f_parser__add_kid(v_n, v_value);
    f_parser__end_statement(v_c, v_value);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_name_statement(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    long long v_line = f_parser__line_now(v_c);
    if (f_parser__ahead_is_sign(v_c, "=")) {
        return f_parser__parse_assign(v_c, 0);
    }
    if (f_parser__ahead_is_sign(v_c, ":")) {
        return f_parser__parse_linkdecl(v_c);
    }
    if (f_parser__ahead_is_sign(v_c, ",")) {
        return f_parser__parse_multi(v_c);
    }
    tb_o_Node * v_target = f_parser__parse_primary(v_c);
    if (f_parser__is_change_sign(v_c)) {
        const char * v_k = ((tb_o_Node *)tb_nn(v_target, 460))->q_kind;
        if (((({ const char *tb_s194 = v_k; !((unsigned char)tb_s194[0] == 110 && (unsigned char)tb_s194[1] == 97 && (unsigned char)tb_s194[2] == 109 && (unsigned char)tb_s194[3] == 101 && tb_s194[4] == 0); }) && ({ const char *tb_s195 = v_k; !((unsigned char)tb_s195[0] == 105 && (unsigned char)tb_s195[1] == 110 && (unsigned char)tb_s195[2] == 100 && (unsigned char)tb_s195[3] == 101 && (unsigned char)tb_s195[4] == 120 && tb_s195[5] == 0); })) && ({ const char *tb_s196 = v_k; !((unsigned char)tb_s196[0] == 102 && (unsigned char)tb_s196[1] == 105 && (unsigned char)tb_s196[2] == 101 && (unsigned char)tb_s196[3] == 108 && (unsigned char)tb_s196[4] == 100 && tb_s196[5] == 0); }))) {
            f_errors__error_at(v_c, v_line, "слева от знака должно быть имя, nums[i] или u.age", "");
        }
        tb_o_Node * v_n = f_parser__new_node("set", v_line);
        { const char * tb_v197 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 464))->q_word; const char * *tb_p197 = &(((tb_o_Node *)tb_nn(v_n, 464))->q_value); *tb_p197 = tb_v197; }
        f_parser__next(v_c);
        f_parser__add_kid(v_n, v_target);
        tb_o_Node * v_value = f_parser__parse_value(v_c);
        f_parser__add_kid(v_n, v_value);
        f_parser__end_statement(v_c, v_value);
        return v_n;
    }
    tb_o_Node * v_n = f_parser__new_node("expr_stmt", v_line);
    if (f_parser__is_cmd(v_c, "else")) {
        tb_o_Node * v_e = f_parser__new_node("else_err", f_parser__line_now(v_c));
        f_parser__next(v_c);
        { const char * tb_v198 = f_parser__expect_name(v_c); const char * *tb_p198 = &(((tb_o_Node *)tb_nn(v_e, 475))->q_name); *tb_p198 = tb_v198; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_target);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        f_parser__add_kid(v_n, v_e);
        return v_n;
    }
    f_parser__add_kid(v_n, v_target);
    f_parser__expect_newline(v_c);
    return v_n;
    return v_result;
}

static _Bool f_parser__later_command(tb_o_Comp * v_c) {
    {
        tb_list * tb_l199 = tb_list_from(4, sizeof(const char *), (const char *[]){"gives", "obj", "danger", "by"});
        for (long long tb_i199 = 0; tb_i199 < tb_l199->len; tb_i199++) {
            const char * const v_w = ((const char * *)tb_l199->data)[tb_i199];
            if (f_parser__is_cmd(v_c, v_w)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(485, "функция later_command дошла до конца без return");
    return 0;
}

static tb_o_Node * f_parser__parse_statement(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    long long v_line = f_parser__line_now(v_c);
    if (f_parser__is_cmd(v_c, "if")) {
        return f_parser__parse_if(v_c);
    }
    if (f_parser__is_cmd(v_c, "each")) {
        return f_parser__parse_each(v_c);
    }
    if (f_parser__is_cmd(v_c, "count")) {
        return f_parser__parse_count(v_c);
    }
    if (f_parser__is_cmd(v_c, "repeat")) {
        return f_parser__parse_repeat(v_c);
    }
    if ((f_parser__is_cmd(v_c, "skip") || f_parser__is_cmd(v_c, "stop"))) {
        tb_o_Node * v_n = f_parser__new_node(((tb_o_Token *)tb_nn(f_parser__now(v_c), 502))->q_word, v_line);
        f_parser__next(v_c);
        f_parser__expect_newline(v_c);
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "return")) {
        tb_o_Node * v_n = f_parser__new_node("return", v_line);
        f_parser__next(v_c);
        if (({ const char *tb_s200 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 509))->q_kind; !((unsigned char)tb_s200[0] == 110 && (unsigned char)tb_s200[1] == 101 && (unsigned char)tb_s200[2] == 119 && (unsigned char)tb_s200[3] == 108 && (unsigned char)tb_s200[4] == 105 && (unsigned char)tb_s200[5] == 110 && (unsigned char)tb_s200[6] == 101 && tb_s200[7] == 0); })) {
            tb_o_Node * v_value = f_parser__parse_value(v_c);
            f_parser__add_kid(v_n, v_value);
            f_parser__end_statement(v_c, v_value);
        } else {
            f_parser__expect_newline(v_c);
        }
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "fail")) {
        tb_o_Node * v_n = f_parser__new_node("fail", v_line);
        f_parser__next(v_c);
        f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
        f_parser__expect_newline(v_c);
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "try")) {
        tb_o_Node * v_n = f_parser__new_node("expr_stmt", v_line);
        f_parser__add_kid(v_n, f_parser__parse_value(v_c));
        f_parser__expect_newline(v_c);
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "const")) {
        f_parser__next(v_c);
        return f_parser__parse_assign(v_c, 1);
    }
    if (f_parser__is_cmd(v_c, "together")) {
        tb_o_Node * v_n = f_parser__new_node("together", v_line);
        f_parser__next(v_c);
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_n, f_parser__parse_block(v_c));
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "temp")) {
        tb_o_Node * v_n = f_parser__new_node("temp", v_line);
        f_parser__next(v_c);
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_n, f_parser__parse_block(v_c));
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "shared")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s201 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 547))->q_kind; !((unsigned char)tb_s201[0] == 110 && (unsigned char)tb_s201[1] == 97 && (unsigned char)tb_s201[2] == 109 && (unsigned char)tb_s201[3] == 101 && tb_s201[4] == 0); }) || (!f_parser__ahead_is_sign(v_c, "=")))) {
            f_errors__error_at(v_c, v_line, "после shared пишется новая переменная: shared y = int 0", "");
        }
        tb_o_Node * v_n = f_parser__parse_assign(v_c, 0);
        { _Bool tb_v202 = 1; _Bool *tb_p202 = &(((tb_o_Node *)tb_nn(v_n, 550))->q_is_shared); *tb_p202 = tb_v202; }
        return v_n;
    }
    if (f_parser__is_cmd(v_c, "else")) {
        f_errors__error_at(v_c, v_line, "else без if над ним (проверь отступ)", "");
    }
    if (f_parser__is_cmd(v_c, "func")) {
        f_errors__error_at(v_c, v_line, "функцию нельзя писать внутри другой функции", "");
    }
    if (f_parser__is_cmd(v_c, "link")) {
        f_errors__error_at(v_c, v_line, "link в начале строки подключает файл: это будет в версии 0.3", "");
    }
    if (((f_parser__is_cmd(v_c, "use") || f_parser__is_cmd(v_c, "modpub")) || f_parser__is_cmd(v_c, "modpri"))) {
        f_errors__error_at(v_c, v_line, "use, modpub и modpri пишутся снаружи функций, в начале строки", "");
    }
    if (f_parser__later_command(v_c)) {
        f_errors__error_at(v_c, v_line, "эта команда будет в следующей версии", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 561))->q_word);
    }
    if (({ const char *tb_s203 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 562))->q_kind; ((unsigned char)tb_s203[0] == 110 && (unsigned char)tb_s203[1] == 97 && (unsigned char)tb_s203[2] == 109 && (unsigned char)tb_s203[3] == 101 && tb_s203[4] == 0); })) {
        return f_parser__parse_name_statement(v_c);
    }
    if ((({ const char *tb_s204 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 564))->q_kind; ((unsigned char)tb_s204[0] == 99 && (unsigned char)tb_s204[1] == 111 && (unsigned char)tb_s204[2] == 109 && (unsigned char)tb_s204[3] == 109 && (unsigned char)tb_s204[4] == 97 && (unsigned char)tb_s204[5] == 110 && (unsigned char)tb_s204[6] == 100 && tb_s204[7] == 0); }) && f_parser__ahead_is_sign(v_c, "="))) {
        f_errors__error_at(v_c, v_line, "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 565))->q_word);
    }
    f_errors__error_at(v_c, v_line, "не понимаю, что делает строка, начиная с", f_parser__what_is_here(v_c));
    return f_parser__new_node("missing", v_line);
    return v_result;
}

static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_block = f_parser__new_node("block", f_parser__line_now(v_c));
    if (({ const char *tb_s205 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 571))->q_kind; !((unsigned char)tb_s205[0] == 105 && (unsigned char)tb_s205[1] == 110 && (unsigned char)tb_s205[2] == 100 && (unsigned char)tb_s205[3] == 101 && (unsigned char)tb_s205[4] == 110 && (unsigned char)tb_s205[5] == 116 && tb_s205[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после этой строки нужен блок с отступом", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s206 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s206[0] == 100 && (unsigned char)tb_s206[1] == 101 && (unsigned char)tb_s206[2] == 100 && (unsigned char)tb_s206[3] == 101 && (unsigned char)tb_s206[4] == 110 && (unsigned char)tb_s206[5] == 116 && tb_s206[6] == 0); }) && ({ const char *tb_s207 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s207[0] == 101 && (unsigned char)tb_s207[1] == 110 && (unsigned char)tb_s207[2] == 100 && tb_s207[3] == 0); }))) {
        if (({ const char *tb_s208 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 575))->q_kind; ((unsigned char)tb_s208[0] == 105 && (unsigned char)tb_s208[1] == 110 && (unsigned char)tb_s208[2] == 100 && (unsigned char)tb_s208[3] == 101 && (unsigned char)tb_s208[4] == 110 && (unsigned char)tb_s208[5] == 116 && tb_s208[6] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "лишний отступ", "");
        }
        f_parser__add_kid(v_block, f_parser__parse_statement(v_c));
    }
    f_parser__next(v_c);
    return v_block;
    return v_result;
}

static tb_o_Node * f_parser__parse_param(tb_o_Comp * v_c, _Bool v_allow_link) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_p = f_parser__new_node("param", f_parser__line_now(v_c));
    if (f_parser__is_cmd(v_c, "link")) {
        if ((!v_allow_link)) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "link здесь нельзя", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v209 = 1; _Bool *tb_p209 = &(((tb_o_Node *)tb_nn(v_p, 588))->q_is_link); *tb_p209 = tb_v209; }
        { const char * tb_v210 = f_parser__expect_name(v_c); const char * *tb_p210 = &(((tb_o_Node *)tb_nn(v_p, 589))->q_name); *tb_p210 = tb_v210; }
        return v_p;
    }
    { const char * tb_v211 = f_parser__expect_name(v_c); const char * *tb_p211 = &(((tb_o_Node *)tb_nn(v_p, 591))->q_name); *tb_p211 = tb_v211; }
    f_parser__expect_sign(v_c, ":");
    { tb_o_Node * tb_v212 = f_parser__parse_type(v_c); tb_o_Node * *tb_p212 = &(((tb_o_Node *)tb_nn(v_p, 593))->q_tsyn); *tb_p212 = tb_v212; }
    { _Bool tb_v213 = 1; _Bool *tb_p213 = &(((tb_o_Node *)tb_nn(v_p, 594))->q_has_tsyn); *tb_p213 = tb_v213; }
    return v_p;
    return v_result;
}

static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_f = f_parser__new_node("func", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "func");
    { const char * tb_v214 = f_parser__expect_name(v_c); const char * *tb_p214 = &(((tb_o_Node *)tb_nn(v_f, 601))->q_name); *tb_p214 = tb_v214; }
    f_parser__expect_sign(v_c, "(");
    tb_o_Node * v_params = f_parser__new_node("block", f_parser__line_now(v_c));
    while ((!f_parser__is_sign(v_c, ")"))) {
        f_parser__add_kid(v_params, f_parser__parse_param(v_c, 1));
        if ((!f_parser__is_sign(v_c, ","))) {
            break;
        }
        f_parser__next(v_c);
    }
    f_parser__expect_sign(v_c, ")");
    tb_o_Node * v_outputs = f_parser__new_node("block", f_parser__line_now(v_c));
    if (f_parser__is_cmd(v_c, "gives")) {
        f_parser__next(v_c);
        while (1) {
            f_parser__add_kid(v_outputs, f_parser__parse_param(v_c, 0));
            if ((!f_parser__is_sign(v_c, ","))) {
                break;
            }
            f_parser__next(v_c);
        }
    }
    f_parser__expect_newline(v_c);
    f_parser__add_kid(v_f, v_params);
    f_parser__add_kid(v_f, f_parser__parse_block(v_c));
    f_parser__add_kid(v_f, v_outputs);
    return v_f;
    return v_result;
}

static tb_o_Node * f_parser__parse_obj(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_o = f_parser__new_node("obj", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "obj");
    { const char * tb_v215 = f_parser__expect_name(v_c); const char * *tb_p215 = &(((tb_o_Node *)tb_nn(v_o, 630))->q_name); *tb_p215 = tb_v215; }
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s216 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_kind; !((unsigned char)tb_s216[0] == 110 && (unsigned char)tb_s216[1] == 97 && (unsigned char)tb_s216[2] == 109 && (unsigned char)tb_s216[3] == 101 && tb_s216[4] == 0); }) || ({ const char *tb_s217 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_word; !((unsigned char)tb_s217[0] == 99 && (unsigned char)tb_s217[1] == 111 && (unsigned char)tb_s217[2] == 108 && (unsigned char)tb_s217[3] == 117 && (unsigned char)tb_s217[4] == 109 && (unsigned char)tb_s217[5] == 110 && (unsigned char)tb_s217[6] == 115 && tb_s217[7] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после obj ... by пишется columns: obj Point by columns", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v218 = 1; _Bool *tb_p218 = &(((tb_o_Node *)tb_nn(v_o, 637))->q_by_cols); *tb_p218 = tb_v218; }
    }
    f_parser__expect_newline(v_c);
    if (({ const char *tb_s219 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 639))->q_kind; !((unsigned char)tb_s219[0] == 105 && (unsigned char)tb_s219[1] == 110 && (unsigned char)tb_s219[2] == 100 && (unsigned char)tb_s219[3] == 101 && (unsigned char)tb_s219[4] == 110 && (unsigned char)tb_s219[5] == 116 && tb_s219[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "у obj ниже с отступом пишутся качества: name: text", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s220 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s220[0] == 100 && (unsigned char)tb_s220[1] == 101 && (unsigned char)tb_s220[2] == 100 && (unsigned char)tb_s220[3] == 101 && (unsigned char)tb_s220[4] == 110 && (unsigned char)tb_s220[5] == 116 && tb_s220[6] == 0); }) && ({ const char *tb_s221 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s221[0] == 101 && (unsigned char)tb_s221[1] == 110 && (unsigned char)tb_s221[2] == 100 && tb_s221[3] == 0); }))) {
        f_parser__add_kid(v_o, f_parser__parse_param(v_c, 0));
        f_parser__expect_newline(v_c);
    }
    f_parser__next(v_c);
    return v_o;
    return v_result;
}

static tb_o_Node * f_parser__parse(tb_o_Comp * v_c) {
    { long long tb_v222 = 0LL; long long *tb_p222 = &(((tb_o_Comp *)tb_nn(v_c, 649))->q_pos); *tb_p222 = tb_v222; }
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    while (({ const char *tb_s223 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 651))->q_kind; !((unsigned char)tb_s223[0] == 101 && (unsigned char)tb_s223[1] == 110 && (unsigned char)tb_s223[2] == 100 && tb_s223[3] == 0); })) {
        if (f_parser__is_cmd(v_c, "use")) {
            tb_o_Node * v_u = f_parser__new_node("use", f_parser__line_now(v_c));
            f_parser__next(v_c);
            { const char * tb_v224 = f_parser__expect_name(v_c); const char * *tb_p224 = &(((tb_o_Node *)tb_nn(v_u, 656))->q_name); *tb_p224 = tb_v224; }
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_program, v_u);
        } else if ((f_parser__is_cmd(v_c, "modpub") || f_parser__is_cmd(v_c, "modpri"))) {
            _Bool v_is_pub = f_parser__is_cmd(v_c, "modpub");
            f_parser__next(v_c);
            if ((!f_parser__is_cmd(v_c, "func"))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "modpub и modpri ставятся перед func", "");
            }
            tb_o_Node * v_f = f_parser__parse_func(v_c);
            { _Bool tb_v225 = v_is_pub; _Bool *tb_p225 = &(((tb_o_Node *)tb_nn(v_f, 665))->q_is_pub); *tb_p225 = tb_v225; }
            f_parser__add_kid(v_program, v_f);
        } else if (f_parser__is_cmd(v_c, "func")) {
            f_parser__add_kid(v_program, f_parser__parse_func(v_c));
        } else if (f_parser__is_cmd(v_c, "obj")) {
            f_parser__add_kid(v_program, f_parser__parse_obj(v_c));
        } else if (f_parser__is_cmd(v_c, "link")) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", "");
        } else if (({ const char *tb_s226 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 673))->q_kind; ((unsigned char)tb_s226[0] == 105 && (unsigned char)tb_s226[1] == 110 && (unsigned char)tb_s226[2] == 100 && (unsigned char)tb_s226[3] == 101 && (unsigned char)tb_s226[4] == 110 && (unsigned char)tb_s226[5] == 116 && tb_s226[6] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "лишний отступ", "");
        } else {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "снаружи функций можно писать только use, func и obj", f_parser__what_is_here(v_c));
        }
    }
    return v_program;
    tb_crash(648, "функция parse дошла до конца без return");
    return 0;
}

static const char * f_parser__node_kind_name(const char * v_kind) {
    tb_dict * v_names = tb_dict_from(1, sizeof(const char *), 40, (const char *[]){"program", "func", "param", "block", "obj", "type", "assign", "set", "linkdecl", "multi", "if", "each", "count", "repeat_times", "repeat_while", "skip", "stop", "return", "expr_stmt", "number", "decimal", "string", "bool", "name", "list", "call", "arg", "binop", "unary", "index", "field", "method", "dict", "use", "fail", "try", "else_err", "temp", "together", "background"}, (const char *[]){"ПРОГРАММА", "ФУНКЦИЯ", "ПАРАМЕТР", "БЛОК", "OBJ", "ТИП", "ПРИСВОИТЬ", "ИЗМЕНИТЬ", "ЯРЛЫК", "НЕСКОЛЬКО", "ЕСЛИ", "EACH", "COUNT", "REPEAT TIMES", "REPEAT WHILE", "SKIP", "STOP", "RETURN", "ВЫЗОВ-СТРОКА", "ЧИСЛО", "ДРОБЬ", "ТЕКСТ", "ДА/НЕТ", "ИМЯ", "СПИСОК", "ВЫЗОВ", "АРГУМЕНТ", "ОПЕРАЦИЯ", "ОДИНОЧНАЯ", "ИНДЕКС", "КАЧЕСТВО", "МЕТОД", "СЛОВАРЬ", "USE", "FAIL", "TRY", "ELSE ERR", "TEMP", "TOGETHER", "BACKGROUND"});
    return (*(const char * *)tb_dget(v_names, &(const char *){v_kind}, 683));
    tb_crash(681, "функция node_kind_name дошла до конца без return");
    return "";
}

static void f_parser__tree_lines(tb_o_Node * v_n, long long v_depth, tb_list * v_out) {
    long long tb_rl_s = -1, tb_rc_s = 0; (void)tb_rl_s; (void)tb_rc_s;
    const char * v_s = ""; tb_rl_s = -1;
    {
        long long tb_end227 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end227; v_i++) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "  ", sizeof("  ") - 1); 
        }
    }
    tb_app(&v_s, &tb_rl_s, &tb_rc_s, f_parser__node_kind_name(((tb_o_Node *)tb_nn(v_n, 689))->q_kind)); 
    if (({ const char *tb_s228 = ((tb_o_Node *)tb_nn(v_n, 690))->q_name; !(tb_s228[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " ", sizeof(" ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, ((tb_o_Node *)tb_nn(v_n, 691))->q_name); 
    }
    if (({ const char *tb_s229 = ((tb_o_Node *)tb_nn(v_n, 692))->q_value; !(tb_s229[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " ", sizeof(" ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, ((tb_o_Node *)tb_nn(v_n, 693))->q_value); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 694))->q_is_const) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (const)", sizeof(" (const)") - 1); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 696))->q_is_link) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (link)", sizeof(" (link)") - 1); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 698))->q_is_shared) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (shared)", sizeof(" (shared)") - 1); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 700))->q_is_par) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (on all cores)", sizeof(" (on all cores)") - 1); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 702))->q_by_cols) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (by columns)", sizeof(" (by columns)") - 1); 
    }
    if (((tb_o_Node *)tb_nn(v_n, 704))->q_by_vec) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " (by vec)", sizeof(" (by vec)") - 1); 
    }
    { const char * tb_v230 = (tb_rl_s = -1, v_s); *(const char * *)tb_grow(v_out) = tb_v230; }
    if (((tb_o_Node *)tb_nn(v_n, 707))->q_has_tsyn) {
        f_parser__tree_lines(((tb_o_Node *)tb_nn(v_n, 708))->q_tsyn, (v_depth + 1LL), v_out);
    }
    {
        tb_list * tb_l231 = ((tb_o_Node *)tb_nn(v_n, 709))->q_kids;
        for (long long tb_i231 = 0; tb_i231 < tb_l231->len; tb_i231++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l231->data)[tb_i231];
            f_parser__tree_lines(v_k, (v_depth + 1LL), v_out);
        }
    }
}

static long long f_check__make_type_key(tb_o_Comp * v_c, const char * v_kind, long long v_key, long long v_elem, tb_o_Node * v_onode, _Bool v_has_obj, tb_o_Node * v_fnode, _Bool v_has_func) {
    long long v_r = 0LL;
    const char * v_obj_name = "";
    if (v_has_obj) {
        v_obj_name = ((tb_o_Node *)tb_nn(v_onode, 14))->q_name;
    }
    const char * v_func_key = "";
    if (v_has_func) {
        v_func_key = ({ const char *tb_c232_0 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_mod; long long tb_c232_0n = (long long)strlen(tb_c232_0); const char *tb_c232_1 = "."; long long tb_c232_1n = sizeof(".") - 1; const char *tb_c232_2 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_name; long long tb_c232_2n = (long long)strlen(tb_c232_2); char *tb_c232r = tb_mem(tb_c232_0n + tb_c232_1n + tb_c232_2n + 1), *tb_c232rw = tb_c232r; memcpy(tb_c232rw, tb_c232_0, tb_c232_0n); tb_c232rw += tb_c232_0n; memcpy(tb_c232rw, tb_c232_1, tb_c232_1n); tb_c232rw += tb_c232_1n; memcpy(tb_c232rw, tb_c232_2, tb_c232_2n); tb_c232rw += tb_c232_2n; *tb_c232rw = 0; (const char *)tb_c232r; });
    }
    {
        tb_list * tb_l233 = ((tb_o_Comp *)tb_nn(v_c, 18))->q_types;
        for (long long tb_i233 = 0; tb_i233 < tb_l233->len; tb_i233++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l233->data)[tb_i233];
            if ((((((strcmp(((tb_o_Type *)tb_nn(v_t, 19))->q_kind, v_kind) == 0) && (((tb_o_Type *)tb_nn(v_t, 19))->q_key == v_key)) && (((tb_o_Type *)tb_nn(v_t, 19))->q_elem == v_elem)) && (strcmp(((tb_o_Type *)tb_nn(v_t, 19))->q_obj_name, v_obj_name) == 0)) && (strcmp(((tb_o_Type *)tb_nn(v_t, 19))->q_func_key, v_func_key) == 0))) {
                return ((tb_o_Type *)tb_nn(v_t, 20))->q_id;
            }
        }
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 21))->q_types)->len == 1000LL)) {
        f_errors__error_at(v_c, 0LL, "слишком много разных типов", "");
    }
    tb_o_Type * v_t = tb_new_Type(v_kind, v_key, v_elem, 0, v_obj_name, 0, v_func_key, (((tb_o_Comp *)tb_nn(v_c, 23))->q_types)->len);
    if (v_has_obj) {
        { tb_o_Node * tb_v234 = v_onode; tb_o_Node * *tb_p234 = &(((tb_o_Type *)tb_nn(v_t, 25))->q_obj_node); *tb_p234 = tb_v234; }
    }
    if (v_has_func) {
        { tb_o_Node * tb_v235 = v_fnode; tb_o_Node * *tb_p235 = &(((tb_o_Type *)tb_nn(v_t, 27))->q_func_node); *tb_p235 = tb_v235; }
    }
    { tb_o_Type * tb_v236 = v_t; *(tb_o_Type * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 28))->q_types) = tb_v236; }
    return ((tb_o_Type *)tb_nn(v_t, 29))->q_id;
    return v_r;
}

static long long f_check__simple_type(tb_o_Comp * v_c, const char * v_kind) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, v_kind, (-1LL), (-1LL), v_nothing, 0, v_nothing, 0);
    tb_crash(31, "функция simple_type дошла до конца без return");
    return 0LL;
}

static long long f_check__list_of(tb_o_Comp * v_c, long long v_elem) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "list", (-1LL), v_elem, v_nothing, 0, v_nothing, 0);
    tb_crash(35, "функция list_of дошла до конца без return");
    return 0LL;
}

static long long f_check__task_of(tb_o_Comp * v_c, long long v_elem) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "task", (-1LL), v_elem, v_nothing, 0, v_nothing, 0);
    tb_crash(40, "функция task_of дошла до конца без return");
    return 0LL;
}

static long long f_check__dict_of(tb_o_Comp * v_c, long long v_key, long long v_value) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "dict", v_key, v_value, v_nothing, 0, v_nothing, 0);
    tb_crash(44, "функция dict_of дошла до конца без return");
    return 0LL;
}

static long long f_check__obj_type(tb_o_Comp * v_c, tb_o_Node * v_o) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "obj", (-1LL), (-1LL), v_o, 1, v_nothing, 0);
    tb_crash(48, "функция obj_type дошла до конца без return");
    return 0LL;
}

static long long f_check__multi_type(tb_o_Comp * v_c, tb_o_Node * v_fnode) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "multi", (-1LL), (-1LL), v_nothing, 0, v_fnode, 1);
    tb_crash(52, "функция multi_type дошла до конца без return");
    return 0LL;
}

static const char * f_check__kind_of(tb_o_Comp * v_c, long long v_t) {
    if ((v_t < 0LL)) {
        return "unknown";
    }
    return ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 59))->q_types, v_t, sizeof(tb_o_Type *), 59)), 59))->q_kind;
    tb_crash(56, "функция kind_of дошла до конца без return");
    return "";
}

static const char * f_check__type_name(tb_o_Comp * v_c, long long v_t) {
    const char * v_r = "";
    if ((v_t < 0LL)) {
        return "?";
    }
    tb_o_Type * v_ty = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 64))->q_types, v_t, sizeof(tb_o_Type *), 64));
    if (({ const char *tb_s237 = ((tb_o_Type *)tb_nn(v_ty, 65))->q_kind; ((unsigned char)tb_s237[0] == 108 && (unsigned char)tb_s237[1] == 105 && (unsigned char)tb_s237[2] == 115 && (unsigned char)tb_s237[3] == 116 && tb_s237[4] == 0); })) {
        return ({ const char *tb_c238_0 = "list "; long long tb_c238_0n = sizeof("list ") - 1; const char *tb_c238_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 66))->q_elem); long long tb_c238_1n = (long long)strlen(tb_c238_1); char *tb_c238r = tb_mem(tb_c238_0n + tb_c238_1n + 1), *tb_c238rw = tb_c238r; memcpy(tb_c238rw, tb_c238_0, tb_c238_0n); tb_c238rw += tb_c238_0n; memcpy(tb_c238rw, tb_c238_1, tb_c238_1n); tb_c238rw += tb_c238_1n; *tb_c238rw = 0; (const char *)tb_c238r; });
    }
    if (({ const char *tb_s239 = ((tb_o_Type *)tb_nn(v_ty, 67))->q_kind; ((unsigned char)tb_s239[0] == 100 && (unsigned char)tb_s239[1] == 105 && (unsigned char)tb_s239[2] == 99 && (unsigned char)tb_s239[3] == 116 && tb_s239[4] == 0); })) {
        return ({ const char *tb_c240_0 = "dict "; long long tb_c240_0n = sizeof("dict ") - 1; const char *tb_c240_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_key); long long tb_c240_1n = (long long)strlen(tb_c240_1); const char *tb_c240_2 = " "; long long tb_c240_2n = sizeof(" ") - 1; const char *tb_c240_3 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_elem); long long tb_c240_3n = (long long)strlen(tb_c240_3); char *tb_c240r = tb_mem(tb_c240_0n + tb_c240_1n + tb_c240_2n + tb_c240_3n + 1), *tb_c240rw = tb_c240r; memcpy(tb_c240rw, tb_c240_0, tb_c240_0n); tb_c240rw += tb_c240_0n; memcpy(tb_c240rw, tb_c240_1, tb_c240_1n); tb_c240rw += tb_c240_1n; memcpy(tb_c240rw, tb_c240_2, tb_c240_2n); tb_c240rw += tb_c240_2n; memcpy(tb_c240rw, tb_c240_3, tb_c240_3n); tb_c240rw += tb_c240_3n; *tb_c240rw = 0; (const char *)tb_c240r; });
    }
    if (({ const char *tb_s241 = ((tb_o_Type *)tb_nn(v_ty, 69))->q_kind; ((unsigned char)tb_s241[0] == 111 && (unsigned char)tb_s241[1] == 98 && (unsigned char)tb_s241[2] == 106 && tb_s241[3] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn(v_ty, 70))->q_obj_node, 70))->q_name;
    }
    if (({ const char *tb_s242 = ((tb_o_Type *)tb_nn(v_ty, 71))->q_kind; ((unsigned char)tb_s242[0] == 109 && (unsigned char)tb_s242[1] == 117 && (unsigned char)tb_s242[2] == 108 && (unsigned char)tb_s242[3] == 116 && (unsigned char)tb_s242[4] == 105 && tb_s242[5] == 0); })) {
        return "несколько выходов";
    }
    if (({ const char *tb_s243 = ((tb_o_Type *)tb_nn(v_ty, 73))->q_kind; ((unsigned char)tb_s243[0] == 110 && (unsigned char)tb_s243[1] == 111 && (unsigned char)tb_s243[2] == 110 && (unsigned char)tb_s243[3] == 101 && tb_s243[4] == 0); })) {
        return "ничего";
    }
    if (({ const char *tb_s244 = ((tb_o_Type *)tb_nn(v_ty, 75))->q_kind; ((unsigned char)tb_s244[0] == 116 && (unsigned char)tb_s244[1] == 97 && (unsigned char)tb_s244[2] == 115 && (unsigned char)tb_s244[3] == 107 && tb_s244[4] == 0); })) {
        return "номерок background";
    }
    return ((tb_o_Type *)tb_nn(v_ty, 77))->q_kind;
    return v_r;
}

static _Bool f_check__is_number(long long v_t) {
    return ((v_t == 1LL) || (v_t == 2LL));
    tb_crash(79, "функция is_number дошла до конца без return");
    return 0;
}

static _Bool f_check__is_list(tb_o_Comp * v_c, long long v_t) {
    return ({ const char *tb_s245 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s245[0] == 108 && (unsigned char)tb_s245[1] == 105 && (unsigned char)tb_s245[2] == 115 && (unsigned char)tb_s245[3] == 116 && tb_s245[4] == 0); });
    tb_crash(82, "функция is_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t) {
    return ({ const char *tb_s246 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s246[0] == 100 && (unsigned char)tb_s246[1] == 105 && (unsigned char)tb_s246[2] == 99 && (unsigned char)tb_s246[3] == 116 && tb_s246[4] == 0); });
    tb_crash(85, "функция is_dict дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s247 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s247[0] == 111 && (unsigned char)tb_s247[1] == 98 && (unsigned char)tb_s247[2] == 106 && tb_s247[3] == 0); }) && ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 90))->q_types, v_t, sizeof(tb_o_Type *), 90)), 90))->q_obj_node, 90))->q_by_cols);
    tb_crash(89, "функция is_cols_obj дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s248 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s248[0] == 108 && (unsigned char)tb_s248[1] == 105 && (unsigned char)tb_s248[2] == 115 && (unsigned char)tb_s248[3] == 116 && tb_s248[4] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 94))->q_types, v_t, sizeof(tb_o_Type *), 94)), 94))->q_elem));
    tb_crash(93, "функция is_cols_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_scalar(long long v_t) {
    return ((((v_t == 1LL) || (v_t == 2LL)) || (v_t == 3LL)) || (v_t == 4LL));
    tb_crash(96, "функция is_scalar дошла до конца без return");
    return 0;
}

static _Bool f_check__fits(long long v_want, long long v_got) {
    if ((v_want == v_got)) {
        return 1;
    }
    if (((v_want == 2LL) && (v_got == 1LL))) {
        return 1;
    }
    return 0;
    tb_crash(100, "функция fits дошла до конца без return");
    return 0;
}

static _Bool f_check__is_builtin(const char * v_name) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    {
        tb_list * tb_l249 = tb_list_from(29, sizeof(const char *), (const char *[]){"print", "len", "round", "code", "char", "cut", "to_int", "to_dec", "read_file", "write_file", "input", "args", "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with", "sort", "abs", "min", "max", "sqrt", "run", "exit", "file_exists"});
        for (long long tb_i249 = 0; tb_i249 < tb_l249->len; tb_i249++) {
            const char * const v_b = ((const char * *)tb_l249->data)[tb_i249];
            if ((strcmp(v_b, v_name) == 0)) {
                { _Bool tb_rv = 1; if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
            }
        }
    }
    { _Bool tb_rv = 0; if (tb_fa) tb_temp_end(tb_fm); return tb_rv; }
    if (tb_fa) tb_temp_end(tb_fm);
    tb_crash(109, "функция is_builtin дошла до конца без return");
    return 0;
}

static _Bool f_check__builtin_can_fail(const char * v_name) {
    return ((((({ const char *tb_s250 = v_name; ((unsigned char)tb_s250[0] == 116 && (unsigned char)tb_s250[1] == 111 && (unsigned char)tb_s250[2] == 95 && (unsigned char)tb_s250[3] == 105 && (unsigned char)tb_s250[4] == 110 && (unsigned char)tb_s250[5] == 116 && tb_s250[6] == 0); }) || ({ const char *tb_s251 = v_name; ((unsigned char)tb_s251[0] == 116 && (unsigned char)tb_s251[1] == 111 && (unsigned char)tb_s251[2] == 95 && (unsigned char)tb_s251[3] == 100 && (unsigned char)tb_s251[4] == 101 && (unsigned char)tb_s251[5] == 99 && tb_s251[6] == 0); })) || (strcmp(v_name, "read_file") == 0)) || (strcmp(v_name, "write_file") == 0)) || ({ const char *tb_s252 = v_name; ((unsigned char)tb_s252[0] == 105 && (unsigned char)tb_s252[1] == 110 && (unsigned char)tb_s252[2] == 112 && (unsigned char)tb_s252[3] == 117 && (unsigned char)tb_s252[4] == 116 && tb_s252[5] == 0); }));
    tb_crash(116, "функция builtin_can_fail дошла до конца без return");
    return 0;
}

static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name) {
    {
        tb_list * tb_l253 = ((tb_o_Comp *)tb_nn(v_c, 123))->q_all_mods;
        for (long long tb_i253 = 0; tb_i253 < tb_l253->len; tb_i253++) {
            const char * const v_m = ((const char * *)tb_l253->data)[tb_i253];
            if ((strcmp(v_m, v_name) == 0)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(122, "функция is_module дошла до конца без return");
    return 0;
}

static _Bool f_check__module_uses(tb_o_Comp * v_c, const char * v_mod, const char * v_other) {
    if ((strcmp(v_mod, v_other) == 0)) {
        return 1;
    }
    {
        tb_list * tb_l254 = ((tb_o_Comp *)tb_nn(v_c, 131))->q_uses;
        for (long long tb_i254 = 0; tb_i254 < tb_l254->len; tb_i254++) {
            tb_o_Use * const v_u = ((tb_o_Use * *)tb_l254->data)[tb_i254];
            if (((strcmp(((tb_o_Use *)tb_nn(v_u, 132))->q_mod, v_mod) == 0) && (strcmp(((tb_o_Use *)tb_nn(v_u, 132))->q_used, v_other) == 0))) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(128, "функция module_uses дошла до конца без return");
    return 0;
}

static long long f_check__find_func_in(tb_o_Comp * v_c, const char * v_name, const char * v_mod) {
    {
        long long tb_end255 = ((((tb_o_Comp *)tb_nn(v_c, 138))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end255; v_i++) {
            tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 139))->q_funcs, v_i, sizeof(tb_o_Func *), 139));
            if (((strcmp(((tb_o_Func *)tb_nn(v_f, 140))->q_name, v_name) == 0) && (strcmp(((tb_o_Func *)tb_nn(v_f, 140))->q_mod, v_mod) == 0))) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(137, "функция find_func_in дошла до конца без return");
    return 0LL;
}

static long long f_check__find_func(tb_o_Comp * v_c, const char * v_name) {
    long long v_own = f_check__find_func_in(v_c, v_name, ((tb_o_Comp *)tb_nn(v_c, 146))->q_cur_mod);
    if ((v_own >= 0LL)) {
        return v_own;
    }
    long long v_found = (-1LL);
    {
        long long tb_end256 = ((((tb_o_Comp *)tb_nn(v_c, 150))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end256; v_i++) {
            tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 151))->q_funcs, v_i, sizeof(tb_o_Func *), 151));
            if ((((strcmp(((tb_o_Func *)tb_nn(v_f, 152))->q_name, v_name) == 0) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 152))->q_node, 152))->q_is_pub) && f_check__module_uses(v_c, ((tb_o_Comp *)tb_nn(v_c, 152))->q_cur_mod, ((tb_o_Func *)tb_nn(v_f, 152))->q_mod))) {
                if ((v_found >= 0LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 154))->q_node, 154))->q_line, "функция с таким именем есть в двух модулях, напиши модуль: money.total(...)", v_name);
                }
                v_found = v_i;
            }
        }
    }
    return v_found;
    tb_crash(145, "функция find_func дошла до конца без return");
    return 0LL;
}

static void f_check__explain_missing_func(tb_o_Comp * v_c, tb_o_Node * v_n) {
    {
        tb_list * tb_l257 = ((tb_o_Comp *)tb_nn(v_c, 160))->q_funcs;
        for (long long tb_i257 = 0; tb_i257 < tb_l257->len; tb_i257++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l257->data)[tb_i257];
            if ((strcmp(((tb_o_Func *)tb_nn(v_f, 161))->q_name, ((tb_o_Node *)tb_nn(v_n, 161))->q_name) == 0)) {
                if ((!((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 162))->q_node, 162))->q_is_pub)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 163))->q_line, "эта функция спрятана в своём модуле: поставь перед ней modpub", ((tb_o_Func *)tb_nn(v_f, 163))->q_mod);
                }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 164))->q_line, "функция лежит в другом модуле: добавь в начало файла  use", ((tb_o_Func *)tb_nn(v_f, 164))->q_mod);
            }
        }
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 165))->q_line, "нет такой функции", ((tb_o_Node *)tb_nn(v_n, 165))->q_name);
}

static long long f_check__find_obj(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end258 = ((((tb_o_Comp *)tb_nn(v_c, 169))->q_objs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end258; v_i++) {
            if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 170))->q_objs, v_i, sizeof(tb_o_Node *), 170)), 170))->q_name, v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(168, "функция find_obj дошла до конца без return");
    return 0LL;
}

static long long f_check__find_field(tb_o_Node * v_o, const char * v_name) {
    {
        long long tb_end259 = ((((tb_o_Node *)tb_nn(v_o, 175))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end259; v_i++) {
            if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 176))->q_kids, v_i, sizeof(tb_o_Node *), 176)), 176))->q_name, v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(174, "функция find_field дошла до конца без return");
    return 0LL;
}

static long long f_check__resolve_type(tb_o_Comp * v_c, tb_o_Node * v_t) {
    long long v_r = 0LL;
    if (({ const char *tb_s260 = ((tb_o_Node *)tb_nn(v_t, 181))->q_name; ((unsigned char)tb_s260[0] == 105 && (unsigned char)tb_s260[1] == 110 && (unsigned char)tb_s260[2] == 116 && tb_s260[3] == 0); })) {
        return 1LL;
    }
    if (({ const char *tb_s261 = ((tb_o_Node *)tb_nn(v_t, 183))->q_name; ((unsigned char)tb_s261[0] == 100 && (unsigned char)tb_s261[1] == 101 && (unsigned char)tb_s261[2] == 99 && tb_s261[3] == 0); })) {
        return 2LL;
    }
    if (({ const char *tb_s262 = ((tb_o_Node *)tb_nn(v_t, 185))->q_name; ((unsigned char)tb_s262[0] == 116 && (unsigned char)tb_s262[1] == 101 && (unsigned char)tb_s262[2] == 120 && (unsigned char)tb_s262[3] == 116 && tb_s262[4] == 0); })) {
        return 4LL;
    }
    if (({ const char *tb_s263 = ((tb_o_Node *)tb_nn(v_t, 187))->q_name; ((unsigned char)tb_s263[0] == 98 && (unsigned char)tb_s263[1] == 111 && (unsigned char)tb_s263[2] == 111 && (unsigned char)tb_s263[3] == 108 && tb_s263[4] == 0); })) {
        return 3LL;
    }
    if (({ const char *tb_s264 = ((tb_o_Node *)tb_nn(v_t, 189))->q_name; ((unsigned char)tb_s264[0] == 108 && (unsigned char)tb_s264[1] == 105 && (unsigned char)tb_s264[2] == 115 && (unsigned char)tb_s264[3] == 116 && tb_s264[4] == 0); })) {
        return f_check__list_of(v_c, f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 190))->q_kids, 0LL, sizeof(tb_o_Node *), 190))));
    }
    if (({ const char *tb_s265 = ((tb_o_Node *)tb_nn(v_t, 191))->q_name; ((unsigned char)tb_s265[0] == 100 && (unsigned char)tb_s265[1] == 105 && (unsigned char)tb_s265[2] == 99 && (unsigned char)tb_s265[3] == 116 && tb_s265[4] == 0); })) {
        long long v_key = f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 192))->q_kids, 0LL, sizeof(tb_o_Node *), 192)));
        long long v_value = f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 193))->q_kids, 1LL, sizeof(tb_o_Node *), 193)));
        return f_check__dict_of(v_c, v_key, v_value);
    }
    long long v_o = f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_t, 195))->q_name);
    if ((v_o < 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_t, 197))->q_line, "нет такого типа и такого obj", ((tb_o_Node *)tb_nn(v_t, 197))->q_name);
    }
    return f_check__obj_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 198))->q_objs, v_o, sizeof(tb_o_Node *), 198)));
    return v_r;
}

static long long f_check__find_var(tb_o_Comp * v_c, const char * v_name) {
    long long v_i = ((((tb_o_Comp *)tb_nn(v_c, 204))->q_vars)->len - 1LL);
    while ((v_i >= ((tb_o_Comp *)tb_nn(v_c, 205))->q_var_base)) {
        if ((strcmp(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 206))->q_vars, v_i, sizeof(tb_o_Var *), 206)), 206))->q_name, v_name) == 0)) {
            return v_i;
        }
        { long long tb_v266 = 1LL; long long *tb_p266 = &(v_i); *tb_p266 -= tb_v266; }
    }
    return (-1LL);
    tb_crash(203, "функция find_var дошла до конца без return");
    return 0LL;
}

static void f_check__name_is_free(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    if ((f_check__find_var(v_c, v_name) >= 0LL)) {
        f_errors__error_at(v_c, v_line, "такое имя уже есть, возьми другое", v_name);
    }
    if ((f_check__find_func(v_c, v_name) >= 0LL)) {
        f_errors__error_at(v_c, v_line, "так уже называется функция", v_name);
    }
    if ((f_check__find_obj(v_c, v_name) >= 0LL)) {
        f_errors__error_at(v_c, v_line, "так уже называется obj", v_name);
    }
}

static void f_check__add_var(tb_o_Comp * v_c, const char * v_name, long long v_type, _Bool v_is_const, _Bool v_is_link, long long v_line) {
    f_check__name_is_free(v_c, v_name, v_line);
    if (((((tb_o_Comp *)tb_nn(v_c, 222))->q_vars)->len == 2000LL)) {
        f_errors__error_at(v_c, v_line, "слишком много переменных", "");
    }
    { tb_o_Var * tb_v267 = tb_new_Var(v_name, v_type, v_is_const, v_is_link, ((tb_o_Comp *)tb_nn(v_c, 224))->q_depth, 0, ""); *(tb_o_Var * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 224))->q_vars) = tb_v267; }
}

static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    while (((((tb_o_Comp *)tb_nn(v_c, 227))->q_vars)->len > v_count_left)) {
        tb_mark tb_am268 = tb_temp_begin();
        do {
            tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 228))->q_vars, 228));
        } while (0);
        tb_temp_end(tb_am268);
    }
    if (tb_fa) tb_temp_end(tb_fm);
}

static void f_check__open_scope(tb_o_Comp * v_c) {
    { long long tb_v269 = 1LL; long long *tb_p269 = &(((tb_o_Comp *)tb_nn(v_c, 231))->q_depth); *tb_p269 += tb_v269; }
}

static void f_check__close_scope(tb_o_Comp * v_c) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    while ((((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len > 0LL) && (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 234))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 234)), 234))->q_depth == ((tb_o_Comp *)tb_nn(v_c, 234))->q_depth))) {
        tb_mark tb_am270 = tb_temp_begin();
        do {
            tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 235))->q_vars, 235));
        } while (0);
        tb_temp_end(tb_am270);
    }
    { long long tb_v271 = 1LL; long long *tb_p271 = &(((tb_o_Comp *)tb_nn(v_c, 236))->q_depth); *tb_p271 -= tb_v271; }
    if (tb_fa) tb_temp_end(tb_fm);
}

static const char * f_check__loop_word(tb_o_Comp * v_c) {
    if (((tb_o_Comp *)tb_nn(v_c, 241))->q_par_vec) {
        return "by vec";
    }
    return "on all cores";
    tb_crash(240, "функция loop_word дошла до конца без return");
    return "";
}

static _Bool f_check__outside_par(tb_o_Comp * v_c, long long v_v) {
    return (((((tb_o_Comp *)tb_nn(v_c, 247))->q_par_base >= 0LL) && (v_v >= 0LL)) && (v_v < ((tb_o_Comp *)tb_nn(v_c, 247))->q_par_base));
    tb_crash(246, "функция outside_par дошла до конца без return");
    return 0;
}

static long long f_check__root_var(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (((({ const char *tb_s272 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s272[0] == 105 && (unsigned char)tb_s272[1] == 110 && (unsigned char)tb_s272[2] == 100 && (unsigned char)tb_s272[3] == 101 && (unsigned char)tb_s272[4] == 120 && tb_s272[5] == 0); }) || ({ const char *tb_s273 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s273[0] == 102 && (unsigned char)tb_s273[1] == 105 && (unsigned char)tb_s273[2] == 101 && (unsigned char)tb_s273[3] == 108 && (unsigned char)tb_s273[4] == 100 && tb_s273[5] == 0); })) || ({ const char *tb_s274 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s274[0] == 109 && (unsigned char)tb_s274[1] == 101 && (unsigned char)tb_s274[2] == 116 && (unsigned char)tb_s274[3] == 104 && (unsigned char)tb_s274[4] == 111 && (unsigned char)tb_s274[5] == 100 && tb_s274[6] == 0); }))) {
        return f_check__root_var(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)));
    }
    if (({ const char *tb_s275 = ((tb_o_Node *)tb_nn(v_n, 253))->q_kind; ((unsigned char)tb_s275[0] == 110 && (unsigned char)tb_s275[1] == 97 && (unsigned char)tb_s275[2] == 109 && (unsigned char)tb_s275[3] == 101 && tb_s275[4] == 0); })) {
        return f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 254))->q_name);
    }
    return (-1LL);
    return v_r;
}

static void f_check__capture(tb_o_Comp * v_c, long long v_v) {
    tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 259))->q_vars, v_v, sizeof(tb_o_Var *), 259));
    {
        tb_list * tb_l276 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 260))->q_par_node, 260))->q_caps;
        for (long long tb_i276 = 0; tb_i276 < tb_l276->len; tb_i276++) {
            tb_o_Var * const v_old = ((tb_o_Var * *)tb_l276->data)[tb_i276];
            if (((strcmp(((tb_o_Var *)tb_nn(v_old, 261))->q_name, ((tb_o_Var *)tb_nn(v_var, 261))->q_name) == 0) && ((!((tb_o_Comp *)tb_nn(v_c, 261))->q_par_vec) || (!((tb_o_Var *)tb_nn(v_old, 261))->q_is_shared)))) {
                return;
            }
        }
    }
    { tb_o_Var * tb_v277 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 263))->q_name, ((tb_o_Var *)tb_nn(v_var, 263))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 263))->q_is_link, 0LL, (((tb_o_Var *)tb_nn(v_var, 263))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 263))->q_par_vec)), ""); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 263))->q_par_node, 263))->q_caps) = tb_v277; }
}

static _Bool f_check__outside_temp(tb_o_Comp * v_c, long long v_v) {
    return (((((tb_o_Comp *)tb_nn(v_c, 269))->q_temp_base >= 0LL) && (v_v >= 0LL)) && (v_v < ((tb_o_Comp *)tb_nn(v_c, 269))->q_temp_base));
    tb_crash(268, "функция outside_temp дошла до конца без return");
    return 0;
}

static _Bool f_check__is_plain(long long v_t) {
    return (((v_t == 1LL) || (v_t == 2LL)) || (v_t == 3LL));
    tb_crash(271, "функция is_plain дошла до конца без return");
    return 0;
}

static _Bool f_check__temp_can_see(tb_o_Comp * v_c, long long v_t) {
    if ((f_check__is_plain(v_t) || (v_t == 4LL))) {
        return 1;
    }
    if (f_check__is_list(v_c, v_t)) {
        return f_check__is_plain(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 278))->q_types, v_t, sizeof(tb_o_Type *), 278)), 278))->q_elem);
    }
    if (f_check__is_dict(v_c, v_t)) {
        return ((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 280))->q_types, v_t, sizeof(tb_o_Type *), 280)), 280))->q_key == 1LL) && f_check__is_plain(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 280))->q_types, v_t, sizeof(tb_o_Type *), 280)), 280))->q_elem));
    }
    return 0;
    tb_crash(274, "функция temp_can_see дошла до конца без return");
    return 0;
}

static void f_check__temp_keep_out(tb_o_Comp * v_c, long long v_line, const char * v_name) {
    f_errors__error_at(v_c, v_line, "из temp наружу можно сохранить только int, dec или bool: текст, списки и obj удалятся в конце блока", v_name);
}

static void f_check__no_outside_change(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (f_check__outside_par(v_c, f_check__root_var(v_c, v_n))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 288))->q_line, ({ const char *tb_c278_0 = "внутри "; long long tb_c278_0n = sizeof("внутри ") - 1; const char *tb_c278_1 = f_check__loop_word(v_c); long long tb_c278_1n = (long long)strlen(tb_c278_1); const char *tb_c278_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c278_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c278r = tb_mem(tb_c278_0n + tb_c278_1n + tb_c278_2n + 1), *tb_c278rw = tb_c278r; memcpy(tb_c278rw, tb_c278_0, tb_c278_0n); tb_c278rw += tb_c278_0n; memcpy(tb_c278rw, tb_c278_1, tb_c278_1n); tb_c278rw += tb_c278_1n; memcpy(tb_c278rw, tb_c278_2, tb_c278_2n); tb_c278rw += tb_c278_2n; *tb_c278rw = 0; (const char *)tb_c278r; }), "");
    }
}

static void f_check__need_value(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((v_t == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 294))->q_line, "это ничего не возвращает, значение нельзя использовать", ((tb_o_Node *)tb_nn(v_n, 294))->q_name);
    }
    if (({ const char *tb_s279 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s279[0] == 109 && (unsigned char)tb_s279[1] == 117 && (unsigned char)tb_s279[2] == 108 && (unsigned char)tb_s279[3] == 116 && (unsigned char)tb_s279[4] == 105 && tb_s279[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 296))->q_line, "у функции несколько выходов: пиши  a, b = ...", ((tb_o_Node *)tb_nn(v_n, 296))->q_name);
    }
}

static long long f_check__check_value(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = f_check__check_expr(v_c, v_n);
    f_check__need_value(v_c, v_n, v_t);
    return v_t;
    return v_r;
}

static long long f_check__check_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 304))->q_value;
    if ((({ const char *tb_s280 = v_op; ((unsigned char)tb_s280[0] == 111 && (unsigned char)tb_s280[1] == 114 && tb_s280[2] == 0); }) && f_check__can_fail_call(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 305))->q_kids, 0LL, sizeof(tb_o_Node *), 305))))) {
        long long v_a = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 307))->q_kids, 0LL, sizeof(tb_o_Node *), 307)), "or");
        long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 308))->q_kids, 1LL, sizeof(tb_o_Node *), 308)));
        if ((!f_check__fits(v_a, v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 310))->q_line, "запасное значение после or другого типа", f_check__type_name(v_c, v_a));
        }
        if ((v_a == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 312))->q_line, "у функции нет значения, тут подходит  try  или  else err", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 313))->q_temp_types)->len == 2001LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 314))->q_line, "слишком много or", "");
        }
        { long long tb_v281 = v_a; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 315))->q_temp_types) = tb_v281; }
        { long long tb_v282 = ((((tb_o_Comp *)tb_nn(v_c, 316))->q_temp_types)->len - 1LL); long long *tb_p282 = &(((tb_o_Node *)tb_nn(v_n, 316))->q_temp_no); *tb_p282 = tb_v282; }
        { const char * tb_v283 = "orelse"; const char * *tb_p283 = &(((tb_o_Node *)tb_nn(v_n, 317))->q_value); *tb_p283 = tb_v283; }
        return v_a;
    }
    long long v_a = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 319))->q_kids, 0LL, sizeof(tb_o_Node *), 319)));
    long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 320))->q_kids, 1LL, sizeof(tb_o_Node *), 320)));
    if ((({ const char *tb_s284 = v_op; ((unsigned char)tb_s284[0] == 97 && (unsigned char)tb_s284[1] == 110 && (unsigned char)tb_s284[2] == 100 && tb_s284[3] == 0); }) || ({ const char *tb_s285 = v_op; ((unsigned char)tb_s285[0] == 111 && (unsigned char)tb_s285[1] == 114 && tb_s285[2] == 0); }))) {
        if (((v_a != 3LL) || (v_b != 3LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_line, "and / or работают только с true и false", "");
        }
        return 3LL;
    }
    if ((({ const char *tb_s286 = v_op; ((unsigned char)tb_s286[0] == 61 && (unsigned char)tb_s286[1] == 61 && tb_s286[2] == 0); }) || ({ const char *tb_s287 = v_op; ((unsigned char)tb_s287[0] == 33 && (unsigned char)tb_s287[1] == 61 && tb_s287[2] == 0); }))) {
        if (((!(f_check__is_number(v_a) && f_check__is_number(v_b))) && (v_a != v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_line, "нельзя сравнить разные типы", "");
        }
        if ((!f_check__is_scalar(v_a))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 329))->q_line, "списки и объекты пока нельзя сравнивать", "");
        }
        return 3LL;
    }
    if ((((({ const char *tb_s288 = v_op; ((unsigned char)tb_s288[0] == 60 && tb_s288[1] == 0); }) || ({ const char *tb_s289 = v_op; ((unsigned char)tb_s289[0] == 62 && tb_s289[1] == 0); })) || ({ const char *tb_s290 = v_op; ((unsigned char)tb_s290[0] == 60 && (unsigned char)tb_s290[1] == 61 && tb_s290[2] == 0); })) || ({ const char *tb_s291 = v_op; ((unsigned char)tb_s291[0] == 62 && (unsigned char)tb_s291[1] == 61 && tb_s291[2] == 0); }))) {
        if (((v_a == 4LL) && (v_b == 4LL))) {
            return 3LL;
        }
        if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 335))->q_line, "больше / меньше работают с числами или с двумя текстами", v_op);
        }
        return 3LL;
    }
    if ((({ const char *tb_s292 = v_op; ((unsigned char)tb_s292[0] == 43 && tb_s292[1] == 0); }) && ((v_a == 4LL) || (v_b == 4LL)))) {
        if (((!f_check__is_scalar(v_a)) || (!f_check__is_scalar(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 339))->q_line, "список или объект нельзя сложить с текстом", "");
        }
        return 4LL;
    }
    if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 342))->q_line, "эта операция работает только с числами", v_op);
    }
    if ((({ const char *tb_s293 = v_op; ((unsigned char)tb_s293[0] == 37 && tb_s293[1] == 0); }) && ((v_a == 2LL) || (v_b == 2LL)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 344))->q_line, "остаток % считается только для int", "");
    }
    if (((v_a == 2LL) || (v_b == 2LL))) {
        return 2LL;
    }
    return 1LL;
    return v_r;
}

static long long f_check__check_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 350))->q_kids)->len == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 351))->q_line, "пустой список пишется с типом: x = list int", "");
    }
    long long v_elem = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 352))->q_kids, 0LL, sizeof(tb_o_Node *), 352)));
    {
        long long tb_end294 = ((((tb_o_Node *)tb_nn(v_n, 353))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end294; v_i++) {
            long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 354))->q_kids, v_i, sizeof(tb_o_Node *), 354)));
            if (f_check__fits(v_elem, v_t)) {
                continue;
            }
            if (((v_elem == 1LL) && (v_t == 2LL))) {
                v_elem = 2LL;
                continue;
            }
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 360))->q_kids, v_i, sizeof(tb_o_Node *), 360)), 360))->q_line, "в одном списке все значения одного типа", f_check__type_name(v_c, v_elem));
        }
    }
    return f_check__list_of(v_c, v_elem);
    return v_r;
}

static long long f_check__builtin_arg(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_i) {
    long long v_r = 0LL;
    if (({ const char *tb_s295 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 365))->q_kids, v_i, sizeof(tb_o_Node *), 365)), 365))->q_name; !(tb_s295[0] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 366))->q_line, "у встроенных функций аргументы пишутся без имён", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 366))->q_kids, v_i, sizeof(tb_o_Node *), 366)), 366))->q_name);
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 367))->q_kids, v_i, sizeof(tb_o_Node *), 367)), 367))->q_kids, 0LL, sizeof(tb_o_Node *), 367)));
    { long long tb_v296 = v_t; long long *tb_p296 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 368))->q_kids, v_i, sizeof(tb_o_Node *), 368)), 368))->q_ty); *tb_p296 = tb_v296; }
    return v_t;
    return v_r;
}

static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage) {
    if (((((tb_o_Node *)tb_nn(v_n, 374))->q_kids)->len != tb_tlen(v_pattern))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 375))->q_line, "не то количество значений, правильно так", v_usage);
    }
    {
        long long tb_end297 = (tb_tlen(v_pattern) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end297; v_i++) {
            long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            const char * v_p = tb_char_at(v_pattern, v_i, -1, 378);
            _Bool v_ok = 1;
            if (({ const char *tb_s298 = v_p; ((unsigned char)tb_s298[0] == 116 && tb_s298[1] == 0); })) {
                v_ok = (v_t == 4LL);
            } else if (({ const char *tb_s299 = v_p; ((unsigned char)tb_s299[0] == 105 && tb_s299[1] == 0); })) {
                v_ok = (v_t == 1LL);
            } else if (({ const char *tb_s300 = v_p; ((unsigned char)tb_s300[0] == 110 && tb_s300[1] == 0); })) {
                v_ok = f_check__is_number(v_t);
            } else if (({ const char *tb_s301 = v_p; ((unsigned char)tb_s301[0] == 76 && tb_s301[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 387))->q_types, v_t, sizeof(tb_o_Type *), 387)), 387))->q_elem == 4LL));
            } else if (({ const char *tb_s302 = v_p; ((unsigned char)tb_s302[0] == 115 && tb_s302[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 1LL) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 2LL)) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 4LL)));
            } else if (({ const char *tb_s303 = v_p; ((unsigned char)tb_s303[0] == 120 && tb_s303[1] == 0); })) {
                v_ok = ((f_check__is_list(v_c, v_t) || f_check__is_dict(v_c, v_t)) || (v_t == 4LL));
            }
            if ((!v_ok)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 393))->q_line, "не тот тип значения, правильно так", v_usage);
            }
        }
    }
}

static long long f_check__check_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 396))->q_name;
    if (({ const char *tb_s304 = v_name; ((unsigned char)tb_s304[0] == 112 && (unsigned char)tb_s304[1] == 114 && (unsigned char)tb_s304[2] == 105 && (unsigned char)tb_s304[3] == 110 && (unsigned char)tb_s304[4] == 116 && tb_s304[5] == 0); })) {
        {
            long long tb_end305 = ((((tb_o_Node *)tb_nn(v_n, 398))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end305; v_i++) {
                long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            }
        }
        return 0LL;
    }
    if (({ const char *tb_s306 = v_name; ((unsigned char)tb_s306[0] == 108 && (unsigned char)tb_s306[1] == 101 && (unsigned char)tb_s306[2] == 110 && tb_s306[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "x", "len(список, текст или dict)");
        return 1LL;
    }
    if (({ const char *tb_s307 = v_name; ((unsigned char)tb_s307[0] == 114 && (unsigned char)tb_s307[1] == 111 && (unsigned char)tb_s307[2] == 117 && (unsigned char)tb_s307[3] == 110 && (unsigned char)tb_s307[4] == 100 && tb_s307[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "round(число)");
        return 1LL;
    }
    if (({ const char *tb_s308 = v_name; ((unsigned char)tb_s308[0] == 99 && (unsigned char)tb_s308[1] == 111 && (unsigned char)tb_s308[2] == 100 && (unsigned char)tb_s308[3] == 101 && tb_s308[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "code(\"A\")  →  65");
        return 1LL;
    }
    if (({ const char *tb_s309 = v_name; ((unsigned char)tb_s309[0] == 99 && (unsigned char)tb_s309[1] == 104 && (unsigned char)tb_s309[2] == 97 && (unsigned char)tb_s309[3] == 114 && tb_s309[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "char(66)  →  \"B\"");
        return 4LL;
    }
    if (({ const char *tb_s310 = v_name; ((unsigned char)tb_s310[0] == 99 && (unsigned char)tb_s310[1] == 117 && (unsigned char)tb_s310[2] == 116 && tb_s310[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tii", "cut(текст, с какого номера, по какой номер)");
        return 4LL;
    }
    if (({ const char *tb_s311 = v_name; ((unsigned char)tb_s311[0] == 116 && (unsigned char)tb_s311[1] == 111 && (unsigned char)tb_s311[2] == 95 && (unsigned char)tb_s311[3] == 105 && (unsigned char)tb_s311[4] == 110 && (unsigned char)tb_s311[5] == 116 && tb_s311[6] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "to_int(\"42\")");
        return 1LL;
    }
    if (({ const char *tb_s312 = v_name; ((unsigned char)tb_s312[0] == 116 && (unsigned char)tb_s312[1] == 111 && (unsigned char)tb_s312[2] == 95 && (unsigned char)tb_s312[3] == 100 && (unsigned char)tb_s312[4] == 101 && (unsigned char)tb_s312[5] == 99 && tb_s312[6] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "to_dec(\"9.5\")");
        return 2LL;
    }
    if ((strcmp(v_name, "read_file") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "read_file(\"путь\")");
        return 4LL;
    }
    if ((strcmp(v_name, "write_file") == 0)) {
        f_check__builtin_args(v_c, v_n, "tt", "write_file(\"путь\", текст)");
        return 0LL;
    }
    if (({ const char *tb_s313 = v_name; ((unsigned char)tb_s313[0] == 105 && (unsigned char)tb_s313[1] == 110 && (unsigned char)tb_s313[2] == 112 && (unsigned char)tb_s313[3] == 117 && (unsigned char)tb_s313[4] == 116 && tb_s313[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "input()");
        return 4LL;
    }
    if (({ const char *tb_s314 = v_name; ((unsigned char)tb_s314[0] == 97 && (unsigned char)tb_s314[1] == 114 && (unsigned char)tb_s314[2] == 103 && (unsigned char)tb_s314[3] == 115 && tb_s314[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "args()");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s315 = v_name; ((unsigned char)tb_s315[0] == 115 && (unsigned char)tb_s315[1] == 112 && (unsigned char)tb_s315[2] == 108 && (unsigned char)tb_s315[3] == 105 && (unsigned char)tb_s315[4] == 116 && tb_s315[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "split(текст, разделитель)");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s316 = v_name; ((unsigned char)tb_s316[0] == 106 && (unsigned char)tb_s316[1] == 111 && (unsigned char)tb_s316[2] == 105 && (unsigned char)tb_s316[3] == 110 && tb_s316[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "Lt", "join(список text, разделитель)");
        return 4LL;
    }
    if (({ const char *tb_s317 = v_name; ((unsigned char)tb_s317[0] == 102 && (unsigned char)tb_s317[1] == 105 && (unsigned char)tb_s317[2] == 110 && (unsigned char)tb_s317[3] == 100 && tb_s317[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "find(текст, что ищем)  →  номер или -1");
        return 1LL;
    }
    if (({ const char *tb_s318 = v_name; ((unsigned char)tb_s318[0] == 114 && (unsigned char)tb_s318[1] == 101 && (unsigned char)tb_s318[2] == 112 && (unsigned char)tb_s318[3] == 108 && (unsigned char)tb_s318[4] == 97 && (unsigned char)tb_s318[5] == 99 && (unsigned char)tb_s318[6] == 101 && tb_s318[7] == 0); })) {
        f_check__builtin_args(v_c, v_n, "ttt", "replace(текст, что, на что)");
        return 4LL;
    }
    if (((({ const char *tb_s319 = v_name; ((unsigned char)tb_s319[0] == 117 && (unsigned char)tb_s319[1] == 112 && (unsigned char)tb_s319[2] == 112 && (unsigned char)tb_s319[3] == 101 && (unsigned char)tb_s319[4] == 114 && tb_s319[5] == 0); }) || ({ const char *tb_s320 = v_name; ((unsigned char)tb_s320[0] == 108 && (unsigned char)tb_s320[1] == 111 && (unsigned char)tb_s320[2] == 119 && (unsigned char)tb_s320[3] == 101 && (unsigned char)tb_s320[4] == 114 && tb_s320[5] == 0); })) || ({ const char *tb_s321 = v_name; ((unsigned char)tb_s321[0] == 116 && (unsigned char)tb_s321[1] == 114 && (unsigned char)tb_s321[2] == 105 && (unsigned char)tb_s321[3] == 109 && tb_s321[4] == 0); }))) {
        f_check__builtin_args(v_c, v_n, "t", "upper(текст)");
        return 4LL;
    }
    if (((strcmp(v_name, "starts_with") == 0) || (strcmp(v_name, "ends_with") == 0))) {
        f_check__builtin_args(v_c, v_n, "tt", "starts_with(текст, начало)");
        return 3LL;
    }
    if (({ const char *tb_s322 = v_name; ((unsigned char)tb_s322[0] == 115 && (unsigned char)tb_s322[1] == 111 && (unsigned char)tb_s322[2] == 114 && (unsigned char)tb_s322[3] == 116 && tb_s322[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "s", "sort(список int, dec или text)");
        return 0LL;
    }
    if (({ const char *tb_s323 = v_name; ((unsigned char)tb_s323[0] == 114 && (unsigned char)tb_s323[1] == 117 && (unsigned char)tb_s323[2] == 110 && tb_s323[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "run(\"команда терминала\")  →  код выхода int");
        return 1LL;
    }
    if (({ const char *tb_s324 = v_name; ((unsigned char)tb_s324[0] == 101 && (unsigned char)tb_s324[1] == 120 && (unsigned char)tb_s324[2] == 105 && (unsigned char)tb_s324[3] == 116 && tb_s324[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "exit(код)");
        return 0LL;
    }
    if ((strcmp(v_name, "file_exists") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "file_exists(\"путь\")");
        return 3LL;
    }
    if (({ const char *tb_s325 = v_name; ((unsigned char)tb_s325[0] == 97 && (unsigned char)tb_s325[1] == 98 && (unsigned char)tb_s325[2] == 115 && tb_s325[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "abs(число)");
        return ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_ty;
    }
    if (({ const char *tb_s326 = v_name; ((unsigned char)tb_s326[0] == 115 && (unsigned char)tb_s326[1] == 113 && (unsigned char)tb_s326[2] == 114 && (unsigned char)tb_s326[3] == 116 && tb_s326[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "sqrt(число)");
        return 2LL;
    }
    f_check__builtin_args(v_c, v_n, "nn", "min(a, b)");
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 472))->q_kids, 0LL, sizeof(tb_o_Node *), 472)), 472))->q_ty == 1LL) && (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 472))->q_kids, 1LL, sizeof(tb_o_Node *), 472)), 472))->q_ty == 1LL))) {
        return 1LL;
    }
    return 2LL;
    return v_r;
}

static void f_check__order_args(tb_o_Comp * v_c, tb_o_Node * v_call, tb_o_Node * v_params, _Bool v_allow_missing) {
    long long v_np = (((tb_o_Node *)tb_nn(v_params, 479))->q_kids)->len;
    if ((((((tb_o_Node *)tb_nn(v_call, 480))->q_kids)->len > v_np) || ((!v_allow_missing) && ((((tb_o_Node *)tb_nn(v_call, 480))->q_kids)->len != v_np)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_call, 481))->q_line, "не то количество значений в вызове", ((tb_o_Node *)tb_nn(v_call, 481))->q_name);
    }
    tb_list * v_ordered = tb_list_new(sizeof(tb_o_Node *));
    tb_list * v_taken = tb_bits_new();
    {
        long long tb_end327 = v_np;
        for (long long v_j = 1LL; v_j <= tb_end327; v_j++) {
            { tb_o_Node * tb_v328 = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *))); *(tb_o_Node * *)tb_grow(v_ordered) = tb_v328; }
            { _Bool tb_v329 = 0; tb_badd(v_taken, tb_v329); }
        }
    }
    _Bool v_named = 0;
    {
        long long tb_end330 = ((((tb_o_Node *)tb_nn(v_call, 488))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end330; v_i++) {
            tb_o_Node * v_arg = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 489))->q_kids, v_i, sizeof(tb_o_Node *), 489));
            long long v_slot = v_i;
            if (({ const char *tb_s331 = ((tb_o_Node *)tb_nn(v_arg, 491))->q_name; !(tb_s331[0] == 0); })) {
                v_named = 1;
                v_slot = (-1LL);
                {
                    long long tb_end332 = (v_np - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end332; v_j++) {
                        if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 495))->q_kids, v_j, sizeof(tb_o_Node *), 495)), 495))->q_name, ((tb_o_Node *)tb_nn(v_arg, 495))->q_name) == 0)) {
                            v_slot = v_j;
                        }
                    }
                }
                if ((v_slot < 0LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_arg, 498))->q_line, "нет параметра с таким именем", ((tb_o_Node *)tb_nn(v_arg, 498))->q_name);
                }
            } else if (v_named) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_arg, 500))->q_line, "после именованных аргументов обычные писать нельзя", "");
            }
            if (tb_bget(v_taken, v_slot, 501)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_arg, 502))->q_line, "параметр указан два раза", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 502))->q_kids, v_slot, sizeof(tb_o_Node *), 502)), 502))->q_name);
            }
            { tb_o_Node * tb_v333 = v_arg; tb_o_Node * *tb_p333 = &((*(tb_o_Node * *)tb_at(v_ordered, v_slot, sizeof(tb_o_Node *), 503))); *tb_p333 = tb_v333; }
            { _Bool tb_v334 = 1; tb_bset(v_taken, v_slot, tb_v334, 504); }
        }
    }
    { tb_list * tb_v335 = v_ordered; tb_list * *tb_p335 = &(((tb_o_Node *)tb_nn(v_call, 506))->q_kids); *tb_p335 = tb_v335; }
}

static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link) {
    if ((({ const char *tb_s336 = ((tb_o_Node *)tb_nn(v_n, 510))->q_kind; ((unsigned char)tb_s336[0] == 105 && (unsigned char)tb_s336[1] == 110 && (unsigned char)tb_s336[2] == 100 && (unsigned char)tb_s336[3] == 101 && (unsigned char)tb_s336[4] == 120 && tb_s336[5] == 0); }) && f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)), 510))->q_ty))) {
        if (v_for_link) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 512))->q_line, "ярлык на значение в dict нельзя: dict может переложить значения", "");
        }
        return;
    }
    if (({ const char *tb_s337 = ((tb_o_Node *)tb_nn(v_n, 514))->q_kind; ((unsigned char)tb_s337[0] == 110 && (unsigned char)tb_s337[1] == 97 && (unsigned char)tb_s337[2] == 109 && (unsigned char)tb_s337[3] == 101 && tb_s337[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_name);
        if (((v_v >= 0LL) && ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 516))->q_vars, v_v, sizeof(tb_o_Var *), 516)), 516))->q_is_const)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 517))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 517))->q_name);
        }
        return;
    }
    if (({ const char *tb_s338 = ((tb_o_Node *)tb_nn(v_n, 519))->q_kind; ((unsigned char)tb_s338[0] == 102 && (unsigned char)tb_s338[1] == 105 && (unsigned char)tb_s338[2] == 101 && (unsigned char)tb_s338[3] == 108 && (unsigned char)tb_s338[4] == 100 && tb_s338[5] == 0); })) {
        return;
    }
    if ((((({ const char *tb_s339 = ((tb_o_Node *)tb_nn(v_n, 521))->q_kind; ((unsigned char)tb_s339[0] == 105 && (unsigned char)tb_s339[1] == 110 && (unsigned char)tb_s339[2] == 100 && (unsigned char)tb_s339[3] == 101 && (unsigned char)tb_s339[4] == 120 && tb_s339[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty)) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 521))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty, sizeof(tb_o_Type *), 521)), 521))->q_elem == 3LL)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 522))->q_line, "ярлык на элемент list bool нельзя: там каждый элемент — один бит, у бита нет своего адреса", "");
    }
    if (((({ const char *tb_s340 = ((tb_o_Node *)tb_nn(v_n, 523))->q_kind; ((unsigned char)tb_s340[0] == 105 && (unsigned char)tb_s340[1] == 110 && (unsigned char)tb_s340[2] == 100 && (unsigned char)tb_s340[3] == 101 && (unsigned char)tb_s340[4] == 120 && tb_s340[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), 523))->q_ty)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 524))->q_line, "ярлык на элемент списка колонок нельзя: возьми ярлык на качество, например pts[i].x", "");
    }
    if ((({ const char *tb_s341 = ((tb_o_Node *)tb_nn(v_n, 525))->q_kind; ((unsigned char)tb_s341[0] == 105 && (unsigned char)tb_s341[1] == 110 && (unsigned char)tb_s341[2] == 100 && (unsigned char)tb_s341[3] == 101 && (unsigned char)tb_s341[4] == 120 && tb_s341[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)), 525))->q_ty))) {
        return;
    }
    if (({ const char *tb_s342 = ((tb_o_Node *)tb_nn(v_n, 527))->q_kind; ((unsigned char)tb_s342[0] == 105 && (unsigned char)tb_s342[1] == 110 && (unsigned char)tb_s342[2] == 100 && (unsigned char)tb_s342[3] == 101 && (unsigned char)tb_s342[4] == 120 && tb_s342[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 528))->q_line, "буквы в тексте менять нельзя, собери новый текст", "");
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 529))->q_line, v_why, "");
}

static void f_check__need_place(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why) {
    f_check__need_place_for(v_c, v_n, v_why, 0);
}

static long long f_check__check_new_obj(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_o) {
    long long v_r = 0LL;
    f_check__order_args(v_c, v_n, v_o, 1);
    {
        long long tb_end343 = ((((tb_o_Node *)tb_nn(v_n, 536))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end343; v_i++) {
            if (({ const char *tb_s344 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, v_i, sizeof(tb_o_Node *), 537)), 537))->q_kind; ((unsigned char)tb_s344[0] == 109 && (unsigned char)tb_s344[1] == 105 && (unsigned char)tb_s344[2] == 115 && (unsigned char)tb_s344[3] == 115 && (unsigned char)tb_s344[4] == 105 && (unsigned char)tb_s344[5] == 110 && (unsigned char)tb_s344[6] == 103 && tb_s344[7] == 0); })) {
                continue;
            }
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 539))->q_kids, v_i, sizeof(tb_o_Node *), 539)), 539))->q_kids, 0LL, sizeof(tb_o_Node *), 539));
            long long v_t = f_check__check_value(v_c, v_value);
            if ((!f_check__fits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 542))->q_line, "качество другого типа", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 542))->q_kids, v_i, sizeof(tb_o_Node *), 542)), 542))->q_name);
            }
            { long long tb_v345 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty; long long *tb_p345 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty); *tb_p345 = tb_v345; }
        }
    }
    { tb_o_Node * tb_v346 = v_o; tb_o_Node * *tb_p346 = &(((tb_o_Node *)tb_nn(v_n, 544))->q_ref); *tb_p346 = tb_v346; }
    { _Bool tb_v347 = 1; _Bool *tb_p347 = &(((tb_o_Node *)tb_nn(v_n, 545))->q_has_ref); *tb_p347 = tb_v347; }
    return f_check__obj_type(v_c, v_o);
    return v_r;
}

static long long f_check__check_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 549))->q_name)) {
        if ((f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 550))->q_name) && (!((tb_o_Node *)tb_nn(v_n, 550))->q_creates))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 551))->q_line, "это может закончиться ошибкой: напиши  try ...,  ... or запасное  или  ... else err", ((tb_o_Node *)tb_nn(v_n, 551))->q_name);
        }
        return f_check__check_builtin(v_c, v_n);
    }
    if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 553))->q_name) >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 554))->q_line, "это переменная, а не функция", ((tb_o_Node *)tb_nn(v_n, 554))->q_name);
    }
    long long v_o = f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 555))->q_name);
    if ((v_o >= 0LL)) {
        return f_check__check_new_obj(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 557))->q_objs, v_o, sizeof(tb_o_Node *), 557)));
    }
    long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 558))->q_name);
    if ((v_f < 0LL)) {
        f_check__explain_missing_func(v_c, v_n);
    }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_user_call(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_fi) {
    long long v_r = 0LL;
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 565))->q_funcs, v_fi, sizeof(tb_o_Func *), 565));
    if (({ const char *tb_s348 = ((tb_o_Node *)tb_nn(v_n, 566))->q_name; ((unsigned char)tb_s348[0] == 109 && (unsigned char)tb_s348[1] == 97 && (unsigned char)tb_s348[2] == 105 && (unsigned char)tb_s348[3] == 110 && tb_s348[4] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 567))->q_line, "main запускается сама, её нельзя вызывать", "");
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 568))->q_node, 568))->q_can_fail && (!((tb_o_Node *)tb_nn(v_n, 568))->q_creates))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 569))->q_line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", ((tb_o_Node *)tb_nn(v_n, 569))->q_name);
    }
    { tb_o_Node * tb_v349 = ((tb_o_Func *)tb_nn(v_f, 570))->q_node; tb_o_Node * *tb_p349 = &(((tb_o_Node *)tb_nn(v_n, 570))->q_ref); *tb_p349 = tb_v349; }
    { _Bool tb_v350 = 1; _Bool *tb_p350 = &(((tb_o_Node *)tb_nn(v_n, 571))->q_has_ref); *tb_p350 = tb_v350; }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 572))->q_node, 572))->q_kids, 0LL, sizeof(tb_o_Node *), 572));
    f_check__order_args(v_c, v_n, v_params, 0);
    {
        long long tb_end351 = ((((tb_o_Node *)tb_nn(v_n, 574))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end351; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 575))->q_kids, v_i, sizeof(tb_o_Node *), 575));
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 576))->q_kids, v_i, sizeof(tb_o_Node *), 576)), 576))->q_kids, 0LL, sizeof(tb_o_Node *), 576));
            long long v_t = f_check__check_value(v_c, v_value);
            if (((tb_o_Node *)tb_nn(v_p, 578))->q_is_link) {
                f_check__need_place_for(v_c, v_value, "в ярлык link можно передать только переменную, u.age или nums[i]", 1);
                f_check__no_outside_change(v_c, v_value);
                if ((f_check__outside_temp(v_c, f_check__root_var(v_c, v_value)) && (!f_check__is_plain(v_t)))) {
                    f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_value, 582))->q_line, ((tb_o_Node *)tb_nn(v_p, 582))->q_name);
                }
                if ((((tb_o_Node *)tb_nn(v_p, 583))->q_ty < 0LL)) {
                    { long long tb_v352 = v_t; long long *tb_p352 = &(((tb_o_Node *)tb_nn(v_p, 585))->q_ty); *tb_p352 = tb_v352; }
                } else if ((((tb_o_Node *)tb_nn(v_p, 586))->q_ty != v_t)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 587))->q_line, "этот ярлык уже был другого типа", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_p, 587))->q_ty));
                }
            } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_p, 588))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 589))->q_line, "параметр ждёт другой тип", ((tb_o_Node *)tb_nn(v_p, 589))->q_name);
            }
            { long long tb_v353 = ((tb_o_Node *)tb_nn(v_p, 590))->q_ty; long long *tb_p353 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 590))->q_kids, v_i, sizeof(tb_o_Node *), 590)), 590))->q_ty); *tb_p353 = tb_v353; }
        }
    }
    { long long tb_v354 = ((tb_o_Comp *)tb_nn(v_c, 591))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 591))->q_call_from) = tb_v354; }
    { long long tb_v355 = v_fi; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 592))->q_call_to) = tb_v355; }
    { _Bool tb_v356 = (((tb_o_Comp *)tb_nn(v_c, 593))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 593))->q_call_loop, tb_v356); }
    if ((((tb_o_Func *)tb_nn(v_f, 594))->q_state == 0LL)) {
        long long v_saved_current = ((tb_o_Comp *)tb_nn(v_c, 596))->q_current;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 597))->q_loop_depth;
        long long v_saved_vars = (((tb_o_Comp *)tb_nn(v_c, 598))->q_vars)->len;
        long long v_saved_depth = ((tb_o_Comp *)tb_nn(v_c, 599))->q_depth;
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 600))->q_var_base;
        const char * v_saved_mod = ((tb_o_Comp *)tb_nn(v_c, 601))->q_cur_mod;
        const char * v_saved_file = ((tb_o_Comp *)tb_nn(v_c, 602))->q_file_name;
        long long v_saved_par = ((tb_o_Comp *)tb_nn(v_c, 603))->q_par_base;
        long long v_saved_temp = ((tb_o_Comp *)tb_nn(v_c, 604))->q_temp_base;
        long long v_saved_temp_loops = ((tb_o_Comp *)tb_nn(v_c, 605))->q_temp_loops;
        long long v_saved_pieces = ((tb_o_Comp *)tb_nn(v_c, 606))->q_piece_depth;
        tb_list * v_saved_loop_stack = ((tb_o_Comp *)tb_nn(v_c, 607))->q_loops;
        tb_list * v_fresh_loops = tb_list_new(sizeof(tb_o_Node *));
        { tb_list * tb_v357 = v_fresh_loops; tb_list * *tb_p357 = &(((tb_o_Comp *)tb_nn(v_c, 609))->q_loops); *tb_p357 = tb_v357; }
        { long long tb_v358 = (-1LL); long long *tb_p358 = &(((tb_o_Comp *)tb_nn(v_c, 610))->q_par_base); *tb_p358 = tb_v358; }
        { long long tb_v359 = (-1LL); long long *tb_p359 = &(((tb_o_Comp *)tb_nn(v_c, 611))->q_temp_base); *tb_p359 = tb_v359; }
        { long long tb_v360 = 0LL; long long *tb_p360 = &(((tb_o_Comp *)tb_nn(v_c, 612))->q_piece_depth); *tb_p360 = tb_v360; }
        { long long tb_v361 = (((tb_o_Comp *)tb_nn(v_c, 613))->q_vars)->len; long long *tb_p361 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_var_base); *tb_p361 = tb_v361; }
        f_check__check_func(v_c, v_fi);
        { long long tb_v362 = v_saved_pieces; long long *tb_p362 = &(((tb_o_Comp *)tb_nn(v_c, 615))->q_piece_depth); *tb_p362 = tb_v362; }
        { tb_list * tb_v363 = v_saved_loop_stack; tb_list * *tb_p363 = &(((tb_o_Comp *)tb_nn(v_c, 616))->q_loops); *tb_p363 = tb_v363; }
        { long long tb_v364 = v_saved_par; long long *tb_p364 = &(((tb_o_Comp *)tb_nn(v_c, 617))->q_par_base); *tb_p364 = tb_v364; }
        { long long tb_v365 = v_saved_temp; long long *tb_p365 = &(((tb_o_Comp *)tb_nn(v_c, 618))->q_temp_base); *tb_p365 = tb_v365; }
        { long long tb_v366 = v_saved_temp_loops; long long *tb_p366 = &(((tb_o_Comp *)tb_nn(v_c, 619))->q_temp_loops); *tb_p366 = tb_v366; }
        { long long tb_v367 = v_saved_base; long long *tb_p367 = &(((tb_o_Comp *)tb_nn(v_c, 620))->q_var_base); *tb_p367 = tb_v367; }
        { const char * tb_v368 = v_saved_mod; const char * *tb_p368 = &(((tb_o_Comp *)tb_nn(v_c, 621))->q_cur_mod); *tb_p368 = tb_v368; }
        { const char * tb_v369 = v_saved_file; const char * *tb_p369 = &(((tb_o_Comp *)tb_nn(v_c, 622))->q_file_name); *tb_p369 = tb_v369; }
        { long long tb_v370 = v_saved_current; long long *tb_p370 = &(((tb_o_Comp *)tb_nn(v_c, 623))->q_current); *tb_p370 = tb_v370; }
        { long long tb_v371 = v_saved_loops; long long *tb_p371 = &(((tb_o_Comp *)tb_nn(v_c, 624))->q_loop_depth); *tb_p371 = tb_v371; }
        f_check__cut_vars(v_c, v_saved_vars);
        { long long tb_v372 = v_saved_depth; long long *tb_p372 = &(((tb_o_Comp *)tb_nn(v_c, 626))->q_depth); *tb_p372 = tb_v372; }
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 627))->q_node, 627))->q_ty < 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 628))->q_line, "функция вызывает сама себя раньше первого return со значением: поставь простой return выше", ((tb_o_Node *)tb_nn(v_n, 628))->q_name);
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 629))->q_node, 629))->q_ty;
    return v_r;
}

static long long f_check__check_module_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    const char * v_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 633))->q_kids, 0LL, sizeof(tb_o_Node *), 633)), 633))->q_name;
    if ((!f_check__module_uses(v_c, ((tb_o_Comp *)tb_nn(v_c, 634))->q_cur_mod, v_mod))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 635))->q_line, "этот модуль не подключён: добавь в начало файла  use", v_mod);
    }
    long long v_f = f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_n, 636))->q_name, v_mod);
    if ((v_f < 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 638))->q_line, "в этом модуле нет такой функции", ((tb_o_Node *)tb_nn(v_n, 638))->q_name);
    }
    if (((!((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 639))->q_funcs, v_f, sizeof(tb_o_Func *), 639)), 639))->q_node, 639))->q_is_pub) && (strcmp(v_mod, ((tb_o_Comp *)tb_nn(v_c, 639))->q_cur_mod) != 0))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 640))->q_line, "эта функция спрятана в своём модуле: поставь перед ней modpub", ((tb_o_Node *)tb_nn(v_n, 640))->q_name);
    }
    { const char * tb_v373 = "call"; const char * *tb_p373 = &(((tb_o_Node *)tb_nn(v_n, 642))->q_kind); *tb_p373 = tb_v373; }
    tb_list * v_rest = tb_list_new(sizeof(tb_o_Node *));
    {
        long long tb_end374 = ((((tb_o_Node *)tb_nn(v_n, 644))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end374; v_i++) {
            { tb_o_Node * tb_v375 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 645))->q_kids, v_i, sizeof(tb_o_Node *), 645)); *(tb_o_Node * *)tb_grow(v_rest) = tb_v375; }
        }
    }
    { tb_list * tb_v376 = v_rest; tb_list * *tb_p376 = &(((tb_o_Node *)tb_nn(v_n, 646))->q_kids); *tb_p376 = tb_v376; }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    long long v_r = 0LL;
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 650))->q_kids)->len - 1LL);
    tb_o_Type * v_d = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 651))->q_types, v_t, sizeof(tb_o_Type *), 651));
    if ((({ const char *tb_s377 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s377[0] == 104 && (unsigned char)tb_s377[1] == 97 && (unsigned char)tb_s377[2] == 115 && tb_s377[3] == 0); }) || ({ const char *tb_s378 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s378[0] == 114 && (unsigned char)tb_s378[1] == 101 && (unsigned char)tb_s378[2] == 109 && (unsigned char)tb_s378[3] == 111 && (unsigned char)tb_s378[4] == 118 && (unsigned char)tb_s378[5] == 101 && tb_s378[6] == 0); }))) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 654))->q_line, "нужен ровно один ключ", ((tb_o_Node *)tb_nn(v_n, 654))->q_name);
        }
        long long v_k = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655)), 655))->q_kids, 0LL, sizeof(tb_o_Node *), 655)));
        if ((v_k != ((tb_o_Type *)tb_nn(v_d, 656))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_d, 657))->q_key));
        }
        { long long tb_v379 = ((tb_o_Type *)tb_nn(v_d, 658))->q_key; long long *tb_p379 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 658))->q_kids, 1LL, sizeof(tb_o_Node *), 658)), 658))->q_ty); *tb_p379 = tb_v379; }
        if (({ const char *tb_s380 = ((tb_o_Node *)tb_nn(v_n, 659))->q_name; ((unsigned char)tb_s380[0] == 104 && (unsigned char)tb_s380[1] == 97 && (unsigned char)tb_s380[2] == 115 && tb_s380[3] == 0); })) {
            return 3LL;
        }
        return 0LL;
    }
    if ((({ const char *tb_s381 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s381[0] == 107 && (unsigned char)tb_s381[1] == 101 && (unsigned char)tb_s381[2] == 121 && (unsigned char)tb_s381[3] == 115 && tb_s381[4] == 0); }) || ({ const char *tb_s382 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s382[0] == 118 && (unsigned char)tb_s382[1] == 97 && (unsigned char)tb_s382[2] == 108 && (unsigned char)tb_s382[3] == 117 && (unsigned char)tb_s382[4] == 101 && (unsigned char)tb_s382[5] == 115 && tb_s382[6] == 0); }))) {
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 664))->q_line, "пишется без значений", ((tb_o_Node *)tb_nn(v_n, 664))->q_name);
        }
        if (({ const char *tb_s383 = ((tb_o_Node *)tb_nn(v_n, 665))->q_name; ((unsigned char)tb_s383[0] == 107 && (unsigned char)tb_s383[1] == 101 && (unsigned char)tb_s383[2] == 121 && (unsigned char)tb_s383[3] == 115 && tb_s383[4] == 0); })) {
            return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 666))->q_key);
        }
        return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 667))->q_elem);
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 668))->q_line, "у dict нет такого метода (есть .has .remove .keys .values .clear)", ((tb_o_Node *)tb_nn(v_n, 668))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 672))->q_kids, 0LL, sizeof(tb_o_Node *), 672));
    if (((({ const char *tb_s384 = ((tb_o_Node *)tb_nn(v_base, 673))->q_kind; ((unsigned char)tb_s384[0] == 110 && (unsigned char)tb_s384[1] == 97 && (unsigned char)tb_s384[2] == 109 && (unsigned char)tb_s384[3] == 101 && tb_s384[4] == 0); }) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name))) {
        return f_check__check_module_call(v_c, v_n);
    }
    long long v_t = f_check__check_value(v_c, v_base);
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 676))->q_kids)->len - 1LL);
    {
        long long tb_end385 = ((((tb_o_Node *)tb_nn(v_n, 677))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end385; v_i++) {
            if (({ const char *tb_s386 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 678))->q_kids, v_i, sizeof(tb_o_Node *), 678)), 678))->q_name; !(tb_s386[0] == 0); })) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 679))->q_line, "у методов аргументы пишутся без имён", "");
            }
        }
    }
    if ((((({ const char *tb_s387 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s387[0] == 97 && (unsigned char)tb_s387[1] == 100 && (unsigned char)tb_s387[2] == 100 && tb_s387[3] == 0); }) || ({ const char *tb_s388 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s388[0] == 112 && (unsigned char)tb_s388[1] == 111 && (unsigned char)tb_s388[2] == 112 && tb_s388[3] == 0); })) || ({ const char *tb_s389 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s389[0] == 114 && (unsigned char)tb_s389[1] == 101 && (unsigned char)tb_s389[2] == 109 && (unsigned char)tb_s389[3] == 111 && (unsigned char)tb_s389[4] == 118 && (unsigned char)tb_s389[5] == 101 && tb_s389[6] == 0); })) || ({ const char *tb_s390 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s390[0] == 99 && (unsigned char)tb_s390[1] == 108 && (unsigned char)tb_s390[2] == 101 && (unsigned char)tb_s390[3] == 97 && (unsigned char)tb_s390[4] == 114 && tb_s390[5] == 0); }))) {
        f_check__no_outside_change(v_c, v_base);
    }
    if (({ const char *tb_s391 = ((tb_o_Node *)tb_nn(v_n, 682))->q_name; ((unsigned char)tb_s391[0] == 99 && (unsigned char)tb_s391[1] == 108 && (unsigned char)tb_s391[2] == 101 && (unsigned char)tb_s391[3] == 97 && (unsigned char)tb_s391[4] == 114 && tb_s391[5] == 0); })) {
        if (((!f_check__is_list(v_c, v_t)) && (!f_check__is_dict(v_c, v_t)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 685))->q_line, ".clear() есть у списков и dict", "");
        }
        if ((f_check__is_list(v_c, v_t) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 686))->q_types, v_t, sizeof(tb_o_Type *), 686)), 686))->q_elem))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 687))->q_line, "у списка колонок (by columns) .clear() пока нет", "");
        }
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 689))->q_line, ".clear() пишется без значений", "");
        }
        return 0LL;
    }
    if (f_check__is_dict(v_c, v_t)) {
        return f_check__check_dict_method(v_c, v_n, v_t);
    }
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 694))->q_line, "у этого значения нет методов: они есть у списков (.add .pop) и dict (.has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 694))->q_name);
    }
    long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 695))->q_types, v_t, sizeof(tb_o_Type *), 695)), 695))->q_elem;
    if (({ const char *tb_s392 = ((tb_o_Node *)tb_nn(v_n, 696))->q_name; ((unsigned char)tb_s392[0] == 97 && (unsigned char)tb_s392[1] == 100 && (unsigned char)tb_s392[2] == 100 && tb_s392[3] == 0); })) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 698))->q_line, ".add кладёт ровно одно значение", "");
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 699))->q_kids, 1LL, sizeof(tb_o_Node *), 699)), 699))->q_kids, 0LL, sizeof(tb_o_Node *), 699)));
        if ((!f_check__fits(v_elem, v_v))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_line, "в этот список кладут другой тип", f_check__type_name(v_c, v_elem));
        }
        { long long tb_v393 = v_elem; long long *tb_p393 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 702))->q_kids, 1LL, sizeof(tb_o_Node *), 702)), 702))->q_ty); *tb_p393 = tb_v393; }
        return 0LL;
    }
    if (({ const char *tb_s394 = ((tb_o_Node *)tb_nn(v_n, 704))->q_name; ((unsigned char)tb_s394[0] == 112 && (unsigned char)tb_s394[1] == 111 && (unsigned char)tb_s394[2] == 112 && tb_s394[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_elem)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 706))->q_line, "у списка колонок (by columns) .pop() пока нет", "");
        }
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 708))->q_line, ".pop() пишется без значений", "");
        }
        return v_elem;
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 710))->q_line, "у списка нет такого метода (есть .add и .pop)", ((tb_o_Node *)tb_nn(v_n, 710))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_dict(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 714))->q_kids)->len == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 715))->q_line, "пустой dict пишется с типом: d = dict text int", "");
    }
    long long v_key = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 716))->q_kids, 0LL, sizeof(tb_o_Node *), 716)));
    long long v_value = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 717))->q_kids, 1LL, sizeof(tb_o_Node *), 717)));
    if (((v_key != 4LL) && (v_key != 1LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 719))->q_line, "ключ у dict бывает text или int", "");
    }
    long long v_i = 2LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 721))->q_kids)->len)) {
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 722))->q_kids, v_i, sizeof(tb_o_Node *), 722))) != v_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 723))->q_kids, v_i, sizeof(tb_o_Node *), 723)), 723))->q_line, "все ключи одного типа", f_check__type_name(v_c, v_key));
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 724))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 724)));
        if ((!f_check__fits(v_value, v_v))) {
            if (((v_value == 1LL) && (v_v == 2LL))) {
                v_value = 2LL;
            } else {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 729))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 729)), 729))->q_line, "все значения одного типа", f_check__type_name(v_c, v_value));
            }
        }
        { long long tb_v395 = 2LL; long long *tb_p395 = &(v_i); *tb_p395 += tb_v395; }
    }
    return f_check__dict_of(v_c, v_key, v_value);
    return v_r;
}

static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (({ const char *tb_s396 = ((tb_o_Node *)tb_nn(v_n, 735))->q_kind; ((unsigned char)tb_s396[0] == 99 && (unsigned char)tb_s396[1] == 97 && (unsigned char)tb_s396[2] == 108 && (unsigned char)tb_s396[3] == 108 && tb_s396[4] == 0); })) {
        if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 736))->q_name)) {
            return f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 737))->q_name);
        }
        if (((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 738))->q_name) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 738))->q_name) >= 0LL))) {
            return 0;
        }
        long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 740))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 741))->q_funcs, v_f, sizeof(tb_o_Func *), 741)), 741))->q_node, 741))->q_can_fail);
    }
    if ((((({ const char *tb_s397 = ((tb_o_Node *)tb_nn(v_n, 742))->q_kind; ((unsigned char)tb_s397[0] == 109 && (unsigned char)tb_s397[1] == 101 && (unsigned char)tb_s397[2] == 116 && (unsigned char)tb_s397[3] == 104 && (unsigned char)tb_s397[4] == 111 && (unsigned char)tb_s397[5] == 100 && tb_s397[6] == 0); }) && ({ const char *tb_s398 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_kind; ((unsigned char)tb_s398[0] == 110 && (unsigned char)tb_s398[1] == 97 && (unsigned char)tb_s398[2] == 109 && (unsigned char)tb_s398[3] == 101 && tb_s398[4] == 0); })) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_name))) {
        long long v_f = f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_n, 743))->q_name, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 743))->q_kids, 0LL, sizeof(tb_o_Node *), 743)), 743))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 744))->q_funcs, v_f, sizeof(tb_o_Func *), 744)), 744))->q_node, 744))->q_can_fail);
    }
    return 0;
    tb_crash(734, "функция can_fail_call дошла до конца без return");
    return 0;
}

static long long f_check__check_handled(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_how) {
    long long v_r = 0LL;
    if ((!f_check__can_fail_call(v_c, v_n))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 750))->q_line, "этот вызов не может закончиться ошибкой, здесь не нужно", v_how);
    }
    { _Bool tb_v399 = 1; _Bool *tb_p399 = &(((tb_o_Node *)tb_nn(v_n, 751))->q_creates); *tb_p399 = tb_v399; }
    return f_check__check_expr(v_c, v_n);
    return v_r;
}

static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = (-1LL);
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 756))->q_kind;
    if (({ const char *tb_s400 = v_k; ((unsigned char)tb_s400[0] == 110 && (unsigned char)tb_s400[1] == 117 && (unsigned char)tb_s400[2] == 109 && (unsigned char)tb_s400[3] == 98 && (unsigned char)tb_s400[4] == 101 && (unsigned char)tb_s400[5] == 114 && tb_s400[6] == 0); })) {
        v_t = 1LL;
    } else if (({ const char *tb_s401 = v_k; ((unsigned char)tb_s401[0] == 100 && (unsigned char)tb_s401[1] == 101 && (unsigned char)tb_s401[2] == 99 && (unsigned char)tb_s401[3] == 105 && (unsigned char)tb_s401[4] == 109 && (unsigned char)tb_s401[5] == 97 && (unsigned char)tb_s401[6] == 108 && tb_s401[7] == 0); })) {
        v_t = 2LL;
    } else if (({ const char *tb_s402 = v_k; ((unsigned char)tb_s402[0] == 115 && (unsigned char)tb_s402[1] == 116 && (unsigned char)tb_s402[2] == 114 && (unsigned char)tb_s402[3] == 105 && (unsigned char)tb_s402[4] == 110 && (unsigned char)tb_s402[5] == 103 && tb_s402[6] == 0); })) {
        v_t = 4LL;
    } else if (({ const char *tb_s403 = v_k; ((unsigned char)tb_s403[0] == 98 && (unsigned char)tb_s403[1] == 111 && (unsigned char)tb_s403[2] == 111 && (unsigned char)tb_s403[3] == 108 && tb_s403[4] == 0); })) {
        v_t = 3LL;
    } else if (({ const char *tb_s404 = v_k; ((unsigned char)tb_s404[0] == 110 && (unsigned char)tb_s404[1] == 97 && (unsigned char)tb_s404[2] == 109 && (unsigned char)tb_s404[3] == 101 && tb_s404[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 766))->q_name);
        if ((v_v < 0LL)) {
            if ((f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 768))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 769))->q_line, "у функции нужны скобки", ((tb_o_Node *)tb_nn(v_n, 769))->q_name);
            }
            if ((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 770))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 771))->q_line, "чтобы создать obj, нужны скобки", ({ const char *tb_c405_0 = ((tb_o_Node *)tb_nn(v_n, 771))->q_name; long long tb_c405_0n = (long long)strlen(tb_c405_0); const char *tb_c405_1 = "(...)"; long long tb_c405_1n = sizeof("(...)") - 1; char *tb_c405r = tb_mem(tb_c405_0n + tb_c405_1n + 1), *tb_c405rw = tb_c405r; memcpy(tb_c405rw, tb_c405_0, tb_c405_0n); tb_c405rw += tb_c405_0n; memcpy(tb_c405rw, tb_c405_1, tb_c405_1n); tb_c405rw += tb_c405_1n; *tb_c405rw = 0; (const char *)tb_c405r; }));
            }
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 772))->q_line, "нет такой переменной", ((tb_o_Node *)tb_nn(v_n, 772))->q_name);
        }
        if ((f_check__outside_temp(v_c, v_v) && (!f_check__temp_can_see(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 773))->q_vars, v_v, sizeof(tb_o_Var *), 773)), 773))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 774))->q_line, "внутри temp не видно списков, dict и obj снаружи (кроме списков чисел): создай данные внутри temp или передай числа", ((tb_o_Node *)tb_nn(v_n, 774))->q_name);
        }
        if (f_check__outside_par(v_c, v_v)) {
            if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 776))->q_vars, v_v, sizeof(tb_o_Var *), 776)), 776))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 776))->q_shared_ok))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 777))->q_line, "shared внутри on all cores можно только менять через += или -=, а читать — после цикла", ((tb_o_Node *)tb_nn(v_n, 777))->q_name);
            }
            if ((!(((tb_o_Comp *)tb_nn(v_c, 778))->q_par_vec && ((tb_o_Comp *)tb_nn(v_c, 778))->q_shared_ok))) {
                f_check__capture(v_c, v_v);
            }
        }
        { _Bool tb_v406 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 780))->q_vars, v_v, sizeof(tb_o_Var *), 780)), 780))->q_is_link; _Bool *tb_p406 = &(((tb_o_Node *)tb_nn(v_n, 780))->q_is_link); *tb_p406 = tb_v406; }
        v_t = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 781))->q_vars, v_v, sizeof(tb_o_Var *), 781)), 781))->q_type;
    } else if (({ const char *tb_s407 = v_k; ((unsigned char)tb_s407[0] == 108 && (unsigned char)tb_s407[1] == 105 && (unsigned char)tb_s407[2] == 115 && (unsigned char)tb_s407[3] == 116 && tb_s407[4] == 0); })) {
        v_t = f_check__check_list(v_c, v_n);
    } else if (({ const char *tb_s408 = v_k; ((unsigned char)tb_s408[0] == 100 && (unsigned char)tb_s408[1] == 105 && (unsigned char)tb_s408[2] == 99 && (unsigned char)tb_s408[3] == 116 && tb_s408[4] == 0); })) {
        v_t = f_check__check_dict(v_c, v_n);
    } else if (({ const char *tb_s409 = v_k; ((unsigned char)tb_s409[0] == 116 && (unsigned char)tb_s409[1] == 114 && (unsigned char)tb_s409[2] == 121 && tb_s409[3] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 787))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 788))->q_line, ({ const char *tb_c410_0 = "внутри "; long long tb_c410_0n = sizeof("внутри ") - 1; const char *tb_c410_1 = f_check__loop_word(v_c); long long tb_c410_1n = (long long)strlen(tb_c410_1); const char *tb_c410_2 = " try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err"; long long tb_c410_2n = sizeof(" try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err") - 1; char *tb_c410r = tb_mem(tb_c410_0n + tb_c410_1n + tb_c410_2n + 1), *tb_c410rw = tb_c410r; memcpy(tb_c410rw, tb_c410_0, tb_c410_0n); tb_c410rw += tb_c410_0n; memcpy(tb_c410rw, tb_c410_1, tb_c410_1n); tb_c410rw += tb_c410_1n; memcpy(tb_c410rw, tb_c410_2, tb_c410_2n); tb_c410rw += tb_c410_2n; *tb_c410rw = 0; (const char *)tb_c410r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 789))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 790))->q_line, "внутри temp try нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 791))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 792))->q_line, "внутри each ... by N try нельзя: пиши  or  или  else err", "");
        }
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 793))->q_kids, 0LL, sizeof(tb_o_Node *), 793)), "try");
    } else if (({ const char *tb_s411 = v_k; ((unsigned char)tb_s411[0] == 101 && (unsigned char)tb_s411[1] == 108 && (unsigned char)tb_s411[2] == 115 && (unsigned char)tb_s411[3] == 101 && (unsigned char)tb_s411[4] == 95 && (unsigned char)tb_s411[5] == 101 && (unsigned char)tb_s411[6] == 114 && (unsigned char)tb_s411[7] == 114 && tb_s411[8] == 0); })) {
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 795))->q_kids, 0LL, sizeof(tb_o_Node *), 795)), "else");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 797))->q_name, 4LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 797))->q_line);
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 798))->q_kids, 1LL, sizeof(tb_o_Node *), 798)));
        f_check__close_scope(v_c);
    } else if (({ const char *tb_s412 = v_k; ((unsigned char)tb_s412[0] == 99 && (unsigned char)tb_s412[1] == 97 && (unsigned char)tb_s412[2] == 108 && (unsigned char)tb_s412[3] == 108 && tb_s412[4] == 0); })) {
        v_t = f_check__check_call(v_c, v_n);
    } else if (({ const char *tb_s413 = v_k; ((unsigned char)tb_s413[0] == 109 && (unsigned char)tb_s413[1] == 101 && (unsigned char)tb_s413[2] == 116 && (unsigned char)tb_s413[3] == 104 && (unsigned char)tb_s413[4] == 111 && (unsigned char)tb_s413[5] == 100 && tb_s413[6] == 0); })) {
        v_t = f_check__check_method(v_c, v_n);
    } else if (({ const char *tb_s414 = v_k; ((unsigned char)tb_s414[0] == 98 && (unsigned char)tb_s414[1] == 105 && (unsigned char)tb_s414[2] == 110 && (unsigned char)tb_s414[3] == 111 && (unsigned char)tb_s414[4] == 112 && tb_s414[5] == 0); })) {
        v_t = f_check__check_binop(v_c, v_n);
    } else if (({ const char *tb_s415 = v_k; ((unsigned char)tb_s415[0] == 117 && (unsigned char)tb_s415[1] == 110 && (unsigned char)tb_s415[2] == 97 && (unsigned char)tb_s415[3] == 114 && (unsigned char)tb_s415[4] == 121 && tb_s415[5] == 0); })) {
        long long v_inner = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 807))->q_kids, 0LL, sizeof(tb_o_Node *), 807)));
        if (({ const char *tb_s416 = ((tb_o_Node *)tb_nn(v_n, 808))->q_value; ((unsigned char)tb_s416[0] == 110 && (unsigned char)tb_s416[1] == 111 && (unsigned char)tb_s416[2] == 116 && tb_s416[3] == 0); })) {
            if ((v_inner != 3LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 810))->q_line, "not работает только с true и false", "");
            }
            v_t = 3LL;
        } else {
            if ((!f_check__is_number(v_inner))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 814))->q_line, "минус ставится только перед числом", "");
            }
            v_t = v_inner;
        }
    } else if ((({ const char *tb_s417 = v_k; ((unsigned char)tb_s417[0] == 105 && (unsigned char)tb_s417[1] == 110 && (unsigned char)tb_s417[2] == 100 && (unsigned char)tb_s417[3] == 101 && (unsigned char)tb_s417[4] == 120 && tb_s417[5] == 0); }) && f_check__is_dict(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 816))->q_kids, 0LL, sizeof(tb_o_Node *), 816)))))) {
        tb_o_Type * v_base = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 817))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 817))->q_kids, 0LL, sizeof(tb_o_Node *), 817)), 817))->q_ty, sizeof(tb_o_Type *), 817));
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 818))->q_kids, 1LL, sizeof(tb_o_Node *), 818))) != ((tb_o_Type *)tb_nn(v_base, 818))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 819))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_base, 819))->q_key));
        }
        v_t = ((tb_o_Type *)tb_nn(v_base, 820))->q_elem;
    } else if (({ const char *tb_s418 = v_k; ((unsigned char)tb_s418[0] == 105 && (unsigned char)tb_s418[1] == 110 && (unsigned char)tb_s418[2] == 100 && (unsigned char)tb_s418[3] == 101 && (unsigned char)tb_s418[4] == 120 && tb_s418[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 822))->q_kids, 0LL, sizeof(tb_o_Node *), 822)));
        long long v_index = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 823))->q_kids, 1LL, sizeof(tb_o_Node *), 823)));
        if ((v_index != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 825))->q_line, "номер в списке должен быть int", "");
        }
        if ((v_base == 4LL)) {
            v_t = 4LL;
        } else if (f_check__is_list(v_c, v_base)) {
            v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 829))->q_types, v_base, sizeof(tb_o_Type *), 829)), 829))->q_elem;
        } else {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 831))->q_line, "[номер] берут у списка или текста", "");
        }
    } else if (((({ const char *tb_s419 = v_k; ((unsigned char)tb_s419[0] == 102 && (unsigned char)tb_s419[1] == 105 && (unsigned char)tb_s419[2] == 101 && (unsigned char)tb_s419[3] == 108 && (unsigned char)tb_s419[4] == 100 && tb_s419[5] == 0); }) && ({ const char *tb_s420 = ((tb_o_Node *)tb_nn(v_n, 832))->q_name; ((unsigned char)tb_s420[0] == 119 && (unsigned char)tb_s420[1] == 97 && (unsigned char)tb_s420[2] == 105 && (unsigned char)tb_s420[3] == 116 && tb_s420[4] == 0); })) && ({ const char *tb_s421 = f_check__kind_of(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 832))->q_kids, 0LL, sizeof(tb_o_Node *), 832)))); ((unsigned char)tb_s421[0] == 116 && (unsigned char)tb_s421[1] == 97 && (unsigned char)tb_s421[2] == 115 && (unsigned char)tb_s421[3] == 107 && tb_s421[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 833))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 834))->q_line, ({ const char *tb_c422_0 = "внутри "; long long tb_c422_0n = sizeof("внутри ") - 1; const char *tb_c422_1 = f_check__loop_word(v_c); long long tb_c422_1n = (long long)strlen(tb_c422_1); const char *tb_c422_2 = " .wait нельзя: забери готовое до цикла или после"; long long tb_c422_2n = sizeof(" .wait нельзя: забери готовое до цикла или после") - 1; char *tb_c422r = tb_mem(tb_c422_0n + tb_c422_1n + tb_c422_2n + 1), *tb_c422rw = tb_c422r; memcpy(tb_c422rw, tb_c422_0, tb_c422_0n); tb_c422rw += tb_c422_0n; memcpy(tb_c422rw, tb_c422_1, tb_c422_1n); tb_c422rw += tb_c422_1n; memcpy(tb_c422rw, tb_c422_2, tb_c422_2n); tb_c422rw += tb_c422_2n; *tb_c422rw = 0; (const char *)tb_c422r; }), "");
        }
        v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 835))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 835))->q_kids, 0LL, sizeof(tb_o_Node *), 835)), 835))->q_ty, sizeof(tb_o_Type *), 835)), 835))->q_elem;
    } else if ((strcmp(v_k, "background") == 0)) {
        v_t = f_check__check_background(v_c, v_n);
    } else if (({ const char *tb_s423 = v_k; ((unsigned char)tb_s423[0] == 102 && (unsigned char)tb_s423[1] == 105 && (unsigned char)tb_s423[2] == 101 && (unsigned char)tb_s423[3] == 108 && (unsigned char)tb_s423[4] == 100 && tb_s423[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 839))->q_kids, 0LL, sizeof(tb_o_Node *), 839)));
        if (({ const char *tb_s424 = f_check__kind_of(v_c, v_base); !((unsigned char)tb_s424[0] == 111 && (unsigned char)tb_s424[1] == 98 && (unsigned char)tb_s424[2] == 106 && tb_s424[3] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 841))->q_line, "точка с качеством бывает только у obj", ((tb_o_Node *)tb_nn(v_n, 841))->q_name);
        }
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 842))->q_types, v_base, sizeof(tb_o_Type *), 842)), 842))->q_obj_node;
        long long v_field = f_check__find_field(v_o, ((tb_o_Node *)tb_nn(v_n, 843))->q_name);
        if ((v_field < 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 845))->q_line, "у этого obj нет такого качества", ((tb_o_Node *)tb_nn(v_n, 845))->q_name);
        }
        v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 846))->q_kids, v_field, sizeof(tb_o_Node *), 846)), 846))->q_ty;
        { tb_o_Node * tb_v425 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 847))->q_kids, v_field, sizeof(tb_o_Node *), 847)); *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 847))->q_acc_field) = tb_v425; }
        { long long tb_v426 = ((tb_o_Comp *)tb_nn(v_c, 848))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 848))->q_acc_func) = tb_v426; }
        { _Bool tb_v427 = (((tb_o_Comp *)tb_nn(v_c, 849))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 849))->q_acc_loop, tb_v427); }
    }
    { long long tb_v428 = v_t; long long *tb_p428 = &(((tb_o_Node *)tb_nn(v_n, 850))->q_ty); *tb_p428 = tb_v428; }
    if (({ const char *tb_s429 = v_k; ((unsigned char)tb_s429[0] == 105 && (unsigned char)tb_s429[1] == 110 && (unsigned char)tb_s429[2] == 100 && (unsigned char)tb_s429[3] == 101 && (unsigned char)tb_s429[4] == 120 && tb_s429[5] == 0); })) {
        f_check__note_prefetch(v_c, v_n);
    }
    return v_t;
    return v_r;
}

static void f_check__note_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (((((tb_o_Comp *)tb_nn(v_c, 859))->q_loops)->len == 0LL)) {
        return;
    }
    tb_o_Node * v_loop = (*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 861))->q_loops, ((((tb_o_Comp *)tb_nn(v_c, 861))->q_loops)->len - 1LL), sizeof(tb_o_Node *), 861));
    if (((((tb_o_Node *)tb_nn(v_loop, 862))->q_is_par || ((tb_o_Node *)tb_nn(v_loop, 862))->q_by_vec) || ((tb_o_Node *)tb_nn(v_loop, 862))->q_by_piece)) {
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 864))->q_kids, 0LL, sizeof(tb_o_Node *), 864));
    if ((({ const char *tb_s430 = ((tb_o_Node *)tb_nn(v_a, 865))->q_kind; !((unsigned char)tb_s430[0] == 110 && (unsigned char)tb_s430[1] == 97 && (unsigned char)tb_s430[2] == 109 && (unsigned char)tb_s430[3] == 101 && tb_s430[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_a, 865))->q_name, ((tb_o_Node *)tb_nn(v_loop, 865))->q_name)))) {
        return;
    }
    if ((!((f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty))) || f_check__is_dict(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty)))) {
        return;
    }
    tb_o_Node * v_i = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 869))->q_kids, 1LL, sizeof(tb_o_Node *), 869));
    tb_o_Node * v_plan = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    if (({ const char *tb_s431 = ((tb_o_Node *)tb_nn(v_loop, 871))->q_kind; ((unsigned char)tb_s431[0] == 101 && (unsigned char)tb_s431[1] == 97 && (unsigned char)tb_s431[2] == 99 && (unsigned char)tb_s431[3] == 104 && tb_s431[4] == 0); })) {
        if (((!f_check__is_name_of(v_i, ((tb_o_Node *)tb_nn(v_loop, 872))->q_name)) || ((!f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_kids, 0LL, sizeof(tb_o_Node *), 872)), 872))->q_ty)) && (!f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_kids, 0LL, sizeof(tb_o_Node *), 872)), 872))->q_ty))))) {
            return;
        }
    } else {
        if ((({ const char *tb_s432 = ((tb_o_Node *)tb_nn(v_i, 875))->q_kind; !((unsigned char)tb_s432[0] == 105 && (unsigned char)tb_s432[1] == 110 && (unsigned char)tb_s432[2] == 100 && (unsigned char)tb_s432[3] == 101 && (unsigned char)tb_s432[4] == 120 && tb_s432[5] == 0); }) || (!f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 875))->q_kids, 1LL, sizeof(tb_o_Node *), 875)), ((tb_o_Node *)tb_nn(v_loop, 875))->q_name)))) {
            return;
        }
        v_plan = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 877))->q_kids, 0LL, sizeof(tb_o_Node *), 877));
        if ((((({ const char *tb_s433 = ((tb_o_Node *)tb_nn(v_plan, 878))->q_kind; !((unsigned char)tb_s433[0] == 110 && (unsigned char)tb_s433[1] == 97 && (unsigned char)tb_s433[2] == 109 && (unsigned char)tb_s433[3] == 101 && tb_s433[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_name, ((tb_o_Node *)tb_nn(v_loop, 878))->q_name))) || (!f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_ty))) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_ty))) {
            return;
        }
    }
    {
        long long tb_end434 = ((((tb_o_Node *)tb_nn(v_loop, 880))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end434; v_j++) {
            if (((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 881))->q_pre_bases, v_j, sizeof(tb_o_Node *), 881)), 881))->q_name, ((tb_o_Node *)tb_nn(v_a, 881))->q_name) == 0) && (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 881))->q_pre_idx, v_j, sizeof(tb_o_Node *), 881)), 881))->q_name, ((tb_o_Node *)tb_nn(v_plan, 881))->q_name) == 0))) {
                return;
            }
        }
    }
    { tb_o_Node * tb_v435 = v_a; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 883))->q_pre_bases) = tb_v435; }
    { tb_o_Node * tb_v436 = v_plan; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 884))->q_pre_idx) = tb_v436; }
}

static _Bool f_check__declared_before(tb_o_Comp * v_c, const char * v_name, const char * v_loop_var) {
    long long v_a = f_check__find_var(v_c, v_name);
    long long v_b = f_check__find_var(v_c, v_loop_var);
    return (((v_a >= 0LL) && (v_b >= 0LL)) && (v_a < v_b));
    tb_crash(887, "функция declared_before дошла до конца без return");
    return 0;
}

static void f_check__need_bool(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((f_check__check_value(v_c, v_n) != 3LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 894))->q_line, "условие должно быть true или false (например a > 3)", "");
    }
}

static void f_check__need_int(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_what) {
    if ((f_check__check_value(v_c, v_n) != 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 898))->q_line, "здесь нужно целое число int", v_what);
    }
}

static void f_check__check_assign(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_declared = (-1LL);
    if (((tb_o_Node *)tb_nn(v_n, 904))->q_has_tsyn) {
        v_declared = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_n, 905))->q_tsyn);
    }
    long long v_t = (-1LL);
    if (((((tb_o_Node *)tb_nn(v_n, 907))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 908))->q_kids, 0LL, sizeof(tb_o_Node *), 908)));
    }
    long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 909))->q_name);
    if (((tb_o_Node *)tb_nn(v_n, 910))->q_is_shared) {
        if ((v_v >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 912))->q_line, "shared пишется только у новой переменной", ((tb_o_Node *)tb_nn(v_n, 912))->q_name);
        }
        if (((((((v_declared != 1LL) && (v_declared != 2LL)) && (v_declared != 4LL)) && (v_t != 1LL)) && (v_t != 2LL)) && (v_t != 4LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 914))->q_line, "shared бывает int, dec или text: shared y = int 0, shared out = \"\"", ((tb_o_Node *)tb_nn(v_n, 914))->q_name);
        }
    }
    if ((v_v < 0LL)) {
        long long v_type = v_t;
        if ((v_declared >= 0LL)) {
            if (((v_t >= 0LL) && (!f_check__fits(v_declared, v_t)))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 919))->q_line, "значение не того типа, что написан", f_check__type_name(v_c, v_declared));
            }
            v_type = v_declared;
        }
        { _Bool tb_v437 = 1; _Bool *tb_p437 = &(((tb_o_Node *)tb_nn(v_n, 921))->q_creates); *tb_p437 = tb_v437; }
        { long long tb_v438 = v_type; long long *tb_p438 = &(((tb_o_Node *)tb_nn(v_n, 922))->q_ty); *tb_p438 = tb_v438; }
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 923))->q_name, v_type, ((tb_o_Node *)tb_nn(v_n, 923))->q_is_const, 0, ((tb_o_Node *)tb_nn(v_n, 923))->q_line);
        { _Bool tb_v439 = ((tb_o_Node *)tb_nn(v_n, 924))->q_is_shared; _Bool *tb_p439 = &(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 924))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 924))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 924)), 924))->q_is_shared); *tb_p439 = tb_v439; }
        return;
    }
    if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 926))->q_vars, v_v, sizeof(tb_o_Var *), 926)), 926))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 927))->q_line, ((tb_o_Node *)tb_nn(v_n, 927))->q_name);
    }
    if (f_check__outside_par(v_c, v_v)) {
        if (((tb_o_Comp *)tb_nn(v_c, 929))->q_par_vec) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 930))->q_line, "внутри by vec нельзя менять переменную снаружи: копи её через += или -=", ((tb_o_Node *)tb_nn(v_n, 930))->q_name);
        }
        if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 931))->q_vars, v_v, sizeof(tb_o_Var *), 931)), 931))->q_is_shared) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 932))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_n, 932))->q_name);
        }
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 933))->q_line, ({ const char *tb_c440_0 = "внутри "; long long tb_c440_0n = sizeof("внутри ") - 1; const char *tb_c440_1 = f_check__loop_word(v_c); long long tb_c440_1n = (long long)strlen(tb_c440_1); const char *tb_c440_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c440_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c440r = tb_mem(tb_c440_0n + tb_c440_1n + tb_c440_2n + 1), *tb_c440rw = tb_c440r; memcpy(tb_c440rw, tb_c440_0, tb_c440_0n); tb_c440rw += tb_c440_0n; memcpy(tb_c440rw, tb_c440_1, tb_c440_1n); tb_c440rw += tb_c440_1n; memcpy(tb_c440rw, tb_c440_2, tb_c440_2n); tb_c440rw += tb_c440_2n; *tb_c440rw = 0; (const char *)tb_c440r; }), ((tb_o_Node *)tb_nn(v_n, 933))->q_name);
    }
    if ((((tb_o_Node *)tb_nn(v_n, 934))->q_is_const || (v_declared >= 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 935))->q_line, "переменная уже есть: тип и const пишутся только в первый раз", ((tb_o_Node *)tb_nn(v_n, 935))->q_name);
    }
    if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 936))->q_vars, v_v, sizeof(tb_o_Var *), 936)), 936))->q_is_const) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 937))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 937))->q_name);
    }
    if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 938))->q_vars, v_v, sizeof(tb_o_Var *), 938)), 938))->q_type, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 939))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 939))->q_vars, v_v, sizeof(tb_o_Var *), 939)), 939))->q_type));
    }
    { _Bool tb_v441 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 940))->q_vars, v_v, sizeof(tb_o_Var *), 940)), 940))->q_is_link; _Bool *tb_p441 = &(((tb_o_Node *)tb_nn(v_n, 940))->q_is_link); *tb_p441 = tb_v441; }
    { long long tb_v442 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 941))->q_vars, v_v, sizeof(tb_o_Var *), 941)), 941))->q_type; long long *tb_p442 = &(((tb_o_Node *)tb_nn(v_n, 941))->q_ty); *tb_p442 = tb_v442; }
}

static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 944))->q_kids, 0LL, sizeof(tb_o_Node *), 944));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 945))->q_value;
    { _Bool tb_v443 = (({ const char *tb_s444 = ((tb_o_Node *)tb_nn(v_target, 946))->q_kind; ((unsigned char)tb_s444[0] == 110 && (unsigned char)tb_s444[1] == 97 && (unsigned char)tb_s444[2] == 109 && (unsigned char)tb_s444[3] == 101 && tb_s444[4] == 0); }) && (({ const char *tb_s445 = v_op; ((unsigned char)tb_s445[0] == 43 && (unsigned char)tb_s445[1] == 61 && tb_s445[2] == 0); }) || ({ const char *tb_s446 = v_op; ((unsigned char)tb_s446[0] == 45 && (unsigned char)tb_s446[1] == 61 && tb_s446[2] == 0); }))); _Bool *tb_p443 = &(((tb_o_Comp *)tb_nn(v_c, 946))->q_shared_ok); *tb_p443 = tb_v443; }
    long long v_to_ty = f_check__check_value(v_c, v_target);
    { _Bool tb_v447 = 0; _Bool *tb_p447 = &(((tb_o_Comp *)tb_nn(v_c, 948))->q_shared_ok); *tb_p447 = tb_v447; }
    f_check__need_place(v_c, v_target, "менять можно только переменную, u.age или nums[i]");
    long long v_r = f_check__root_var(v_c, v_target);
    if (((f_check__outside_temp(v_c, v_r) && ({ const char *tb_s448 = ((tb_o_Node *)tb_nn(v_target, 951))->q_kind; ((unsigned char)tb_s448[0] == 110 && (unsigned char)tb_s448[1] == 97 && (unsigned char)tb_s448[2] == 109 && (unsigned char)tb_s448[3] == 101 && tb_s448[4] == 0); })) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 951))->q_vars, v_r, sizeof(tb_o_Var *), 951)), 951))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 952))->q_line, ((tb_o_Node *)tb_nn(v_target, 952))->q_name);
    }
    if ((f_check__outside_par(v_c, v_r) && ((tb_o_Comp *)tb_nn(v_c, 953))->q_par_vec)) {
        if (({ const char *tb_s449 = ((tb_o_Node *)tb_nn(v_target, 954))->q_kind; !((unsigned char)tb_s449[0] == 110 && (unsigned char)tb_s449[1] == 97 && (unsigned char)tb_s449[2] == 109 && (unsigned char)tb_s449[3] == 101 && tb_s449[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 955))->q_line, "внутри by vec нельзя менять списки и obj снаружи цикла", "");
        }
        tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 956))->q_vars, v_r, sizeof(tb_o_Var *), 956));
        if (((({ const char *tb_s450 = v_op; !((unsigned char)tb_s450[0] == 43 && (unsigned char)tb_s450[1] == 61 && tb_s450[2] == 0); }) && ({ const char *tb_s451 = v_op; !((unsigned char)tb_s451[0] == 45 && (unsigned char)tb_s451[1] == 61 && tb_s451[2] == 0); })) || (!f_check__is_number(((tb_o_Var *)tb_nn(v_var, 957))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 958))->q_line, "внутри by vec число снаружи можно только копить через += или -=", ((tb_o_Node *)tb_nn(v_target, 958))->q_name);
        }
        _Bool v_found = 0;
        {
            tb_list * tb_l452 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 960))->q_par_node, 960))->q_caps;
            for (long long tb_i452 = 0; tb_i452 < tb_l452->len; tb_i452++) {
                tb_o_Var * const v_old = ((tb_o_Var * *)tb_l452->data)[tb_i452];
                if (((strcmp(((tb_o_Var *)tb_nn(v_old, 961))->q_name, ((tb_o_Var *)tb_nn(v_var, 961))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_old, 961))->q_is_shared)) {
                    v_found = 1;
                    if ((strcmp(((tb_o_Var *)tb_nn(v_old, 963))->q_op, v_op) != 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 964))->q_line, "в by vec одну переменную копят одним знаком: или +=, или -=", ((tb_o_Node *)tb_nn(v_target, 964))->q_name);
                    }
                }
            }
        }
        if ((!v_found)) {
            { tb_o_Var * tb_v453 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 966))->q_name, ((tb_o_Var *)tb_nn(v_var, 966))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 966))->q_is_link, 0LL, 1, v_op); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 966))->q_par_node, 966))->q_caps) = tb_v453; }
        }
    } else if (f_check__outside_par(v_c, v_r)) {
        if (({ const char *tb_s454 = ((tb_o_Node *)tb_nn(v_target, 968))->q_kind; !((unsigned char)tb_s454[0] == 110 && (unsigned char)tb_s454[1] == 97 && (unsigned char)tb_s454[2] == 109 && (unsigned char)tb_s454[3] == 101 && tb_s454[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 969))->q_line, ({ const char *tb_c455_0 = "внутри "; long long tb_c455_0n = sizeof("внутри ") - 1; const char *tb_c455_1 = f_check__loop_word(v_c); long long tb_c455_1n = (long long)strlen(tb_c455_1); const char *tb_c455_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c455_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c455r = tb_mem(tb_c455_0n + tb_c455_1n + tb_c455_2n + 1), *tb_c455rw = tb_c455r; memcpy(tb_c455rw, tb_c455_0, tb_c455_0n); tb_c455rw += tb_c455_0n; memcpy(tb_c455rw, tb_c455_1, tb_c455_1n); tb_c455rw += tb_c455_1n; memcpy(tb_c455rw, tb_c455_2, tb_c455_2n); tb_c455rw += tb_c455_2n; *tb_c455rw = 0; (const char *)tb_c455r; }), "");
        }
        if ((!((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 970))->q_vars, v_r, sizeof(tb_o_Var *), 970)), 970))->q_is_shared)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 971))->q_line, ({ const char *tb_c456_0 = "внутри "; long long tb_c456_0n = sizeof("внутри ") - 1; const char *tb_c456_1 = f_check__loop_word(v_c); long long tb_c456_1n = (long long)strlen(tb_c456_1); const char *tb_c456_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c456_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c456r = tb_mem(tb_c456_0n + tb_c456_1n + tb_c456_2n + 1), *tb_c456rw = tb_c456r; memcpy(tb_c456rw, tb_c456_0, tb_c456_0n); tb_c456rw += tb_c456_0n; memcpy(tb_c456rw, tb_c456_1, tb_c456_1n); tb_c456rw += tb_c456_1n; memcpy(tb_c456rw, tb_c456_2, tb_c456_2n); tb_c456rw += tb_c456_2n; *tb_c456rw = 0; (const char *)tb_c456r; }), ((tb_o_Node *)tb_nn(v_target, 971))->q_name);
        }
        if ((({ const char *tb_s457 = v_op; !((unsigned char)tb_s457[0] == 43 && (unsigned char)tb_s457[1] == 61 && tb_s457[2] == 0); }) && ({ const char *tb_s458 = v_op; !((unsigned char)tb_s458[0] == 45 && (unsigned char)tb_s458[1] == 61 && tb_s458[2] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 973))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_target, 973))->q_name);
        }
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 974))->q_kids, 1LL, sizeof(tb_o_Node *), 974)));
    { long long tb_v459 = v_to_ty; long long *tb_p459 = &(((tb_o_Node *)tb_nn(v_n, 975))->q_ty); *tb_p459 = tb_v459; }
    if (({ const char *tb_s460 = v_op; ((unsigned char)tb_s460[0] == 61 && tb_s460[1] == 0); })) {
        if ((!f_check__fits(v_to_ty, v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 978))->q_line, "здесь лежит другой тип", f_check__type_name(v_c, v_to_ty));
        }
        return;
    }
    if (((v_to_ty == 4LL) && ({ const char *tb_s461 = v_op; ((unsigned char)tb_s461[0] == 43 && (unsigned char)tb_s461[1] == 61 && tb_s461[2] == 0); }))) {
        if ((!f_check__is_scalar(v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 982))->q_line, "к тексту можно приписать только текст, число или bool", "");
        }
        return;
    }
    if (((!f_check__is_number(v_to_ty)) || (!f_check__is_number(v_t)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 985))->q_line, "эта операция работает только с числами", v_op);
    }
    if (((v_to_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 987))->q_line, "в int нельзя добавить дробь: сделай его dec", "");
    }
}

static void f_check__check_linkdecl(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 990))->q_kids, 0LL, sizeof(tb_o_Node *), 990));
    long long v_t = f_check__check_value(v_c, v_target);
    f_check__need_place_for(v_c, v_target, "ярлык link ведёт к переменной, u.age или nums[i]", 1);
    f_check__no_outside_change(v_c, v_target);
    if ((f_check__outside_temp(v_c, f_check__root_var(v_c, v_target)) && (!f_check__is_plain(v_t)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 995))->q_line, ((tb_o_Node *)tb_nn(v_n, 995))->q_name);
    }
    { long long tb_v462 = v_t; long long *tb_p462 = &(((tb_o_Node *)tb_nn(v_n, 996))->q_ty); *tb_p462 = tb_v462; }
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 997))->q_name, v_t, 0, 1, ((tb_o_Node *)tb_nn(v_n, 997))->q_line);
}

static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1000))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1000))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1000));
    tb_o_Node * v_call = v_value;
    if ((({ const char *tb_s463 = ((tb_o_Node *)tb_nn(v_value, 1002))->q_kind; ((unsigned char)tb_s463[0] == 116 && (unsigned char)tb_s463[1] == 114 && (unsigned char)tb_s463[2] == 121 && tb_s463[3] == 0); }) || ({ const char *tb_s464 = ((tb_o_Node *)tb_nn(v_value, 1002))->q_kind; ((unsigned char)tb_s464[0] == 101 && (unsigned char)tb_s464[1] == 108 && (unsigned char)tb_s464[2] == 115 && (unsigned char)tb_s464[3] == 101 && (unsigned char)tb_s464[4] == 95 && (unsigned char)tb_s464[5] == 101 && (unsigned char)tb_s464[6] == 114 && (unsigned char)tb_s464[7] == 114 && tb_s464[8] == 0); }))) {
        v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1003))->q_kids, 0LL, sizeof(tb_o_Node *), 1003));
    }
    if ((({ const char *tb_s465 = ((tb_o_Node *)tb_nn(v_call, 1004))->q_kind; !((unsigned char)tb_s465[0] == 99 && (unsigned char)tb_s465[1] == 97 && (unsigned char)tb_s465[2] == 108 && (unsigned char)tb_s465[3] == 108 && tb_s465[4] == 0); }) && ({ const char *tb_s466 = ((tb_o_Node *)tb_nn(v_call, 1004))->q_kind; !((unsigned char)tb_s466[0] == 109 && (unsigned char)tb_s466[1] == 101 && (unsigned char)tb_s466[2] == 116 && (unsigned char)tb_s466[3] == 104 && (unsigned char)tb_s466[4] == 111 && (unsigned char)tb_s466[5] == 100 && tb_s466[6] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1005))->q_line, "справа должен быть вызов функции с gives", "");
    }
    long long v_t = f_check__check_expr(v_c, v_value);
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 1007))->q_kids)->len - 1LL);
    if (({ const char *tb_s467 = f_check__kind_of(v_c, v_t); !((unsigned char)tb_s467[0] == 109 && (unsigned char)tb_s467[1] == 117 && (unsigned char)tb_s467[2] == 108 && (unsigned char)tb_s467[3] == 116 && (unsigned char)tb_s467[4] == 105 && tb_s467[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1009))->q_line, "у этой функции один выход, пиши  x = ...", ((tb_o_Node *)tb_nn(v_call, 1009))->q_name);
    }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1010))->q_types, v_t, sizeof(tb_o_Type *), 1010)), 1010))->q_func_node, 1010))->q_kids, 2LL, sizeof(tb_o_Node *), 1010));
    if (((((tb_o_Node *)tb_nn(v_outputs, 1011))->q_kids)->len != v_names)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1012))->q_line, "у функции другое число выходов", ((tb_o_Node *)tb_nn(v_call, 1012))->q_name);
    }
    {
        long long tb_end468 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end468; v_i++) {
            tb_o_Node * v_name = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1014))->q_kids, v_i, sizeof(tb_o_Node *), 1014));
            long long v_out = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1015))->q_kids, v_i, sizeof(tb_o_Node *), 1015)), 1015))->q_ty;
            long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1016))->q_name);
            if ((v_v < 0LL)) {
                { _Bool tb_v469 = 1; _Bool *tb_p469 = &(((tb_o_Node *)tb_nn(v_name, 1018))->q_creates); *tb_p469 = tb_v469; }
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1019))->q_name, v_out, 0, 0, ((tb_o_Node *)tb_nn(v_name, 1019))->q_line);
            } else {
                if (f_check__outside_par(v_c, v_v)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1022))->q_line, ({ const char *tb_c470_0 = "внутри "; long long tb_c470_0n = sizeof("внутри ") - 1; const char *tb_c470_1 = f_check__loop_word(v_c); long long tb_c470_1n = (long long)strlen(tb_c470_1); const char *tb_c470_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c470_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c470r = tb_mem(tb_c470_0n + tb_c470_1n + tb_c470_2n + 1), *tb_c470rw = tb_c470r; memcpy(tb_c470rw, tb_c470_0, tb_c470_0n); tb_c470rw += tb_c470_0n; memcpy(tb_c470rw, tb_c470_1, tb_c470_1n); tb_c470rw += tb_c470_1n; memcpy(tb_c470rw, tb_c470_2, tb_c470_2n); tb_c470rw += tb_c470_2n; *tb_c470rw = 0; (const char *)tb_c470r; }), ((tb_o_Node *)tb_nn(v_name, 1022))->q_name);
                }
                if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1023))->q_vars, v_v, sizeof(tb_o_Var *), 1023)), 1023))->q_type)))) {
                    f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_name, 1024))->q_line, ((tb_o_Node *)tb_nn(v_name, 1024))->q_name);
                }
                if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1025))->q_vars, v_v, sizeof(tb_o_Var *), 1025)), 1025))->q_is_const) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1026))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_name, 1026))->q_name);
                }
                if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1027))->q_vars, v_v, sizeof(tb_o_Var *), 1027)), 1027))->q_type, v_out))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1028))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1028))->q_vars, v_v, sizeof(tb_o_Var *), 1028)), 1028))->q_type));
                }
                { _Bool tb_v471 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1029))->q_vars, v_v, sizeof(tb_o_Var *), 1029)), 1029))->q_is_link; _Bool *tb_p471 = &(((tb_o_Node *)tb_nn(v_name, 1029))->q_is_link); *tb_p471 = tb_v471; }
            }
            { long long tb_v472 = v_out; long long *tb_p472 = &(((tb_o_Node *)tb_nn(v_name, 1030))->q_ty); *tb_p472 = tb_v472; }
        }
    }
}

static void f_check__check_return(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_f = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1033))->q_funcs, ((tb_o_Comp *)tb_nn(v_c, 1033))->q_current, sizeof(tb_o_Func *), 1033)), 1033))->q_node;
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1034))->q_kids, 2LL, sizeof(tb_o_Node *), 1034)), 1034))->q_kids)->len > 0LL)) {
        if (((((tb_o_Node *)tb_nn(v_n, 1035))->q_kids)->len != 0LL)) {
            if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1037))->q_kids, 2LL, sizeof(tb_o_Node *), 1037)), 1037))->q_kids)->len > 1LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1038))->q_line, "у функции с несколькими gives return пишется без значения: он отдаёт выходы", "");
            }
            long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1039))->q_kids, 0LL, sizeof(tb_o_Node *), 1039)));
            if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1040))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1041))->q_line, "return отдаёт другой тип, чем написано в gives", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1041))->q_ty));
            }
        }
        return;
    }
    long long v_t = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 1044))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1045))->q_kids, 0LL, sizeof(tb_o_Node *), 1045)));
    }
    if ((({ const char *tb_s473 = ((tb_o_Node *)tb_nn(v_f, 1046))->q_name; ((unsigned char)tb_s473[0] == 109 && (unsigned char)tb_s473[1] == 97 && (unsigned char)tb_s473[2] == 105 && (unsigned char)tb_s473[3] == 110 && tb_s473[4] == 0); }) && (v_t != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1047))->q_line, "main ничего не возвращает: пиши просто return", "");
    }
    if ((((tb_o_Node *)tb_nn(v_f, 1048))->q_ty < 0LL)) {
        { long long tb_v474 = v_t; long long *tb_p474 = &(((tb_o_Node *)tb_nn(v_f, 1049))->q_ty); *tb_p474 = tb_v474; }
    } else if (((((tb_o_Node *)tb_nn(v_f, 1050))->q_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1051))->q_line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", "");
    } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1052))->q_ty, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1053))->q_line, "раньше эта функция вернула другой тип", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1053))->q_ty));
    }
}

static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1056))->q_kind;
    if (({ const char *tb_s475 = v_k; ((unsigned char)tb_s475[0] == 97 && (unsigned char)tb_s475[1] == 115 && (unsigned char)tb_s475[2] == 115 && (unsigned char)tb_s475[3] == 105 && (unsigned char)tb_s475[4] == 103 && (unsigned char)tb_s475[5] == 110 && tb_s475[6] == 0); })) {
        f_check__check_assign(v_c, v_n);
    } else if (({ const char *tb_s476 = v_k; ((unsigned char)tb_s476[0] == 115 && (unsigned char)tb_s476[1] == 101 && (unsigned char)tb_s476[2] == 116 && tb_s476[3] == 0); })) {
        f_check__check_set(v_c, v_n);
    } else if (({ const char *tb_s477 = v_k; ((unsigned char)tb_s477[0] == 108 && (unsigned char)tb_s477[1] == 105 && (unsigned char)tb_s477[2] == 110 && (unsigned char)tb_s477[3] == 107 && (unsigned char)tb_s477[4] == 100 && (unsigned char)tb_s477[5] == 101 && (unsigned char)tb_s477[6] == 99 && (unsigned char)tb_s477[7] == 108 && tb_s477[8] == 0); })) {
        f_check__check_linkdecl(v_c, v_n);
    } else if (({ const char *tb_s478 = v_k; ((unsigned char)tb_s478[0] == 109 && (unsigned char)tb_s478[1] == 117 && (unsigned char)tb_s478[2] == 108 && (unsigned char)tb_s478[3] == 116 && (unsigned char)tb_s478[4] == 105 && tb_s478[5] == 0); })) {
        f_check__check_multi(v_c, v_n);
    } else if (({ const char *tb_s479 = v_k; ((unsigned char)tb_s479[0] == 105 && (unsigned char)tb_s479[1] == 102 && tb_s479[2] == 0); })) {
        long long v_i = 0LL;
        while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 1067))->q_kids)->len)) {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1068))->q_kids, v_i, sizeof(tb_o_Node *), 1068)));
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1069))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 1069)));
            { long long tb_v480 = 2LL; long long *tb_p480 = &(v_i); *tb_p480 += tb_v480; }
        }
        if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 1071))->q_kids)->len, 2LL, 1071) == 1LL)) {
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1072))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1072))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1072)));
        }
    } else if (({ const char *tb_s481 = v_k; ((unsigned char)tb_s481[0] == 101 && (unsigned char)tb_s481[1] == 97 && (unsigned char)tb_s481[2] == 99 && (unsigned char)tb_s481[3] == 104 && tb_s481[4] == 0); })) {
        long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1074))->q_kids, 0LL, sizeof(tb_o_Node *), 1074)));
        long long v_elem = 4LL;
        if (f_check__is_list(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1077))->q_types, v_t, sizeof(tb_o_Type *), 1077)), 1077))->q_elem;
        } else if (f_check__is_dict(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1079))->q_types, v_t, sizeof(tb_o_Type *), 1079)), 1079))->q_key;
        } else if ((v_t != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1081))->q_line, "each ходит по списку, по ключам dict или по буквам текста", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1082))->q_is_par && (!f_check__is_list(v_c, v_t)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1083))->q_line, "on all cores ходит по списку (или count from ... to ...)", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1084))->q_by_vec && (!(f_check__is_list(v_c, v_t) && f_check__is_number(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1084))->q_types, v_t, sizeof(tb_o_Type *), 1084)), 1084))->q_elem))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1085))->q_line, "by vec ходит по списку чисел: list int или list dec", "");
        }
        if (((tb_o_Node *)tb_nn(v_n, 1086))->q_by_piece) {
            f_check__check_pieces(v_c, v_n, v_t);
        } else {
            f_check__open_scope(v_c);
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1090))->q_name, v_elem, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1090))->q_line);
            { tb_o_Node * tb_v482 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1091))->q_loops) = tb_v482; }
            f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 1LL, sizeof(tb_o_Node *), 1092)));
            tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1093))->q_loops, 1093));
            f_check__close_scope(v_c);
        }
    } else if (({ const char *tb_s483 = v_k; ((unsigned char)tb_s483[0] == 99 && (unsigned char)tb_s483[1] == 111 && (unsigned char)tb_s483[2] == 117 && (unsigned char)tb_s483[3] == 110 && (unsigned char)tb_s483[4] == 116 && tb_s483[5] == 0); })) {
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1096))->q_kids, 0LL, sizeof(tb_o_Node *), 1096)), "from");
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1097))->q_kids, 1LL, sizeof(tb_o_Node *), 1097)), "to");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1099))->q_name, 1LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1099))->q_line);
        { tb_o_Node * tb_v484 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1100))->q_loops) = tb_v484; }
        f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 2LL, sizeof(tb_o_Node *), 1101)));
        tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1102))->q_loops, 1102));
        f_check__close_scope(v_c);
    } else if (((strcmp(v_k, "repeat_times") == 0) || (strcmp(v_k, "repeat_while") == 0))) {
        if ((strcmp(v_k, "repeat_times") == 0)) {
            f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1106))->q_kids, 0LL, sizeof(tb_o_Node *), 1106)), "times");
        } else {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1108))->q_kids, 0LL, sizeof(tb_o_Node *), 1108)));
        }
        { long long tb_v485 = 1LL; long long *tb_p485 = &(((tb_o_Comp *)tb_nn(v_c, 1109))->q_loop_depth); *tb_p485 += tb_v485; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1110))->q_kids, 1LL, sizeof(tb_o_Node *), 1110)));
        { long long tb_v486 = 1LL; long long *tb_p486 = &(((tb_o_Comp *)tb_nn(v_c, 1111))->q_loop_depth); *tb_p486 -= tb_v486; }
    } else if (({ const char *tb_s487 = v_k; ((unsigned char)tb_s487[0] == 116 && (unsigned char)tb_s487[1] == 111 && (unsigned char)tb_s487[2] == 103 && (unsigned char)tb_s487[3] == 101 && (unsigned char)tb_s487[4] == 116 && (unsigned char)tb_s487[5] == 104 && (unsigned char)tb_s487[6] == 101 && (unsigned char)tb_s487[7] == 114 && tb_s487[8] == 0); })) {
        f_check__check_together(v_c, v_n);
    } else if (({ const char *tb_s488 = v_k; ((unsigned char)tb_s488[0] == 116 && (unsigned char)tb_s488[1] == 101 && (unsigned char)tb_s488[2] == 109 && (unsigned char)tb_s488[3] == 112 && tb_s488[4] == 0); })) {
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 1115))->q_temp_base;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 1116))->q_temp_loops;
        { long long tb_v489 = (((tb_o_Comp *)tb_nn(v_c, 1117))->q_vars)->len; long long *tb_p489 = &(((tb_o_Comp *)tb_nn(v_c, 1117))->q_temp_base); *tb_p489 = tb_v489; }
        { long long tb_v490 = ((tb_o_Comp *)tb_nn(v_c, 1118))->q_loop_depth; long long *tb_p490 = &(((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_loops); *tb_p490 = tb_v490; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1119))->q_kids, 0LL, sizeof(tb_o_Node *), 1119)));
        { long long tb_v491 = v_saved_base; long long *tb_p491 = &(((tb_o_Comp *)tb_nn(v_c, 1120))->q_temp_base); *tb_p491 = tb_v491; }
        { long long tb_v492 = v_saved_loops; long long *tb_p492 = &(((tb_o_Comp *)tb_nn(v_c, 1121))->q_temp_loops); *tb_p492 = tb_v492; }
    } else if ((({ const char *tb_s493 = v_k; ((unsigned char)tb_s493[0] == 115 && (unsigned char)tb_s493[1] == 107 && (unsigned char)tb_s493[2] == 105 && (unsigned char)tb_s493[3] == 112 && tb_s493[4] == 0); }) || ({ const char *tb_s494 = v_k; ((unsigned char)tb_s494[0] == 115 && (unsigned char)tb_s494[1] == 116 && (unsigned char)tb_s494[2] == 111 && (unsigned char)tb_s494[3] == 112 && tb_s494[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1123))->q_loop_depth == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1124))->q_line, "skip и stop пишутся только внутри цикла", "");
        }
        if (((({ const char *tb_s495 = v_k; ((unsigned char)tb_s495[0] == 115 && (unsigned char)tb_s495[1] == 116 && (unsigned char)tb_s495[2] == 111 && (unsigned char)tb_s495[3] == 112 && tb_s495[4] == 0); }) && (((tb_o_Comp *)tb_nn(v_c, 1125))->q_par_base >= 0LL)) && (((tb_o_Comp *)tb_nn(v_c, 1125))->q_loop_depth == ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1125))->q_par_node, 1125))->q_temp_no))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1126))->q_line, "stop внутри on all cores нельзя: остальные ядра всё равно работают", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 1127))->q_temp_base >= 0LL) && (((tb_o_Comp *)tb_nn(v_c, 1127))->q_loop_depth == ((tb_o_Comp *)tb_nn(v_c, 1127))->q_temp_loops))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1128))->q_line, "skip и stop внутри temp работают только для циклов внутри temp", "");
        }
    } else if (({ const char *tb_s496 = v_k; ((unsigned char)tb_s496[0] == 114 && (unsigned char)tb_s496[1] == 101 && (unsigned char)tb_s496[2] == 116 && (unsigned char)tb_s496[3] == 117 && (unsigned char)tb_s496[4] == 114 && (unsigned char)tb_s496[5] == 110 && tb_s496[6] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1130))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1131))->q_line, ({ const char *tb_c497_0 = "внутри "; long long tb_c497_0n = sizeof("внутри ") - 1; const char *tb_c497_1 = f_check__loop_word(v_c); long long tb_c497_1n = (long long)strlen(tb_c497_1); const char *tb_c497_2 = " return нельзя"; long long tb_c497_2n = sizeof(" return нельзя") - 1; char *tb_c497r = tb_mem(tb_c497_0n + tb_c497_1n + tb_c497_2n + 1), *tb_c497rw = tb_c497r; memcpy(tb_c497rw, tb_c497_0, tb_c497_0n); tb_c497rw += tb_c497_0n; memcpy(tb_c497rw, tb_c497_1, tb_c497_1n); tb_c497rw += tb_c497_1n; memcpy(tb_c497rw, tb_c497_2, tb_c497_2n); tb_c497rw += tb_c497_2n; *tb_c497rw = 0; (const char *)tb_c497r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1132))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1133))->q_line, "внутри temp return нельзя: сохрани число в переменную снаружи и выйди из блока", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1134))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1135))->q_line, "внутри each ... by N return нельзя: сохрани число в переменную снаружи и выйди через stop", "");
        }
        f_check__check_return(v_c, v_n);
    } else if (({ const char *tb_s498 = v_k; ((unsigned char)tb_s498[0] == 102 && (unsigned char)tb_s498[1] == 97 && (unsigned char)tb_s498[2] == 105 && (unsigned char)tb_s498[3] == 108 && tb_s498[4] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1138))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1139))->q_line, ({ const char *tb_c499_0 = "внутри "; long long tb_c499_0n = sizeof("внутри ") - 1; const char *tb_c499_1 = f_check__loop_word(v_c); long long tb_c499_1n = (long long)strlen(tb_c499_1); const char *tb_c499_2 = " fail нельзя: пиши  or  или  else err"; long long tb_c499_2n = sizeof(" fail нельзя: пиши  or  или  else err") - 1; char *tb_c499r = tb_mem(tb_c499_0n + tb_c499_1n + tb_c499_2n + 1), *tb_c499rw = tb_c499r; memcpy(tb_c499rw, tb_c499_0, tb_c499_0n); tb_c499rw += tb_c499_0n; memcpy(tb_c499rw, tb_c499_1, tb_c499_1n); tb_c499rw += tb_c499_1n; memcpy(tb_c499rw, tb_c499_2, tb_c499_2n); tb_c499rw += tb_c499_2n; *tb_c499rw = 0; (const char *)tb_c499r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1140))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1141))->q_line, "внутри temp fail нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1142))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1143))->q_line, "внутри each ... by N fail нельзя: пиши  or  или  else err", "");
        }
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1144))->q_kids, 0LL, sizeof(tb_o_Node *), 1144))) != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1145))->q_line, "после fail пишется текст ошибки: fail \"нет файла\"", "");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1147))->q_kids, 0LL, sizeof(tb_o_Node *), 1147));
        tb_o_Node * v_inner = v_e;
        if ((({ const char *tb_s500 = ((tb_o_Node *)tb_nn(v_e, 1149))->q_kind; ((unsigned char)tb_s500[0] == 116 && (unsigned char)tb_s500[1] == 114 && (unsigned char)tb_s500[2] == 121 && tb_s500[3] == 0); }) || ({ const char *tb_s501 = ((tb_o_Node *)tb_nn(v_e, 1149))->q_kind; ((unsigned char)tb_s501[0] == 101 && (unsigned char)tb_s501[1] == 108 && (unsigned char)tb_s501[2] == 115 && (unsigned char)tb_s501[3] == 101 && (unsigned char)tb_s501[4] == 95 && (unsigned char)tb_s501[5] == 101 && (unsigned char)tb_s501[6] == 114 && (unsigned char)tb_s501[7] == 114 && tb_s501[8] == 0); }))) {
            v_inner = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1150))->q_kids, 0LL, sizeof(tb_o_Node *), 1150));
        }
        if (((({ const char *tb_s502 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; !((unsigned char)tb_s502[0] == 99 && (unsigned char)tb_s502[1] == 97 && (unsigned char)tb_s502[2] == 108 && (unsigned char)tb_s502[3] == 108 && tb_s502[4] == 0); }) && ({ const char *tb_s503 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; !((unsigned char)tb_s503[0] == 109 && (unsigned char)tb_s503[1] == 101 && (unsigned char)tb_s503[2] == 116 && (unsigned char)tb_s503[3] == 104 && (unsigned char)tb_s503[4] == 111 && (unsigned char)tb_s503[5] == 100 && tb_s503[6] == 0); })) && (!(({ const char *tb_s504 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; ((unsigned char)tb_s504[0] == 102 && (unsigned char)tb_s504[1] == 105 && (unsigned char)tb_s504[2] == 101 && (unsigned char)tb_s504[3] == 108 && (unsigned char)tb_s504[4] == 100 && tb_s504[5] == 0); }) && ({ const char *tb_s505 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_name; ((unsigned char)tb_s505[0] == 119 && (unsigned char)tb_s505[1] == 97 && (unsigned char)tb_s505[2] == 105 && (unsigned char)tb_s505[3] == 116 && tb_s505[4] == 0); }))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1152))->q_line, "строка ничего не делает", "");
        }
        long long v_t = f_check__check_expr(v_c, v_e);
        if ((v_t != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1155))->q_line, "результат вызова пропадает: сохрани его, например x = ...", ((tb_o_Node *)tb_nn(v_e, 1155))->q_name);
        }
    }
}

static long long f_check__check_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if ((((tb_o_Comp *)tb_nn(v_c, 1161))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1162))->q_line, ({ const char *tb_c506_0 = "внутри "; long long tb_c506_0n = sizeof("внутри ") - 1; const char *tb_c506_1 = f_check__loop_word(v_c); long long tb_c506_1n = (long long)strlen(tb_c506_1); const char *tb_c506_2 = " background нельзя: цикл и так идёт на всех ядрах"; long long tb_c506_2n = sizeof(" background нельзя: цикл и так идёт на всех ядрах") - 1; char *tb_c506r = tb_mem(tb_c506_0n + tb_c506_1n + tb_c506_2n + 1), *tb_c506rw = tb_c506r; memcpy(tb_c506rw, tb_c506_0, tb_c506_0n); tb_c506rw += tb_c506_0n; memcpy(tb_c506rw, tb_c506_1, tb_c506_1n); tb_c506rw += tb_c506_1n; memcpy(tb_c506rw, tb_c506_2, tb_c506_2n); tb_c506rw += tb_c506_2n; *tb_c506rw = 0; (const char *)tb_c506r; }), "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1163))->q_temp_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1164))->q_line, "внутри temp background нельзя: память блока удалится, пока функция ещё работает", "");
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1165))->q_kids, 0LL, sizeof(tb_o_Node *), 1165));
    if ((({ const char *tb_s507 = ((tb_o_Node *)tb_nn(v_call, 1166))->q_kind; ((unsigned char)tb_s507[0] == 99 && (unsigned char)tb_s507[1] == 97 && (unsigned char)tb_s507[2] == 108 && (unsigned char)tb_s507[3] == 108 && tb_s507[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_call, 1166))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1167))->q_line, "background запускает свою функцию: a = background load(name)", ((tb_o_Node *)tb_nn(v_call, 1167))->q_name);
    }
    { _Bool tb_v508 = 1; _Bool *tb_p508 = &(((tb_o_Node *)tb_nn(v_call, 1168))->q_creates); *tb_p508 = tb_v508; }
    long long v_t = f_check__check_expr(v_c, v_call);
    if (((({ const char *tb_s509 = ((tb_o_Node *)tb_nn(v_call, 1170))->q_kind; !((unsigned char)tb_s509[0] == 99 && (unsigned char)tb_s509[1] == 97 && (unsigned char)tb_s509[2] == 108 && (unsigned char)tb_s509[3] == 108 && tb_s509[4] == 0); }) || (!((tb_o_Node *)tb_nn(v_call, 1170))->q_has_ref)) || ({ const char *tb_s510 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_call, 1170))->q_ref, 1170))->q_kind; !((unsigned char)tb_s510[0] == 102 && (unsigned char)tb_s510[1] == 117 && (unsigned char)tb_s510[2] == 110 && (unsigned char)tb_s510[3] == 99 && tb_s510[4] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1171))->q_line, "background запускает свою функцию: a = background load(name)", "");
    }
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1172))->q_ref;
    if (((tb_o_Node *)tb_nn(v_f, 1173))->q_can_fail) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1174))->q_line, "функция с fail в background пока нельзя: пусть она отдаёт текст ошибки обычным выходом", ((tb_o_Node *)tb_nn(v_f, 1174))->q_name);
    }
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1175))->q_kids, 2LL, sizeof(tb_o_Node *), 1175)), 1175))->q_kids)->len > 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1176))->q_line, "в background функция с несколькими выходами пока нельзя", ((tb_o_Node *)tb_nn(v_f, 1176))->q_name);
    }
    {
        tb_list * tb_l511 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1177))->q_kids, 0LL, sizeof(tb_o_Node *), 1177)), 1177))->q_kids;
        for (long long tb_i511 = 0; tb_i511 < tb_l511->len; tb_i511++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l511->data)[tb_i511];
            if ((((tb_o_Node *)tb_nn(v_p, 1178))->q_is_link || (!(f_check__is_plain(((tb_o_Node *)tb_nn(v_p, 1178))->q_ty) || (((tb_o_Node *)tb_nn(v_p, 1178))->q_ty == 4LL))))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1179))->q_line, "в background функция получает только int, dec, bool и text: списки и obj могли бы меняться сразу из двух мест", ((tb_o_Node *)tb_nn(v_p, 1179))->q_name);
            }
        }
    }
    if ((v_t < 0LL)) {
        v_t = 0LL;
    }
    return f_check__task_of(v_c, v_t);
    return v_r;
}

static void f_check__check_together(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1187))->q_kids, 0LL, sizeof(tb_o_Node *), 1187));
    tb_list * v_names = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l512 = ((tb_o_Node *)tb_nn(v_block, 1189))->q_kids;
        for (long long tb_i512 = 0; tb_i512 < tb_l512->len; tb_i512++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l512->data)[tb_i512];
            if ((({ const char *tb_s513 = ((tb_o_Node *)tb_nn(v_s, 1190))->q_kind; !((unsigned char)tb_s513[0] == 97 && (unsigned char)tb_s513[1] == 115 && (unsigned char)tb_s513[2] == 115 && (unsigned char)tb_s513[3] == 105 && (unsigned char)tb_s513[4] == 103 && (unsigned char)tb_s513[5] == 110 && tb_s513[6] == 0); }) || ((((tb_o_Node *)tb_nn(v_s, 1190))->q_kids)->len != 1LL))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1191))->q_line, "в together пишутся только новые переменные со значением: a = d[key]", "");
            }
            if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_s, 1192))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1193))->q_line, "в together заводятся только новые переменные, а эта уже есть", ((tb_o_Node *)tb_nn(v_s, 1193))->q_name);
            }
            {
                tb_list * tb_l514 = v_names;
                for (long long tb_i514 = 0; tb_i514 < tb_l514->len; tb_i514++) {
                    const char * const v_w = ((const char * *)tb_l514->data)[tb_i514];
                    if ((strcmp(v_w, ((tb_o_Node *)tb_nn(v_s, 1195))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1196))->q_line, "в together эта переменная уже заведена строкой выше", ((tb_o_Node *)tb_nn(v_s, 1196))->q_name);
                    }
                    if (f_check__uses_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_s, 1197))->q_kids, 0LL, sizeof(tb_o_Node *), 1197)), v_w)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1198))->q_line, "строки в together не должны зависеть друг от друга, а здесь нужна", v_w);
                    }
                }
            }
            { const char * tb_v515 = ((tb_o_Node *)tb_nn(v_s, 1199))->q_name; *(const char * *)tb_grow(v_names) = tb_v515; }
            f_check__check_statement(v_c, v_s);
        }
    }
}

static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s516 = ((tb_o_Node *)tb_nn(v_n, 1204))->q_kind; ((unsigned char)tb_s516[0] == 110 && (unsigned char)tb_s516[1] == 97 && (unsigned char)tb_s516[2] == 109 && (unsigned char)tb_s516[3] == 101 && tb_s516[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1204))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l517 = ((tb_o_Node *)tb_nn(v_n, 1206))->q_kids;
        for (long long tb_i517 = 0; tb_i517 < tb_l517->len; tb_i517++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l517->data)[tb_i517];
            if (f_check__uses_name(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1203, "функция uses_name дошла до конца без return");
    return 0;
}

static void f_check__check_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1215))->q_line, "by N режет на куски список: each piece in nums by 64", "");
    }
    if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1216))->q_types, v_t, sizeof(tb_o_Type *), 1216)), 1216))->q_elem)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1217))->q_line, "список колонок (by columns) на куски пока не режется", "");
    }
    if ((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1218))->q_types, v_t, sizeof(tb_o_Type *), 1218)), 1218))->q_elem == 3LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1219))->q_line, "list bool хранится битами и на куски пока не режется", "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1220))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1221))->q_line, ({ const char *tb_c518_0 = "each ... by N внутри "; long long tb_c518_0n = sizeof("each ... by N внутри ") - 1; const char *tb_c518_1 = f_check__loop_word(v_c); long long tb_c518_1n = (long long)strlen(tb_c518_1); const char *tb_c518_2 = " пока нельзя"; long long tb_c518_2n = sizeof(" пока нельзя") - 1; char *tb_c518r = tb_mem(tb_c518_0n + tb_c518_1n + tb_c518_2n + 1), *tb_c518rw = tb_c518r; memcpy(tb_c518rw, tb_c518_0, tb_c518_0n); tb_c518rw += tb_c518_0n; memcpy(tb_c518rw, tb_c518_1, tb_c518_1n); tb_c518rw += tb_c518_1n; memcpy(tb_c518rw, tb_c518_2, tb_c518_2n); tb_c518rw += tb_c518_2n; *tb_c518rw = 0; (const char *)tb_c518r; }), "");
    }
    f_check__need_int(v_c, ((tb_o_Node *)tb_nn(v_n, 1222))->q_piece_size, "by");
    { const char * tb_v519 = ((tb_o_Comp *)tb_nn(v_c, 1223))->q_file_name; const char * *tb_p519 = &(((tb_o_Node *)tb_nn(v_n, 1223))->q_file); *tb_p519 = tb_v519; }
    { tb_o_Node * tb_v520 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1224))->q_pieces) = tb_v520; }
    f_check__open_scope(v_c);
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1226))->q_name, v_t, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1226))->q_line);
    { long long tb_v521 = 1LL; long long *tb_p521 = &(((tb_o_Comp *)tb_nn(v_c, 1227))->q_piece_depth); *tb_p521 += tb_v521; }
    { tb_o_Node * tb_v522 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1228))->q_loops) = tb_v522; }
    f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1229))->q_kids, 1LL, sizeof(tb_o_Node *), 1229)));
    tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1230))->q_loops, 1230));
    { long long tb_v523 = 1LL; long long *tb_p523 = &(((tb_o_Comp *)tb_nn(v_c, 1231))->q_piece_depth); *tb_p523 -= tb_v523; }
    f_check__close_scope(v_c);
}

static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name) {
    return (({ const char *tb_s524 = ((tb_o_Node *)tb_nn(v_v, 1236))->q_kind; ((unsigned char)tb_s524[0] == 110 && (unsigned char)tb_s524[1] == 97 && (unsigned char)tb_s524[2] == 109 && (unsigned char)tb_s524[3] == 101 && tb_s524[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_v, 1236))->q_name, v_name) == 0));
    tb_crash(1235, "функция is_name_of дошла до конца без return");
    return 0;
}

static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name) {
    long long v_r = 0LL;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1242))->q_kind;
    if ((({ const char *tb_s525 = v_k; ((unsigned char)tb_s525[0] == 110 && (unsigned char)tb_s525[1] == 97 && (unsigned char)tb_s525[2] == 109 && (unsigned char)tb_s525[3] == 101 && tb_s525[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1243))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1244))->q_line;
    }
    if (((({ const char *tb_s526 = v_k; ((unsigned char)tb_s526[0] == 97 && (unsigned char)tb_s526[1] == 115 && (unsigned char)tb_s526[2] == 115 && (unsigned char)tb_s526[3] == 105 && (unsigned char)tb_s526[4] == 103 && (unsigned char)tb_s526[5] == 110 && tb_s526[6] == 0); }) || ({ const char *tb_s527 = v_k; ((unsigned char)tb_s527[0] == 108 && (unsigned char)tb_s527[1] == 105 && (unsigned char)tb_s527[2] == 110 && (unsigned char)tb_s527[3] == 107 && (unsigned char)tb_s527[4] == 100 && (unsigned char)tb_s527[5] == 101 && (unsigned char)tb_s527[6] == 99 && (unsigned char)tb_s527[7] == 108 && tb_s527[8] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1245))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1246))->q_line;
    }
    if (((({ const char *tb_s528 = v_k; ((unsigned char)tb_s528[0] == 99 && (unsigned char)tb_s528[1] == 97 && (unsigned char)tb_s528[2] == 108 && (unsigned char)tb_s528[3] == 108 && tb_s528[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1247))->q_has_ref) && ({ const char *tb_s529 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1247))->q_ref, 1247))->q_kind; ((unsigned char)tb_s529[0] == 102 && (unsigned char)tb_s529[1] == 117 && (unsigned char)tb_s529[2] == 110 && (unsigned char)tb_s529[3] == 99 && tb_s529[4] == 0); }))) {
        {
            long long tb_end530 = ((((tb_o_Node *)tb_nn(v_n, 1248))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end530; v_i++) {
                tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1249))->q_kids, v_i, sizeof(tb_o_Node *), 1249)), 1249))->q_kids, 0LL, sizeof(tb_o_Node *), 1249));
                if (f_check__is_name_of(v_v, v_name)) {
                    if ((!f_check__param_keeps_piece(v_c, ((tb_o_Node *)tb_nn(v_n, 1251))->q_ref, v_i))) {
                        continue;
                    }
                    return ((tb_o_Node *)tb_nn(v_v, 1253))->q_line;
                }
                long long v_bad = f_check__piece_leak(v_c, v_v, v_name);
                if ((v_bad != 0LL)) {
                    return v_bad;
                }
            }
        }
        return 0LL;
    }
    if ((({ const char *tb_s531 = v_k; ((unsigned char)tb_s531[0] == 99 && (unsigned char)tb_s531[1] == 97 && (unsigned char)tb_s531[2] == 108 && (unsigned char)tb_s531[3] == 108 && tb_s531[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 1258))->q_name))) {
        {
            tb_list * tb_l532 = ((tb_o_Node *)tb_nn(v_n, 1259))->q_kids;
            for (long long tb_i532 = 0; tb_i532 < tb_l532->len; tb_i532++) {
                tb_o_Node * const v_a = ((tb_o_Node * *)tb_l532->data)[tb_i532];
                tb_o_Node * v_v = v_a;
                if (({ const char *tb_s533 = ((tb_o_Node *)tb_nn(v_a, 1261))->q_kind; ((unsigned char)tb_s533[0] == 97 && (unsigned char)tb_s533[1] == 114 && (unsigned char)tb_s533[2] == 103 && tb_s533[3] == 0); })) {
                    v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_a, 1262))->q_kids, 0LL, sizeof(tb_o_Node *), 1262));
                }
                if ((!f_check__is_name_of(v_v, v_name))) {
                    long long v_bad = f_check__piece_leak(v_c, v_v, v_name);
                    if ((v_bad != 0LL)) {
                        return v_bad;
                    }
                }
            }
        }
        return 0LL;
    }
    long long v_start = 0LL;
    if ((((({ const char *tb_s534 = v_k; ((unsigned char)tb_s534[0] == 105 && (unsigned char)tb_s534[1] == 110 && (unsigned char)tb_s534[2] == 100 && (unsigned char)tb_s534[3] == 101 && (unsigned char)tb_s534[4] == 120 && tb_s534[5] == 0); }) || ({ const char *tb_s535 = v_k; ((unsigned char)tb_s535[0] == 101 && (unsigned char)tb_s535[1] == 97 && (unsigned char)tb_s535[2] == 99 && (unsigned char)tb_s535[3] == 104 && tb_s535[4] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1269))->q_kids)->len > 0LL)) && f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1269))->q_kids, 0LL, sizeof(tb_o_Node *), 1269)), v_name))) {
        v_start = 1LL;
    }
    if ((({ const char *tb_s536 = v_k; ((unsigned char)tb_s536[0] == 101 && (unsigned char)tb_s536[1] == 97 && (unsigned char)tb_s536[2] == 99 && (unsigned char)tb_s536[3] == 104 && tb_s536[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1271))->q_by_piece)) {
        long long v_bad = f_check__piece_leak(v_c, ((tb_o_Node *)tb_nn(v_n, 1272))->q_piece_size, v_name);
        if ((v_bad != 0LL)) {
            return v_bad;
        }
    }
    {
        long long tb_end537 = ((((tb_o_Node *)tb_nn(v_n, 1275))->q_kids)->len - 1LL);
        for (long long v_i = v_start; v_i <= tb_end537; v_i++) {
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1276))->q_kids, v_i, sizeof(tb_o_Node *), 1276)), v_name);
            if ((v_bad != 0LL)) {
                return v_bad;
            }
        }
    }
    return 0LL;
    return v_r;
}

static _Bool f_check__param_keeps_piece(tb_o_Comp * v_c, tb_o_Node * v_f, long long v_i) {
    const char * v_key = ({ const char *tb_c538_0 = ((tb_o_Node *)tb_nn(v_f, 1283))->q_name; long long tb_c538_0n = (long long)strlen(tb_c538_0); const char *tb_c538_1 = "@"; long long tb_c538_1n = sizeof("@") - 1; const char *tb_c538_2 = ((tb_o_Node *)tb_nn(v_f, 1283))->q_mod; long long tb_c538_2n = (long long)strlen(tb_c538_2); const char *tb_c538_3 = "#"; long long tb_c538_3n = sizeof("#") - 1; char tb_c538_4d[24]; long long tb_c538_4n; const char *tb_c538_4 = tb_digits(v_i, tb_c538_4d, &tb_c538_4n); char *tb_c538r = tb_mem(tb_c538_0n + tb_c538_1n + tb_c538_2n + tb_c538_3n + tb_c538_4n + 1), *tb_c538rw = tb_c538r; memcpy(tb_c538rw, tb_c538_0, tb_c538_0n); tb_c538rw += tb_c538_0n; memcpy(tb_c538rw, tb_c538_1, tb_c538_1n); tb_c538rw += tb_c538_1n; memcpy(tb_c538rw, tb_c538_2, tb_c538_2n); tb_c538rw += tb_c538_2n; memcpy(tb_c538rw, tb_c538_3, tb_c538_3n); tb_c538rw += tb_c538_3n; memcpy(tb_c538rw, tb_c538_4, tb_c538_4n); tb_c538rw += tb_c538_4n; *tb_c538rw = 0; (const char *)tb_c538r; });
    {
        long long tb_end539 = ((((tb_o_Comp *)tb_nn(v_c, 1284))->q_safe_keys)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end539; v_j++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1285))->q_safe_keys, v_j, sizeof(const char *), 1285)), v_key) == 0)) {
                return ((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1286))->q_safe_vals, v_j, sizeof(long long), 1286)) == 2LL);
            }
        }
    }
    { const char * tb_v540 = v_key; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1288))->q_safe_keys) = tb_v540; }
    { long long tb_v541 = 1LL; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1289))->q_safe_vals) = tb_v541; }
    long long v_slot = ((((tb_o_Comp *)tb_nn(v_c, 1290))->q_safe_keys)->len - 1LL);
    { long long tb_v542 = 1LL; long long *tb_p542 = &(((tb_o_Comp *)tb_nn(v_c, 1291))->q_safe_busy); *tb_p542 += tb_v542; }
    long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1292))->q_kids, 1LL, sizeof(tb_o_Node *), 1292)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1292))->q_kids, 0LL, sizeof(tb_o_Node *), 1292)), 1292))->q_kids, v_i, sizeof(tb_o_Node *), 1292)), 1292))->q_name);
    { long long tb_v543 = 1LL; long long *tb_p543 = &(((tb_o_Comp *)tb_nn(v_c, 1293))->q_safe_busy); *tb_p543 -= tb_v543; }
    if ((v_bad != 0LL)) {
        { long long tb_v544 = 2LL; long long *tb_p544 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1295))->q_safe_vals, v_slot, sizeof(long long), 1295))); *tb_p544 = tb_v544; }
        return 1;
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1297))->q_safe_busy > 0LL)) {
        { long long tb_v545 = 3LL; long long *tb_p545 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1299))->q_safe_vals, v_slot, sizeof(long long), 1299))); *tb_p545 = tb_v545; }
        { const char * tb_v546 = ""; const char * *tb_p546 = &((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1300))->q_safe_keys, v_slot, sizeof(const char *), 1300))); *tb_p546 = tb_v546; }
    }
    return 0;
    tb_crash(1282, "функция param_keeps_piece дошла до конца без return");
    return 0;
}

static void f_check__mark_hot_fields(tb_o_Comp * v_c) {
    tb_list * v_hot_func = tb_bits_new();
    {
        long long tb_end547 = (((tb_o_Comp *)tb_nn(v_c, 1310))->q_funcs)->len;
        for (long long v_i = 1LL; v_i <= tb_end547; v_i++) {
            { _Bool tb_v548 = 0; tb_badd(v_hot_func, tb_v548); }
        }
    }
    {
        long long tb_end549 = ((((tb_o_Comp *)tb_nn(v_c, 1312))->q_call_to)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end549; v_j++) {
            if (tb_bget(((tb_o_Comp *)tb_nn(v_c, 1313))->q_call_loop, v_j, 1313)) {
                { _Bool tb_v550 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_call_to, v_j, sizeof(long long), 1314)), tb_v550, 1314); }
            }
        }
    }
    _Bool v_changed = 1;
    while (v_changed) {
        v_changed = 0;
        {
            long long tb_end551 = ((((tb_o_Comp *)tb_nn(v_c, 1318))->q_call_to)->len - 1LL);
            for (long long v_j = 0LL; v_j <= tb_end551; v_j++) {
                if (((((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_from, v_j, sizeof(long long), 1319)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_from, v_j, sizeof(long long), 1319)), 1319)) && (!tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_to, v_j, sizeof(long long), 1319)), 1319)))) {
                    { _Bool tb_v552 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1320))->q_call_to, v_j, sizeof(long long), 1320)), tb_v552, 1320); }
                    v_changed = 1;
                }
            }
        }
    }
    {
        long long tb_end553 = ((((tb_o_Comp *)tb_nn(v_c, 1322))->q_acc_field)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end553; v_j++) {
            if ((tb_bget(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_loop, v_j, 1323) || (((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_func, v_j, sizeof(long long), 1323)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_func, v_j, sizeof(long long), 1323)), 1323)))) {
                { _Bool tb_v554 = 1; _Bool *tb_p554 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1324))->q_acc_field, v_j, sizeof(tb_o_Node *), 1324)), 1324))->q_hot); *tb_p554 = tb_v554; }
            }
        }
    }
    {
        tb_list * tb_l555 = ((tb_o_Comp *)tb_nn(v_c, 1325))->q_objs;
        for (long long tb_i555 = 0; tb_i555 < tb_l555->len; tb_i555++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l555->data)[tb_i555];
            if (((tb_o_Node *)tb_nn(v_o, 1326))->q_by_cols) {
                continue;
            }
            long long v_hot = 0LL;
            long long v_cold_bytes = 0LL;
            {
                tb_list * tb_l556 = ((tb_o_Node *)tb_nn(v_o, 1330))->q_kids;
                for (long long tb_i556 = 0; tb_i556 < tb_l556->len; tb_i556++) {
                    tb_o_Node * const v_f = ((tb_o_Node * *)tb_l556->data)[tb_i556];
                    if (((tb_o_Node *)tb_nn(v_f, 1331))->q_hot) {
                        { long long tb_v557 = 1LL; long long *tb_p557 = &(v_hot); *tb_p557 += tb_v557; }
                    } else if ((((tb_o_Node *)tb_nn(v_f, 1333))->q_ty == 3LL)) {
                        { long long tb_v558 = 1LL; long long *tb_p558 = &(v_cold_bytes); *tb_p558 += tb_v558; }
                    } else {
                        { long long tb_v559 = 8LL; long long *tb_p559 = &(v_cold_bytes); *tb_p559 += tb_v559; }
                    }
                }
            }
            if (((v_hot > 0LL) && (v_cold_bytes >= 16LL))) {
                {
                    tb_list * tb_l560 = ((tb_o_Node *)tb_nn(v_o, 1338))->q_kids;
                    for (long long tb_i560 = 0; tb_i560 < tb_l560->len; tb_i560++) {
                        tb_o_Node * const v_f = ((tb_o_Node * *)tb_l560->data)[tb_i560];
                        { _Bool tb_v561 = (!((tb_o_Node *)tb_nn(v_f, 1339))->q_hot); _Bool *tb_p561 = &(((tb_o_Node *)tb_nn(v_f, 1339))->q_is_cold); *tb_p561 = tb_v561; }
                    }
                }
            }
        }
    }
}

static void f_check__check_piece_leaks(tb_o_Comp * v_c) {
    {
        tb_list * tb_l562 = ((tb_o_Comp *)tb_nn(v_c, 1343))->q_pieces;
        for (long long tb_i562 = 0; tb_i562 < tb_l562->len; tb_i562++) {
            tb_o_Node * const v_n = ((tb_o_Node * *)tb_l562->data)[tb_i562];
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1344))->q_kids, 1LL, sizeof(tb_o_Node *), 1344)), ((tb_o_Node *)tb_nn(v_n, 1344))->q_name);
            if ((v_bad != 0LL)) {
                { const char * tb_v563 = ((tb_o_Node *)tb_nn(v_n, 1346))->q_file; const char * *tb_p563 = &(((tb_o_Comp *)tb_nn(v_c, 1346))->q_file_name); *tb_p563 = tb_v563; }
                f_errors__error_at(v_c, v_bad, "кусок живёт только внутри своего цикла: его можно читать и менять по номеру, ходить по нему each и передавать в функцию, но нельзя сохранять, растить .add и уменьшать .pop", ((tb_o_Node *)tb_nn(v_n, 1347))->q_name);
            }
        }
    }
}

static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body) {
    { long long tb_v564 = 1LL; long long *tb_p564 = &(((tb_o_Comp *)tb_nn(v_c, 1352))->q_loop_depth); *tb_p564 += tb_v564; }
    if ((((tb_o_Node *)tb_nn(v_n, 1353))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1353))->q_by_vec)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1354))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1355))->q_line, "on all cores и by vec внутри другого on all cores или by vec нельзя", "");
        }
        { long long tb_v565 = ((((tb_o_Comp *)tb_nn(v_c, 1356))->q_vars)->len - 1LL); long long *tb_p565 = &(((tb_o_Comp *)tb_nn(v_c, 1356))->q_par_base); *tb_p565 = tb_v565; }
        { tb_o_Node * tb_v566 = v_n; tb_o_Node * *tb_p566 = &(((tb_o_Comp *)tb_nn(v_c, 1357))->q_par_node); *tb_p566 = tb_v566; }
        { _Bool tb_v567 = ((tb_o_Node *)tb_nn(v_n, 1358))->q_by_vec; _Bool *tb_p567 = &(((tb_o_Comp *)tb_nn(v_c, 1358))->q_par_vec); *tb_p567 = tb_v567; }
        { long long tb_v568 = ((tb_o_Comp *)tb_nn(v_c, 1360))->q_loop_depth; long long *tb_p568 = &(((tb_o_Node *)tb_nn(v_n, 1360))->q_temp_no); *tb_p568 = tb_v568; }
        f_check__check_block(v_c, v_body);
        { long long tb_v569 = (-1LL); long long *tb_p569 = &(((tb_o_Comp *)tb_nn(v_c, 1362))->q_par_base); *tb_p569 = tb_v569; }
        { _Bool tb_v570 = 0; _Bool *tb_p570 = &(((tb_o_Comp *)tb_nn(v_c, 1363))->q_par_vec); *tb_p570 = tb_v570; }
        if (((tb_o_Node *)tb_nn(v_n, 1364))->q_by_vec) {
            {
                tb_list * tb_l571 = ((tb_o_Node *)tb_nn(v_n, 1366))->q_caps;
                for (long long tb_i571 = 0; tb_i571 < tb_l571->len; tb_i571++) {
                    tb_o_Var * const v_a = ((tb_o_Var * *)tb_l571->data)[tb_i571];
                    {
                        tb_list * tb_l572 = ((tb_o_Node *)tb_nn(v_n, 1367))->q_caps;
                        for (long long tb_i572 = 0; tb_i572 < tb_l572->len; tb_i572++) {
                            tb_o_Var * const v_b = ((tb_o_Var * *)tb_l572->data)[tb_i572];
                            if ((((strcmp(((tb_o_Var *)tb_nn(v_a, 1368))->q_name, ((tb_o_Var *)tb_nn(v_b, 1368))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_a, 1368))->q_is_shared) && (!((tb_o_Var *)tb_nn(v_b, 1368))->q_is_shared))) {
                                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1369))->q_line, "в by vec переменную, которую копишь через += или -=, нельзя читать внутри цикла", ((tb_o_Var *)tb_nn(v_a, 1369))->q_name);
                            }
                        }
                    }
                }
            }
        }
    } else {
        f_check__check_block(v_c, v_body);
    }
    { long long tb_v573 = 1LL; long long *tb_p573 = &(((tb_o_Comp *)tb_nn(v_c, 1372))->q_loop_depth); *tb_p573 -= tb_v573; }
}

static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block) {
    f_check__open_scope(v_c);
    {
        tb_list * tb_l574 = ((tb_o_Node *)tb_nn(v_block, 1376))->q_kids;
        for (long long tb_i574 = 0; tb_i574 < tb_l574->len; tb_i574++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l574->data)[tb_i574];
            f_check__check_statement(v_c, v_s);
        }
    }
    f_check__close_scope(v_c);
}

static void f_check__check_func(tb_o_Comp * v_c, long long v_fi) {
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1381))->q_funcs, v_fi, sizeof(tb_o_Func *), 1381));
    { long long tb_v575 = v_fi; long long *tb_p575 = &(((tb_o_Comp *)tb_nn(v_c, 1382))->q_current); *tb_p575 = tb_v575; }
    { const char * tb_v576 = ((tb_o_Func *)tb_nn(v_f, 1383))->q_mod; const char * *tb_p576 = &(((tb_o_Comp *)tb_nn(v_c, 1383))->q_cur_mod); *tb_p576 = tb_v576; }
    { const char * tb_v577 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1384))->q_node, 1384))->q_file; const char * *tb_p577 = &(((tb_o_Comp *)tb_nn(v_c, 1384))->q_file_name); *tb_p577 = tb_v577; }
    { long long tb_v578 = 1LL; long long *tb_p578 = &(((tb_o_Func *)tb_nn(v_f, 1385))->q_state); *tb_p578 = tb_v578; }
    { long long tb_v579 = 0LL; long long *tb_p579 = &(((tb_o_Comp *)tb_nn(v_c, 1386))->q_loop_depth); *tb_p579 = tb_v579; }
    f_check__open_scope(v_c);
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1388))->q_node, 1388))->q_kids, 0LL, sizeof(tb_o_Node *), 1388));
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1389))->q_node, 1389))->q_kids, 2LL, sizeof(tb_o_Node *), 1389));
    {
        tb_list * tb_l580 = ((tb_o_Node *)tb_nn(v_params, 1390))->q_kids;
        for (long long tb_i580 = 0; tb_i580 < tb_l580->len; tb_i580++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l580->data)[tb_i580];
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_p, 1391))->q_name, ((tb_o_Node *)tb_nn(v_p, 1391))->q_ty, 0, ((tb_o_Node *)tb_nn(v_p, 1391))->q_is_link, ((tb_o_Node *)tb_nn(v_p, 1391))->q_line);
        }
    }
    {
        tb_list * tb_l581 = ((tb_o_Node *)tb_nn(v_outputs, 1392))->q_kids;
        for (long long tb_i581 = 0; tb_i581 < tb_l581->len; tb_i581++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l581->data)[tb_i581];
            long long v_same_param = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1393))->q_name);
            if ((v_same_param >= 0LL)) {
                if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1396))->q_vars, v_same_param, sizeof(tb_o_Var *), 1396)), 1396))->q_type != ((tb_o_Node *)tb_nn(v_o, 1396))->q_ty)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1397))->q_line, "выход и параметр с одним именем должны быть одного типа", ((tb_o_Node *)tb_nn(v_o, 1397))->q_name);
                }
                { _Bool tb_v582 = 1; _Bool *tb_p582 = &(((tb_o_Node *)tb_nn(v_o, 1399))->q_is_const); *tb_p582 = tb_v582; }
            } else {
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1401))->q_name, ((tb_o_Node *)tb_nn(v_o, 1401))->q_ty, 0, 0, ((tb_o_Node *)tb_nn(v_o, 1401))->q_line);
            }
        }
    }
    f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1402))->q_node, 1402))->q_kids, 1LL, sizeof(tb_o_Node *), 1402)));
    f_check__close_scope(v_c);
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1404))->q_node, 1404))->q_ty < 0LL)) {
        { long long tb_v583 = 0LL; long long *tb_p583 = &(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1405))->q_node, 1405))->q_ty); *tb_p583 = tb_v583; }
    }
    { long long tb_v584 = 2LL; long long *tb_p584 = &(((tb_o_Func *)tb_nn(v_f, 1406))->q_state); *tb_p584 = tb_v584; }
}

static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_o, 1409))->q_name) >= 0LL) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_o, 1409))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1410))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_o, 1410))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1411))->q_objs)->len == 500LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1412))->q_line, "слишком много obj", "");
    }
    { tb_o_Node * tb_v585 = v_o; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1413))->q_objs) = tb_v585; }
}

static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn) {
    if ((((f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_mod) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_name) >= 0LL)) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_fn, 1416))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1417))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_fn, 1417))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1418))->q_funcs)->len == 1000LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1419))->q_line, "слишком много функций", "");
    }
    { tb_o_Func * tb_v586 = tb_new_Func(((tb_o_Node *)tb_nn(v_fn, 1420))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1420))->q_mod, v_fn, 0LL); *(tb_o_Func * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1420))->q_funcs) = tb_v586; }
}

static _Bool f_check__has_fail(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if ((({ const char *tb_s587 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s587[0] == 102 && (unsigned char)tb_s587[1] == 97 && (unsigned char)tb_s587[2] == 105 && (unsigned char)tb_s587[3] == 108 && tb_s587[4] == 0); }) || ({ const char *tb_s588 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s588[0] == 116 && (unsigned char)tb_s588[1] == 114 && (unsigned char)tb_s588[2] == 121 && tb_s588[3] == 0); }))) {
        return 1;
    }
    {
        tb_list * tb_l589 = ((tb_o_Node *)tb_nn(v_n, 1426))->q_kids;
        for (long long tb_i589 = 0; tb_i589 < tb_l589->len; tb_i589++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l589->data)[tb_i589];
            if (f_check__has_fail(v_k)) {
                return 1;
            }
        }
    }
    return 0;
    return v_r;
}

static _Bool f_check__has_value_return(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if ((({ const char *tb_s590 = ((tb_o_Node *)tb_nn(v_n, 1433))->q_kind; ((unsigned char)tb_s590[0] == 114 && (unsigned char)tb_s590[1] == 101 && (unsigned char)tb_s590[2] == 116 && (unsigned char)tb_s590[3] == 117 && (unsigned char)tb_s590[4] == 114 && (unsigned char)tb_s590[5] == 110 && tb_s590[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1433))->q_kids)->len > 0LL))) {
        return 1;
    }
    {
        tb_list * tb_l591 = ((tb_o_Node *)tb_nn(v_n, 1435))->q_kids;
        for (long long tb_i591 = 0; tb_i591 < tb_l591->len; tb_i591++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l591->data)[tb_i591];
            if (f_check__has_value_return(v_k)) {
                return 1;
            }
        }
    }
    return 0;
    return v_r;
}

static _Bool f_check__links_known(tb_o_Node * v_fn) {
    {
        tb_list * tb_l592 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_fn, 1441))->q_kids, 0LL, sizeof(tb_o_Node *), 1441)), 1441))->q_kids;
        for (long long tb_i592 = 0; tb_i592 < tb_l592->len; tb_i592++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l592->data)[tb_i592];
            if ((((tb_o_Node *)tb_nn(v_p, 1442))->q_ty < 0LL)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1440, "функция links_known дошла до конца без return");
    return 0;
}

static void f_check__resolve_params(tb_o_Comp * v_c, tb_o_Node * v_params) {
    {
        long long tb_end593 = ((((tb_o_Node *)tb_nn(v_params, 1447))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end593; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1448))->q_kids, v_i, sizeof(tb_o_Node *), 1448));
            {
                long long tb_end594 = (v_i - 1LL);
                for (long long v_j = 0LL; v_j <= tb_end594; v_j++) {
                    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1450))->q_kids, v_j, sizeof(tb_o_Node *), 1450)), 1450))->q_name, ((tb_o_Node *)tb_nn(v_p, 1450))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_p, 1451))->q_line, "это имя уже есть в списке", ((tb_o_Node *)tb_nn(v_p, 1451))->q_name);
                    }
                }
            }
            if (((tb_o_Node *)tb_nn(v_p, 1452))->q_has_tsyn) {
                { long long tb_v595 = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1453))->q_tsyn); long long *tb_p595 = &(((tb_o_Node *)tb_nn(v_p, 1453))->q_ty); *tb_p595 = tb_v595; }
            }
        }
    }
}

static void f_check__check(tb_o_Comp * v_c, tb_o_Node * v_program) {
    long long v_t = f_check__simple_type(v_c, "none");
    v_t = f_check__simple_type(v_c, "int");
    v_t = f_check__simple_type(v_c, "dec");
    v_t = f_check__simple_type(v_c, "bool");
    v_t = f_check__simple_type(v_c, "text");
    { long long tb_v596 = (-1LL); *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1461))->q_temp_types) = tb_v596; }
    { long long tb_v597 = (-1LL); long long *tb_p597 = &(((tb_o_Comp *)tb_nn(v_c, 1462))->q_par_base); *tb_p597 = tb_v597; }
    { long long tb_v598 = (-1LL); long long *tb_p598 = &(((tb_o_Comp *)tb_nn(v_c, 1463))->q_temp_base); *tb_p598 = tb_v598; }
    const char * v_main_mod = "";
    if (((((tb_o_Node *)tb_nn(v_program, 1467))->q_kids)->len > 0LL)) {
        v_main_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1468))->q_kids, 0LL, sizeof(tb_o_Node *), 1468)), 1468))->q_mod;
    }
    {
        tb_list * tb_l599 = ((tb_o_Node *)tb_nn(v_program, 1469))->q_kids;
        for (long long tb_i599 = 0; tb_i599 < tb_l599->len; tb_i599++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l599->data)[tb_i599];
            if ((!f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_top, 1470))->q_mod))) {
                { const char * tb_v600 = ((tb_o_Node *)tb_nn(v_top, 1471))->q_mod; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1471))->q_all_mods) = tb_v600; }
            }
            if (({ const char *tb_s601 = ((tb_o_Node *)tb_nn(v_top, 1472))->q_kind; ((unsigned char)tb_s601[0] == 117 && (unsigned char)tb_s601[1] == 115 && (unsigned char)tb_s601[2] == 101 && tb_s601[3] == 0); })) {
                if (((((tb_o_Comp *)tb_nn(v_c, 1473))->q_uses)->len == 500LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1474))->q_line, "слишком много use", "");
                }
                { tb_o_Use * tb_v602 = tb_new_Use(((tb_o_Node *)tb_nn(v_top, 1475))->q_mod, ((tb_o_Node *)tb_nn(v_top, 1475))->q_name); *(tb_o_Use * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1475))->q_uses) = tb_v602; }
            }
        }
    }
    {
        tb_list * tb_l603 = ((tb_o_Node *)tb_nn(v_program, 1477))->q_kids;
        for (long long tb_i603 = 0; tb_i603 < tb_l603->len; tb_i603++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l603->data)[tb_i603];
            { const char * tb_v604 = ((tb_o_Node *)tb_nn(v_top, 1478))->q_file; const char * *tb_p604 = &(((tb_o_Comp *)tb_nn(v_c, 1478))->q_file_name); *tb_p604 = tb_v604; }
            if (({ const char *tb_s605 = ((tb_o_Node *)tb_nn(v_top, 1479))->q_kind; ((unsigned char)tb_s605[0] == 111 && (unsigned char)tb_s605[1] == 98 && (unsigned char)tb_s605[2] == 106 && tb_s605[3] == 0); })) {
                f_check__register_obj(v_c, v_top);
            } else if (({ const char *tb_s606 = ((tb_o_Node *)tb_nn(v_top, 1481))->q_kind; ((unsigned char)tb_s606[0] == 102 && (unsigned char)tb_s606[1] == 117 && (unsigned char)tb_s606[2] == 110 && (unsigned char)tb_s606[3] == 99 && tb_s606[4] == 0); })) {
                f_check__register_func(v_c, v_top);
                { _Bool tb_v607 = f_check__has_fail((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1483))->q_kids, 1LL, sizeof(tb_o_Node *), 1483))); _Bool *tb_p607 = &(((tb_o_Node *)tb_nn(v_top, 1483))->q_can_fail); *tb_p607 = tb_v607; }
                if ((({ const char *tb_s608 = ((tb_o_Node *)tb_nn(v_top, 1484))->q_name; ((unsigned char)tb_s608[0] == 109 && (unsigned char)tb_s608[1] == 97 && (unsigned char)tb_s608[2] == 105 && (unsigned char)tb_s608[3] == 110 && tb_s608[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1484))->q_mod, v_main_mod) != 0))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1485))->q_line, "main бывает только в главном файле, в модуле её быть не должно", "");
                }
            }
        }
    }
    {
        tb_list * tb_l609 = ((tb_o_Node *)tb_nn(v_program, 1487))->q_kids;
        for (long long tb_i609 = 0; tb_i609 < tb_l609->len; tb_i609++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l609->data)[tb_i609];
            { const char * tb_v610 = ((tb_o_Node *)tb_nn(v_top, 1488))->q_file; const char * *tb_p610 = &(((tb_o_Comp *)tb_nn(v_c, 1488))->q_file_name); *tb_p610 = tb_v610; }
            if (({ const char *tb_s611 = ((tb_o_Node *)tb_nn(v_top, 1489))->q_kind; ((unsigned char)tb_s611[0] == 111 && (unsigned char)tb_s611[1] == 98 && (unsigned char)tb_s611[2] == 106 && tb_s611[3] == 0); })) {
                f_check__resolve_params(v_c, v_top);
            } else if (({ const char *tb_s612 = ((tb_o_Node *)tb_nn(v_top, 1491))->q_kind; ((unsigned char)tb_s612[0] == 102 && (unsigned char)tb_s612[1] == 117 && (unsigned char)tb_s612[2] == 110 && (unsigned char)tb_s612[3] == 99 && tb_s612[4] == 0); })) {
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1492))->q_kids, 0LL, sizeof(tb_o_Node *), 1492)));
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1493))->q_kids, 2LL, sizeof(tb_o_Node *), 1493)));
                tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1494))->q_kids, 2LL, sizeof(tb_o_Node *), 1494));
                if (((((tb_o_Node *)tb_nn(v_outputs, 1495))->q_kids)->len == 1LL)) {
                    { long long tb_v613 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1496))->q_kids, 0LL, sizeof(tb_o_Node *), 1496)), 1496))->q_ty; long long *tb_p613 = &(((tb_o_Node *)tb_nn(v_top, 1496))->q_ty); *tb_p613 = tb_v613; }
                } else if (((((tb_o_Node *)tb_nn(v_outputs, 1497))->q_kids)->len > 1LL)) {
                    { long long tb_v614 = f_check__multi_type(v_c, v_top); long long *tb_p614 = &(((tb_o_Node *)tb_nn(v_top, 1498))->q_ty); *tb_p614 = tb_v614; }
                } else if ((!f_check__has_value_return((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1499))->q_kids, 1LL, sizeof(tb_o_Node *), 1499))))) {
                    { long long tb_v615 = 0LL; long long *tb_p615 = &(((tb_o_Node *)tb_nn(v_top, 1501))->q_ty); *tb_p615 = tb_v615; }
                }
            }
        }
    }
    long long v_main_func = f_check__find_func_in(v_c, "main", v_main_mod);
    if (((((tb_o_Node *)tb_nn(v_program, 1503))->q_kids)->len > 0LL)) {
        { const char * tb_v616 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1504))->q_kids, 0LL, sizeof(tb_o_Node *), 1504)), 1504))->q_file; const char * *tb_p616 = &(((tb_o_Comp *)tb_nn(v_c, 1504))->q_file_name); *tb_p616 = tb_v616; }
    }
    if ((v_main_func < 0LL)) {
        f_errors__error_at(v_c, 0LL, "нет функции main: программа начинается с func main()", "");
    }
    tb_o_Node * v_main_node = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1507))->q_funcs, v_main_func, sizeof(tb_o_Func *), 1507)), 1507))->q_node;
    if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1508))->q_kids, 0LL, sizeof(tb_o_Node *), 1508)), 1508))->q_kids)->len != 0LL) || ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1508))->q_kids, 2LL, sizeof(tb_o_Node *), 1508)), 1508))->q_kids)->len != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_main_node, 1509))->q_line, "у main не бывает параметров и gives", "");
    }
    f_check__check_func(v_c, v_main_func);
    _Bool v_progress = 1;
    while (v_progress) {
        v_progress = 0;
        {
            long long tb_end617 = ((((tb_o_Comp *)tb_nn(v_c, 1516))->q_funcs)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end617; v_i++) {
                if (((((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1517))->q_funcs, v_i, sizeof(tb_o_Func *), 1517)), 1517))->q_state != 0LL) || (!f_check__links_known(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1517))->q_funcs, v_i, sizeof(tb_o_Func *), 1517)), 1517))->q_node)))) {
                    continue;
                }
                f_check__cut_vars(v_c, 0LL);
                { long long tb_v618 = 0LL; long long *tb_p618 = &(((tb_o_Comp *)tb_nn(v_c, 1520))->q_depth); *tb_p618 = tb_v618; }
                { long long tb_v619 = 0LL; long long *tb_p619 = &(((tb_o_Comp *)tb_nn(v_c, 1521))->q_var_base); *tb_p619 = tb_v619; }
                f_check__check_func(v_c, v_i);
                v_progress = 1;
            }
        }
    }
    {
        tb_list * tb_l620 = ((tb_o_Comp *)tb_nn(v_c, 1524))->q_funcs;
        for (long long tb_i620 = 0; tb_i620 < tb_l620->len; tb_i620++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l620->data)[tb_i620];
            if ((((tb_o_Func *)tb_nn(v_f, 1525))->q_state == 0LL)) {
                { const char * tb_v621 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1526))->q_node, 1526))->q_file; const char * *tb_p621 = &(((tb_o_Comp *)tb_nn(v_c, 1526))->q_file_name); *tb_p621 = tb_v621; }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1527))->q_node, 1527))->q_line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", ((tb_o_Func *)tb_nn(v_f, 1527))->q_name);
            }
        }
    }
    f_check__check_piece_leaks(v_c);
    f_check__mark_hot_fields(v_c);
}

static void f_emit__put(tb_o_Comp * v_c, const char * v_s) {
    { const char * tb_v622 = v_s; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 10))->q_out) = tb_v622; }
}

static const char * f_emit__fname(tb_o_Node * v_f) {
    long long tb_rl_mod = -1, tb_rc_mod = 0; (void)tb_rl_mod; (void)tb_rc_mod;
    const char * v_mod = ""; tb_rl_mod = -1;
    {
        const char * tb_l623 = ((tb_o_Node *)tb_nn(v_f, 15))->q_mod;
        for (long long tb_i623 = 0; tb_l623[tb_i623] != 0; tb_i623++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l623[tb_i623]);
            long long v_k = tb_code(v_ch, 16);
            if (((((v_k >= 97LL) && (v_k <= 122LL)) || ((v_k >= 65LL) && (v_k <= 90LL))) || ((v_k >= 48LL) && (v_k <= 57LL)))) {
                tb_app(&v_mod, &tb_rl_mod, &tb_rc_mod, v_ch); 
            } else {
                tb_app_n(&v_mod, &tb_rl_mod, &tb_rc_mod, "_", sizeof("_") - 1); 
            }
        }
    }
    return ({ const char *tb_c624_0 = "f_"; long long tb_c624_0n = sizeof("f_") - 1; const char *tb_c624_1 = v_mod; long long tb_c624_1n = (long long)strlen(tb_c624_1); const char *tb_c624_2 = "__"; long long tb_c624_2n = sizeof("__") - 1; const char *tb_c624_3 = ((tb_o_Node *)tb_nn(v_f, 21))->q_name; long long tb_c624_3n = (long long)strlen(tb_c624_3); char *tb_c624r = tb_mem(tb_c624_0n + tb_c624_1n + tb_c624_2n + tb_c624_3n + 1), *tb_c624rw = tb_c624r; memcpy(tb_c624rw, tb_c624_0, tb_c624_0n); tb_c624rw += tb_c624_0n; memcpy(tb_c624rw, tb_c624_1, tb_c624_1n); tb_c624rw += tb_c624_1n; memcpy(tb_c624rw, tb_c624_2, tb_c624_2n); tb_c624rw += tb_c624_2n; memcpy(tb_c624rw, tb_c624_3, tb_c624_3n); tb_c624rw += tb_c624_3n; *tb_c624rw = 0; (const char *)tb_c624r; });
    tb_crash(13, "функция fname дошла до конца без return");
    return "";
}

static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s625 = v_k; ((unsigned char)tb_s625[0] == 105 && (unsigned char)tb_s625[1] == 110 && (unsigned char)tb_s625[2] == 116 && tb_s625[3] == 0); })) {
        return "long long";
    }
    if (({ const char *tb_s626 = v_k; ((unsigned char)tb_s626[0] == 100 && (unsigned char)tb_s626[1] == 101 && (unsigned char)tb_s626[2] == 99 && tb_s626[3] == 0); })) {
        return "double";
    }
    if (({ const char *tb_s627 = v_k; ((unsigned char)tb_s627[0] == 98 && (unsigned char)tb_s627[1] == 111 && (unsigned char)tb_s627[2] == 111 && (unsigned char)tb_s627[3] == 108 && tb_s627[4] == 0); })) {
        return "_Bool";
    }
    if (({ const char *tb_s628 = v_k; ((unsigned char)tb_s628[0] == 116 && (unsigned char)tb_s628[1] == 101 && (unsigned char)tb_s628[2] == 120 && (unsigned char)tb_s628[3] == 116 && tb_s628[4] == 0); })) {
        return "const char *";
    }
    if (({ const char *tb_s629 = v_k; ((unsigned char)tb_s629[0] == 108 && (unsigned char)tb_s629[1] == 105 && (unsigned char)tb_s629[2] == 115 && (unsigned char)tb_s629[3] == 116 && tb_s629[4] == 0); })) {
        if (f_check__is_cols_list(v_c, v_t)) {
            return ({ const char *tb_c630_0 = "tb_cols_"; long long tb_c630_0n = sizeof("tb_cols_") - 1; const char *tb_c630_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, v_t, sizeof(tb_o_Type *), 35)), 35))->q_elem, sizeof(tb_o_Type *), 35)), 35))->q_obj_node, 35))->q_name; long long tb_c630_1n = (long long)strlen(tb_c630_1); const char *tb_c630_2 = " *"; long long tb_c630_2n = sizeof(" *") - 1; char *tb_c630r = tb_mem(tb_c630_0n + tb_c630_1n + tb_c630_2n + 1), *tb_c630rw = tb_c630r; memcpy(tb_c630rw, tb_c630_0, tb_c630_0n); tb_c630rw += tb_c630_0n; memcpy(tb_c630rw, tb_c630_1, tb_c630_1n); tb_c630rw += tb_c630_1n; memcpy(tb_c630rw, tb_c630_2, tb_c630_2n); tb_c630rw += tb_c630_2n; *tb_c630rw = 0; (const char *)tb_c630r; });
        }
        return "tb_list *";
    }
    if (({ const char *tb_s631 = v_k; ((unsigned char)tb_s631[0] == 100 && (unsigned char)tb_s631[1] == 105 && (unsigned char)tb_s631[2] == 99 && (unsigned char)tb_s631[3] == 116 && tb_s631[4] == 0); })) {
        return "tb_dict *";
    }
    if (({ const char *tb_s632 = v_k; ((unsigned char)tb_s632[0] == 111 && (unsigned char)tb_s632[1] == 98 && (unsigned char)tb_s632[2] == 106 && tb_s632[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_t)) {
            return ({ const char *tb_c633_0 = "tb_row_"; long long tb_c633_0n = sizeof("tb_row_") - 1; const char *tb_c633_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 41))->q_types, v_t, sizeof(tb_o_Type *), 41)), 41))->q_obj_node, 41))->q_name; long long tb_c633_1n = (long long)strlen(tb_c633_1); char *tb_c633r = tb_mem(tb_c633_0n + tb_c633_1n + 1), *tb_c633rw = tb_c633r; memcpy(tb_c633rw, tb_c633_0, tb_c633_0n); tb_c633rw += tb_c633_0n; memcpy(tb_c633rw, tb_c633_1, tb_c633_1n); tb_c633rw += tb_c633_1n; *tb_c633rw = 0; (const char *)tb_c633r; });
        }
        return ({ const char *tb_c634_0 = "tb_o_"; long long tb_c634_0n = sizeof("tb_o_") - 1; const char *tb_c634_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 42))->q_types, v_t, sizeof(tb_o_Type *), 42)), 42))->q_obj_node, 42))->q_name; long long tb_c634_1n = (long long)strlen(tb_c634_1); const char *tb_c634_2 = " *"; long long tb_c634_2n = sizeof(" *") - 1; char *tb_c634r = tb_mem(tb_c634_0n + tb_c634_1n + tb_c634_2n + 1), *tb_c634rw = tb_c634r; memcpy(tb_c634rw, tb_c634_0, tb_c634_0n); tb_c634rw += tb_c634_0n; memcpy(tb_c634rw, tb_c634_1, tb_c634_1n); tb_c634rw += tb_c634_1n; memcpy(tb_c634rw, tb_c634_2, tb_c634_2n); tb_c634rw += tb_c634_2n; *tb_c634rw = 0; (const char *)tb_c634r; });
    }
    if (({ const char *tb_s635 = v_k; ((unsigned char)tb_s635[0] == 109 && (unsigned char)tb_s635[1] == 117 && (unsigned char)tb_s635[2] == 108 && (unsigned char)tb_s635[3] == 116 && (unsigned char)tb_s635[4] == 105 && tb_s635[5] == 0); })) {
        return ({ const char *tb_c636_0 = "tb_r_"; long long tb_c636_0n = sizeof("tb_r_") - 1; const char *tb_c636_1 = f_emit__fname(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 44))->q_types, v_t, sizeof(tb_o_Type *), 44)), 44))->q_func_node); long long tb_c636_1n = (long long)strlen(tb_c636_1); char *tb_c636r = tb_mem(tb_c636_0n + tb_c636_1n + 1), *tb_c636rw = tb_c636r; memcpy(tb_c636rw, tb_c636_0, tb_c636_0n); tb_c636rw += tb_c636_0n; memcpy(tb_c636rw, tb_c636_1, tb_c636_1n); tb_c636rw += tb_c636_1n; *tb_c636rw = 0; (const char *)tb_c636r; });
    }
    if (({ const char *tb_s637 = v_k; ((unsigned char)tb_s637[0] == 116 && (unsigned char)tb_s637[1] == 97 && (unsigned char)tb_s637[2] == 115 && (unsigned char)tb_s637[3] == 107 && tb_s637[4] == 0); })) {
        return "tb_task *";
    }
    return "void";
    tb_crash(23, "функция c_type дошла до конца без return");
    return "";
}

static void f_emit__indent(tb_o_Comp * v_c, long long v_depth) {
    {
        long long tb_end638 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end638; v_i++) {
            f_emit__put(v_c, "    ");
        }
    }
}

static _Bool f_emit__is_bits(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s639 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s639[0] == 108 && (unsigned char)tb_s639[1] == 105 && (unsigned char)tb_s639[2] == 115 && (unsigned char)tb_s639[3] == 116 && tb_s639[4] == 0); }) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 55))->q_types, v_t, sizeof(tb_o_Type *), 55)), 55))->q_elem == 3LL));
    tb_crash(54, "функция is_bits дошла до конца без return");
    return 0;
}

static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s640 = v_k; ((unsigned char)tb_s640[0] == 105 && (unsigned char)tb_s640[1] == 110 && (unsigned char)tb_s640[2] == 116 && tb_s640[3] == 0); })) {
        f_emit__put(v_c, "0LL");
    } else if (({ const char *tb_s641 = v_k; ((unsigned char)tb_s641[0] == 100 && (unsigned char)tb_s641[1] == 101 && (unsigned char)tb_s641[2] == 99 && tb_s641[3] == 0); })) {
        f_emit__put(v_c, "0.0");
    } else if (({ const char *tb_s642 = v_k; ((unsigned char)tb_s642[0] == 116 && (unsigned char)tb_s642[1] == 101 && (unsigned char)tb_s642[2] == 120 && (unsigned char)tb_s642[3] == 116 && tb_s642[4] == 0); })) {
        f_emit__put(v_c, "\"\"");
    } else if ((({ const char *tb_s643 = v_k; ((unsigned char)tb_s643[0] == 108 && (unsigned char)tb_s643[1] == 105 && (unsigned char)tb_s643[2] == 115 && (unsigned char)tb_s643[3] == 116 && tb_s643[4] == 0); }) && f_check__is_cols_list(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c644_0 = "tb_cols_new_"; long long tb_c644_0n = sizeof("tb_cols_new_") - 1; const char *tb_c644_1 = f_emit__cols_name(v_c, v_t); long long tb_c644_1n = (long long)strlen(tb_c644_1); const char *tb_c644_2 = "()"; long long tb_c644_2n = sizeof("()") - 1; char *tb_c644r = tb_mem(tb_c644_0n + tb_c644_1n + tb_c644_2n + 1), *tb_c644rw = tb_c644r; memcpy(tb_c644rw, tb_c644_0, tb_c644_0n); tb_c644rw += tb_c644_0n; memcpy(tb_c644rw, tb_c644_1, tb_c644_1n); tb_c644rw += tb_c644_1n; memcpy(tb_c644rw, tb_c644_2, tb_c644_2n); tb_c644rw += tb_c644_2n; *tb_c644rw = 0; (const char *)tb_c644r; }));
    } else if ((({ const char *tb_s645 = v_k; ((unsigned char)tb_s645[0] == 111 && (unsigned char)tb_s645[1] == 98 && (unsigned char)tb_s645[2] == 106 && tb_s645[3] == 0); }) && f_check__is_cols_obj(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c646_0 = "("; long long tb_c646_0n = sizeof("(") - 1; const char *tb_c646_1 = f_emit__c_type(v_c, v_t); long long tb_c646_1n = (long long)strlen(tb_c646_1); const char *tb_c646_2 = "){0, 0}"; long long tb_c646_2n = sizeof("){0, 0}") - 1; char *tb_c646r = tb_mem(tb_c646_0n + tb_c646_1n + tb_c646_2n + 1), *tb_c646rw = tb_c646r; memcpy(tb_c646rw, tb_c646_0, tb_c646_0n); tb_c646rw += tb_c646_0n; memcpy(tb_c646rw, tb_c646_1, tb_c646_1n); tb_c646rw += tb_c646_1n; memcpy(tb_c646rw, tb_c646_2, tb_c646_2n); tb_c646rw += tb_c646_2n; *tb_c646rw = 0; (const char *)tb_c646r; }));
    } else if ((({ const char *tb_s647 = v_k; ((unsigned char)tb_s647[0] == 108 && (unsigned char)tb_s647[1] == 105 && (unsigned char)tb_s647[2] == 115 && (unsigned char)tb_s647[3] == 116 && tb_s647[4] == 0); }) && f_emit__is_bits(v_c, v_t))) {
        f_emit__put(v_c, "tb_bits_new()");
    } else if (({ const char *tb_s648 = v_k; ((unsigned char)tb_s648[0] == 108 && (unsigned char)tb_s648[1] == 105 && (unsigned char)tb_s648[2] == 115 && (unsigned char)tb_s648[3] == 116 && tb_s648[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c649_0 = "tb_list_new(sizeof("; long long tb_c649_0n = sizeof("tb_list_new(sizeof(") - 1; const char *tb_c649_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 72))->q_types, v_t, sizeof(tb_o_Type *), 72)), 72))->q_elem); long long tb_c649_1n = (long long)strlen(tb_c649_1); const char *tb_c649_2 = "))"; long long tb_c649_2n = sizeof("))") - 1; char *tb_c649r = tb_mem(tb_c649_0n + tb_c649_1n + tb_c649_2n + 1), *tb_c649rw = tb_c649r; memcpy(tb_c649rw, tb_c649_0, tb_c649_0n); tb_c649rw += tb_c649_0n; memcpy(tb_c649rw, tb_c649_1, tb_c649_1n); tb_c649rw += tb_c649_1n; memcpy(tb_c649rw, tb_c649_2, tb_c649_2n); tb_c649rw += tb_c649_2n; *tb_c649rw = 0; (const char *)tb_c649r; }));
    } else if (({ const char *tb_s650 = v_k; ((unsigned char)tb_s650[0] == 100 && (unsigned char)tb_s650[1] == 105 && (unsigned char)tb_s650[2] == 99 && (unsigned char)tb_s650[3] == 116 && tb_s650[4] == 0); })) {
        const char * v_is_text = "0";
        if (({ const char *tb_s651 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 75))->q_types, v_t, sizeof(tb_o_Type *), 75)), 75))->q_key); ((unsigned char)tb_s651[0] == 116 && (unsigned char)tb_s651[1] == 101 && (unsigned char)tb_s651[2] == 120 && (unsigned char)tb_s651[3] == 116 && tb_s651[4] == 0); })) {
            v_is_text = "1";
        }
        f_emit__put(v_c, ({ const char *tb_c652_0 = "tb_dict_new("; long long tb_c652_0n = sizeof("tb_dict_new(") - 1; const char *tb_c652_1 = v_is_text; long long tb_c652_1n = (long long)strlen(tb_c652_1); const char *tb_c652_2 = ", sizeof("; long long tb_c652_2n = sizeof(", sizeof(") - 1; const char *tb_c652_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 77))->q_types, v_t, sizeof(tb_o_Type *), 77)), 77))->q_elem); long long tb_c652_3n = (long long)strlen(tb_c652_3); const char *tb_c652_4 = "))"; long long tb_c652_4n = sizeof("))") - 1; char *tb_c652r = tb_mem(tb_c652_0n + tb_c652_1n + tb_c652_2n + tb_c652_3n + tb_c652_4n + 1), *tb_c652rw = tb_c652r; memcpy(tb_c652rw, tb_c652_0, tb_c652_0n); tb_c652rw += tb_c652_0n; memcpy(tb_c652rw, tb_c652_1, tb_c652_1n); tb_c652rw += tb_c652_1n; memcpy(tb_c652rw, tb_c652_2, tb_c652_2n); tb_c652rw += tb_c652_2n; memcpy(tb_c652rw, tb_c652_3, tb_c652_3n); tb_c652rw += tb_c652_3n; memcpy(tb_c652rw, tb_c652_4, tb_c652_4n); tb_c652rw += tb_c652_4n; *tb_c652rw = 0; (const char *)tb_c652r; }));
    } else {
        f_emit__put(v_c, "0");
    }
}

static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t) {
    if (({ const char *tb_s653 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s653[0] == 108 && (unsigned char)tb_s653[1] == 105 && (unsigned char)tb_s653[2] == 115 && (unsigned char)tb_s653[3] == 116 && tb_s653[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, v_t, sizeof(tb_o_Type *), 84)), 84))->q_elem, sizeof(tb_o_Type *), 84)), 84))->q_obj_node, 84))->q_name;
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 85))->q_types, v_t, sizeof(tb_o_Type *), 85)), 85))->q_obj_node, 85))->q_name;
    tb_crash(82, "функция cols_name дошла до конца без return");
    return "";
}

static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    return (((({ const char *tb_s654 = v_k; ((unsigned char)tb_s654[0] == 105 && (unsigned char)tb_s654[1] == 110 && (unsigned char)tb_s654[2] == 116 && tb_s654[3] == 0); }) || ({ const char *tb_s655 = v_k; ((unsigned char)tb_s655[0] == 100 && (unsigned char)tb_s655[1] == 101 && (unsigned char)tb_s655[2] == 99 && tb_s655[3] == 0); })) || ({ const char *tb_s656 = v_k; ((unsigned char)tb_s656[0] == 98 && (unsigned char)tb_s656[1] == 111 && (unsigned char)tb_s656[2] == 111 && (unsigned char)tb_s656[3] == 108 && tb_s656[4] == 0); })) || ({ const char *tb_s657 = v_k; ((unsigned char)tb_s657[0] == 116 && (unsigned char)tb_s657[1] == 101 && (unsigned char)tb_s657[2] == 120 && (unsigned char)tb_s657[3] == 116 && tb_s657[4] == 0); }));
    tb_crash(87, "функция is_simple дошла до конца без return");
    return 0;
}

static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key) {
    f_emit__put(v_c, ({ const char *tb_c658_0 = "&("; long long tb_c658_0n = sizeof("&(") - 1; const char *tb_c658_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 93))->q_types, v_dt, sizeof(tb_o_Type *), 93)), 93))->q_key); long long tb_c658_1n = (long long)strlen(tb_c658_1); const char *tb_c658_2 = "){"; long long tb_c658_2n = sizeof("){") - 1; char *tb_c658r = tb_mem(tb_c658_0n + tb_c658_1n + tb_c658_2n + 1), *tb_c658rw = tb_c658r; memcpy(tb_c658rw, tb_c658_0, tb_c658_0n); tb_c658rw += tb_c658_0n; memcpy(tb_c658rw, tb_c658_1, tb_c658_1n); tb_c658rw += tb_c658_1n; memcpy(tb_c658rw, tb_c658_2, tb_c658_2n); tb_c658rw += tb_c658_2n; *tb_c658rw = 0; (const char *)tb_c658r; }));
    f_emit__emit_expr(v_c, v_key);
    f_emit__put(v_c, "}");
}

static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want) {
    if ((({ const char *tb_s659 = f_check__kind_of(v_c, v_want); ((unsigned char)tb_s659[0] == 100 && (unsigned char)tb_s659[1] == 101 && (unsigned char)tb_s659[2] == 99 && tb_s659[3] == 0); }) && ({ const char *tb_s660 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 101))->q_ty); ((unsigned char)tb_s660[0] == 105 && (unsigned char)tb_s660[1] == 110 && (unsigned char)tb_s660[2] == 116 && tb_s660[3] == 0); }))) {
        f_emit__put(v_c, "(double)(");
        f_emit__emit_expr(v_c, v_n);
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__emit_expr(v_c, v_n);
}

static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 110))->q_ty);
    if (({ const char *tb_s661 = v_k; ((unsigned char)tb_s661[0] == 116 && (unsigned char)tb_s661[1] == 101 && (unsigned char)tb_s661[2] == 120 && (unsigned char)tb_s661[3] == 116 && tb_s661[4] == 0); })) {
        f_emit__emit_q(v_c, v_n);
        return;
    }
    if (({ const char *tb_s662 = v_k; ((unsigned char)tb_s662[0] == 105 && (unsigned char)tb_s662[1] == 110 && (unsigned char)tb_s662[2] == 116 && tb_s662[3] == 0); })) {
        f_emit__put(v_c, "tb_itos(");
    } else if (({ const char *tb_s663 = v_k; ((unsigned char)tb_s663[0] == 100 && (unsigned char)tb_s663[1] == 101 && (unsigned char)tb_s663[2] == 99 && tb_s663[3] == 0); })) {
        f_emit__put(v_c, "tb_dtos(");
    } else {
        f_emit__put(v_c, "tb_btos(");
    }
    f_emit__emit_expr(v_c, v_n);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_print(tb_o_Comp * v_c, tb_o_Node * v_n) {
    f_emit__put(v_c, "(");
    {
        long long tb_end664 = ((((tb_o_Node *)tb_nn(v_n, 125))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end664; v_i++) {
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 126))->q_kids, v_i, sizeof(tb_o_Node *), 126)), 126))->q_kids, 0LL, sizeof(tb_o_Node *), 126));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", tb_ps(\" \"), ");
            }
            f_emit__put(v_c, ({ const char *tb_c665_0 = "tb_show_"; long long tb_c665_0n = sizeof("tb_show_") - 1; char tb_c665_1d[24]; long long tb_c665_1n; const char *tb_c665_1 = tb_digits(((tb_o_Node *)tb_nn(v_value, 129))->q_ty, tb_c665_1d, &tb_c665_1n); const char *tb_c665_2 = "("; long long tb_c665_2n = sizeof("(") - 1; char *tb_c665r = tb_mem(tb_c665_0n + tb_c665_1n + tb_c665_2n + 1), *tb_c665rw = tb_c665r; memcpy(tb_c665rw, tb_c665_0, tb_c665_0n); tb_c665rw += tb_c665_0n; memcpy(tb_c665rw, tb_c665_1, tb_c665_1n); tb_c665rw += tb_c665_1n; memcpy(tb_c665rw, tb_c665_2, tb_c665_2n); tb_c665rw += tb_c665_2n; *tb_c665rw = 0; (const char *)tb_c665r; }));
            f_emit__emit_q(v_c, v_value);
            f_emit__put(v_c, ")");
        }
    }
    if (((((tb_o_Node *)tb_nn(v_n, 132))->q_kids)->len > 0LL)) {
        f_emit__put(v_c, ", ");
    }
    f_emit__put(v_c, "tb_ps(\"\\n\"))");
}

static void f_emit__emit_args(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_with_line) {
    {
        long long tb_end666 = ((((tb_o_Node *)tb_nn(v_n, 138))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end666; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 141))->q_kids, v_i, sizeof(tb_o_Node *), 141)), 141))->q_kids, 0LL, sizeof(tb_o_Node *), 141)));
        }
    }
    if (v_with_line) {
        if (((((tb_o_Node *)tb_nn(v_n, 143))->q_kids)->len > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__put(v_c, ({ const char *tb_c667_0 = ""; long long tb_c667_0n = sizeof("") - 1; char tb_c667_1d[24]; long long tb_c667_1n; const char *tb_c667_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 145))->q_line, tb_c667_1d, &tb_c667_1n); char *tb_c667r = tb_mem(tb_c667_0n + tb_c667_1n + 1), *tb_c667rw = tb_c667r; memcpy(tb_c667rw, tb_c667_0, tb_c667_0n); tb_c667rw += tb_c667_0n; memcpy(tb_c667rw, tb_c667_1, tb_c667_1n); tb_c667rw += tb_c667_1n; *tb_c667rw = 0; (const char *)tb_c667r; }));
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 149))->q_name;
    if (({ const char *tb_s668 = v_name; ((unsigned char)tb_s668[0] == 112 && (unsigned char)tb_s668[1] == 114 && (unsigned char)tb_s668[2] == 105 && (unsigned char)tb_s668[3] == 110 && (unsigned char)tb_s668[4] == 116 && tb_s668[5] == 0); })) {
        f_emit__emit_print(v_c, v_n);
        return;
    }
    if (({ const char *tb_s669 = v_name; ((unsigned char)tb_s669[0] == 97 && (unsigned char)tb_s669[1] == 114 && (unsigned char)tb_s669[2] == 103 && (unsigned char)tb_s669[3] == 115 && tb_s669[4] == 0); })) {
        f_emit__put(v_c, "tb_args");
        return;
    }
    if (({ const char *tb_s670 = v_name; ((unsigned char)tb_s670[0] == 105 && (unsigned char)tb_s670[1] == 110 && (unsigned char)tb_s670[2] == 112 && (unsigned char)tb_s670[3] == 117 && (unsigned char)tb_s670[4] == 116 && tb_s670[5] == 0); })) {
        f_emit__put(v_c, "tb_input()");
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159)), 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159));
    if (({ const char *tb_s671 = v_name; ((unsigned char)tb_s671[0] == 108 && (unsigned char)tb_s671[1] == 101 && (unsigned char)tb_s671[2] == 110 && tb_s671[3] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 161))->q_ty);
        if ((({ const char *tb_s672 = v_k; ((unsigned char)tb_s672[0] == 116 && (unsigned char)tb_s672[1] == 101 && (unsigned char)tb_s672[2] == 120 && (unsigned char)tb_s672[3] == 116 && tb_s672[4] == 0); }) && f_emit__is_bld(v_c, v_a))) {
            f_emit__put(v_c, ({ const char *tb_c673_0 = "(tb_rl_"; long long tb_c673_0n = sizeof("(tb_rl_") - 1; const char *tb_c673_1 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c673_1n = (long long)strlen(tb_c673_1); const char *tb_c673_2 = " >= 0 ? tb_rl_"; long long tb_c673_2n = sizeof(" >= 0 ? tb_rl_") - 1; const char *tb_c673_3 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c673_3n = (long long)strlen(tb_c673_3); const char *tb_c673_4 = " : tb_tlen(v_"; long long tb_c673_4n = sizeof(" : tb_tlen(v_") - 1; const char *tb_c673_5 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c673_5n = (long long)strlen(tb_c673_5); const char *tb_c673_6 = "))"; long long tb_c673_6n = sizeof("))") - 1; char *tb_c673r = tb_mem(tb_c673_0n + tb_c673_1n + tb_c673_2n + tb_c673_3n + tb_c673_4n + tb_c673_5n + tb_c673_6n + 1), *tb_c673rw = tb_c673r; memcpy(tb_c673rw, tb_c673_0, tb_c673_0n); tb_c673rw += tb_c673_0n; memcpy(tb_c673rw, tb_c673_1, tb_c673_1n); tb_c673rw += tb_c673_1n; memcpy(tb_c673rw, tb_c673_2, tb_c673_2n); tb_c673rw += tb_c673_2n; memcpy(tb_c673rw, tb_c673_3, tb_c673_3n); tb_c673rw += tb_c673_3n; memcpy(tb_c673rw, tb_c673_4, tb_c673_4n); tb_c673rw += tb_c673_4n; memcpy(tb_c673rw, tb_c673_5, tb_c673_5n); tb_c673rw += tb_c673_5n; memcpy(tb_c673rw, tb_c673_6, tb_c673_6n); tb_c673rw += tb_c673_6n; *tb_c673rw = 0; (const char *)tb_c673r; }));
        } else if (({ const char *tb_s674 = v_k; ((unsigned char)tb_s674[0] == 116 && (unsigned char)tb_s674[1] == 101 && (unsigned char)tb_s674[2] == 120 && (unsigned char)tb_s674[3] == 116 && tb_s674[4] == 0); })) {
            f_emit__put(v_c, "tb_tlen(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")");
        } else if (({ const char *tb_s675 = v_k; ((unsigned char)tb_s675[0] == 100 && (unsigned char)tb_s675[1] == 105 && (unsigned char)tb_s675[2] == 99 && (unsigned char)tb_s675[3] == 116 && tb_s675[4] == 0); })) {
            f_emit__put(v_c, "(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")->keys->len");
        } else {
            f_emit__put(v_c, "(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")->len");
        }
        return;
    }
    if (({ const char *tb_s676 = v_name; ((unsigned char)tb_s676[0] == 115 && (unsigned char)tb_s676[1] == 111 && (unsigned char)tb_s676[2] == 114 && (unsigned char)tb_s676[3] == 116 && tb_s676[4] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 178))->q_types, ((tb_o_Node *)tb_nn(v_a, 178))->q_ty, sizeof(tb_o_Type *), 178)), 178))->q_elem);
        f_emit__put(v_c, "tb_sort_list(");
        f_emit__emit_expr(v_c, v_a);
        if (({ const char *tb_s677 = v_k; ((unsigned char)tb_s677[0] == 105 && (unsigned char)tb_s677[1] == 110 && (unsigned char)tb_s677[2] == 116 && tb_s677[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_int)");
        } else if (({ const char *tb_s678 = v_k; ((unsigned char)tb_s678[0] == 100 && (unsigned char)tb_s678[1] == 101 && (unsigned char)tb_s678[2] == 99 && tb_s678[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_dec)");
        } else {
            f_emit__put(v_c, ", tb_cmp_text)");
        }
        return;
    }
    if (({ const char *tb_s679 = v_name; ((unsigned char)tb_s679[0] == 97 && (unsigned char)tb_s679[1] == 98 && (unsigned char)tb_s679[2] == 115 && tb_s679[3] == 0); })) {
        if (({ const char *tb_s680 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 189))->q_ty); ((unsigned char)tb_s680[0] == 105 && (unsigned char)tb_s680[1] == 110 && (unsigned char)tb_s680[2] == 116 && tb_s680[3] == 0); })) {
            f_emit__put(v_c, "tb_absi(");
        } else {
            f_emit__put(v_c, "fabs(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ")");
        return;
    }
    if (({ const char *tb_s681 = v_name; ((unsigned char)tb_s681[0] == 115 && (unsigned char)tb_s681[1] == 113 && (unsigned char)tb_s681[2] == 114 && (unsigned char)tb_s681[3] == 116 && tb_s681[4] == 0); })) {
        f_emit__put(v_c, "tb_sqrt((double)(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ({ const char *tb_c682_0 = "), "; long long tb_c682_0n = sizeof("), ") - 1; char tb_c682_1d[24]; long long tb_c682_1n; const char *tb_c682_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 199))->q_line, tb_c682_1d, &tb_c682_1n); const char *tb_c682_2 = ")"; long long tb_c682_2n = sizeof(")") - 1; char *tb_c682r = tb_mem(tb_c682_0n + tb_c682_1n + tb_c682_2n + 1), *tb_c682rw = tb_c682r; memcpy(tb_c682rw, tb_c682_0, tb_c682_0n); tb_c682rw += tb_c682_0n; memcpy(tb_c682rw, tb_c682_1, tb_c682_1n); tb_c682rw += tb_c682_1n; memcpy(tb_c682rw, tb_c682_2, tb_c682_2n); tb_c682rw += tb_c682_2n; *tb_c682rw = 0; (const char *)tb_c682r; }));
        return;
    }
    if ((({ const char *tb_s683 = v_name; ((unsigned char)tb_s683[0] == 109 && (unsigned char)tb_s683[1] == 105 && (unsigned char)tb_s683[2] == 110 && tb_s683[3] == 0); }) || ({ const char *tb_s684 = v_name; ((unsigned char)tb_s684[0] == 109 && (unsigned char)tb_s684[1] == 97 && (unsigned char)tb_s684[2] == 120 && tb_s684[3] == 0); }))) {
        _Bool v_ints = ({ const char *tb_s685 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 202))->q_ty); ((unsigned char)tb_s685[0] == 105 && (unsigned char)tb_s685[1] == 110 && (unsigned char)tb_s685[2] == 116 && tb_s685[3] == 0); });
        if (v_ints) {
            f_emit__put(v_c, ({ const char *tb_c686_0 = "tb_"; long long tb_c686_0n = sizeof("tb_") - 1; const char *tb_c686_1 = v_name; long long tb_c686_1n = (long long)strlen(tb_c686_1); const char *tb_c686_2 = "i("; long long tb_c686_2n = sizeof("i(") - 1; char *tb_c686r = tb_mem(tb_c686_0n + tb_c686_1n + tb_c686_2n + 1), *tb_c686rw = tb_c686r; memcpy(tb_c686rw, tb_c686_0, tb_c686_0n); tb_c686rw += tb_c686_0n; memcpy(tb_c686rw, tb_c686_1, tb_c686_1n); tb_c686rw += tb_c686_1n; memcpy(tb_c686rw, tb_c686_2, tb_c686_2n); tb_c686rw += tb_c686_2n; *tb_c686rw = 0; (const char *)tb_c686r; }));
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 207))->q_kids, 1LL, sizeof(tb_o_Node *), 207)), 207))->q_kids, 0LL, sizeof(tb_o_Node *), 207)));
        } else {
            f_emit__put(v_c, ({ const char *tb_c687_0 = "tb_"; long long tb_c687_0n = sizeof("tb_") - 1; const char *tb_c687_1 = v_name; long long tb_c687_1n = (long long)strlen(tb_c687_1); const char *tb_c687_2 = "d("; long long tb_c687_2n = sizeof("d(") - 1; char *tb_c687r = tb_mem(tb_c687_0n + tb_c687_1n + tb_c687_2n + 1), *tb_c687rw = tb_c687r; memcpy(tb_c687rw, tb_c687_0, tb_c687_0n); tb_c687rw += tb_c687_0n; memcpy(tb_c687rw, tb_c687_1, tb_c687_1n); tb_c687rw += tb_c687_1n; memcpy(tb_c687rw, tb_c687_2, tb_c687_2n); tb_c687rw += tb_c687_2n; *tb_c687rw = 0; (const char *)tb_c687r; }));
            f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 210))->q_ty);
            f_emit__put(v_c, ", ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 212))->q_kids, 1LL, sizeof(tb_o_Node *), 212)), 212))->q_kids, 0LL, sizeof(tb_o_Node *), 212)), ((tb_o_Node *)tb_nn(v_n, 212))->q_ty);
        }
        f_emit__put(v_c, ")");
        return;
    }
    tb_list * v_c_names = tb_list_from(60, sizeof(const char *), (const char *[]){"round", "tb_round", "0", "char", "tb_chr", "0", "cut", "tb_cut", "1", "code", "tb_code", "1", "to_int", "tb_to_int", "0", "to_dec", "tb_to_dec", "0", "read_file", "tb_read_file", "0", "write_file", "tb_write_file", "0", "split", "tb_split", "1", "join", "tb_join", "0", "find", "tb_find", "0", "replace", "tb_replace", "1", "upper", "tb_upper", "0", "lower", "tb_lower", "0", "trim", "tb_trim", "0", "starts_with", "tb_starts", "0", "ends_with", "tb_ends", "0", "run", "tb_run", "0", "exit", "tb_exit", "0", "file_exists", "tb_file_exists", "0"});
    long long v_i = 0LL;
    while ((v_i < (v_c_names)->len)) {
        if ((strcmp((*(const char * *)tb_at(v_c_names, v_i, sizeof(const char *), 219)), v_name) == 0)) {
            f_emit__put(v_c, ({ const char *tb_c688_0 = (*(const char * *)tb_at(v_c_names, (v_i + 1LL), sizeof(const char *), 220)); long long tb_c688_0n = (long long)strlen(tb_c688_0); const char *tb_c688_1 = "("; long long tb_c688_1n = sizeof("(") - 1; char *tb_c688r = tb_mem(tb_c688_0n + tb_c688_1n + 1), *tb_c688rw = tb_c688r; memcpy(tb_c688rw, tb_c688_0, tb_c688_0n); tb_c688rw += tb_c688_0n; memcpy(tb_c688rw, tb_c688_1, tb_c688_1n); tb_c688rw += tb_c688_1n; *tb_c688rw = 0; (const char *)tb_c688r; }));
            f_emit__emit_args(v_c, v_n, ({ const char *tb_s689 = (*(const char * *)tb_at(v_c_names, (v_i + 2LL), sizeof(const char *), 221)); ((unsigned char)tb_s689[0] == 49 && tb_s689[1] == 0); }));
            return;
        }
        { long long tb_v690 = 3LL; long long *tb_p690 = &(v_i); *tb_p690 += tb_v690; }
    }
    f_emit__put(v_c, ({ const char *tb_c691_0 = "/* неизвестная встроенная "; long long tb_c691_0n = sizeof("/* неизвестная встроенная ") - 1; const char *tb_c691_1 = v_name; long long tb_c691_1n = (long long)strlen(tb_c691_1); const char *tb_c691_2 = " */0"; long long tb_c691_2n = sizeof(" */0") - 1; char *tb_c691r = tb_mem(tb_c691_0n + tb_c691_1n + tb_c691_2n + 1), *tb_c691rw = tb_c691r; memcpy(tb_c691rw, tb_c691_0, tb_c691_0n); tb_c691rw += tb_c691_0n; memcpy(tb_c691rw, tb_c691_1, tb_c691_1n); tb_c691rw += tb_c691_1n; memcpy(tb_c691rw, tb_c691_2, tb_c691_2n); tb_c691rw += tb_c691_2n; *tb_c691rw = 0; (const char *)tb_c691r; }));
}

static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((!((tb_o_Node *)tb_nn(v_n, 227))->q_has_ref)) {
        f_emit__emit_builtin(v_c, v_n);
        return;
    }
    if (({ const char *tb_s692 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 230))->q_ref, 230))->q_kind; ((unsigned char)tb_s692[0] == 111 && (unsigned char)tb_s692[1] == 98 && (unsigned char)tb_s692[2] == 106 && tb_s692[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_n, 231))->q_ref;
        f_emit__put(v_c, ({ const char *tb_c693_0 = "tb_new_"; long long tb_c693_0n = sizeof("tb_new_") - 1; const char *tb_c693_1 = ((tb_o_Node *)tb_nn(v_o, 232))->q_name; long long tb_c693_1n = (long long)strlen(tb_c693_1); const char *tb_c693_2 = "("; long long tb_c693_2n = sizeof("(") - 1; char *tb_c693r = tb_mem(tb_c693_0n + tb_c693_1n + tb_c693_2n + 1), *tb_c693rw = tb_c693r; memcpy(tb_c693rw, tb_c693_0, tb_c693_0n); tb_c693rw += tb_c693_0n; memcpy(tb_c693rw, tb_c693_1, tb_c693_1n); tb_c693rw += tb_c693_1n; memcpy(tb_c693rw, tb_c693_2, tb_c693_2n); tb_c693rw += tb_c693_2n; *tb_c693rw = 0; (const char *)tb_c693r; }));
        {
            long long tb_end694 = ((((tb_o_Node *)tb_nn(v_o, 233))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end694; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                if (({ const char *tb_s695 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 236))->q_kids, v_i, sizeof(tb_o_Node *), 236)), 236))->q_kind; ((unsigned char)tb_s695[0] == 109 && (unsigned char)tb_s695[1] == 105 && (unsigned char)tb_s695[2] == 115 && (unsigned char)tb_s695[3] == 115 && (unsigned char)tb_s695[4] == 105 && (unsigned char)tb_s695[5] == 110 && (unsigned char)tb_s695[6] == 103 && tb_s695[7] == 0); })) {
                    f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 237))->q_kids, v_i, sizeof(tb_o_Node *), 237)), 237))->q_ty);
                } else {
                    f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 239))->q_kids, v_i, sizeof(tb_o_Node *), 239)), 239))->q_kids, 0LL, sizeof(tb_o_Node *), 239)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 239))->q_kids, v_i, sizeof(tb_o_Node *), 239)), 239))->q_ty);
                }
            }
        }
        f_emit__put(v_c, ")");
        return;
    }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 242))->q_ref, 242))->q_kids, 0LL, sizeof(tb_o_Node *), 242));
    f_emit__put(v_c, ({ const char *tb_c696_0 = f_emit__fname(((tb_o_Node *)tb_nn(v_n, 243))->q_ref); long long tb_c696_0n = (long long)strlen(tb_c696_0); const char *tb_c696_1 = "("; long long tb_c696_1n = sizeof("(") - 1; char *tb_c696r = tb_mem(tb_c696_0n + tb_c696_1n + 1), *tb_c696rw = tb_c696r; memcpy(tb_c696rw, tb_c696_0, tb_c696_0n); tb_c696rw += tb_c696_0n; memcpy(tb_c696rw, tb_c696_1, tb_c696_1n); tb_c696rw += tb_c696_1n; *tb_c696rw = 0; (const char *)tb_c696r; }));
    {
        long long tb_end697 = ((((tb_o_Node *)tb_nn(v_n, 244))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end697; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 247))->q_kids, v_i, sizeof(tb_o_Node *), 247)), 247))->q_is_link) {
                f_emit__put(v_c, "&(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 249))->q_kids, v_i, sizeof(tb_o_Node *), 249)), 249))->q_kids, 0LL, sizeof(tb_o_Node *), 249)));
                f_emit__put(v_c, ")");
            } else {
                f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, v_i, sizeof(tb_o_Node *), 252)), 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 252))->q_kids, v_i, sizeof(tb_o_Node *), 252)), 252))->q_ty);
            }
        }
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_dict(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Type * v_t = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 256))->q_types, ((tb_o_Node *)tb_nn(v_n, 256))->q_ty, sizeof(tb_o_Type *), 256));
    const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 257))->q_key);
    const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 258))->q_elem);
    const char * v_is_text = "0";
    if (({ const char *tb_s698 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn(v_t, 260))->q_key); ((unsigned char)tb_s698[0] == 116 && (unsigned char)tb_s698[1] == 101 && (unsigned char)tb_s698[2] == 120 && (unsigned char)tb_s698[3] == 116 && tb_s698[4] == 0); })) {
        v_is_text = "1";
    }
    f_emit__put(v_c, ({ const char *tb_c699_0 = "tb_dict_from("; long long tb_c699_0n = sizeof("tb_dict_from(") - 1; const char *tb_c699_1 = v_is_text; long long tb_c699_1n = (long long)strlen(tb_c699_1); const char *tb_c699_2 = ", sizeof("; long long tb_c699_2n = sizeof(", sizeof(") - 1; const char *tb_c699_3 = v_vt; long long tb_c699_3n = (long long)strlen(tb_c699_3); const char *tb_c699_4 = "), "; long long tb_c699_4n = sizeof("), ") - 1; char tb_c699_5d[24]; long long tb_c699_5n; const char *tb_c699_5 = tb_digits(tb_div((((tb_o_Node *)tb_nn(v_n, 262))->q_kids)->len, 2LL, 262), tb_c699_5d, &tb_c699_5n); const char *tb_c699_6 = ", ("; long long tb_c699_6n = sizeof(", (") - 1; const char *tb_c699_7 = v_kt; long long tb_c699_7n = (long long)strlen(tb_c699_7); const char *tb_c699_8 = "[]){"; long long tb_c699_8n = sizeof("[]){") - 1; char *tb_c699r = tb_mem(tb_c699_0n + tb_c699_1n + tb_c699_2n + tb_c699_3n + tb_c699_4n + tb_c699_5n + tb_c699_6n + tb_c699_7n + tb_c699_8n + 1), *tb_c699rw = tb_c699r; memcpy(tb_c699rw, tb_c699_0, tb_c699_0n); tb_c699rw += tb_c699_0n; memcpy(tb_c699rw, tb_c699_1, tb_c699_1n); tb_c699rw += tb_c699_1n; memcpy(tb_c699rw, tb_c699_2, tb_c699_2n); tb_c699rw += tb_c699_2n; memcpy(tb_c699rw, tb_c699_3, tb_c699_3n); tb_c699rw += tb_c699_3n; memcpy(tb_c699rw, tb_c699_4, tb_c699_4n); tb_c699rw += tb_c699_4n; memcpy(tb_c699rw, tb_c699_5, tb_c699_5n); tb_c699rw += tb_c699_5n; memcpy(tb_c699rw, tb_c699_6, tb_c699_6n); tb_c699rw += tb_c699_6n; memcpy(tb_c699rw, tb_c699_7, tb_c699_7n); tb_c699rw += tb_c699_7n; memcpy(tb_c699rw, tb_c699_8, tb_c699_8n); tb_c699rw += tb_c699_8n; *tb_c699rw = 0; (const char *)tb_c699r; }));
    long long v_i = 0LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 264))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 267))->q_kids, v_i, sizeof(tb_o_Node *), 267)));
        { long long tb_v700 = 2LL; long long *tb_p700 = &(v_i); *tb_p700 += tb_v700; }
    }
    f_emit__put(v_c, ({ const char *tb_c701_0 = "}, ("; long long tb_c701_0n = sizeof("}, (") - 1; const char *tb_c701_1 = v_vt; long long tb_c701_1n = (long long)strlen(tb_c701_1); const char *tb_c701_2 = "[]){"; long long tb_c701_2n = sizeof("[]){") - 1; char *tb_c701r = tb_mem(tb_c701_0n + tb_c701_1n + tb_c701_2n + 1), *tb_c701rw = tb_c701r; memcpy(tb_c701rw, tb_c701_0, tb_c701_0n); tb_c701rw += tb_c701_0n; memcpy(tb_c701rw, tb_c701_1, tb_c701_1n); tb_c701rw += tb_c701_1n; memcpy(tb_c701rw, tb_c701_2, tb_c701_2n); tb_c701rw += tb_c701_2n; *tb_c701rw = 0; (const char *)tb_c701r; }));
    v_i = 1LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 271))->q_kids)->len)) {
        if ((v_i > 1LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 274))->q_kids, v_i, sizeof(tb_o_Node *), 274)), ((tb_o_Type *)tb_nn(v_t, 274))->q_elem);
        { long long tb_v702 = 2LL; long long *tb_p702 = &(v_i); *tb_p702 += tb_v702; }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 280))->q_kids, 0LL, sizeof(tb_o_Node *), 280));
    if (({ const char *tb_s703 = ((tb_o_Node *)tb_nn(v_n, 281))->q_name; ((unsigned char)tb_s703[0] == 99 && (unsigned char)tb_s703[1] == 108 && (unsigned char)tb_s703[2] == 101 && (unsigned char)tb_s703[3] == 97 && (unsigned char)tb_s703[4] == 114 && tb_s703[5] == 0); })) {
        if (({ const char *tb_s704 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 282))->q_ty); ((unsigned char)tb_s704[0] == 100 && (unsigned char)tb_s704[1] == 105 && (unsigned char)tb_s704[2] == 99 && (unsigned char)tb_s704[3] == 116 && tb_s704[4] == 0); })) {
            f_emit__put(v_c, "tb_dclear(");
        } else {
            f_emit__put(v_c, "tb_lclear(");
        }
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ({ const char *tb_c705_0 = ", "; long long tb_c705_0n = sizeof(", ") - 1; char tb_c705_1d[24]; long long tb_c705_1n; const char *tb_c705_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 287))->q_line, tb_c705_1d, &tb_c705_1n); const char *tb_c705_2 = ")"; long long tb_c705_2n = sizeof(")") - 1; char *tb_c705r = tb_mem(tb_c705_0n + tb_c705_1n + tb_c705_2n + 1), *tb_c705rw = tb_c705r; memcpy(tb_c705rw, tb_c705_0, tb_c705_0n); tb_c705rw += tb_c705_0n; memcpy(tb_c705rw, tb_c705_1, tb_c705_1n); tb_c705rw += tb_c705_1n; memcpy(tb_c705rw, tb_c705_2, tb_c705_2n); tb_c705rw += tb_c705_2n; *tb_c705rw = 0; (const char *)tb_c705r; }));
        return;
    }
    if (({ const char *tb_s706 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 289))->q_ty); ((unsigned char)tb_s706[0] == 100 && (unsigned char)tb_s706[1] == 105 && (unsigned char)tb_s706[2] == 99 && (unsigned char)tb_s706[3] == 116 && tb_s706[4] == 0); })) {
        if ((({ const char *tb_s707 = ((tb_o_Node *)tb_nn(v_n, 290))->q_name; ((unsigned char)tb_s707[0] == 118 && (unsigned char)tb_s707[1] == 97 && (unsigned char)tb_s707[2] == 108 && (unsigned char)tb_s707[3] == 117 && (unsigned char)tb_s707[4] == 101 && (unsigned char)tb_s707[5] == 115 && tb_s707[6] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 290))->q_ty))) {
            f_emit__put(v_c, ({ const char *tb_c708_0 = "tb_colsfrom_"; long long tb_c708_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c708_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 291))->q_ty); long long tb_c708_1n = (long long)strlen(tb_c708_1); const char *tb_c708_2 = "(("; long long tb_c708_2n = sizeof("((") - 1; char *tb_c708r = tb_mem(tb_c708_0n + tb_c708_1n + tb_c708_2n + 1), *tb_c708rw = tb_c708r; memcpy(tb_c708rw, tb_c708_0, tb_c708_0n); tb_c708rw += tb_c708_0n; memcpy(tb_c708rw, tb_c708_1, tb_c708_1n); tb_c708rw += tb_c708_1n; memcpy(tb_c708rw, tb_c708_2, tb_c708_2n); tb_c708rw += tb_c708_2n; *tb_c708rw = 0; (const char *)tb_c708r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ({ const char *tb_c709_0 = ")->vals->len, ("; long long tb_c709_0n = sizeof(")->vals->len, (") - 1; const char *tb_c709_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 293))->q_types, ((tb_o_Node *)tb_nn(v_n, 293))->q_ty, sizeof(tb_o_Type *), 293)), 293))->q_elem); long long tb_c709_1n = (long long)strlen(tb_c709_1); const char *tb_c709_2 = " *)("; long long tb_c709_2n = sizeof(" *)(") - 1; char *tb_c709r = tb_mem(tb_c709_0n + tb_c709_1n + tb_c709_2n + 1), *tb_c709rw = tb_c709r; memcpy(tb_c709rw, tb_c709_0, tb_c709_0n); tb_c709rw += tb_c709_0n; memcpy(tb_c709rw, tb_c709_1, tb_c709_1n); tb_c709rw += tb_c709_1n; memcpy(tb_c709rw, tb_c709_2, tb_c709_2n); tb_c709rw += tb_c709_2n; *tb_c709rw = 0; (const char *)tb_c709r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals->data)");
            return;
        }
        if ((({ const char *tb_s710 = ((tb_o_Node *)tb_nn(v_n, 297))->q_name; ((unsigned char)tb_s710[0] == 118 && (unsigned char)tb_s710[1] == 97 && (unsigned char)tb_s710[2] == 108 && (unsigned char)tb_s710[3] == 117 && (unsigned char)tb_s710[4] == 101 && (unsigned char)tb_s710[5] == 115 && tb_s710[6] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 297))->q_ty))) {
            f_emit__put(v_c, "tb_bits_of_bytes((");
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals)");
            return;
        }
        if ((({ const char *tb_s711 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s711[0] == 107 && (unsigned char)tb_s711[1] == 101 && (unsigned char)tb_s711[2] == 121 && (unsigned char)tb_s711[3] == 115 && tb_s711[4] == 0); }) || ({ const char *tb_s712 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s712[0] == 118 && (unsigned char)tb_s712[1] == 97 && (unsigned char)tb_s712[2] == 108 && (unsigned char)tb_s712[3] == 117 && (unsigned char)tb_s712[4] == 101 && (unsigned char)tb_s712[5] == 115 && tb_s712[6] == 0); }))) {
            f_emit__put(v_c, "tb_copy((");
            f_emit__emit_expr(v_c, v_base);
            if (({ const char *tb_s713 = ((tb_o_Node *)tb_nn(v_n, 305))->q_name; ((unsigned char)tb_s713[0] == 107 && (unsigned char)tb_s713[1] == 101 && (unsigned char)tb_s713[2] == 121 && (unsigned char)tb_s713[3] == 115 && tb_s713[4] == 0); })) {
                f_emit__put(v_c, ")->keys)");
            } else {
                f_emit__put(v_c, ")->vals)");
            }
            return;
        }
        if (({ const char *tb_s714 = ((tb_o_Node *)tb_nn(v_n, 310))->q_name; ((unsigned char)tb_s714[0] == 104 && (unsigned char)tb_s714[1] == 97 && (unsigned char)tb_s714[2] == 115 && tb_s714[3] == 0); })) {
            f_emit__put(v_c, "tb_dhas(");
        } else {
            f_emit__put(v_c, "tb_dremove(");
        }
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn(v_base, 316))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 316))->q_kids, 1LL, sizeof(tb_o_Node *), 316)), 316))->q_kids, 0LL, sizeof(tb_o_Node *), 316)));
        f_emit__put(v_c, ")");
        return;
    }
    if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_base, 319))->q_ty)) {
        f_emit__put(v_c, "tb_bpop(");
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ({ const char *tb_c715_0 = ", "; long long tb_c715_0n = sizeof(", ") - 1; char tb_c715_1d[24]; long long tb_c715_1n; const char *tb_c715_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 322))->q_line, tb_c715_1d, &tb_c715_1n); const char *tb_c715_2 = ")"; long long tb_c715_2n = sizeof(")") - 1; char *tb_c715r = tb_mem(tb_c715_0n + tb_c715_1n + tb_c715_2n + 1), *tb_c715rw = tb_c715r; memcpy(tb_c715rw, tb_c715_0, tb_c715_0n); tb_c715rw += tb_c715_0n; memcpy(tb_c715rw, tb_c715_1, tb_c715_1n); tb_c715rw += tb_c715_1n; memcpy(tb_c715rw, tb_c715_2, tb_c715_2n); tb_c715rw += tb_c715_2n; *tb_c715rw = 0; (const char *)tb_c715r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c716_0 = "(*("; long long tb_c716_0n = sizeof("(*(") - 1; const char *tb_c716_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 324))->q_ty); long long tb_c716_1n = (long long)strlen(tb_c716_1); const char *tb_c716_2 = " *)tb_pop("; long long tb_c716_2n = sizeof(" *)tb_pop(") - 1; char *tb_c716r = tb_mem(tb_c716_0n + tb_c716_1n + tb_c716_2n + 1), *tb_c716rw = tb_c716r; memcpy(tb_c716rw, tb_c716_0, tb_c716_0n); tb_c716rw += tb_c716_0n; memcpy(tb_c716rw, tb_c716_1, tb_c716_1n); tb_c716rw += tb_c716_1n; memcpy(tb_c716rw, tb_c716_2, tb_c716_2n); tb_c716rw += tb_c716_2n; *tb_c716rw = 0; (const char *)tb_c716r; }));
    f_emit__emit_expr(v_c, v_base);
    f_emit__put(v_c, ({ const char *tb_c717_0 = ", "; long long tb_c717_0n = sizeof(", ") - 1; char tb_c717_1d[24]; long long tb_c717_1n; const char *tb_c717_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 326))->q_line, tb_c717_1d, &tb_c717_1n); const char *tb_c717_2 = "))"; long long tb_c717_2n = sizeof("))") - 1; char *tb_c717r = tb_mem(tb_c717_0n + tb_c717_1n + tb_c717_2n + 1), *tb_c717rw = tb_c717r; memcpy(tb_c717rw, tb_c717_0, tb_c717_0n); tb_c717rw += tb_c717_0n; memcpy(tb_c717rw, tb_c717_1, tb_c717_1n); tb_c717rw += tb_c717_1n; memcpy(tb_c717rw, tb_c717_2, tb_c717_2n); tb_c717rw += tb_c717_2n; *tb_c717rw = 0; (const char *)tb_c717r; }));
}

static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 329))->q_types, ((tb_o_Node *)tb_nn(v_n, 329))->q_ty, sizeof(tb_o_Type *), 329)), 329))->q_elem;
    const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
    if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 331))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c718_0 = "tb_colsfrom_"; long long tb_c718_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c718_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 332))->q_ty); long long tb_c718_1n = (long long)strlen(tb_c718_1); const char *tb_c718_2 = "("; long long tb_c718_2n = sizeof("(") - 1; char tb_c718_3d[24]; long long tb_c718_3n; const char *tb_c718_3 = tb_digits((((tb_o_Node *)tb_nn(v_n, 332))->q_kids)->len, tb_c718_3d, &tb_c718_3n); const char *tb_c718_4 = ", ("; long long tb_c718_4n = sizeof(", (") - 1; const char *tb_c718_5 = v_elem; long long tb_c718_5n = (long long)strlen(tb_c718_5); const char *tb_c718_6 = "[]){"; long long tb_c718_6n = sizeof("[]){") - 1; char *tb_c718r = tb_mem(tb_c718_0n + tb_c718_1n + tb_c718_2n + tb_c718_3n + tb_c718_4n + tb_c718_5n + tb_c718_6n + 1), *tb_c718rw = tb_c718r; memcpy(tb_c718rw, tb_c718_0, tb_c718_0n); tb_c718rw += tb_c718_0n; memcpy(tb_c718rw, tb_c718_1, tb_c718_1n); tb_c718rw += tb_c718_1n; memcpy(tb_c718rw, tb_c718_2, tb_c718_2n); tb_c718rw += tb_c718_2n; memcpy(tb_c718rw, tb_c718_3, tb_c718_3n); tb_c718rw += tb_c718_3n; memcpy(tb_c718rw, tb_c718_4, tb_c718_4n); tb_c718rw += tb_c718_4n; memcpy(tb_c718rw, tb_c718_5, tb_c718_5n); tb_c718rw += tb_c718_5n; memcpy(tb_c718rw, tb_c718_6, tb_c718_6n); tb_c718rw += tb_c718_6n; *tb_c718rw = 0; (const char *)tb_c718r; }));
    } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 333))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c719_0 = "tb_bits_from("; long long tb_c719_0n = sizeof("tb_bits_from(") - 1; char tb_c719_1d[24]; long long tb_c719_1n; const char *tb_c719_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 334))->q_kids)->len, tb_c719_1d, &tb_c719_1n); const char *tb_c719_2 = ", (unsigned char[]){"; long long tb_c719_2n = sizeof(", (unsigned char[]){") - 1; char *tb_c719r = tb_mem(tb_c719_0n + tb_c719_1n + tb_c719_2n + 1), *tb_c719rw = tb_c719r; memcpy(tb_c719rw, tb_c719_0, tb_c719_0n); tb_c719rw += tb_c719_0n; memcpy(tb_c719rw, tb_c719_1, tb_c719_1n); tb_c719rw += tb_c719_1n; memcpy(tb_c719rw, tb_c719_2, tb_c719_2n); tb_c719rw += tb_c719_2n; *tb_c719rw = 0; (const char *)tb_c719r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c720_0 = "tb_list_from("; long long tb_c720_0n = sizeof("tb_list_from(") - 1; char tb_c720_1d[24]; long long tb_c720_1n; const char *tb_c720_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 336))->q_kids)->len, tb_c720_1d, &tb_c720_1n); const char *tb_c720_2 = ", sizeof("; long long tb_c720_2n = sizeof(", sizeof(") - 1; const char *tb_c720_3 = v_elem; long long tb_c720_3n = (long long)strlen(tb_c720_3); const char *tb_c720_4 = "), ("; long long tb_c720_4n = sizeof("), (") - 1; const char *tb_c720_5 = v_elem; long long tb_c720_5n = (long long)strlen(tb_c720_5); const char *tb_c720_6 = "[]){"; long long tb_c720_6n = sizeof("[]){") - 1; char *tb_c720r = tb_mem(tb_c720_0n + tb_c720_1n + tb_c720_2n + tb_c720_3n + tb_c720_4n + tb_c720_5n + tb_c720_6n + 1), *tb_c720rw = tb_c720r; memcpy(tb_c720rw, tb_c720_0, tb_c720_0n); tb_c720rw += tb_c720_0n; memcpy(tb_c720rw, tb_c720_1, tb_c720_1n); tb_c720rw += tb_c720_1n; memcpy(tb_c720rw, tb_c720_2, tb_c720_2n); tb_c720rw += tb_c720_2n; memcpy(tb_c720rw, tb_c720_3, tb_c720_3n); tb_c720rw += tb_c720_3n; memcpy(tb_c720rw, tb_c720_4, tb_c720_4n); tb_c720rw += tb_c720_4n; memcpy(tb_c720rw, tb_c720_5, tb_c720_5n); tb_c720rw += tb_c720_5n; memcpy(tb_c720rw, tb_c720_6, tb_c720_6n); tb_c720rw += tb_c720_6n; *tb_c720rw = 0; (const char *)tb_c720r; }));
    }
    {
        long long tb_end721 = ((((tb_o_Node *)tb_nn(v_n, 337))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end721; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 340))->q_kids, v_i, sizeof(tb_o_Node *), 340)), v_elem_ty);
        }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__concat_parts(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_parts) {
    if (((({ const char *tb_s722 = ((tb_o_Node *)tb_nn(v_n, 345))->q_kind; ((unsigned char)tb_s722[0] == 98 && (unsigned char)tb_s722[1] == 105 && (unsigned char)tb_s722[2] == 110 && (unsigned char)tb_s722[3] == 111 && (unsigned char)tb_s722[4] == 112 && tb_s722[5] == 0); }) && ({ const char *tb_s723 = ((tb_o_Node *)tb_nn(v_n, 345))->q_value; ((unsigned char)tb_s723[0] == 43 && tb_s723[1] == 0); })) && ({ const char *tb_s724 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 345))->q_ty); ((unsigned char)tb_s724[0] == 116 && (unsigned char)tb_s724[1] == 101 && (unsigned char)tb_s724[2] == 120 && (unsigned char)tb_s724[3] == 116 && tb_s724[4] == 0); }))) {
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 346))->q_kids, 0LL, sizeof(tb_o_Node *), 346)), v_parts);
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 347))->q_kids, 1LL, sizeof(tb_o_Node *), 347)), v_parts);
    } else {
        { tb_o_Node * tb_v725 = v_n; *(tb_o_Node * *)tb_grow(v_parts) = tb_v725; }
    }
}

static void f_emit__emit_concat(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long tb_rl_total = -1, tb_rc_total = 0; (void)tb_rl_total; (void)tb_rc_total;
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    f_emit__concat_parts(v_c, v_n, v_parts);
    { long long tb_v726 = 1LL; long long *tb_p726 = &(((tb_o_Comp *)tb_nn(v_c, 357))->q_label); *tb_p726 += tb_v726; }
    const char * v_id = ({ const char *tb_c727_0 = ""; long long tb_c727_0n = sizeof("") - 1; char tb_c727_1d[24]; long long tb_c727_1n; const char *tb_c727_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 358))->q_label, tb_c727_1d, &tb_c727_1n); char *tb_c727r = tb_mem(tb_c727_0n + tb_c727_1n + 1), *tb_c727rw = tb_c727r; memcpy(tb_c727rw, tb_c727_0, tb_c727_0n); tb_c727rw += tb_c727_0n; memcpy(tb_c727rw, tb_c727_1, tb_c727_1n); tb_c727rw += tb_c727_1n; *tb_c727rw = 0; (const char *)tb_c727r; });
    f_emit__put(v_c, "({ ");
    const char * v_total = ""; tb_rl_total = -1;
    {
        long long tb_end728 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end728; v_i++) {
            tb_o_Node * v_part = (*(tb_o_Node * *)tb_at(v_parts, v_i, sizeof(tb_o_Node *), 362));
            const char * v_v = ({ const char *tb_c729_0 = "tb_c"; long long tb_c729_0n = sizeof("tb_c") - 1; const char *tb_c729_1 = v_id; long long tb_c729_1n = (long long)strlen(tb_c729_1); const char *tb_c729_2 = "_"; long long tb_c729_2n = sizeof("_") - 1; char tb_c729_3d[24]; long long tb_c729_3n; const char *tb_c729_3 = tb_digits(v_i, tb_c729_3d, &tb_c729_3n); char *tb_c729r = tb_mem(tb_c729_0n + tb_c729_1n + tb_c729_2n + tb_c729_3n + 1), *tb_c729rw = tb_c729r; memcpy(tb_c729rw, tb_c729_0, tb_c729_0n); tb_c729rw += tb_c729_0n; memcpy(tb_c729rw, tb_c729_1, tb_c729_1n); tb_c729rw += tb_c729_1n; memcpy(tb_c729rw, tb_c729_2, tb_c729_2n); tb_c729rw += tb_c729_2n; memcpy(tb_c729rw, tb_c729_3, tb_c729_3n); tb_c729rw += tb_c729_3n; *tb_c729rw = 0; (const char *)tb_c729r; });
            const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 364))->q_ty);
            if (({ const char *tb_s730 = v_k; ((unsigned char)tb_s730[0] == 105 && (unsigned char)tb_s730[1] == 110 && (unsigned char)tb_s730[2] == 116 && tb_s730[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c731_0 = "char "; long long tb_c731_0n = sizeof("char ") - 1; const char *tb_c731_1 = v_v; long long tb_c731_1n = (long long)strlen(tb_c731_1); const char *tb_c731_2 = "d[24]; long long "; long long tb_c731_2n = sizeof("d[24]; long long ") - 1; const char *tb_c731_3 = v_v; long long tb_c731_3n = (long long)strlen(tb_c731_3); const char *tb_c731_4 = "n; const char *"; long long tb_c731_4n = sizeof("n; const char *") - 1; const char *tb_c731_5 = v_v; long long tb_c731_5n = (long long)strlen(tb_c731_5); const char *tb_c731_6 = " = tb_digits("; long long tb_c731_6n = sizeof(" = tb_digits(") - 1; char *tb_c731r = tb_mem(tb_c731_0n + tb_c731_1n + tb_c731_2n + tb_c731_3n + tb_c731_4n + tb_c731_5n + tb_c731_6n + 1), *tb_c731rw = tb_c731r; memcpy(tb_c731rw, tb_c731_0, tb_c731_0n); tb_c731rw += tb_c731_0n; memcpy(tb_c731rw, tb_c731_1, tb_c731_1n); tb_c731rw += tb_c731_1n; memcpy(tb_c731rw, tb_c731_2, tb_c731_2n); tb_c731rw += tb_c731_2n; memcpy(tb_c731rw, tb_c731_3, tb_c731_3n); tb_c731rw += tb_c731_3n; memcpy(tb_c731rw, tb_c731_4, tb_c731_4n); tb_c731rw += tb_c731_4n; memcpy(tb_c731rw, tb_c731_5, tb_c731_5n); tb_c731rw += tb_c731_5n; memcpy(tb_c731rw, tb_c731_6, tb_c731_6n); tb_c731rw += tb_c731_6n; *tb_c731rw = 0; (const char *)tb_c731r; }));
                f_emit__emit_expr(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c732_0 = ", "; long long tb_c732_0n = sizeof(", ") - 1; const char *tb_c732_1 = v_v; long long tb_c732_1n = (long long)strlen(tb_c732_1); const char *tb_c732_2 = "d, &"; long long tb_c732_2n = sizeof("d, &") - 1; const char *tb_c732_3 = v_v; long long tb_c732_3n = (long long)strlen(tb_c732_3); const char *tb_c732_4 = "n); "; long long tb_c732_4n = sizeof("n); ") - 1; char *tb_c732r = tb_mem(tb_c732_0n + tb_c732_1n + tb_c732_2n + tb_c732_3n + tb_c732_4n + 1), *tb_c732rw = tb_c732r; memcpy(tb_c732rw, tb_c732_0, tb_c732_0n); tb_c732rw += tb_c732_0n; memcpy(tb_c732rw, tb_c732_1, tb_c732_1n); tb_c732rw += tb_c732_1n; memcpy(tb_c732rw, tb_c732_2, tb_c732_2n); tb_c732rw += tb_c732_2n; memcpy(tb_c732rw, tb_c732_3, tb_c732_3n); tb_c732rw += tb_c732_3n; memcpy(tb_c732rw, tb_c732_4, tb_c732_4n); tb_c732rw += tb_c732_4n; *tb_c732rw = 0; (const char *)tb_c732r; }));
            } else if (({ const char *tb_s733 = ((tb_o_Node *)tb_nn(v_part, 369))->q_kind; ((unsigned char)tb_s733[0] == 115 && (unsigned char)tb_s733[1] == 116 && (unsigned char)tb_s733[2] == 114 && (unsigned char)tb_s733[3] == 105 && (unsigned char)tb_s733[4] == 110 && (unsigned char)tb_s733[5] == 103 && tb_s733[6] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c734_0 = "const char *"; long long tb_c734_0n = sizeof("const char *") - 1; const char *tb_c734_1 = v_v; long long tb_c734_1n = (long long)strlen(tb_c734_1); const char *tb_c734_2 = " = "; long long tb_c734_2n = sizeof(" = ") - 1; const char *tb_c734_3 = ((tb_o_Node *)tb_nn(v_part, 370))->q_value; long long tb_c734_3n = (long long)strlen(tb_c734_3); const char *tb_c734_4 = "; long long "; long long tb_c734_4n = sizeof("; long long ") - 1; const char *tb_c734_5 = v_v; long long tb_c734_5n = (long long)strlen(tb_c734_5); const char *tb_c734_6 = "n = sizeof("; long long tb_c734_6n = sizeof("n = sizeof(") - 1; const char *tb_c734_7 = ((tb_o_Node *)tb_nn(v_part, 370))->q_value; long long tb_c734_7n = (long long)strlen(tb_c734_7); const char *tb_c734_8 = ") - 1; "; long long tb_c734_8n = sizeof(") - 1; ") - 1; char *tb_c734r = tb_mem(tb_c734_0n + tb_c734_1n + tb_c734_2n + tb_c734_3n + tb_c734_4n + tb_c734_5n + tb_c734_6n + tb_c734_7n + tb_c734_8n + 1), *tb_c734rw = tb_c734r; memcpy(tb_c734rw, tb_c734_0, tb_c734_0n); tb_c734rw += tb_c734_0n; memcpy(tb_c734rw, tb_c734_1, tb_c734_1n); tb_c734rw += tb_c734_1n; memcpy(tb_c734rw, tb_c734_2, tb_c734_2n); tb_c734rw += tb_c734_2n; memcpy(tb_c734rw, tb_c734_3, tb_c734_3n); tb_c734rw += tb_c734_3n; memcpy(tb_c734rw, tb_c734_4, tb_c734_4n); tb_c734rw += tb_c734_4n; memcpy(tb_c734rw, tb_c734_5, tb_c734_5n); tb_c734rw += tb_c734_5n; memcpy(tb_c734rw, tb_c734_6, tb_c734_6n); tb_c734rw += tb_c734_6n; memcpy(tb_c734rw, tb_c734_7, tb_c734_7n); tb_c734rw += tb_c734_7n; memcpy(tb_c734rw, tb_c734_8, tb_c734_8n); tb_c734rw += tb_c734_8n; *tb_c734rw = 0; (const char *)tb_c734r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c735_0 = "const char *"; long long tb_c735_0n = sizeof("const char *") - 1; const char *tb_c735_1 = v_v; long long tb_c735_1n = (long long)strlen(tb_c735_1); const char *tb_c735_2 = " = "; long long tb_c735_2n = sizeof(" = ") - 1; char *tb_c735r = tb_mem(tb_c735_0n + tb_c735_1n + tb_c735_2n + 1), *tb_c735rw = tb_c735r; memcpy(tb_c735rw, tb_c735_0, tb_c735_0n); tb_c735rw += tb_c735_0n; memcpy(tb_c735rw, tb_c735_1, tb_c735_1n); tb_c735rw += tb_c735_1n; memcpy(tb_c735rw, tb_c735_2, tb_c735_2n); tb_c735rw += tb_c735_2n; *tb_c735rw = 0; (const char *)tb_c735r; }));
                f_emit__emit_as_text(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c736_0 = "; long long "; long long tb_c736_0n = sizeof("; long long ") - 1; const char *tb_c736_1 = v_v; long long tb_c736_1n = (long long)strlen(tb_c736_1); const char *tb_c736_2 = "n = (long long)strlen("; long long tb_c736_2n = sizeof("n = (long long)strlen(") - 1; const char *tb_c736_3 = v_v; long long tb_c736_3n = (long long)strlen(tb_c736_3); const char *tb_c736_4 = "); "; long long tb_c736_4n = sizeof("); ") - 1; char *tb_c736r = tb_mem(tb_c736_0n + tb_c736_1n + tb_c736_2n + tb_c736_3n + tb_c736_4n + 1), *tb_c736rw = tb_c736r; memcpy(tb_c736rw, tb_c736_0, tb_c736_0n); tb_c736rw += tb_c736_0n; memcpy(tb_c736rw, tb_c736_1, tb_c736_1n); tb_c736rw += tb_c736_1n; memcpy(tb_c736rw, tb_c736_2, tb_c736_2n); tb_c736rw += tb_c736_2n; memcpy(tb_c736rw, tb_c736_3, tb_c736_3n); tb_c736rw += tb_c736_3n; memcpy(tb_c736rw, tb_c736_4, tb_c736_4n); tb_c736rw += tb_c736_4n; *tb_c736rw = 0; (const char *)tb_c736r; }));
            }
            if ((v_i > 0LL)) {
                tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, " + ", sizeof(" + ") - 1); 
            }
            tb_app(&v_total, &tb_rl_total, &tb_rc_total, v_v); tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, "n", sizeof("n") - 1); 
        }
    }
    const char * v_r = ({ const char *tb_c737_0 = "tb_c"; long long tb_c737_0n = sizeof("tb_c") - 1; const char *tb_c737_1 = v_id; long long tb_c737_1n = (long long)strlen(tb_c737_1); const char *tb_c737_2 = "r"; long long tb_c737_2n = sizeof("r") - 1; char *tb_c737r = tb_mem(tb_c737_0n + tb_c737_1n + tb_c737_2n + 1), *tb_c737rw = tb_c737r; memcpy(tb_c737rw, tb_c737_0, tb_c737_0n); tb_c737rw += tb_c737_0n; memcpy(tb_c737rw, tb_c737_1, tb_c737_1n); tb_c737rw += tb_c737_1n; memcpy(tb_c737rw, tb_c737_2, tb_c737_2n); tb_c737rw += tb_c737_2n; *tb_c737rw = 0; (const char *)tb_c737r; });
    f_emit__put(v_c, ({ const char *tb_c738_0 = "char *"; long long tb_c738_0n = sizeof("char *") - 1; const char *tb_c738_1 = v_r; long long tb_c738_1n = (long long)strlen(tb_c738_1); const char *tb_c738_2 = " = tb_mem("; long long tb_c738_2n = sizeof(" = tb_mem(") - 1; const char *tb_c738_3 = v_total; long long tb_c738_3n = (long long)strlen(tb_c738_3); const char *tb_c738_4 = " + 1), *"; long long tb_c738_4n = sizeof(" + 1), *") - 1; const char *tb_c738_5 = v_r; long long tb_c738_5n = (long long)strlen(tb_c738_5); const char *tb_c738_6 = "w = "; long long tb_c738_6n = sizeof("w = ") - 1; const char *tb_c738_7 = v_r; long long tb_c738_7n = (long long)strlen(tb_c738_7); const char *tb_c738_8 = "; "; long long tb_c738_8n = sizeof("; ") - 1; char *tb_c738r = tb_mem(tb_c738_0n + tb_c738_1n + tb_c738_2n + tb_c738_3n + tb_c738_4n + tb_c738_5n + tb_c738_6n + tb_c738_7n + tb_c738_8n + 1), *tb_c738rw = tb_c738r; memcpy(tb_c738rw, tb_c738_0, tb_c738_0n); tb_c738rw += tb_c738_0n; memcpy(tb_c738rw, tb_c738_1, tb_c738_1n); tb_c738rw += tb_c738_1n; memcpy(tb_c738rw, tb_c738_2, tb_c738_2n); tb_c738rw += tb_c738_2n; memcpy(tb_c738rw, tb_c738_3, tb_c738_3n); tb_c738rw += tb_c738_3n; memcpy(tb_c738rw, tb_c738_4, tb_c738_4n); tb_c738rw += tb_c738_4n; memcpy(tb_c738rw, tb_c738_5, tb_c738_5n); tb_c738rw += tb_c738_5n; memcpy(tb_c738rw, tb_c738_6, tb_c738_6n); tb_c738rw += tb_c738_6n; memcpy(tb_c738rw, tb_c738_7, tb_c738_7n); tb_c738rw += tb_c738_7n; memcpy(tb_c738rw, tb_c738_8, tb_c738_8n); tb_c738rw += tb_c738_8n; *tb_c738rw = 0; (const char *)tb_c738r; }));
    {
        long long tb_end739 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end739; v_i++) {
            const char * v_v = ({ const char *tb_c740_0 = "tb_c"; long long tb_c740_0n = sizeof("tb_c") - 1; const char *tb_c740_1 = v_id; long long tb_c740_1n = (long long)strlen(tb_c740_1); const char *tb_c740_2 = "_"; long long tb_c740_2n = sizeof("_") - 1; char tb_c740_3d[24]; long long tb_c740_3n; const char *tb_c740_3 = tb_digits(v_i, tb_c740_3d, &tb_c740_3n); char *tb_c740r = tb_mem(tb_c740_0n + tb_c740_1n + tb_c740_2n + tb_c740_3n + 1), *tb_c740rw = tb_c740r; memcpy(tb_c740rw, tb_c740_0, tb_c740_0n); tb_c740rw += tb_c740_0n; memcpy(tb_c740rw, tb_c740_1, tb_c740_1n); tb_c740rw += tb_c740_1n; memcpy(tb_c740rw, tb_c740_2, tb_c740_2n); tb_c740rw += tb_c740_2n; memcpy(tb_c740rw, tb_c740_3, tb_c740_3n); tb_c740rw += tb_c740_3n; *tb_c740rw = 0; (const char *)tb_c740r; });
            f_emit__put(v_c, ({ const char *tb_c741_0 = "memcpy("; long long tb_c741_0n = sizeof("memcpy(") - 1; const char *tb_c741_1 = v_r; long long tb_c741_1n = (long long)strlen(tb_c741_1); const char *tb_c741_2 = "w, "; long long tb_c741_2n = sizeof("w, ") - 1; const char *tb_c741_3 = v_v; long long tb_c741_3n = (long long)strlen(tb_c741_3); const char *tb_c741_4 = ", "; long long tb_c741_4n = sizeof(", ") - 1; const char *tb_c741_5 = v_v; long long tb_c741_5n = (long long)strlen(tb_c741_5); const char *tb_c741_6 = "n); "; long long tb_c741_6n = sizeof("n); ") - 1; const char *tb_c741_7 = v_r; long long tb_c741_7n = (long long)strlen(tb_c741_7); const char *tb_c741_8 = "w += "; long long tb_c741_8n = sizeof("w += ") - 1; const char *tb_c741_9 = v_v; long long tb_c741_9n = (long long)strlen(tb_c741_9); const char *tb_c741_10 = "n; "; long long tb_c741_10n = sizeof("n; ") - 1; char *tb_c741r = tb_mem(tb_c741_0n + tb_c741_1n + tb_c741_2n + tb_c741_3n + tb_c741_4n + tb_c741_5n + tb_c741_6n + tb_c741_7n + tb_c741_8n + tb_c741_9n + tb_c741_10n + 1), *tb_c741rw = tb_c741r; memcpy(tb_c741rw, tb_c741_0, tb_c741_0n); tb_c741rw += tb_c741_0n; memcpy(tb_c741rw, tb_c741_1, tb_c741_1n); tb_c741rw += tb_c741_1n; memcpy(tb_c741rw, tb_c741_2, tb_c741_2n); tb_c741rw += tb_c741_2n; memcpy(tb_c741rw, tb_c741_3, tb_c741_3n); tb_c741rw += tb_c741_3n; memcpy(tb_c741rw, tb_c741_4, tb_c741_4n); tb_c741rw += tb_c741_4n; memcpy(tb_c741rw, tb_c741_5, tb_c741_5n); tb_c741rw += tb_c741_5n; memcpy(tb_c741rw, tb_c741_6, tb_c741_6n); tb_c741rw += tb_c741_6n; memcpy(tb_c741rw, tb_c741_7, tb_c741_7n); tb_c741rw += tb_c741_7n; memcpy(tb_c741rw, tb_c741_8, tb_c741_8n); tb_c741rw += tb_c741_8n; memcpy(tb_c741rw, tb_c741_9, tb_c741_9n); tb_c741rw += tb_c741_9n; memcpy(tb_c741rw, tb_c741_10, tb_c741_10n); tb_c741rw += tb_c741_10n; *tb_c741rw = 0; (const char *)tb_c741r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c742_0 = "*"; long long tb_c742_0n = sizeof("*") - 1; const char *tb_c742_1 = v_r; long long tb_c742_1n = (long long)strlen(tb_c742_1); const char *tb_c742_2 = "w = 0; (const char *)"; long long tb_c742_2n = sizeof("w = 0; (const char *)") - 1; const char *tb_c742_3 = v_r; long long tb_c742_3n = (long long)strlen(tb_c742_3); const char *tb_c742_4 = "; })"; long long tb_c742_4n = sizeof("; })") - 1; char *tb_c742r = tb_mem(tb_c742_0n + tb_c742_1n + tb_c742_2n + tb_c742_3n + tb_c742_4n + 1), *tb_c742rw = tb_c742r; memcpy(tb_c742rw, tb_c742_0, tb_c742_0n); tb_c742rw += tb_c742_0n; memcpy(tb_c742rw, tb_c742_1, tb_c742_1n); tb_c742rw += tb_c742_1n; memcpy(tb_c742rw, tb_c742_2, tb_c742_2n); tb_c742rw += tb_c742_2n; memcpy(tb_c742rw, tb_c742_3, tb_c742_3n); tb_c742rw += tb_c742_3n; memcpy(tb_c742rw, tb_c742_4, tb_c742_4n); tb_c742rw += tb_c742_4n; *tb_c742rw = 0; (const char *)tb_c742r; }));
}

static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 386))->q_value;
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 387))->q_kids, 0LL, sizeof(tb_o_Node *), 387));
    tb_o_Node * v_b = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 388))->q_kids, 1LL, sizeof(tb_o_Node *), 388));
    if (({ const char *tb_s743 = v_op; ((unsigned char)tb_s743[0] == 111 && (unsigned char)tb_s743[1] == 114 && (unsigned char)tb_s743[2] == 101 && (unsigned char)tb_s743[3] == 108 && (unsigned char)tb_s743[4] == 115 && (unsigned char)tb_s743[5] == 101 && tb_s743[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c744_0 = "(tb_t"; long long tb_c744_0n = sizeof("(tb_t") - 1; char tb_c744_1d[24]; long long tb_c744_1n; const char *tb_c744_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 391))->q_temp_no, tb_c744_1d, &tb_c744_1n); const char *tb_c744_2 = " = "; long long tb_c744_2n = sizeof(" = ") - 1; char *tb_c744r = tb_mem(tb_c744_0n + tb_c744_1n + tb_c744_2n + 1), *tb_c744rw = tb_c744r; memcpy(tb_c744rw, tb_c744_0, tb_c744_0n); tb_c744rw += tb_c744_0n; memcpy(tb_c744rw, tb_c744_1, tb_c744_1n); tb_c744rw += tb_c744_1n; memcpy(tb_c744rw, tb_c744_2, tb_c744_2n); tb_c744rw += tb_c744_2n; *tb_c744rw = 0; (const char *)tb_c744r; }));
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", tb_err ? (tb_err = 0, ");
        f_emit__emit_as(v_c, v_b, ((tb_o_Node *)tb_nn(v_n, 394))->q_ty);
        f_emit__put(v_c, ({ const char *tb_c745_0 = ") : tb_t"; long long tb_c745_0n = sizeof(") : tb_t") - 1; char tb_c745_1d[24]; long long tb_c745_1n; const char *tb_c745_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 395))->q_temp_no, tb_c745_1d, &tb_c745_1n); const char *tb_c745_2 = ")"; long long tb_c745_2n = sizeof(")") - 1; char *tb_c745r = tb_mem(tb_c745_0n + tb_c745_1n + tb_c745_2n + 1), *tb_c745rw = tb_c745r; memcpy(tb_c745rw, tb_c745_0, tb_c745_0n); tb_c745rw += tb_c745_0n; memcpy(tb_c745rw, tb_c745_1, tb_c745_1n); tb_c745rw += tb_c745_1n; memcpy(tb_c745rw, tb_c745_2, tb_c745_2n); tb_c745rw += tb_c745_2n; *tb_c745rw = 0; (const char *)tb_c745r; }));
        return;
    }
    if (({ const char *tb_s746 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 397))->q_ty); ((unsigned char)tb_s746[0] == 116 && (unsigned char)tb_s746[1] == 101 && (unsigned char)tb_s746[2] == 120 && (unsigned char)tb_s746[3] == 116 && tb_s746[4] == 0); })) {
        f_emit__emit_concat(v_c, v_n);
        return;
    }
    if (((({ const char *tb_s747 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 400))->q_ty); ((unsigned char)tb_s747[0] == 116 && (unsigned char)tb_s747[1] == 101 && (unsigned char)tb_s747[2] == 120 && (unsigned char)tb_s747[3] == 116 && tb_s747[4] == 0); }) && (({ const char *tb_s748 = v_op; ((unsigned char)tb_s748[0] == 61 && (unsigned char)tb_s748[1] == 61 && tb_s748[2] == 0); }) || ({ const char *tb_s749 = v_op; ((unsigned char)tb_s749[0] == 33 && (unsigned char)tb_s749[1] == 61 && tb_s749[2] == 0); }))) && (f_emit__short_literal(v_b) || f_emit__short_literal(v_a)))) {
        tb_o_Node * v_lit = v_b;
        tb_o_Node * v_other = v_a;
        if ((!f_emit__short_literal(v_b))) {
            v_lit = v_a;
            v_other = v_b;
        }
        const char * v_body = tb_cut(((tb_o_Node *)tb_nn(v_lit, 407))->q_value, 1LL, (tb_tlen(((tb_o_Node *)tb_nn(v_lit, 407))->q_value) - 2LL), 407);
        { long long tb_v750 = 1LL; long long *tb_p750 = &(((tb_o_Comp *)tb_nn(v_c, 408))->q_label); *tb_p750 += tb_v750; }
        const char * v_sv = ({ const char *tb_c751_0 = "tb_s"; long long tb_c751_0n = sizeof("tb_s") - 1; char tb_c751_1d[24]; long long tb_c751_1n; const char *tb_c751_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 409))->q_label, tb_c751_1d, &tb_c751_1n); char *tb_c751r = tb_mem(tb_c751_0n + tb_c751_1n + 1), *tb_c751rw = tb_c751r; memcpy(tb_c751rw, tb_c751_0, tb_c751_0n); tb_c751rw += tb_c751_0n; memcpy(tb_c751rw, tb_c751_1, tb_c751_1n); tb_c751rw += tb_c751_1n; *tb_c751rw = 0; (const char *)tb_c751r; });
        f_emit__put(v_c, ({ const char *tb_c752_0 = "({ const char *"; long long tb_c752_0n = sizeof("({ const char *") - 1; const char *tb_c752_1 = v_sv; long long tb_c752_1n = (long long)strlen(tb_c752_1); const char *tb_c752_2 = " = "; long long tb_c752_2n = sizeof(" = ") - 1; char *tb_c752r = tb_mem(tb_c752_0n + tb_c752_1n + tb_c752_2n + 1), *tb_c752rw = tb_c752r; memcpy(tb_c752rw, tb_c752_0, tb_c752_0n); tb_c752rw += tb_c752_0n; memcpy(tb_c752rw, tb_c752_1, tb_c752_1n); tb_c752rw += tb_c752_1n; memcpy(tb_c752rw, tb_c752_2, tb_c752_2n); tb_c752rw += tb_c752_2n; *tb_c752rw = 0; (const char *)tb_c752r; }));
        f_emit__emit_q(v_c, v_other);
        f_emit__put(v_c, "; ");
        if (({ const char *tb_s753 = v_op; ((unsigned char)tb_s753[0] == 33 && (unsigned char)tb_s753[1] == 61 && tb_s753[2] == 0); })) {
            f_emit__put(v_c, "!");
        }
        f_emit__put(v_c, "(");
        long long v_i = 0LL;
        {
            const char * tb_l754 = v_body;
            for (long long tb_i754 = 0; tb_l754[tb_i754] != 0; tb_i754++) {
                const char * const v_ch = tb_chr((unsigned char)tb_l754[tb_i754]);
                f_emit__put(v_c, ({ const char *tb_c755_0 = "(unsigned char)"; long long tb_c755_0n = sizeof("(unsigned char)") - 1; const char *tb_c755_1 = v_sv; long long tb_c755_1n = (long long)strlen(tb_c755_1); const char *tb_c755_2 = "["; long long tb_c755_2n = sizeof("[") - 1; char tb_c755_3d[24]; long long tb_c755_3n; const char *tb_c755_3 = tb_digits(v_i, tb_c755_3d, &tb_c755_3n); const char *tb_c755_4 = "] == "; long long tb_c755_4n = sizeof("] == ") - 1; char tb_c755_5d[24]; long long tb_c755_5n; const char *tb_c755_5 = tb_digits(tb_code(v_ch, 418), tb_c755_5d, &tb_c755_5n); const char *tb_c755_6 = " && "; long long tb_c755_6n = sizeof(" && ") - 1; char *tb_c755r = tb_mem(tb_c755_0n + tb_c755_1n + tb_c755_2n + tb_c755_3n + tb_c755_4n + tb_c755_5n + tb_c755_6n + 1), *tb_c755rw = tb_c755r; memcpy(tb_c755rw, tb_c755_0, tb_c755_0n); tb_c755rw += tb_c755_0n; memcpy(tb_c755rw, tb_c755_1, tb_c755_1n); tb_c755rw += tb_c755_1n; memcpy(tb_c755rw, tb_c755_2, tb_c755_2n); tb_c755rw += tb_c755_2n; memcpy(tb_c755rw, tb_c755_3, tb_c755_3n); tb_c755rw += tb_c755_3n; memcpy(tb_c755rw, tb_c755_4, tb_c755_4n); tb_c755rw += tb_c755_4n; memcpy(tb_c755rw, tb_c755_5, tb_c755_5n); tb_c755rw += tb_c755_5n; memcpy(tb_c755rw, tb_c755_6, tb_c755_6n); tb_c755rw += tb_c755_6n; *tb_c755rw = 0; (const char *)tb_c755r; }));
                { long long tb_v756 = 1LL; long long *tb_p756 = &(v_i); *tb_p756 += tb_v756; }
            }
        }
        f_emit__put(v_c, ({ const char *tb_c757_0 = v_sv; long long tb_c757_0n = (long long)strlen(tb_c757_0); const char *tb_c757_1 = "["; long long tb_c757_1n = sizeof("[") - 1; char tb_c757_2d[24]; long long tb_c757_2n; const char *tb_c757_2 = tb_digits(v_i, tb_c757_2d, &tb_c757_2n); const char *tb_c757_3 = "] == 0); })"; long long tb_c757_3n = sizeof("] == 0); })") - 1; char *tb_c757r = tb_mem(tb_c757_0n + tb_c757_1n + tb_c757_2n + tb_c757_3n + 1), *tb_c757rw = tb_c757r; memcpy(tb_c757rw, tb_c757_0, tb_c757_0n); tb_c757rw += tb_c757_0n; memcpy(tb_c757rw, tb_c757_1, tb_c757_1n); tb_c757rw += tb_c757_1n; memcpy(tb_c757rw, tb_c757_2, tb_c757_2n); tb_c757rw += tb_c757_2n; memcpy(tb_c757rw, tb_c757_3, tb_c757_3n); tb_c757rw += tb_c757_3n; *tb_c757rw = 0; (const char *)tb_c757r; }));
        return;
    }
    if (({ const char *tb_s758 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 422))->q_ty); ((unsigned char)tb_s758[0] == 116 && (unsigned char)tb_s758[1] == 101 && (unsigned char)tb_s758[2] == 120 && (unsigned char)tb_s758[3] == 116 && tb_s758[4] == 0); })) {
        f_emit__put(v_c, "(strcmp(");
        f_emit__emit_q(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_q(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c759_0 = ") "; long long tb_c759_0n = sizeof(") ") - 1; const char *tb_c759_1 = v_op; long long tb_c759_1n = (long long)strlen(tb_c759_1); const char *tb_c759_2 = " 0)"; long long tb_c759_2n = sizeof(" 0)") - 1; char *tb_c759r = tb_mem(tb_c759_0n + tb_c759_1n + tb_c759_2n + 1), *tb_c759rw = tb_c759r; memcpy(tb_c759rw, tb_c759_0, tb_c759_0n); tb_c759rw += tb_c759_0n; memcpy(tb_c759rw, tb_c759_1, tb_c759_1n); tb_c759rw += tb_c759_1n; memcpy(tb_c759rw, tb_c759_2, tb_c759_2n); tb_c759rw += tb_c759_2n; *tb_c759rw = 0; (const char *)tb_c759r; }));
        return;
    }
    if ((({ const char *tb_s760 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 429))->q_ty); ((unsigned char)tb_s760[0] == 105 && (unsigned char)tb_s760[1] == 110 && (unsigned char)tb_s760[2] == 116 && tb_s760[3] == 0); }) && (({ const char *tb_s761 = v_op; ((unsigned char)tb_s761[0] == 47 && tb_s761[1] == 0); }) || ({ const char *tb_s762 = v_op; ((unsigned char)tb_s762[0] == 37 && tb_s762[1] == 0); })))) {
        if (({ const char *tb_s763 = v_op; ((unsigned char)tb_s763[0] == 47 && tb_s763[1] == 0); })) {
            f_emit__put(v_c, "tb_div(");
        } else {
            f_emit__put(v_c, "tb_mod(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c764_0 = ", "; long long tb_c764_0n = sizeof(", ") - 1; char tb_c764_1d[24]; long long tb_c764_1n; const char *tb_c764_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 437))->q_line, tb_c764_1d, &tb_c764_1n); const char *tb_c764_2 = ")"; long long tb_c764_2n = sizeof(")") - 1; char *tb_c764r = tb_mem(tb_c764_0n + tb_c764_1n + tb_c764_2n + 1), *tb_c764rw = tb_c764r; memcpy(tb_c764rw, tb_c764_0, tb_c764_0n); tb_c764rw += tb_c764_0n; memcpy(tb_c764rw, tb_c764_1, tb_c764_1n); tb_c764rw += tb_c764_1n; memcpy(tb_c764rw, tb_c764_2, tb_c764_2n); tb_c764rw += tb_c764_2n; *tb_c764rw = 0; (const char *)tb_c764r; }));
        return;
    }
    const char * v_c_op = v_op;
    if (({ const char *tb_s765 = v_op; ((unsigned char)tb_s765[0] == 97 && (unsigned char)tb_s765[1] == 110 && (unsigned char)tb_s765[2] == 100 && tb_s765[3] == 0); })) {
        v_c_op = "&&";
    } else if (({ const char *tb_s766 = v_op; ((unsigned char)tb_s766[0] == 111 && (unsigned char)tb_s766[1] == 114 && tb_s766[2] == 0); })) {
        v_c_op = "||";
    }
    f_emit__put(v_c, "(");
    if ((({ const char *tb_s767 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 445))->q_ty); ((unsigned char)tb_s767[0] == 100 && (unsigned char)tb_s767[1] == 101 && (unsigned char)tb_s767[2] == 99 && tb_s767[3] == 0); }) && ({ const char *tb_s768 = v_op; ((unsigned char)tb_s768[0] == 47 && tb_s768[1] == 0); }))) {
        f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 446))->q_ty);
    } else {
        f_emit__emit_expr(v_c, v_a);
    }
    f_emit__put(v_c, ({ const char *tb_c769_0 = " "; long long tb_c769_0n = sizeof(" ") - 1; const char *tb_c769_1 = v_c_op; long long tb_c769_1n = (long long)strlen(tb_c769_1); const char *tb_c769_2 = " "; long long tb_c769_2n = sizeof(" ") - 1; char *tb_c769r = tb_mem(tb_c769_0n + tb_c769_1n + tb_c769_2n + 1), *tb_c769rw = tb_c769r; memcpy(tb_c769rw, tb_c769_0, tb_c769_0n); tb_c769rw += tb_c769_0n; memcpy(tb_c769rw, tb_c769_1, tb_c769_1n); tb_c769rw += tb_c769_1n; memcpy(tb_c769rw, tb_c769_2, tb_c769_2n); tb_c769rw += tb_c769_2n; *tb_c769rw = 0; (const char *)tb_c769r; }));
    f_emit__emit_expr(v_c, v_b);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 454))->q_kind;
    if (({ const char *tb_s770 = v_k; ((unsigned char)tb_s770[0] == 110 && (unsigned char)tb_s770[1] == 117 && (unsigned char)tb_s770[2] == 109 && (unsigned char)tb_s770[3] == 98 && (unsigned char)tb_s770[4] == 101 && (unsigned char)tb_s770[5] == 114 && tb_s770[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c771_0 = ((tb_o_Node *)tb_nn(v_n, 456))->q_value; long long tb_c771_0n = (long long)strlen(tb_c771_0); const char *tb_c771_1 = "LL"; long long tb_c771_1n = sizeof("LL") - 1; char *tb_c771r = tb_mem(tb_c771_0n + tb_c771_1n + 1), *tb_c771rw = tb_c771r; memcpy(tb_c771rw, tb_c771_0, tb_c771_0n); tb_c771rw += tb_c771_0n; memcpy(tb_c771rw, tb_c771_1, tb_c771_1n); tb_c771rw += tb_c771_1n; *tb_c771rw = 0; (const char *)tb_c771r; }));
    } else if ((({ const char *tb_s772 = v_k; ((unsigned char)tb_s772[0] == 100 && (unsigned char)tb_s772[1] == 101 && (unsigned char)tb_s772[2] == 99 && (unsigned char)tb_s772[3] == 105 && (unsigned char)tb_s772[4] == 109 && (unsigned char)tb_s772[5] == 97 && (unsigned char)tb_s772[6] == 108 && tb_s772[7] == 0); }) || ({ const char *tb_s773 = v_k; ((unsigned char)tb_s773[0] == 115 && (unsigned char)tb_s773[1] == 116 && (unsigned char)tb_s773[2] == 114 && (unsigned char)tb_s773[3] == 105 && (unsigned char)tb_s773[4] == 110 && (unsigned char)tb_s773[5] == 103 && tb_s773[6] == 0); }))) {
        f_emit__put(v_c, ((tb_o_Node *)tb_nn(v_n, 458))->q_value);
    } else if (({ const char *tb_s774 = v_k; ((unsigned char)tb_s774[0] == 98 && (unsigned char)tb_s774[1] == 111 && (unsigned char)tb_s774[2] == 111 && (unsigned char)tb_s774[3] == 108 && tb_s774[4] == 0); })) {
        if (({ const char *tb_s775 = ((tb_o_Node *)tb_nn(v_n, 460))->q_value; ((unsigned char)tb_s775[0] == 116 && (unsigned char)tb_s775[1] == 114 && (unsigned char)tb_s775[2] == 117 && (unsigned char)tb_s775[3] == 101 && tb_s775[4] == 0); })) {
            f_emit__put(v_c, "1");
        } else {
            f_emit__put(v_c, "0");
        }
    } else if (((({ const char *tb_s776 = v_k; ((unsigned char)tb_s776[0] == 110 && (unsigned char)tb_s776[1] == 97 && (unsigned char)tb_s776[2] == 109 && (unsigned char)tb_s776[3] == 101 && tb_s776[4] == 0); }) && ((((tb_o_Comp *)tb_nn(v_c, 464))->q_vec_names)->len > 0LL)) && (f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 464))->q_name) >= 0LL))) {
        f_emit__put(v_c, (*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 465))->q_vec_repl, f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 465))->q_name), sizeof(const char *), 465)));
    } else if (({ const char *tb_s777 = v_k; ((unsigned char)tb_s777[0] == 110 && (unsigned char)tb_s777[1] == 97 && (unsigned char)tb_s777[2] == 109 && (unsigned char)tb_s777[3] == 101 && tb_s777[4] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 467))->q_is_link) {
            f_emit__put(v_c, ({ const char *tb_c778_0 = "(*v_"; long long tb_c778_0n = sizeof("(*v_") - 1; const char *tb_c778_1 = ((tb_o_Node *)tb_nn(v_n, 468))->q_name; long long tb_c778_1n = (long long)strlen(tb_c778_1); const char *tb_c778_2 = ")"; long long tb_c778_2n = sizeof(")") - 1; char *tb_c778r = tb_mem(tb_c778_0n + tb_c778_1n + tb_c778_2n + 1), *tb_c778rw = tb_c778r; memcpy(tb_c778rw, tb_c778_0, tb_c778_0n); tb_c778rw += tb_c778_0n; memcpy(tb_c778rw, tb_c778_1, tb_c778_1n); tb_c778rw += tb_c778_1n; memcpy(tb_c778rw, tb_c778_2, tb_c778_2n); tb_c778rw += tb_c778_2n; *tb_c778rw = 0; (const char *)tb_c778r; }));
        } else if (f_emit__is_bld(v_c, v_n)) {
            f_emit__put(v_c, ({ const char *tb_c779_0 = "(tb_rl_"; long long tb_c779_0n = sizeof("(tb_rl_") - 1; const char *tb_c779_1 = ((tb_o_Node *)tb_nn(v_n, 470))->q_name; long long tb_c779_1n = (long long)strlen(tb_c779_1); const char *tb_c779_2 = " = -1, v_"; long long tb_c779_2n = sizeof(" = -1, v_") - 1; const char *tb_c779_3 = ((tb_o_Node *)tb_nn(v_n, 470))->q_name; long long tb_c779_3n = (long long)strlen(tb_c779_3); const char *tb_c779_4 = ")"; long long tb_c779_4n = sizeof(")") - 1; char *tb_c779r = tb_mem(tb_c779_0n + tb_c779_1n + tb_c779_2n + tb_c779_3n + tb_c779_4n + 1), *tb_c779rw = tb_c779r; memcpy(tb_c779rw, tb_c779_0, tb_c779_0n); tb_c779rw += tb_c779_0n; memcpy(tb_c779rw, tb_c779_1, tb_c779_1n); tb_c779rw += tb_c779_1n; memcpy(tb_c779rw, tb_c779_2, tb_c779_2n); tb_c779rw += tb_c779_2n; memcpy(tb_c779rw, tb_c779_3, tb_c779_3n); tb_c779rw += tb_c779_3n; memcpy(tb_c779rw, tb_c779_4, tb_c779_4n); tb_c779rw += tb_c779_4n; *tb_c779rw = 0; (const char *)tb_c779r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c780_0 = "v_"; long long tb_c780_0n = sizeof("v_") - 1; const char *tb_c780_1 = ((tb_o_Node *)tb_nn(v_n, 472))->q_name; long long tb_c780_1n = (long long)strlen(tb_c780_1); char *tb_c780r = tb_mem(tb_c780_0n + tb_c780_1n + 1), *tb_c780rw = tb_c780r; memcpy(tb_c780rw, tb_c780_0, tb_c780_0n); tb_c780rw += tb_c780_0n; memcpy(tb_c780rw, tb_c780_1, tb_c780_1n); tb_c780rw += tb_c780_1n; *tb_c780rw = 0; (const char *)tb_c780r; }));
        }
    } else if (({ const char *tb_s781 = v_k; ((unsigned char)tb_s781[0] == 108 && (unsigned char)tb_s781[1] == 105 && (unsigned char)tb_s781[2] == 115 && (unsigned char)tb_s781[3] == 116 && tb_s781[4] == 0); })) {
        f_emit__emit_list(v_c, v_n);
    } else if (({ const char *tb_s782 = v_k; ((unsigned char)tb_s782[0] == 100 && (unsigned char)tb_s782[1] == 105 && (unsigned char)tb_s782[2] == 99 && (unsigned char)tb_s782[3] == 116 && tb_s782[4] == 0); })) {
        f_emit__emit_dict(v_c, v_n);
    } else if ((({ const char *tb_s783 = v_k; ((unsigned char)tb_s783[0] == 116 && (unsigned char)tb_s783[1] == 114 && (unsigned char)tb_s783[2] == 121 && tb_s783[3] == 0); }) || ({ const char *tb_s784 = v_k; ((unsigned char)tb_s784[0] == 101 && (unsigned char)tb_s784[1] == 108 && (unsigned char)tb_s784[2] == 115 && (unsigned char)tb_s784[3] == 101 && (unsigned char)tb_s784[4] == 95 && (unsigned char)tb_s784[5] == 101 && (unsigned char)tb_s784[6] == 114 && (unsigned char)tb_s784[7] == 114 && tb_s784[8] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c785_0 = "tb_v"; long long tb_c785_0n = sizeof("tb_v") - 1; char tb_c785_1d[24]; long long tb_c785_1n; const char *tb_c785_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 479))->q_temp_no, tb_c785_1d, &tb_c785_1n); char *tb_c785r = tb_mem(tb_c785_0n + tb_c785_1n + 1), *tb_c785rw = tb_c785r; memcpy(tb_c785rw, tb_c785_0, tb_c785_0n); tb_c785rw += tb_c785_0n; memcpy(tb_c785rw, tb_c785_1, tb_c785_1n); tb_c785rw += tb_c785_1n; *tb_c785rw = 0; (const char *)tb_c785r; }));
    } else if (({ const char *tb_s786 = v_k; ((unsigned char)tb_s786[0] == 99 && (unsigned char)tb_s786[1] == 97 && (unsigned char)tb_s786[2] == 108 && (unsigned char)tb_s786[3] == 108 && tb_s786[4] == 0); })) {
        f_emit__emit_call(v_c, v_n);
    } else if (({ const char *tb_s787 = v_k; ((unsigned char)tb_s787[0] == 109 && (unsigned char)tb_s787[1] == 101 && (unsigned char)tb_s787[2] == 116 && (unsigned char)tb_s787[3] == 104 && (unsigned char)tb_s787[4] == 111 && (unsigned char)tb_s787[5] == 100 && tb_s787[6] == 0); })) {
        f_emit__emit_method(v_c, v_n);
    } else if (({ const char *tb_s788 = v_k; ((unsigned char)tb_s788[0] == 98 && (unsigned char)tb_s788[1] == 105 && (unsigned char)tb_s788[2] == 110 && (unsigned char)tb_s788[3] == 111 && (unsigned char)tb_s788[4] == 112 && tb_s788[5] == 0); })) {
        f_emit__emit_binop(v_c, v_n);
    } else if (({ const char *tb_s789 = v_k; ((unsigned char)tb_s789[0] == 117 && (unsigned char)tb_s789[1] == 110 && (unsigned char)tb_s789[2] == 97 && (unsigned char)tb_s789[3] == 114 && (unsigned char)tb_s789[4] == 121 && tb_s789[5] == 0); })) {
        if (({ const char *tb_s790 = ((tb_o_Node *)tb_nn(v_n, 487))->q_value; ((unsigned char)tb_s790[0] == 110 && (unsigned char)tb_s790[1] == 111 && (unsigned char)tb_s790[2] == 116 && tb_s790[3] == 0); })) {
            f_emit__put(v_c, "(!");
        } else {
            f_emit__put(v_c, "(-");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 491))->q_kids, 0LL, sizeof(tb_o_Node *), 491)));
        f_emit__put(v_c, ")");
    } else if ((({ const char *tb_s791 = v_k; ((unsigned char)tb_s791[0] == 105 && (unsigned char)tb_s791[1] == 110 && (unsigned char)tb_s791[2] == 100 && (unsigned char)tb_s791[3] == 101 && (unsigned char)tb_s791[4] == 120 && tb_s791[5] == 0); }) && ({ const char *tb_s792 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 493))->q_kids, 0LL, sizeof(tb_o_Node *), 493)), 493))->q_ty); ((unsigned char)tb_s792[0] == 100 && (unsigned char)tb_s792[1] == 105 && (unsigned char)tb_s792[2] == 99 && (unsigned char)tb_s792[3] == 116 && tb_s792[4] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c793_0 = "(*("; long long tb_c793_0n = sizeof("(*(") - 1; const char *tb_c793_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 494))->q_ty); long long tb_c793_1n = (long long)strlen(tb_c793_1); const char *tb_c793_2 = " *)tb_dget("; long long tb_c793_2n = sizeof(" *)tb_dget(") - 1; char *tb_c793r = tb_mem(tb_c793_0n + tb_c793_1n + tb_c793_2n + 1), *tb_c793rw = tb_c793r; memcpy(tb_c793rw, tb_c793_0, tb_c793_0n); tb_c793rw += tb_c793_0n; memcpy(tb_c793rw, tb_c793_1, tb_c793_1n); tb_c793rw += tb_c793_1n; memcpy(tb_c793rw, tb_c793_2, tb_c793_2n); tb_c793rw += tb_c793_2n; *tb_c793rw = 0; (const char *)tb_c793r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 495))->q_kids, 0LL, sizeof(tb_o_Node *), 495)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 497))->q_kids, 0LL, sizeof(tb_o_Node *), 497)), 497))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 497))->q_kids, 1LL, sizeof(tb_o_Node *), 497)));
        f_emit__put(v_c, ({ const char *tb_c794_0 = ", "; long long tb_c794_0n = sizeof(", ") - 1; char tb_c794_1d[24]; long long tb_c794_1n; const char *tb_c794_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 498))->q_line, tb_c794_1d, &tb_c794_1n); const char *tb_c794_2 = "))"; long long tb_c794_2n = sizeof("))") - 1; char *tb_c794r = tb_mem(tb_c794_0n + tb_c794_1n + tb_c794_2n + 1), *tb_c794rw = tb_c794r; memcpy(tb_c794rw, tb_c794_0, tb_c794_0n); tb_c794rw += tb_c794_0n; memcpy(tb_c794rw, tb_c794_1, tb_c794_1n); tb_c794rw += tb_c794_1n; memcpy(tb_c794rw, tb_c794_2, tb_c794_2n); tb_c794rw += tb_c794_2n; *tb_c794rw = 0; (const char *)tb_c794r; }));
    } else if ((({ const char *tb_s795 = v_k; ((unsigned char)tb_s795[0] == 105 && (unsigned char)tb_s795[1] == 110 && (unsigned char)tb_s795[2] == 100 && (unsigned char)tb_s795[3] == 101 && (unsigned char)tb_s795[4] == 120 && tb_s795[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 499))->q_kids, 0LL, sizeof(tb_o_Node *), 499)), 499))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c796_0 = "tb_colat_"; long long tb_c796_0n = sizeof("tb_colat_") - 1; const char *tb_c796_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 500))->q_kids, 0LL, sizeof(tb_o_Node *), 500)), 500))->q_ty); long long tb_c796_1n = (long long)strlen(tb_c796_1); const char *tb_c796_2 = "("; long long tb_c796_2n = sizeof("(") - 1; char *tb_c796r = tb_mem(tb_c796_0n + tb_c796_1n + tb_c796_2n + 1), *tb_c796rw = tb_c796r; memcpy(tb_c796rw, tb_c796_0, tb_c796_0n); tb_c796rw += tb_c796_0n; memcpy(tb_c796rw, tb_c796_1, tb_c796_1n); tb_c796rw += tb_c796_1n; memcpy(tb_c796rw, tb_c796_2, tb_c796_2n); tb_c796rw += tb_c796_2n; *tb_c796rw = 0; (const char *)tb_c796r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 501))->q_kids, 0LL, sizeof(tb_o_Node *), 501)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 503))->q_kids, 1LL, sizeof(tb_o_Node *), 503)));
        f_emit__put(v_c, ({ const char *tb_c797_0 = ", "; long long tb_c797_0n = sizeof(", ") - 1; char tb_c797_1d[24]; long long tb_c797_1n; const char *tb_c797_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 504))->q_line, tb_c797_1d, &tb_c797_1n); const char *tb_c797_2 = ")"; long long tb_c797_2n = sizeof(")") - 1; char *tb_c797r = tb_mem(tb_c797_0n + tb_c797_1n + tb_c797_2n + 1), *tb_c797rw = tb_c797r; memcpy(tb_c797rw, tb_c797_0, tb_c797_0n); tb_c797rw += tb_c797_0n; memcpy(tb_c797rw, tb_c797_1, tb_c797_1n); tb_c797rw += tb_c797_1n; memcpy(tb_c797rw, tb_c797_2, tb_c797_2n); tb_c797rw += tb_c797_2n; *tb_c797rw = 0; (const char *)tb_c797r; }));
    } else if (({ const char *tb_s798 = v_k; ((unsigned char)tb_s798[0] == 105 && (unsigned char)tb_s798[1] == 110 && (unsigned char)tb_s798[2] == 100 && (unsigned char)tb_s798[3] == 101 && (unsigned char)tb_s798[4] == 120 && tb_s798[5] == 0); })) {
        if (({ const char *tb_s799 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 506))->q_kids, 0LL, sizeof(tb_o_Node *), 506)), 506))->q_ty); ((unsigned char)tb_s799[0] == 116 && (unsigned char)tb_s799[1] == 101 && (unsigned char)tb_s799[2] == 120 && (unsigned char)tb_s799[3] == 116 && tb_s799[4] == 0); })) {
            f_emit__put(v_c, "tb_char_at(");
            f_emit__emit_q(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 508))->q_kids, 0LL, sizeof(tb_o_Node *), 508)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 1LL, sizeof(tb_o_Node *), 510)));
            if (f_emit__is_bld(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 511))->q_kids, 0LL, sizeof(tb_o_Node *), 511)))) {
                f_emit__put(v_c, ({ const char *tb_c800_0 = ", tb_rl_"; long long tb_c800_0n = sizeof(", tb_rl_") - 1; const char *tb_c800_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 512))->q_kids, 0LL, sizeof(tb_o_Node *), 512)), 512))->q_name; long long tb_c800_1n = (long long)strlen(tb_c800_1); char *tb_c800r = tb_mem(tb_c800_0n + tb_c800_1n + 1), *tb_c800rw = tb_c800r; memcpy(tb_c800rw, tb_c800_0, tb_c800_0n); tb_c800rw += tb_c800_0n; memcpy(tb_c800rw, tb_c800_1, tb_c800_1n); tb_c800rw += tb_c800_1n; *tb_c800rw = 0; (const char *)tb_c800r; }));
            } else {
                f_emit__put(v_c, ", -1");
            }
            f_emit__put(v_c, ({ const char *tb_c801_0 = ", "; long long tb_c801_0n = sizeof(", ") - 1; char tb_c801_1d[24]; long long tb_c801_1n; const char *tb_c801_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 515))->q_line, tb_c801_1d, &tb_c801_1n); const char *tb_c801_2 = ")"; long long tb_c801_2n = sizeof(")") - 1; char *tb_c801r = tb_mem(tb_c801_0n + tb_c801_1n + tb_c801_2n + 1), *tb_c801rw = tb_c801r; memcpy(tb_c801rw, tb_c801_0, tb_c801_0n); tb_c801rw += tb_c801_0n; memcpy(tb_c801rw, tb_c801_1, tb_c801_1n); tb_c801rw += tb_c801_1n; memcpy(tb_c801rw, tb_c801_2, tb_c801_2n); tb_c801rw += tb_c801_2n; *tb_c801rw = 0; (const char *)tb_c801r; }));
        } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 516))->q_kids, 0LL, sizeof(tb_o_Node *), 516)), 516))->q_ty)) {
            f_emit__put(v_c, "tb_bget(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 518))->q_kids, 0LL, sizeof(tb_o_Node *), 518)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 520))->q_kids, 1LL, sizeof(tb_o_Node *), 520)));
            f_emit__put(v_c, ({ const char *tb_c802_0 = ", "; long long tb_c802_0n = sizeof(", ") - 1; char tb_c802_1d[24]; long long tb_c802_1n; const char *tb_c802_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 521))->q_line, tb_c802_1d, &tb_c802_1n); const char *tb_c802_2 = ")"; long long tb_c802_2n = sizeof(")") - 1; char *tb_c802r = tb_mem(tb_c802_0n + tb_c802_1n + tb_c802_2n + 1), *tb_c802rw = tb_c802r; memcpy(tb_c802rw, tb_c802_0, tb_c802_0n); tb_c802rw += tb_c802_0n; memcpy(tb_c802rw, tb_c802_1, tb_c802_1n); tb_c802rw += tb_c802_1n; memcpy(tb_c802rw, tb_c802_2, tb_c802_2n); tb_c802rw += tb_c802_2n; *tb_c802rw = 0; (const char *)tb_c802r; }));
        } else {
            const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 523))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c803_0 = "(*("; long long tb_c803_0n = sizeof("(*(") - 1; const char *tb_c803_1 = v_t; long long tb_c803_1n = (long long)strlen(tb_c803_1); const char *tb_c803_2 = " *)tb_at("; long long tb_c803_2n = sizeof(" *)tb_at(") - 1; char *tb_c803r = tb_mem(tb_c803_0n + tb_c803_1n + tb_c803_2n + 1), *tb_c803rw = tb_c803r; memcpy(tb_c803rw, tb_c803_0, tb_c803_0n); tb_c803rw += tb_c803_0n; memcpy(tb_c803rw, tb_c803_1, tb_c803_1n); tb_c803rw += tb_c803_1n; memcpy(tb_c803rw, tb_c803_2, tb_c803_2n); tb_c803rw += tb_c803_2n; *tb_c803rw = 0; (const char *)tb_c803r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 527))->q_kids, 1LL, sizeof(tb_o_Node *), 527)));
            f_emit__put(v_c, ({ const char *tb_c804_0 = ", sizeof("; long long tb_c804_0n = sizeof(", sizeof(") - 1; const char *tb_c804_1 = v_t; long long tb_c804_1n = (long long)strlen(tb_c804_1); const char *tb_c804_2 = "), "; long long tb_c804_2n = sizeof("), ") - 1; char tb_c804_3d[24]; long long tb_c804_3n; const char *tb_c804_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 528))->q_line, tb_c804_3d, &tb_c804_3n); const char *tb_c804_4 = "))"; long long tb_c804_4n = sizeof("))") - 1; char *tb_c804r = tb_mem(tb_c804_0n + tb_c804_1n + tb_c804_2n + tb_c804_3n + tb_c804_4n + 1), *tb_c804rw = tb_c804r; memcpy(tb_c804rw, tb_c804_0, tb_c804_0n); tb_c804rw += tb_c804_0n; memcpy(tb_c804rw, tb_c804_1, tb_c804_1n); tb_c804rw += tb_c804_1n; memcpy(tb_c804rw, tb_c804_2, tb_c804_2n); tb_c804rw += tb_c804_2n; memcpy(tb_c804rw, tb_c804_3, tb_c804_3n); tb_c804rw += tb_c804_3n; memcpy(tb_c804rw, tb_c804_4, tb_c804_4n); tb_c804rw += tb_c804_4n; *tb_c804rw = 0; (const char *)tb_c804r; }));
        }
    } else if ((({ const char *tb_s805 = v_k; ((unsigned char)tb_s805[0] == 102 && (unsigned char)tb_s805[1] == 105 && (unsigned char)tb_s805[2] == 101 && (unsigned char)tb_s805[3] == 108 && (unsigned char)tb_s805[4] == 100 && tb_s805[5] == 0); }) && ({ const char *tb_s806 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 529))->q_kids, 0LL, sizeof(tb_o_Node *), 529)), 529))->q_ty); ((unsigned char)tb_s806[0] == 116 && (unsigned char)tb_s806[1] == 97 && (unsigned char)tb_s806[2] == 115 && (unsigned char)tb_s806[3] == 107 && tb_s806[4] == 0); }))) {
        long long v_rt = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 530))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 530))->q_kids, 0LL, sizeof(tb_o_Node *), 530)), 530))->q_ty, sizeof(tb_o_Type *), 530)), 530))->q_elem;
        if ((v_rt == 0LL)) {
            f_emit__put(v_c, "((void)tb_wait(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 533))->q_kids, 0LL, sizeof(tb_o_Node *), 533)));
            f_emit__put(v_c, "))");
        } else {
            f_emit__put(v_c, ({ const char *tb_c807_0 = "(*("; long long tb_c807_0n = sizeof("(*(") - 1; const char *tb_c807_1 = f_emit__c_type(v_c, v_rt); long long tb_c807_1n = (long long)strlen(tb_c807_1); const char *tb_c807_2 = " *)tb_wait("; long long tb_c807_2n = sizeof(" *)tb_wait(") - 1; char *tb_c807r = tb_mem(tb_c807_0n + tb_c807_1n + tb_c807_2n + 1), *tb_c807rw = tb_c807r; memcpy(tb_c807rw, tb_c807_0, tb_c807_0n); tb_c807rw += tb_c807_0n; memcpy(tb_c807rw, tb_c807_1, tb_c807_1n); tb_c807rw += tb_c807_1n; memcpy(tb_c807rw, tb_c807_2, tb_c807_2n); tb_c807rw += tb_c807_2n; *tb_c807rw = 0; (const char *)tb_c807r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, 0LL, sizeof(tb_o_Node *), 537)));
            f_emit__put(v_c, "))");
        }
    } else if ((strcmp(v_k, "background") == 0)) {
        f_emit__emit_background(v_c, v_n);
    } else if ((({ const char *tb_s808 = v_k; ((unsigned char)tb_s808[0] == 102 && (unsigned char)tb_s808[1] == 105 && (unsigned char)tb_s808[2] == 101 && (unsigned char)tb_s808[3] == 108 && (unsigned char)tb_s808[4] == 100 && tb_s808[5] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 541))->q_kids, 0LL, sizeof(tb_o_Node *), 541)), 541))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c809_0 = "(*tb_colf_"; long long tb_c809_0n = sizeof("(*tb_colf_") - 1; const char *tb_c809_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, 0LL, sizeof(tb_o_Node *), 543)), 543))->q_ty); long long tb_c809_1n = (long long)strlen(tb_c809_1); const char *tb_c809_2 = "_"; long long tb_c809_2n = sizeof("_") - 1; const char *tb_c809_3 = ((tb_o_Node *)tb_nn(v_n, 543))->q_name; long long tb_c809_3n = (long long)strlen(tb_c809_3); const char *tb_c809_4 = "("; long long tb_c809_4n = sizeof("(") - 1; char *tb_c809r = tb_mem(tb_c809_0n + tb_c809_1n + tb_c809_2n + tb_c809_3n + tb_c809_4n + 1), *tb_c809rw = tb_c809r; memcpy(tb_c809rw, tb_c809_0, tb_c809_0n); tb_c809rw += tb_c809_0n; memcpy(tb_c809rw, tb_c809_1, tb_c809_1n); tb_c809rw += tb_c809_1n; memcpy(tb_c809rw, tb_c809_2, tb_c809_2n); tb_c809rw += tb_c809_2n; memcpy(tb_c809rw, tb_c809_3, tb_c809_3n); tb_c809rw += tb_c809_3n; memcpy(tb_c809rw, tb_c809_4, tb_c809_4n); tb_c809rw += tb_c809_4n; *tb_c809rw = 0; (const char *)tb_c809r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 544))->q_kids, 0LL, sizeof(tb_o_Node *), 544)));
        f_emit__put(v_c, ({ const char *tb_c810_0 = ", "; long long tb_c810_0n = sizeof(", ") - 1; char tb_c810_1d[24]; long long tb_c810_1n; const char *tb_c810_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 545))->q_line, tb_c810_1d, &tb_c810_1n); const char *tb_c810_2 = "))"; long long tb_c810_2n = sizeof("))") - 1; char *tb_c810r = tb_mem(tb_c810_0n + tb_c810_1n + tb_c810_2n + 1), *tb_c810rw = tb_c810r; memcpy(tb_c810rw, tb_c810_0, tb_c810_0n); tb_c810rw += tb_c810_0n; memcpy(tb_c810rw, tb_c810_1, tb_c810_1n); tb_c810rw += tb_c810_1n; memcpy(tb_c810rw, tb_c810_2, tb_c810_2n); tb_c810rw += tb_c810_2n; *tb_c810rw = 0; (const char *)tb_c810r; }));
    } else if (({ const char *tb_s811 = v_k; ((unsigned char)tb_s811[0] == 102 && (unsigned char)tb_s811[1] == 105 && (unsigned char)tb_s811[2] == 101 && (unsigned char)tb_s811[3] == 108 && (unsigned char)tb_s811[4] == 100 && tb_s811[5] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c812_0 = "(("; long long tb_c812_0n = sizeof("((") - 1; const char *tb_c812_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 547))->q_kids, 0LL, sizeof(tb_o_Node *), 547)), 547))->q_ty); long long tb_c812_1n = (long long)strlen(tb_c812_1); const char *tb_c812_2 = ")tb_nn("; long long tb_c812_2n = sizeof(")tb_nn(") - 1; char *tb_c812r = tb_mem(tb_c812_0n + tb_c812_1n + tb_c812_2n + 1), *tb_c812rw = tb_c812r; memcpy(tb_c812rw, tb_c812_0, tb_c812_0n); tb_c812rw += tb_c812_0n; memcpy(tb_c812rw, tb_c812_1, tb_c812_1n); tb_c812rw += tb_c812_1n; memcpy(tb_c812rw, tb_c812_2, tb_c812_2n); tb_c812rw += tb_c812_2n; *tb_c812rw = 0; (const char *)tb_c812r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 548))->q_kids, 0LL, sizeof(tb_o_Node *), 548)));
        f_emit__put(v_c, ({ const char *tb_c813_0 = ", "; long long tb_c813_0n = sizeof(", ") - 1; char tb_c813_1d[24]; long long tb_c813_1n; const char *tb_c813_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 549))->q_line, tb_c813_1d, &tb_c813_1n); const char *tb_c813_2 = "))"; long long tb_c813_2n = sizeof("))") - 1; const char *tb_c813_3 = f_emit__field_path(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 549))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 549))->q_kids, 0LL, sizeof(tb_o_Node *), 549)), 549))->q_ty, sizeof(tb_o_Type *), 549)), 549))->q_obj_node, ((tb_o_Node *)tb_nn(v_n, 549))->q_name); long long tb_c813_3n = (long long)strlen(tb_c813_3); char *tb_c813r = tb_mem(tb_c813_0n + tb_c813_1n + tb_c813_2n + tb_c813_3n + 1), *tb_c813rw = tb_c813r; memcpy(tb_c813rw, tb_c813_0, tb_c813_0n); tb_c813rw += tb_c813_0n; memcpy(tb_c813rw, tb_c813_1, tb_c813_1n); tb_c813rw += tb_c813_1n; memcpy(tb_c813rw, tb_c813_2, tb_c813_2n); tb_c813rw += tb_c813_2n; memcpy(tb_c813rw, tb_c813_3, tb_c813_3n); tb_c813rw += tb_c813_3n; *tb_c813rw = 0; (const char *)tb_c813r; }));
    }
}

static void f_emit__emit_bail(tb_o_Comp * v_c) {
    tb_o_Node * v_f = ((tb_o_Comp *)tb_nn(v_c, 555))->q_cur_func;
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 556))->q_kids, 2LL, sizeof(tb_o_Node *), 556));
    if (({ const char *tb_s814 = ((tb_o_Node *)tb_nn(v_f, 557))->q_name; ((unsigned char)tb_s814[0] == 109 && (unsigned char)tb_s814[1] == 97 && (unsigned char)tb_s814[2] == 105 && (unsigned char)tb_s814[3] == 110 && tb_s814[4] == 0); })) {
        f_emit__put(v_c, "tb_die(tb_err);");
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 559))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c815_0 = "return v_"; long long tb_c815_0n = sizeof("return v_") - 1; const char *tb_c815_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 560))->q_kids, 0LL, sizeof(tb_o_Node *), 560)), 560))->q_name; long long tb_c815_1n = (long long)strlen(tb_c815_1); const char *tb_c815_2 = ";"; long long tb_c815_2n = sizeof(";") - 1; char *tb_c815r = tb_mem(tb_c815_0n + tb_c815_1n + tb_c815_2n + 1), *tb_c815rw = tb_c815r; memcpy(tb_c815rw, tb_c815_0, tb_c815_0n); tb_c815rw += tb_c815_0n; memcpy(tb_c815rw, tb_c815_1, tb_c815_1n); tb_c815rw += tb_c815_1n; memcpy(tb_c815rw, tb_c815_2, tb_c815_2n); tb_c815rw += tb_c815_2n; *tb_c815rw = 0; (const char *)tb_c815r; }));
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 561))->q_kids)->len > 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c816_0 = "return ("; long long tb_c816_0n = sizeof("return (") - 1; const char *tb_c816_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 562))->q_ty); long long tb_c816_1n = (long long)strlen(tb_c816_1); const char *tb_c816_2 = "){"; long long tb_c816_2n = sizeof("){") - 1; char *tb_c816r = tb_mem(tb_c816_0n + tb_c816_1n + tb_c816_2n + 1), *tb_c816rw = tb_c816r; memcpy(tb_c816rw, tb_c816_0, tb_c816_0n); tb_c816rw += tb_c816_0n; memcpy(tb_c816rw, tb_c816_1, tb_c816_1n); tb_c816rw += tb_c816_1n; memcpy(tb_c816rw, tb_c816_2, tb_c816_2n); tb_c816rw += tb_c816_2n; *tb_c816rw = 0; (const char *)tb_c816r; }));
        {
            long long tb_end817 = ((((tb_o_Node *)tb_nn(v_outputs, 563))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end817; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                f_emit__put(v_c, ({ const char *tb_c818_0 = "v_"; long long tb_c818_0n = sizeof("v_") - 1; const char *tb_c818_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 566))->q_kids, v_i, sizeof(tb_o_Node *), 566)), 566))->q_name; long long tb_c818_1n = (long long)strlen(tb_c818_1); char *tb_c818r = tb_mem(tb_c818_0n + tb_c818_1n + 1), *tb_c818rw = tb_c818r; memcpy(tb_c818rw, tb_c818_0, tb_c818_0n); tb_c818rw += tb_c818_0n; memcpy(tb_c818rw, tb_c818_1, tb_c818_1n); tb_c818rw += tb_c818_1n; *tb_c818rw = 0; (const char *)tb_c818r; }));
            }
        }
        f_emit__put(v_c, "};");
    } else if (({ const char *tb_s819 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 568))->q_ty); ((unsigned char)tb_s819[0] == 110 && (unsigned char)tb_s819[1] == 111 && (unsigned char)tb_s819[2] == 110 && (unsigned char)tb_s819[3] == 101 && tb_s819[4] == 0); })) {
        f_emit__put(v_c, "return;");
    } else {
        f_emit__put(v_c, "return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 572))->q_ty);
        f_emit__put(v_c, ";");
    }
}

static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth) {
    if ((({ const char *tb_s820 = ((tb_o_Node *)tb_nn(v_v, 577))->q_kind; !((unsigned char)tb_s820[0] == 116 && (unsigned char)tb_s820[1] == 114 && (unsigned char)tb_s820[2] == 121 && tb_s820[3] == 0); }) && ({ const char *tb_s821 = ((tb_o_Node *)tb_nn(v_v, 577))->q_kind; !((unsigned char)tb_s821[0] == 101 && (unsigned char)tb_s821[1] == 108 && (unsigned char)tb_s821[2] == 115 && (unsigned char)tb_s821[3] == 101 && (unsigned char)tb_s821[4] == 95 && (unsigned char)tb_s821[5] == 101 && (unsigned char)tb_s821[6] == 114 && (unsigned char)tb_s821[7] == 114 && tb_s821[8] == 0); }))) {
        return;
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 579))->q_kids, 0LL, sizeof(tb_o_Node *), 579));
    { long long tb_v822 = 1LL; long long *tb_p822 = &(((tb_o_Comp *)tb_nn(v_c, 580))->q_label); *tb_p822 += tb_v822; }
    { long long tb_v823 = ((tb_o_Comp *)tb_nn(v_c, 581))->q_label; long long *tb_p823 = &(((tb_o_Node *)tb_nn(v_v, 581))->q_temp_no); *tb_p823 = tb_v823; }
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s824 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_call, 583))->q_ty); !((unsigned char)tb_s824[0] == 110 && (unsigned char)tb_s824[1] == 111 && (unsigned char)tb_s824[2] == 110 && (unsigned char)tb_s824[3] == 101 && tb_s824[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c825_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 584))->q_ty); long long tb_c825_0n = (long long)strlen(tb_c825_0); const char *tb_c825_1 = " tb_v"; long long tb_c825_1n = sizeof(" tb_v") - 1; char tb_c825_2d[24]; long long tb_c825_2n; const char *tb_c825_2 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 584))->q_label, tb_c825_2d, &tb_c825_2n); const char *tb_c825_3 = " = "; long long tb_c825_3n = sizeof(" = ") - 1; char *tb_c825r = tb_mem(tb_c825_0n + tb_c825_1n + tb_c825_2n + tb_c825_3n + 1), *tb_c825rw = tb_c825r; memcpy(tb_c825rw, tb_c825_0, tb_c825_0n); tb_c825rw += tb_c825_0n; memcpy(tb_c825rw, tb_c825_1, tb_c825_1n); tb_c825rw += tb_c825_1n; memcpy(tb_c825rw, tb_c825_2, tb_c825_2n); tb_c825rw += tb_c825_2n; memcpy(tb_c825rw, tb_c825_3, tb_c825_3n); tb_c825rw += tb_c825_3n; *tb_c825rw = 0; (const char *)tb_c825r; }));
    }
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s826 = ((tb_o_Node *)tb_nn(v_v, 588))->q_kind; ((unsigned char)tb_s826[0] == 116 && (unsigned char)tb_s826[1] == 114 && (unsigned char)tb_s826[2] == 121 && tb_s826[3] == 0); })) {
        f_emit__put(v_c, "if (TB_RARE(tb_err != 0)) { ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
        return;
    }
    f_emit__put(v_c, "if (tb_err != 0) {\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c827_0 = "const char * const v_"; long long tb_c827_0n = sizeof("const char * const v_") - 1; const char *tb_c827_1 = ((tb_o_Node *)tb_nn(v_v, 595))->q_name; long long tb_c827_1n = (long long)strlen(tb_c827_1); const char *tb_c827_2 = " = tb_err;\n"; long long tb_c827_2n = sizeof(" = tb_err;\n") - 1; char *tb_c827r = tb_mem(tb_c827_0n + tb_c827_1n + tb_c827_2n + 1), *tb_c827rw = tb_c827r; memcpy(tb_c827rw, tb_c827_0, tb_c827_0n); tb_c827rw += tb_c827_0n; memcpy(tb_c827rw, tb_c827_1, tb_c827_1n); tb_c827rw += tb_c827_1n; memcpy(tb_c827rw, tb_c827_2, tb_c827_2n); tb_c827rw += tb_c827_2n; *tb_c827rw = 0; (const char *)tb_c827r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "tb_err = 0;\n");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 598))->q_kids, 1LL, sizeof(tb_o_Node *), 598)), (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t) {
    if ((({ const char *tb_s828 = ((tb_o_Node *)tb_nn(v_t, 604))->q_kind; ((unsigned char)tb_s828[0] == 105 && (unsigned char)tb_s828[1] == 110 && (unsigned char)tb_s828[2] == 100 && (unsigned char)tb_s828[3] == 101 && (unsigned char)tb_s828[4] == 120 && tb_s828[5] == 0); }) && ({ const char *tb_s829 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 604))->q_kids, 0LL, sizeof(tb_o_Node *), 604)), 604))->q_ty); ((unsigned char)tb_s829[0] == 100 && (unsigned char)tb_s829[1] == 105 && (unsigned char)tb_s829[2] == 99 && (unsigned char)tb_s829[3] == 116 && tb_s829[4] == 0); }))) {
        long long v_d = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 605))->q_kids, 0LL, sizeof(tb_o_Node *), 605)), 605))->q_ty;
        long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 606))->q_types, v_d, sizeof(tb_o_Type *), 606)), 606))->q_elem;
        const char * v_vt = f_emit__c_type(v_c, v_elem);
        f_emit__put(v_c, ({ const char *tb_c830_0 = "(*("; long long tb_c830_0n = sizeof("(*(") - 1; const char *tb_c830_1 = v_vt; long long tb_c830_1n = (long long)strlen(tb_c830_1); const char *tb_c830_2 = " *)tb_dput("; long long tb_c830_2n = sizeof(" *)tb_dput(") - 1; char *tb_c830r = tb_mem(tb_c830_0n + tb_c830_1n + tb_c830_2n + 1), *tb_c830rw = tb_c830r; memcpy(tb_c830rw, tb_c830_0, tb_c830_0n); tb_c830rw += tb_c830_0n; memcpy(tb_c830rw, tb_c830_1, tb_c830_1n); tb_c830rw += tb_c830_1n; memcpy(tb_c830rw, tb_c830_2, tb_c830_2n); tb_c830rw += tb_c830_2n; *tb_c830rw = 0; (const char *)tb_c830r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 609))->q_kids, 0LL, sizeof(tb_o_Node *), 609)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, v_d, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 611))->q_kids, 1LL, sizeof(tb_o_Node *), 611)));
        if (f_emit__is_simple(v_c, v_elem)) {
            f_emit__put(v_c, ({ const char *tb_c831_0 = ", &("; long long tb_c831_0n = sizeof(", &(") - 1; const char *tb_c831_1 = v_vt; long long tb_c831_1n = (long long)strlen(tb_c831_1); const char *tb_c831_2 = "){"; long long tb_c831_2n = sizeof("){") - 1; char *tb_c831r = tb_mem(tb_c831_0n + tb_c831_1n + tb_c831_2n + 1), *tb_c831rw = tb_c831r; memcpy(tb_c831rw, tb_c831_0, tb_c831_0n); tb_c831rw += tb_c831_0n; memcpy(tb_c831rw, tb_c831_1, tb_c831_1n); tb_c831rw += tb_c831_1n; memcpy(tb_c831rw, tb_c831_2, tb_c831_2n); tb_c831rw += tb_c831_2n; *tb_c831rw = 0; (const char *)tb_c831r; }));
            f_emit__emit_default(v_c, v_elem);
            f_emit__put(v_c, "}))");
        } else {
            f_emit__put(v_c, ", 0))");
        }
        return;
    }
    f_emit__emit_expr(v_c, v_t);
}

static void f_emit__emit_assign(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((((tb_o_Node *)tb_nn(v_n, 622))->q_kids)->len == 1LL)) {
        f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 623))->q_kids, 0LL, sizeof(tb_o_Node *), 623)), v_depth);
    }
    f_emit__indent(v_c, v_depth);
    _Bool v_bld = (((!((tb_o_Node *)tb_nn(v_n, 625))->q_is_link) && ({ const char *tb_s832 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 625))->q_ty); ((unsigned char)tb_s832[0] == 116 && (unsigned char)tb_s832[1] == 101 && (unsigned char)tb_s832[2] == 120 && (unsigned char)tb_s832[3] == 116 && tb_s832[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 625))->q_builders, ((tb_o_Node *)tb_nn(v_n, 625))->q_name));
    if ((((v_bld && (!((tb_o_Node *)tb_nn(v_n, 626))->q_creates)) && ((((tb_o_Node *)tb_nn(v_n, 626))->q_kids)->len == 1LL)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 626))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 626))->q_kids, 0LL, sizeof(tb_o_Node *), 626))))) {
        {
            tb_list * tb_l833 = f_emit__app_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 628))->q_kids, 0LL, sizeof(tb_o_Node *), 628)));
            for (long long tb_i833 = 0; tb_i833 < tb_l833->len; tb_i833++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l833->data)[tb_i833];
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_n, 629))->q_name, v_part);
                f_emit__put(v_c, " ");
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 633))->q_creates) {
        f_emit__put(v_c, ({ const char *tb_c834_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 634))->q_ty); long long tb_c834_0n = (long long)strlen(tb_c834_0); const char *tb_c834_1 = " "; long long tb_c834_1n = sizeof(" ") - 1; char *tb_c834r = tb_mem(tb_c834_0n + tb_c834_1n + 1), *tb_c834rw = tb_c834r; memcpy(tb_c834rw, tb_c834_0, tb_c834_0n); tb_c834rw += tb_c834_0n; memcpy(tb_c834rw, tb_c834_1, tb_c834_1n); tb_c834rw += tb_c834_1n; *tb_c834rw = 0; (const char *)tb_c834r; }));
        if (((tb_o_Node *)tb_nn(v_n, 635))->q_is_const) {
            f_emit__put(v_c, "const ");
        }
        f_emit__put(v_c, ({ const char *tb_c835_0 = "v_"; long long tb_c835_0n = sizeof("v_") - 1; const char *tb_c835_1 = ((tb_o_Node *)tb_nn(v_n, 637))->q_name; long long tb_c835_1n = (long long)strlen(tb_c835_1); const char *tb_c835_2 = " = "; long long tb_c835_2n = sizeof(" = ") - 1; char *tb_c835r = tb_mem(tb_c835_0n + tb_c835_1n + tb_c835_2n + 1), *tb_c835rw = tb_c835r; memcpy(tb_c835rw, tb_c835_0, tb_c835_0n); tb_c835rw += tb_c835_0n; memcpy(tb_c835rw, tb_c835_1, tb_c835_1n); tb_c835rw += tb_c835_1n; memcpy(tb_c835rw, tb_c835_2, tb_c835_2n); tb_c835rw += tb_c835_2n; *tb_c835rw = 0; (const char *)tb_c835r; }));
    } else if (((tb_o_Node *)tb_nn(v_n, 638))->q_is_link) {
        f_emit__put(v_c, ({ const char *tb_c836_0 = "(*v_"; long long tb_c836_0n = sizeof("(*v_") - 1; const char *tb_c836_1 = ((tb_o_Node *)tb_nn(v_n, 639))->q_name; long long tb_c836_1n = (long long)strlen(tb_c836_1); const char *tb_c836_2 = ") = "; long long tb_c836_2n = sizeof(") = ") - 1; char *tb_c836r = tb_mem(tb_c836_0n + tb_c836_1n + tb_c836_2n + 1), *tb_c836rw = tb_c836r; memcpy(tb_c836rw, tb_c836_0, tb_c836_0n); tb_c836rw += tb_c836_0n; memcpy(tb_c836rw, tb_c836_1, tb_c836_1n); tb_c836rw += tb_c836_1n; memcpy(tb_c836rw, tb_c836_2, tb_c836_2n); tb_c836rw += tb_c836_2n; *tb_c836rw = 0; (const char *)tb_c836r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c837_0 = "v_"; long long tb_c837_0n = sizeof("v_") - 1; const char *tb_c837_1 = ((tb_o_Node *)tb_nn(v_n, 641))->q_name; long long tb_c837_1n = (long long)strlen(tb_c837_1); const char *tb_c837_2 = " = "; long long tb_c837_2n = sizeof(" = ") - 1; char *tb_c837r = tb_mem(tb_c837_0n + tb_c837_1n + tb_c837_2n + 1), *tb_c837rw = tb_c837r; memcpy(tb_c837rw, tb_c837_0, tb_c837_0n); tb_c837rw += tb_c837_0n; memcpy(tb_c837rw, tb_c837_1, tb_c837_1n); tb_c837rw += tb_c837_1n; memcpy(tb_c837rw, tb_c837_2, tb_c837_2n); tb_c837rw += tb_c837_2n; *tb_c837rw = 0; (const char *)tb_c837r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_n, 642))->q_kids)->len == 1LL)) {
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 643))->q_kids, 0LL, sizeof(tb_o_Node *), 643)), ((tb_o_Node *)tb_nn(v_n, 643))->q_ty);
    } else {
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_n, 645))->q_ty);
    }
    f_emit__put(v_c, ";");
    if (v_bld) {
        f_emit__put(v_c, ({ const char *tb_c838_0 = " tb_rl_"; long long tb_c838_0n = sizeof(" tb_rl_") - 1; const char *tb_c838_1 = ((tb_o_Node *)tb_nn(v_n, 649))->q_name; long long tb_c838_1n = (long long)strlen(tb_c838_1); const char *tb_c838_2 = " = -1;"; long long tb_c838_2n = sizeof(" = -1;") - 1; char *tb_c838r = tb_mem(tb_c838_0n + tb_c838_1n + tb_c838_2n + 1), *tb_c838rw = tb_c838r; memcpy(tb_c838rw, tb_c838_0, tb_c838_0n); tb_c838rw += tb_c838_0n; memcpy(tb_c838rw, tb_c838_1, tb_c838_1n); tb_c838rw += tb_c838_1n; memcpy(tb_c838rw, tb_c838_2, tb_c838_2n); tb_c838rw += tb_c838_2n; *tb_c838rw = 0; (const char *)tb_c838r; }));
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 654))->q_kids, 0LL, sizeof(tb_o_Node *), 654));
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 656))->q_value;
    const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_ty);
    f_emit__emit_pre(v_c, v_value, v_depth);
    { long long tb_v839 = 1LL; long long *tb_p839 = &(((tb_o_Comp *)tb_nn(v_c, 659))->q_label); *tb_p839 += tb_v839; }
    const char * v_id = ({ const char *tb_c840_0 = ""; long long tb_c840_0n = sizeof("") - 1; char tb_c840_1d[24]; long long tb_c840_1n; const char *tb_c840_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 660))->q_label, tb_c840_1d, &tb_c840_1n); char *tb_c840r = tb_mem(tb_c840_0n + tb_c840_1n + 1), *tb_c840rw = tb_c840r; memcpy(tb_c840rw, tb_c840_0, tb_c840_0n); tb_c840rw += tb_c840_0n; memcpy(tb_c840rw, tb_c840_1, tb_c840_1n); tb_c840rw += tb_c840_1n; *tb_c840rw = 0; (const char *)tb_c840r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{ ");
    if ((({ const char *tb_s841 = ((tb_o_Node *)tb_nn(v_target, 663))->q_kind; ((unsigned char)tb_s841[0] == 105 && (unsigned char)tb_s841[1] == 110 && (unsigned char)tb_s841[2] == 100 && (unsigned char)tb_s841[3] == 101 && (unsigned char)tb_s841[4] == 120 && tb_s841[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 663))->q_kids, 0LL, sizeof(tb_o_Node *), 663)), 663))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c842_0 = v_t; long long tb_c842_0n = (long long)strlen(tb_c842_0); const char *tb_c842_1 = " tb_v"; long long tb_c842_1n = sizeof(" tb_v") - 1; const char *tb_c842_2 = v_id; long long tb_c842_2n = (long long)strlen(tb_c842_2); const char *tb_c842_3 = " = "; long long tb_c842_3n = sizeof(" = ") - 1; char *tb_c842r = tb_mem(tb_c842_0n + tb_c842_1n + tb_c842_2n + tb_c842_3n + 1), *tb_c842rw = tb_c842r; memcpy(tb_c842rw, tb_c842_0, tb_c842_0n); tb_c842rw += tb_c842_0n; memcpy(tb_c842rw, tb_c842_1, tb_c842_1n); tb_c842rw += tb_c842_1n; memcpy(tb_c842rw, tb_c842_2, tb_c842_2n); tb_c842rw += tb_c842_2n; memcpy(tb_c842rw, tb_c842_3, tb_c842_3n); tb_c842rw += tb_c842_3n; *tb_c842rw = 0; (const char *)tb_c842r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c843_0 = "; tb_colset_"; long long tb_c843_0n = sizeof("; tb_colset_") - 1; const char *tb_c843_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 667))->q_kids, 0LL, sizeof(tb_o_Node *), 667)), 667))->q_ty); long long tb_c843_1n = (long long)strlen(tb_c843_1); const char *tb_c843_2 = "("; long long tb_c843_2n = sizeof("(") - 1; char *tb_c843r = tb_mem(tb_c843_0n + tb_c843_1n + tb_c843_2n + 1), *tb_c843rw = tb_c843r; memcpy(tb_c843rw, tb_c843_0, tb_c843_0n); tb_c843rw += tb_c843_0n; memcpy(tb_c843rw, tb_c843_1, tb_c843_1n); tb_c843rw += tb_c843_1n; memcpy(tb_c843rw, tb_c843_2, tb_c843_2n); tb_c843rw += tb_c843_2n; *tb_c843rw = 0; (const char *)tb_c843r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 668))->q_kids, 0LL, sizeof(tb_o_Node *), 668)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 670))->q_kids, 1LL, sizeof(tb_o_Node *), 670)));
        f_emit__put(v_c, ({ const char *tb_c844_0 = ", tb_v"; long long tb_c844_0n = sizeof(", tb_v") - 1; const char *tb_c844_1 = v_id; long long tb_c844_1n = (long long)strlen(tb_c844_1); const char *tb_c844_2 = ", "; long long tb_c844_2n = sizeof(", ") - 1; char tb_c844_3d[24]; long long tb_c844_3n; const char *tb_c844_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 671))->q_line, tb_c844_3d, &tb_c844_3n); const char *tb_c844_4 = "); }\n"; long long tb_c844_4n = sizeof("); }\n") - 1; char *tb_c844r = tb_mem(tb_c844_0n + tb_c844_1n + tb_c844_2n + tb_c844_3n + tb_c844_4n + 1), *tb_c844rw = tb_c844r; memcpy(tb_c844rw, tb_c844_0, tb_c844_0n); tb_c844rw += tb_c844_0n; memcpy(tb_c844rw, tb_c844_1, tb_c844_1n); tb_c844rw += tb_c844_1n; memcpy(tb_c844rw, tb_c844_2, tb_c844_2n); tb_c844rw += tb_c844_2n; memcpy(tb_c844rw, tb_c844_3, tb_c844_3n); tb_c844rw += tb_c844_3n; memcpy(tb_c844rw, tb_c844_4, tb_c844_4n); tb_c844rw += tb_c844_4n; *tb_c844rw = 0; (const char *)tb_c844r; }));
        return;
    }
    if (((({ const char *tb_s845 = ((tb_o_Node *)tb_nn(v_target, 673))->q_kind; ((unsigned char)tb_s845[0] == 105 && (unsigned char)tb_s845[1] == 110 && (unsigned char)tb_s845[2] == 100 && (unsigned char)tb_s845[3] == 101 && (unsigned char)tb_s845[4] == 120 && tb_s845[5] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 673))->q_kids, 0LL, sizeof(tb_o_Node *), 673)), 673))->q_ty)) && ({ const char *tb_s846 = v_op; ((unsigned char)tb_s846[0] == 61 && tb_s846[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c847_0 = "_Bool tb_v"; long long tb_c847_0n = sizeof("_Bool tb_v") - 1; const char *tb_c847_1 = v_id; long long tb_c847_1n = (long long)strlen(tb_c847_1); const char *tb_c847_2 = " = "; long long tb_c847_2n = sizeof(" = ") - 1; char *tb_c847r = tb_mem(tb_c847_0n + tb_c847_1n + tb_c847_2n + 1), *tb_c847rw = tb_c847r; memcpy(tb_c847rw, tb_c847_0, tb_c847_0n); tb_c847rw += tb_c847_0n; memcpy(tb_c847rw, tb_c847_1, tb_c847_1n); tb_c847rw += tb_c847_1n; memcpy(tb_c847rw, tb_c847_2, tb_c847_2n); tb_c847rw += tb_c847_2n; *tb_c847rw = 0; (const char *)tb_c847r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, "; tb_bset(");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 678))->q_kids, 0LL, sizeof(tb_o_Node *), 678)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 680))->q_kids, 1LL, sizeof(tb_o_Node *), 680)));
        f_emit__put(v_c, ({ const char *tb_c848_0 = ", tb_v"; long long tb_c848_0n = sizeof(", tb_v") - 1; const char *tb_c848_1 = v_id; long long tb_c848_1n = (long long)strlen(tb_c848_1); const char *tb_c848_2 = ", "; long long tb_c848_2n = sizeof(", ") - 1; char tb_c848_3d[24]; long long tb_c848_3n; const char *tb_c848_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 681))->q_line, tb_c848_3d, &tb_c848_3n); const char *tb_c848_4 = "); }\n"; long long tb_c848_4n = sizeof("); }\n") - 1; char *tb_c848r = tb_mem(tb_c848_0n + tb_c848_1n + tb_c848_2n + tb_c848_3n + tb_c848_4n + 1), *tb_c848rw = tb_c848r; memcpy(tb_c848rw, tb_c848_0, tb_c848_0n); tb_c848rw += tb_c848_0n; memcpy(tb_c848rw, tb_c848_1, tb_c848_1n); tb_c848rw += tb_c848_1n; memcpy(tb_c848rw, tb_c848_2, tb_c848_2n); tb_c848rw += tb_c848_2n; memcpy(tb_c848rw, tb_c848_3, tb_c848_3n); tb_c848rw += tb_c848_3n; memcpy(tb_c848rw, tb_c848_4, tb_c848_4n); tb_c848rw += tb_c848_4n; *tb_c848rw = 0; (const char *)tb_c848r; }));
        return;
    }
    if ((((({ const char *tb_s849 = v_op; ((unsigned char)tb_s849[0] == 43 && (unsigned char)tb_s849[1] == 61 && tb_s849[2] == 0); }) && f_emit__is_bld(v_c, v_target)) && f_emit__is_text_plus(v_c, v_value)) && (!f_emit__bld_uses(v_value, ((tb_o_Node *)tb_nn(v_target, 683))->q_name)))) {
        f_emit__put(v_c, "} ");
        tb_o_Node * v_r = v_value;
        while (f_emit__is_text_plus(v_c, v_r)) {
            v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 688))->q_kids, 0LL, sizeof(tb_o_Node *), 688));
        }
        f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 689))->q_name, v_r);
        {
            tb_list * tb_l850 = f_emit__app_parts(v_c, v_value);
            for (long long tb_i850 = 0; tb_i850 < tb_l850->len; tb_i850++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l850->data)[tb_i850];
                f_emit__put(v_c, " ");
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 692))->q_name, v_part);
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if ((({ const char *tb_s851 = v_op; ((unsigned char)tb_s851[0] == 43 && (unsigned char)tb_s851[1] == 61 && tb_s851[2] == 0); }) && f_emit__is_bld(v_c, v_target))) {
        f_emit__put(v_c, ({ const char *tb_c852_0 = "const char *tb_v"; long long tb_c852_0n = sizeof("const char *tb_v") - 1; const char *tb_c852_1 = v_id; long long tb_c852_1n = (long long)strlen(tb_c852_1); const char *tb_c852_2 = " = "; long long tb_c852_2n = sizeof(" = ") - 1; char *tb_c852r = tb_mem(tb_c852_0n + tb_c852_1n + tb_c852_2n + 1), *tb_c852rw = tb_c852r; memcpy(tb_c852rw, tb_c852_0, tb_c852_0n); tb_c852rw += tb_c852_0n; memcpy(tb_c852rw, tb_c852_1, tb_c852_1n); tb_c852rw += tb_c852_1n; memcpy(tb_c852rw, tb_c852_2, tb_c852_2n); tb_c852rw += tb_c852_2n; *tb_c852rw = 0; (const char *)tb_c852r; }));
        f_emit__emit_as_text(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c853_0 = "; tb_app(&v_"; long long tb_c853_0n = sizeof("; tb_app(&v_") - 1; const char *tb_c853_1 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c853_1n = (long long)strlen(tb_c853_1); const char *tb_c853_2 = ", &tb_rl_"; long long tb_c853_2n = sizeof(", &tb_rl_") - 1; const char *tb_c853_3 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c853_3n = (long long)strlen(tb_c853_3); const char *tb_c853_4 = ", &tb_rc_"; long long tb_c853_4n = sizeof(", &tb_rc_") - 1; const char *tb_c853_5 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c853_5n = (long long)strlen(tb_c853_5); const char *tb_c853_6 = ", tb_v"; long long tb_c853_6n = sizeof(", tb_v") - 1; const char *tb_c853_7 = v_id; long long tb_c853_7n = (long long)strlen(tb_c853_7); const char *tb_c853_8 = "); }\n"; long long tb_c853_8n = sizeof("); }\n") - 1; char *tb_c853r = tb_mem(tb_c853_0n + tb_c853_1n + tb_c853_2n + tb_c853_3n + tb_c853_4n + tb_c853_5n + tb_c853_6n + tb_c853_7n + tb_c853_8n + 1), *tb_c853rw = tb_c853r; memcpy(tb_c853rw, tb_c853_0, tb_c853_0n); tb_c853rw += tb_c853_0n; memcpy(tb_c853rw, tb_c853_1, tb_c853_1n); tb_c853rw += tb_c853_1n; memcpy(tb_c853rw, tb_c853_2, tb_c853_2n); tb_c853rw += tb_c853_2n; memcpy(tb_c853rw, tb_c853_3, tb_c853_3n); tb_c853rw += tb_c853_3n; memcpy(tb_c853rw, tb_c853_4, tb_c853_4n); tb_c853rw += tb_c853_4n; memcpy(tb_c853rw, tb_c853_5, tb_c853_5n); tb_c853rw += tb_c853_5n; memcpy(tb_c853rw, tb_c853_6, tb_c853_6n); tb_c853rw += tb_c853_6n; memcpy(tb_c853rw, tb_c853_7, tb_c853_7n); tb_c853rw += tb_c853_7n; memcpy(tb_c853rw, tb_c853_8, tb_c853_8n); tb_c853rw += tb_c853_8n; *tb_c853rw = 0; (const char *)tb_c853r; }));
        return;
    }
    if ((({ const char *tb_s854 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_ty); ((unsigned char)tb_s854[0] == 116 && (unsigned char)tb_s854[1] == 101 && (unsigned char)tb_s854[2] == 120 && (unsigned char)tb_s854[3] == 116 && tb_s854[4] == 0); }) && ({ const char *tb_s855 = v_op; !((unsigned char)tb_s855[0] == 61 && tb_s855[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c856_0 = "const char *tb_v"; long long tb_c856_0n = sizeof("const char *tb_v") - 1; const char *tb_c856_1 = v_id; long long tb_c856_1n = (long long)strlen(tb_c856_1); const char *tb_c856_2 = " = "; long long tb_c856_2n = sizeof(" = ") - 1; char *tb_c856r = tb_mem(tb_c856_0n + tb_c856_1n + tb_c856_2n + 1), *tb_c856rw = tb_c856r; memcpy(tb_c856rw, tb_c856_0, tb_c856_0n); tb_c856rw += tb_c856_0n; memcpy(tb_c856rw, tb_c856_1, tb_c856_1n); tb_c856rw += tb_c856_1n; memcpy(tb_c856rw, tb_c856_2, tb_c856_2n); tb_c856rw += tb_c856_2n; *tb_c856rw = 0; (const char *)tb_c856r; }));
        f_emit__emit_as_text(v_c, v_value);
    } else {
        if (({ const char *tb_s857 = v_op; ((unsigned char)tb_s857[0] == 61 && tb_s857[1] == 0); })) {
            f_emit__put(v_c, ({ const char *tb_c858_0 = v_t; long long tb_c858_0n = (long long)strlen(tb_c858_0); const char *tb_c858_1 = " tb_v"; long long tb_c858_1n = sizeof(" tb_v") - 1; const char *tb_c858_2 = v_id; long long tb_c858_2n = (long long)strlen(tb_c858_2); const char *tb_c858_3 = " = "; long long tb_c858_3n = sizeof(" = ") - 1; char *tb_c858r = tb_mem(tb_c858_0n + tb_c858_1n + tb_c858_2n + tb_c858_3n + 1), *tb_c858rw = tb_c858r; memcpy(tb_c858rw, tb_c858_0, tb_c858_0n); tb_c858rw += tb_c858_0n; memcpy(tb_c858rw, tb_c858_1, tb_c858_1n); tb_c858rw += tb_c858_1n; memcpy(tb_c858rw, tb_c858_2, tb_c858_2n); tb_c858rw += tb_c858_2n; memcpy(tb_c858rw, tb_c858_3, tb_c858_3n); tb_c858rw += tb_c858_3n; *tb_c858rw = 0; (const char *)tb_c858r; }));
            f_emit__emit_as(v_c, v_value, ((tb_o_Node *)tb_nn(v_n, 707))->q_ty);
        } else {
            f_emit__put(v_c, ({ const char *tb_c859_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_value, 709))->q_ty); long long tb_c859_0n = (long long)strlen(tb_c859_0); const char *tb_c859_1 = " tb_v"; long long tb_c859_1n = sizeof(" tb_v") - 1; const char *tb_c859_2 = v_id; long long tb_c859_2n = (long long)strlen(tb_c859_2); const char *tb_c859_3 = " = "; long long tb_c859_3n = sizeof(" = ") - 1; char *tb_c859r = tb_mem(tb_c859_0n + tb_c859_1n + tb_c859_2n + tb_c859_3n + 1), *tb_c859rw = tb_c859r; memcpy(tb_c859rw, tb_c859_0, tb_c859_0n); tb_c859rw += tb_c859_0n; memcpy(tb_c859rw, tb_c859_1, tb_c859_1n); tb_c859rw += tb_c859_1n; memcpy(tb_c859rw, tb_c859_2, tb_c859_2n); tb_c859rw += tb_c859_2n; memcpy(tb_c859rw, tb_c859_3, tb_c859_3n); tb_c859rw += tb_c859_3n; *tb_c859rw = 0; (const char *)tb_c859r; }));
            f_emit__emit_expr(v_c, v_value);
        }
    }
    f_emit__put(v_c, ({ const char *tb_c860_0 = "; "; long long tb_c860_0n = sizeof("; ") - 1; const char *tb_c860_1 = v_t; long long tb_c860_1n = (long long)strlen(tb_c860_1); const char *tb_c860_2 = " *tb_p"; long long tb_c860_2n = sizeof(" *tb_p") - 1; const char *tb_c860_3 = v_id; long long tb_c860_3n = (long long)strlen(tb_c860_3); const char *tb_c860_4 = " = &("; long long tb_c860_4n = sizeof(" = &(") - 1; char *tb_c860r = tb_mem(tb_c860_0n + tb_c860_1n + tb_c860_2n + tb_c860_3n + tb_c860_4n + 1), *tb_c860rw = tb_c860r; memcpy(tb_c860rw, tb_c860_0, tb_c860_0n); tb_c860rw += tb_c860_0n; memcpy(tb_c860rw, tb_c860_1, tb_c860_1n); tb_c860rw += tb_c860_1n; memcpy(tb_c860rw, tb_c860_2, tb_c860_2n); tb_c860rw += tb_c860_2n; memcpy(tb_c860rw, tb_c860_3, tb_c860_3n); tb_c860rw += tb_c860_3n; memcpy(tb_c860rw, tb_c860_4, tb_c860_4n); tb_c860rw += tb_c860_4n; *tb_c860rw = 0; (const char *)tb_c860r; }));
    f_emit__emit_target(v_c, v_target);
    f_emit__put(v_c, "); ");
    if (({ const char *tb_s861 = v_op; ((unsigned char)tb_s861[0] == 61 && tb_s861[1] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c862_0 = "*tb_p"; long long tb_c862_0n = sizeof("*tb_p") - 1; const char *tb_c862_1 = v_id; long long tb_c862_1n = (long long)strlen(tb_c862_1); const char *tb_c862_2 = " = tb_v"; long long tb_c862_2n = sizeof(" = tb_v") - 1; const char *tb_c862_3 = v_id; long long tb_c862_3n = (long long)strlen(tb_c862_3); const char *tb_c862_4 = ";"; long long tb_c862_4n = sizeof(";") - 1; char *tb_c862r = tb_mem(tb_c862_0n + tb_c862_1n + tb_c862_2n + tb_c862_3n + tb_c862_4n + 1), *tb_c862rw = tb_c862r; memcpy(tb_c862rw, tb_c862_0, tb_c862_0n); tb_c862rw += tb_c862_0n; memcpy(tb_c862rw, tb_c862_1, tb_c862_1n); tb_c862rw += tb_c862_1n; memcpy(tb_c862rw, tb_c862_2, tb_c862_2n); tb_c862rw += tb_c862_2n; memcpy(tb_c862rw, tb_c862_3, tb_c862_3n); tb_c862rw += tb_c862_3n; memcpy(tb_c862rw, tb_c862_4, tb_c862_4n); tb_c862rw += tb_c862_4n; *tb_c862rw = 0; (const char *)tb_c862r; }));
    } else if (({ const char *tb_s863 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 716))->q_ty); ((unsigned char)tb_s863[0] == 116 && (unsigned char)tb_s863[1] == 101 && (unsigned char)tb_s863[2] == 120 && (unsigned char)tb_s863[3] == 116 && tb_s863[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c864_0 = "*tb_p"; long long tb_c864_0n = sizeof("*tb_p") - 1; const char *tb_c864_1 = v_id; long long tb_c864_1n = (long long)strlen(tb_c864_1); const char *tb_c864_2 = " = tb_cat(*tb_p"; long long tb_c864_2n = sizeof(" = tb_cat(*tb_p") - 1; const char *tb_c864_3 = v_id; long long tb_c864_3n = (long long)strlen(tb_c864_3); const char *tb_c864_4 = ", tb_v"; long long tb_c864_4n = sizeof(", tb_v") - 1; const char *tb_c864_5 = v_id; long long tb_c864_5n = (long long)strlen(tb_c864_5); const char *tb_c864_6 = ");"; long long tb_c864_6n = sizeof(");") - 1; char *tb_c864r = tb_mem(tb_c864_0n + tb_c864_1n + tb_c864_2n + tb_c864_3n + tb_c864_4n + tb_c864_5n + tb_c864_6n + 1), *tb_c864rw = tb_c864r; memcpy(tb_c864rw, tb_c864_0, tb_c864_0n); tb_c864rw += tb_c864_0n; memcpy(tb_c864rw, tb_c864_1, tb_c864_1n); tb_c864rw += tb_c864_1n; memcpy(tb_c864rw, tb_c864_2, tb_c864_2n); tb_c864rw += tb_c864_2n; memcpy(tb_c864rw, tb_c864_3, tb_c864_3n); tb_c864rw += tb_c864_3n; memcpy(tb_c864rw, tb_c864_4, tb_c864_4n); tb_c864rw += tb_c864_4n; memcpy(tb_c864rw, tb_c864_5, tb_c864_5n); tb_c864rw += tb_c864_5n; memcpy(tb_c864rw, tb_c864_6, tb_c864_6n); tb_c864rw += tb_c864_6n; *tb_c864rw = 0; (const char *)tb_c864r; }));
    } else if ((({ const char *tb_s865 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 718))->q_ty); ((unsigned char)tb_s865[0] == 105 && (unsigned char)tb_s865[1] == 110 && (unsigned char)tb_s865[2] == 116 && tb_s865[3] == 0); }) && ({ const char *tb_s866 = v_op; ((unsigned char)tb_s866[0] == 47 && (unsigned char)tb_s866[1] == 61 && tb_s866[2] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c867_0 = "*tb_p"; long long tb_c867_0n = sizeof("*tb_p") - 1; const char *tb_c867_1 = v_id; long long tb_c867_1n = (long long)strlen(tb_c867_1); const char *tb_c867_2 = " = tb_div(*tb_p"; long long tb_c867_2n = sizeof(" = tb_div(*tb_p") - 1; const char *tb_c867_3 = v_id; long long tb_c867_3n = (long long)strlen(tb_c867_3); const char *tb_c867_4 = ", tb_v"; long long tb_c867_4n = sizeof(", tb_v") - 1; const char *tb_c867_5 = v_id; long long tb_c867_5n = (long long)strlen(tb_c867_5); const char *tb_c867_6 = ", "; long long tb_c867_6n = sizeof(", ") - 1; char tb_c867_7d[24]; long long tb_c867_7n; const char *tb_c867_7 = tb_digits(((tb_o_Node *)tb_nn(v_n, 719))->q_line, tb_c867_7d, &tb_c867_7n); const char *tb_c867_8 = ");"; long long tb_c867_8n = sizeof(");") - 1; char *tb_c867r = tb_mem(tb_c867_0n + tb_c867_1n + tb_c867_2n + tb_c867_3n + tb_c867_4n + tb_c867_5n + tb_c867_6n + tb_c867_7n + tb_c867_8n + 1), *tb_c867rw = tb_c867r; memcpy(tb_c867rw, tb_c867_0, tb_c867_0n); tb_c867rw += tb_c867_0n; memcpy(tb_c867rw, tb_c867_1, tb_c867_1n); tb_c867rw += tb_c867_1n; memcpy(tb_c867rw, tb_c867_2, tb_c867_2n); tb_c867rw += tb_c867_2n; memcpy(tb_c867rw, tb_c867_3, tb_c867_3n); tb_c867rw += tb_c867_3n; memcpy(tb_c867rw, tb_c867_4, tb_c867_4n); tb_c867rw += tb_c867_4n; memcpy(tb_c867rw, tb_c867_5, tb_c867_5n); tb_c867rw += tb_c867_5n; memcpy(tb_c867rw, tb_c867_6, tb_c867_6n); tb_c867rw += tb_c867_6n; memcpy(tb_c867rw, tb_c867_7, tb_c867_7n); tb_c867rw += tb_c867_7n; memcpy(tb_c867rw, tb_c867_8, tb_c867_8n); tb_c867rw += tb_c867_8n; *tb_c867rw = 0; (const char *)tb_c867r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c868_0 = "*tb_p"; long long tb_c868_0n = sizeof("*tb_p") - 1; const char *tb_c868_1 = v_id; long long tb_c868_1n = (long long)strlen(tb_c868_1); const char *tb_c868_2 = " "; long long tb_c868_2n = sizeof(" ") - 1; const char *tb_c868_3 = v_op; long long tb_c868_3n = (long long)strlen(tb_c868_3); const char *tb_c868_4 = " tb_v"; long long tb_c868_4n = sizeof(" tb_v") - 1; const char *tb_c868_5 = v_id; long long tb_c868_5n = (long long)strlen(tb_c868_5); const char *tb_c868_6 = ";"; long long tb_c868_6n = sizeof(";") - 1; char *tb_c868r = tb_mem(tb_c868_0n + tb_c868_1n + tb_c868_2n + tb_c868_3n + tb_c868_4n + tb_c868_5n + tb_c868_6n + 1), *tb_c868rw = tb_c868r; memcpy(tb_c868rw, tb_c868_0, tb_c868_0n); tb_c868rw += tb_c868_0n; memcpy(tb_c868rw, tb_c868_1, tb_c868_1n); tb_c868rw += tb_c868_1n; memcpy(tb_c868rw, tb_c868_2, tb_c868_2n); tb_c868rw += tb_c868_2n; memcpy(tb_c868rw, tb_c868_3, tb_c868_3n); tb_c868rw += tb_c868_3n; memcpy(tb_c868rw, tb_c868_4, tb_c868_4n); tb_c868rw += tb_c868_4n; memcpy(tb_c868rw, tb_c868_5, tb_c868_5n); tb_c868rw += tb_c868_5n; memcpy(tb_c868rw, tb_c868_6, tb_c868_6n); tb_c868rw += tb_c868_6n; *tb_c868rw = 0; (const char *)tb_c868r; }));
    }
    f_emit__put(v_c, " }\n");
}

static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 725))->q_kids)->len - 1LL);
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 726))->q_kids, v_names, sizeof(tb_o_Node *), 726));
    f_emit__emit_pre(v_c, v_call, v_depth);
    { long long tb_v869 = 1LL; long long *tb_p869 = &(((tb_o_Comp *)tb_nn(v_c, 728))->q_label); *tb_p869 += tb_v869; }
    const char * v_id = ({ const char *tb_c870_0 = ""; long long tb_c870_0n = sizeof("") - 1; char tb_c870_1d[24]; long long tb_c870_1n; const char *tb_c870_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 729))->q_label, tb_c870_1d, &tb_c870_1n); char *tb_c870r = tb_mem(tb_c870_0n + tb_c870_1n + 1), *tb_c870rw = tb_c870r; memcpy(tb_c870rw, tb_c870_0, tb_c870_0n); tb_c870rw += tb_c870_0n; memcpy(tb_c870rw, tb_c870_1, tb_c870_1n); tb_c870rw += tb_c870_1n; *tb_c870rw = 0; (const char *)tb_c870r; });
    {
        long long tb_end871 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end871; v_i++) {
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 731))->q_kids, v_i, sizeof(tb_o_Node *), 731)), 731))->q_creates) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, ({ const char *tb_c872_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, v_i, sizeof(tb_o_Node *), 733)), 733))->q_ty); long long tb_c872_0n = (long long)strlen(tb_c872_0); const char *tb_c872_1 = " v_"; long long tb_c872_1n = sizeof(" v_") - 1; const char *tb_c872_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, v_i, sizeof(tb_o_Node *), 733)), 733))->q_name; long long tb_c872_2n = (long long)strlen(tb_c872_2); const char *tb_c872_3 = ";\n"; long long tb_c872_3n = sizeof(";\n") - 1; char *tb_c872r = tb_mem(tb_c872_0n + tb_c872_1n + tb_c872_2n + tb_c872_3n + 1), *tb_c872rw = tb_c872r; memcpy(tb_c872rw, tb_c872_0, tb_c872_0n); tb_c872rw += tb_c872_0n; memcpy(tb_c872rw, tb_c872_1, tb_c872_1n); tb_c872rw += tb_c872_1n; memcpy(tb_c872rw, tb_c872_2, tb_c872_2n); tb_c872rw += tb_c872_2n; memcpy(tb_c872rw, tb_c872_3, tb_c872_3n); tb_c872rw += tb_c872_3n; *tb_c872rw = 0; (const char *)tb_c872r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c873_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 737))->q_ty); long long tb_c873_0n = (long long)strlen(tb_c873_0); const char *tb_c873_1 = " tb_m"; long long tb_c873_1n = sizeof(" tb_m") - 1; const char *tb_c873_2 = v_id; long long tb_c873_2n = (long long)strlen(tb_c873_2); const char *tb_c873_3 = " = "; long long tb_c873_3n = sizeof(" = ") - 1; char *tb_c873r = tb_mem(tb_c873_0n + tb_c873_1n + tb_c873_2n + tb_c873_3n + 1), *tb_c873rw = tb_c873r; memcpy(tb_c873rw, tb_c873_0, tb_c873_0n); tb_c873rw += tb_c873_0n; memcpy(tb_c873rw, tb_c873_1, tb_c873_1n); tb_c873rw += tb_c873_1n; memcpy(tb_c873rw, tb_c873_2, tb_c873_2n); tb_c873rw += tb_c873_2n; memcpy(tb_c873rw, tb_c873_3, tb_c873_3n); tb_c873rw += tb_c873_3n; *tb_c873rw = 0; (const char *)tb_c873r; }));
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    {
        long long tb_end874 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end874; v_i++) {
            f_emit__indent(v_c, (v_depth + 1LL));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, v_i, sizeof(tb_o_Node *), 742)));
            f_emit__put(v_c, ({ const char *tb_c875_0 = " = tb_m"; long long tb_c875_0n = sizeof(" = tb_m") - 1; const char *tb_c875_1 = v_id; long long tb_c875_1n = (long long)strlen(tb_c875_1); const char *tb_c875_2 = ".o"; long long tb_c875_2n = sizeof(".o") - 1; char tb_c875_3d[24]; long long tb_c875_3n; const char *tb_c875_3 = tb_digits(v_i, tb_c875_3d, &tb_c875_3n); const char *tb_c875_4 = ";\n"; long long tb_c875_4n = sizeof(";\n") - 1; char *tb_c875r = tb_mem(tb_c875_0n + tb_c875_1n + tb_c875_2n + tb_c875_3n + tb_c875_4n + 1), *tb_c875rw = tb_c875r; memcpy(tb_c875rw, tb_c875_0, tb_c875_0n); tb_c875rw += tb_c875_0n; memcpy(tb_c875rw, tb_c875_1, tb_c875_1n); tb_c875rw += tb_c875_1n; memcpy(tb_c875rw, tb_c875_2, tb_c875_2n); tb_c875rw += tb_c875_2n; memcpy(tb_c875rw, tb_c875_3, tb_c875_3n); tb_c875rw += tb_c875_3n; memcpy(tb_c875rw, tb_c875_4, tb_c875_4n); tb_c875rw += tb_c875_4n; *tb_c875rw = 0; (const char *)tb_c875r; }));
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_if(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    f_emit__indent(v_c, v_depth);
    long long v_i = 0LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 750))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, " else ");
        }
        f_emit__put(v_c, "if (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 754))->q_kids, v_i, sizeof(tb_o_Node *), 754)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 756))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 756)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
        { long long tb_v876 = 2LL; long long *tb_p876 = &(v_i); *tb_p876 += tb_v876; }
    }
    if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 760))->q_kids)->len, 2LL, 760) == 1LL)) {
        f_emit__put(v_c, " else {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 762))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 762))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 762)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_par(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v877 = 1LL; long long *tb_p877 = &(((tb_o_Comp *)tb_nn(v_c, 771))->q_label); *tb_p877 += tb_v877; }
    const char * v_id = ({ const char *tb_c878_0 = ""; long long tb_c878_0n = sizeof("") - 1; char tb_c878_1d[24]; long long tb_c878_1n; const char *tb_c878_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 772))->q_label, tb_c878_1d, &tb_c878_1n); char *tb_c878r = tb_mem(tb_c878_0n + tb_c878_1n + 1), *tb_c878rw = tb_c878r; memcpy(tb_c878rw, tb_c878_0, tb_c878_0n); tb_c878rw += tb_c878_0n; memcpy(tb_c878rw, tb_c878_1, tb_c878_1n); tb_c878rw += tb_c878_1n; *tb_c878rw = 0; (const char *)tb_c878r; });
    _Bool v_is_each = ({ const char *tb_s879 = ((tb_o_Node *)tb_nn(v_n, 773))->q_kind; ((unsigned char)tb_s879[0] == 101 && (unsigned char)tb_s879[1] == 97 && (unsigned char)tb_s879[2] == 99 && (unsigned char)tb_s879[3] == 104 && tb_s879[4] == 0); });
    const char * v_elem = "";
    if (v_is_each) {
        v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 776))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 776))->q_kids, 0LL, sizeof(tb_o_Node *), 776)), 776))->q_ty, sizeof(tb_o_Type *), 776)), 776))->q_elem);
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 778))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v880 = v_w; tb_list * *tb_p880 = &(((tb_o_Comp *)tb_nn(v_c, 780))->q_out); *tb_p880 = tb_v880; }
    const char * v_list_type = "tb_list *";
    if (v_is_each) {
        v_list_type = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 783))->q_kids, 0LL, sizeof(tb_o_Node *), 783)), 783))->q_ty);
    }
    f_emit__put(v_c, ({ const char *tb_c881_0 = "\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    "; long long tb_c881_0n = sizeof("\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    ") - 1; const char *tb_c881_1 = v_list_type; long long tb_c881_1n = (long long)strlen(tb_c881_1); const char *tb_c881_2 = " tb_l;\n"; long long tb_c881_2n = sizeof(" tb_l;\n") - 1; char *tb_c881r = tb_mem(tb_c881_0n + tb_c881_1n + tb_c881_2n + 1), *tb_c881rw = tb_c881r; memcpy(tb_c881rw, tb_c881_0, tb_c881_0n); tb_c881rw += tb_c881_0n; memcpy(tb_c881rw, tb_c881_1, tb_c881_1n); tb_c881rw += tb_c881_1n; memcpy(tb_c881rw, tb_c881_2, tb_c881_2n); tb_c881rw += tb_c881_2n; *tb_c881rw = 0; (const char *)tb_c881r; }));
    {
        tb_list * tb_l882 = ((tb_o_Node *)tb_nn(v_n, 785))->q_caps;
        for (long long tb_i882 = 0; tb_i882 < tb_l882->len; tb_i882++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l882->data)[tb_i882];
            if ((((tb_o_Var *)tb_nn(v_v, 786))->q_is_shared && ({ const char *tb_s883 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 786))->q_type); ((unsigned char)tb_s883[0] == 116 && (unsigned char)tb_s883[1] == 101 && (unsigned char)tb_s883[2] == 120 && (unsigned char)tb_s883[3] == 116 && tb_s883[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c884_0 = "    const char *m_"; long long tb_c884_0n = sizeof("    const char *m_") - 1; const char *tb_c884_1 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c884_1n = (long long)strlen(tb_c884_1); const char *tb_c884_2 = "; long long m_rl_"; long long tb_c884_2n = sizeof("; long long m_rl_") - 1; const char *tb_c884_3 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c884_3n = (long long)strlen(tb_c884_3); const char *tb_c884_4 = ", m_rc_"; long long tb_c884_4n = sizeof(", m_rc_") - 1; const char *tb_c884_5 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c884_5n = (long long)strlen(tb_c884_5); const char *tb_c884_6 = ";\n"; long long tb_c884_6n = sizeof(";\n") - 1; char *tb_c884r = tb_mem(tb_c884_0n + tb_c884_1n + tb_c884_2n + tb_c884_3n + tb_c884_4n + tb_c884_5n + tb_c884_6n + 1), *tb_c884rw = tb_c884r; memcpy(tb_c884rw, tb_c884_0, tb_c884_0n); tb_c884rw += tb_c884_0n; memcpy(tb_c884rw, tb_c884_1, tb_c884_1n); tb_c884rw += tb_c884_1n; memcpy(tb_c884rw, tb_c884_2, tb_c884_2n); tb_c884rw += tb_c884_2n; memcpy(tb_c884rw, tb_c884_3, tb_c884_3n); tb_c884rw += tb_c884_3n; memcpy(tb_c884rw, tb_c884_4, tb_c884_4n); tb_c884rw += tb_c884_4n; memcpy(tb_c884rw, tb_c884_5, tb_c884_5n); tb_c884rw += tb_c884_5n; memcpy(tb_c884rw, tb_c884_6, tb_c884_6n); tb_c884rw += tb_c884_6n; *tb_c884rw = 0; (const char *)tb_c884r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 789))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c885_0 = "    "; long long tb_c885_0n = sizeof("    ") - 1; const char *tb_c885_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 790))->q_type); long long tb_c885_1n = (long long)strlen(tb_c885_1); const char *tb_c885_2 = " *p_"; long long tb_c885_2n = sizeof(" *p_") - 1; const char *tb_c885_3 = ((tb_o_Var *)tb_nn(v_v, 790))->q_name; long long tb_c885_3n = (long long)strlen(tb_c885_3); const char *tb_c885_4 = ";\n"; long long tb_c885_4n = sizeof(";\n") - 1; char *tb_c885r = tb_mem(tb_c885_0n + tb_c885_1n + tb_c885_2n + tb_c885_3n + tb_c885_4n + 1), *tb_c885rw = tb_c885r; memcpy(tb_c885rw, tb_c885_0, tb_c885_0n); tb_c885rw += tb_c885_0n; memcpy(tb_c885rw, tb_c885_1, tb_c885_1n); tb_c885rw += tb_c885_1n; memcpy(tb_c885rw, tb_c885_2, tb_c885_2n); tb_c885rw += tb_c885_2n; memcpy(tb_c885rw, tb_c885_3, tb_c885_3n); tb_c885rw += tb_c885_3n; memcpy(tb_c885rw, tb_c885_4, tb_c885_4n); tb_c885rw += tb_c885_4n; *tb_c885rw = 0; (const char *)tb_c885r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 791))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c886_0 = "    "; long long tb_c886_0n = sizeof("    ") - 1; const char *tb_c886_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 792))->q_type); long long tb_c886_1n = (long long)strlen(tb_c886_1); const char *tb_c886_2 = " *v_"; long long tb_c886_2n = sizeof(" *v_") - 1; const char *tb_c886_3 = ((tb_o_Var *)tb_nn(v_v, 792))->q_name; long long tb_c886_3n = (long long)strlen(tb_c886_3); const char *tb_c886_4 = ";\n"; long long tb_c886_4n = sizeof(";\n") - 1; char *tb_c886r = tb_mem(tb_c886_0n + tb_c886_1n + tb_c886_2n + tb_c886_3n + tb_c886_4n + 1), *tb_c886rw = tb_c886r; memcpy(tb_c886rw, tb_c886_0, tb_c886_0n); tb_c886rw += tb_c886_0n; memcpy(tb_c886rw, tb_c886_1, tb_c886_1n); tb_c886rw += tb_c886_1n; memcpy(tb_c886rw, tb_c886_2, tb_c886_2n); tb_c886rw += tb_c886_2n; memcpy(tb_c886rw, tb_c886_3, tb_c886_3n); tb_c886rw += tb_c886_3n; memcpy(tb_c886rw, tb_c886_4, tb_c886_4n); tb_c886rw += tb_c886_4n; *tb_c886rw = 0; (const char *)tb_c886r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c887_0 = "    "; long long tb_c887_0n = sizeof("    ") - 1; const char *tb_c887_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 794))->q_type); long long tb_c887_1n = (long long)strlen(tb_c887_1); const char *tb_c887_2 = " v_"; long long tb_c887_2n = sizeof(" v_") - 1; const char *tb_c887_3 = ((tb_o_Var *)tb_nn(v_v, 794))->q_name; long long tb_c887_3n = (long long)strlen(tb_c887_3); const char *tb_c887_4 = ";\n"; long long tb_c887_4n = sizeof(";\n") - 1; char *tb_c887r = tb_mem(tb_c887_0n + tb_c887_1n + tb_c887_2n + tb_c887_3n + tb_c887_4n + 1), *tb_c887rw = tb_c887r; memcpy(tb_c887rw, tb_c887_0, tb_c887_0n); tb_c887rw += tb_c887_0n; memcpy(tb_c887rw, tb_c887_1, tb_c887_1n); tb_c887rw += tb_c887_1n; memcpy(tb_c887rw, tb_c887_2, tb_c887_2n); tb_c887rw += tb_c887_2n; memcpy(tb_c887rw, tb_c887_3, tb_c887_3n); tb_c887rw += tb_c887_3n; memcpy(tb_c887rw, tb_c887_4, tb_c887_4n); tb_c887rw += tb_c887_4n; *tb_c887rw = 0; (const char *)tb_c887r; }));
            }
        }
    }
    f_emit__put(v_c, ({ const char *tb_c888_0 = "} tb_job"; long long tb_c888_0n = sizeof("} tb_job") - 1; const char *tb_c888_1 = v_id; long long tb_c888_1n = (long long)strlen(tb_c888_1); const char *tb_c888_2 = ";\n"; long long tb_c888_2n = sizeof(";\n") - 1; char *tb_c888r = tb_mem(tb_c888_0n + tb_c888_1n + tb_c888_2n + 1), *tb_c888rw = tb_c888r; memcpy(tb_c888rw, tb_c888_0, tb_c888_0n); tb_c888rw += tb_c888_0n; memcpy(tb_c888rw, tb_c888_1, tb_c888_1n); tb_c888rw += tb_c888_1n; memcpy(tb_c888rw, tb_c888_2, tb_c888_2n); tb_c888rw += tb_c888_2n; *tb_c888rw = 0; (const char *)tb_c888r; }));
    f_emit__put(v_c, ({ const char *tb_c889_0 = "static void *tb_work"; long long tb_c889_0n = sizeof("static void *tb_work") - 1; const char *tb_c889_1 = v_id; long long tb_c889_1n = (long long)strlen(tb_c889_1); const char *tb_c889_2 = "(void *tb_arg) {\n"; long long tb_c889_2n = sizeof("(void *tb_arg) {\n") - 1; char *tb_c889r = tb_mem(tb_c889_0n + tb_c889_1n + tb_c889_2n + 1), *tb_c889rw = tb_c889r; memcpy(tb_c889rw, tb_c889_0, tb_c889_0n); tb_c889rw += tb_c889_0n; memcpy(tb_c889rw, tb_c889_1, tb_c889_1n); tb_c889rw += tb_c889_1n; memcpy(tb_c889rw, tb_c889_2, tb_c889_2n); tb_c889rw += tb_c889_2n; *tb_c889rw = 0; (const char *)tb_c889r; }));
    f_emit__put(v_c, ({ const char *tb_c890_0 = "    tb_job"; long long tb_c890_0n = sizeof("    tb_job") - 1; const char *tb_c890_1 = v_id; long long tb_c890_1n = (long long)strlen(tb_c890_1); const char *tb_c890_2 = " *tb_j = tb_arg;\n"; long long tb_c890_2n = sizeof(" *tb_j = tb_arg;\n") - 1; char *tb_c890r = tb_mem(tb_c890_0n + tb_c890_1n + tb_c890_2n + 1), *tb_c890rw = tb_c890r; memcpy(tb_c890rw, tb_c890_0, tb_c890_0n); tb_c890rw += tb_c890_0n; memcpy(tb_c890rw, tb_c890_1, tb_c890_1n); tb_c890rw += tb_c890_1n; memcpy(tb_c890rw, tb_c890_2, tb_c890_2n); tb_c890rw += tb_c890_2n; *tb_c890rw = 0; (const char *)tb_c890r; }));
    _Bool v_has_shared = 0;
    tb_list * v_saved_builders = ((tb_o_Comp *)tb_nn(v_c, 800))->q_builders;
    tb_list * v_par_builders = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l891 = ((tb_o_Node *)tb_nn(v_n, 802))->q_caps;
        for (long long tb_i891 = 0; tb_i891 < tb_l891->len; tb_i891++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l891->data)[tb_i891];
            if (((tb_o_Var *)tb_nn(v_v, 803))->q_is_shared) {
                v_has_shared = 1;
                f_emit__put(v_c, ({ const char *tb_c892_0 = "    "; long long tb_c892_0n = sizeof("    ") - 1; const char *tb_c892_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 805))->q_type); long long tb_c892_1n = (long long)strlen(tb_c892_1); const char *tb_c892_2 = " v_"; long long tb_c892_2n = sizeof(" v_") - 1; const char *tb_c892_3 = ((tb_o_Var *)tb_nn(v_v, 805))->q_name; long long tb_c892_3n = (long long)strlen(tb_c892_3); const char *tb_c892_4 = " = "; long long tb_c892_4n = sizeof(" = ") - 1; char *tb_c892r = tb_mem(tb_c892_0n + tb_c892_1n + tb_c892_2n + tb_c892_3n + tb_c892_4n + 1), *tb_c892rw = tb_c892r; memcpy(tb_c892rw, tb_c892_0, tb_c892_0n); tb_c892rw += tb_c892_0n; memcpy(tb_c892rw, tb_c892_1, tb_c892_1n); tb_c892rw += tb_c892_1n; memcpy(tb_c892rw, tb_c892_2, tb_c892_2n); tb_c892rw += tb_c892_2n; memcpy(tb_c892rw, tb_c892_3, tb_c892_3n); tb_c892rw += tb_c892_3n; memcpy(tb_c892rw, tb_c892_4, tb_c892_4n); tb_c892rw += tb_c892_4n; *tb_c892rw = 0; (const char *)tb_c892r; }));
                f_emit__emit_default(v_c, ((tb_o_Var *)tb_nn(v_v, 806))->q_type);
                f_emit__put(v_c, ";\n");
                if (({ const char *tb_s893 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 808))->q_type); ((unsigned char)tb_s893[0] == 116 && (unsigned char)tb_s893[1] == 101 && (unsigned char)tb_s893[2] == 120 && (unsigned char)tb_s893[3] == 116 && tb_s893[4] == 0); })) {
                    { const char * tb_v894 = ((tb_o_Var *)tb_nn(v_v, 809))->q_name; *(const char * *)tb_grow(v_par_builders) = tb_v894; }
                    f_emit__put(v_c, ({ const char *tb_c895_0 = "    long long tb_rl_"; long long tb_c895_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c895_1 = ((tb_o_Var *)tb_nn(v_v, 810))->q_name; long long tb_c895_1n = (long long)strlen(tb_c895_1); const char *tb_c895_2 = " = -1, tb_rc_"; long long tb_c895_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c895_3 = ((tb_o_Var *)tb_nn(v_v, 810))->q_name; long long tb_c895_3n = (long long)strlen(tb_c895_3); const char *tb_c895_4 = " = 0;\n"; long long tb_c895_4n = sizeof(" = 0;\n") - 1; char *tb_c895r = tb_mem(tb_c895_0n + tb_c895_1n + tb_c895_2n + tb_c895_3n + tb_c895_4n + 1), *tb_c895rw = tb_c895r; memcpy(tb_c895rw, tb_c895_0, tb_c895_0n); tb_c895rw += tb_c895_0n; memcpy(tb_c895rw, tb_c895_1, tb_c895_1n); tb_c895rw += tb_c895_1n; memcpy(tb_c895rw, tb_c895_2, tb_c895_2n); tb_c895rw += tb_c895_2n; memcpy(tb_c895rw, tb_c895_3, tb_c895_3n); tb_c895rw += tb_c895_3n; memcpy(tb_c895rw, tb_c895_4, tb_c895_4n); tb_c895rw += tb_c895_4n; *tb_c895rw = 0; (const char *)tb_c895r; }));
                }
            } else if (((tb_o_Var *)tb_nn(v_v, 811))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c896_0 = "    "; long long tb_c896_0n = sizeof("    ") - 1; const char *tb_c896_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 812))->q_type); long long tb_c896_1n = (long long)strlen(tb_c896_1); const char *tb_c896_2 = " *v_"; long long tb_c896_2n = sizeof(" *v_") - 1; const char *tb_c896_3 = ((tb_o_Var *)tb_nn(v_v, 812))->q_name; long long tb_c896_3n = (long long)strlen(tb_c896_3); const char *tb_c896_4 = " = tb_j->v_"; long long tb_c896_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c896_5 = ((tb_o_Var *)tb_nn(v_v, 812))->q_name; long long tb_c896_5n = (long long)strlen(tb_c896_5); const char *tb_c896_6 = ";\n"; long long tb_c896_6n = sizeof(";\n") - 1; char *tb_c896r = tb_mem(tb_c896_0n + tb_c896_1n + tb_c896_2n + tb_c896_3n + tb_c896_4n + tb_c896_5n + tb_c896_6n + 1), *tb_c896rw = tb_c896r; memcpy(tb_c896rw, tb_c896_0, tb_c896_0n); tb_c896rw += tb_c896_0n; memcpy(tb_c896rw, tb_c896_1, tb_c896_1n); tb_c896rw += tb_c896_1n; memcpy(tb_c896rw, tb_c896_2, tb_c896_2n); tb_c896rw += tb_c896_2n; memcpy(tb_c896rw, tb_c896_3, tb_c896_3n); tb_c896rw += tb_c896_3n; memcpy(tb_c896rw, tb_c896_4, tb_c896_4n); tb_c896rw += tb_c896_4n; memcpy(tb_c896rw, tb_c896_5, tb_c896_5n); tb_c896rw += tb_c896_5n; memcpy(tb_c896rw, tb_c896_6, tb_c896_6n); tb_c896rw += tb_c896_6n; *tb_c896rw = 0; (const char *)tb_c896r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c897_0 = "    "; long long tb_c897_0n = sizeof("    ") - 1; const char *tb_c897_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 814))->q_type); long long tb_c897_1n = (long long)strlen(tb_c897_1); const char *tb_c897_2 = " v_"; long long tb_c897_2n = sizeof(" v_") - 1; const char *tb_c897_3 = ((tb_o_Var *)tb_nn(v_v, 814))->q_name; long long tb_c897_3n = (long long)strlen(tb_c897_3); const char *tb_c897_4 = " = tb_j->v_"; long long tb_c897_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c897_5 = ((tb_o_Var *)tb_nn(v_v, 814))->q_name; long long tb_c897_5n = (long long)strlen(tb_c897_5); const char *tb_c897_6 = ";\n"; long long tb_c897_6n = sizeof(";\n") - 1; char *tb_c897r = tb_mem(tb_c897_0n + tb_c897_1n + tb_c897_2n + tb_c897_3n + tb_c897_4n + tb_c897_5n + tb_c897_6n + 1), *tb_c897rw = tb_c897r; memcpy(tb_c897rw, tb_c897_0, tb_c897_0n); tb_c897rw += tb_c897_0n; memcpy(tb_c897rw, tb_c897_1, tb_c897_1n); tb_c897rw += tb_c897_1n; memcpy(tb_c897rw, tb_c897_2, tb_c897_2n); tb_c897rw += tb_c897_2n; memcpy(tb_c897rw, tb_c897_3, tb_c897_3n); tb_c897rw += tb_c897_3n; memcpy(tb_c897rw, tb_c897_4, tb_c897_4n); tb_c897rw += tb_c897_4n; memcpy(tb_c897rw, tb_c897_5, tb_c897_5n); tb_c897rw += tb_c897_5n; memcpy(tb_c897rw, tb_c897_6, tb_c897_6n); tb_c897rw += tb_c897_6n; *tb_c897rw = 0; (const char *)tb_c897r; }));
            }
        }
    }
    { tb_list * tb_v898 = v_par_builders; tb_list * *tb_p898 = &(((tb_o_Comp *)tb_nn(v_c, 815))->q_builders); *tb_p898 = tb_v898; }
    f_emit__put(v_c, "    for (;;) {\n");
    f_emit__put(v_c, "        long long tb_s = __atomic_fetch_add(&tb_j->tb_next, tb_j->tb_step, __ATOMIC_RELAXED);\n");
    f_emit__put(v_c, "        if (tb_s >= tb_j->tb_total) break;\n");
    f_emit__put(v_c, "        long long tb_e = tb_s + tb_j->tb_step;\n");
    f_emit__put(v_c, "        if (tb_e > tb_j->tb_total) tb_e = tb_j->tb_total;\n");
    f_emit__put(v_c, "        for (long long tb_i = tb_s; tb_i < tb_e; tb_i++) {\n");
    if ((v_is_each && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 822))->q_kids, 0LL, sizeof(tb_o_Node *), 822)), 822))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c899_0 = "            "; long long tb_c899_0n = sizeof("            ") - 1; const char *tb_c899_1 = v_elem; long long tb_c899_1n = (long long)strlen(tb_c899_1); const char *tb_c899_2 = " const v_"; long long tb_c899_2n = sizeof(" const v_") - 1; const char *tb_c899_3 = ((tb_o_Node *)tb_nn(v_n, 823))->q_name; long long tb_c899_3n = (long long)strlen(tb_c899_3); const char *tb_c899_4 = " = {tb_j->tb_l, tb_i};\n"; long long tb_c899_4n = sizeof(" = {tb_j->tb_l, tb_i};\n") - 1; char *tb_c899r = tb_mem(tb_c899_0n + tb_c899_1n + tb_c899_2n + tb_c899_3n + tb_c899_4n + 1), *tb_c899rw = tb_c899r; memcpy(tb_c899rw, tb_c899_0, tb_c899_0n); tb_c899rw += tb_c899_0n; memcpy(tb_c899rw, tb_c899_1, tb_c899_1n); tb_c899rw += tb_c899_1n; memcpy(tb_c899rw, tb_c899_2, tb_c899_2n); tb_c899rw += tb_c899_2n; memcpy(tb_c899rw, tb_c899_3, tb_c899_3n); tb_c899rw += tb_c899_3n; memcpy(tb_c899rw, tb_c899_4, tb_c899_4n); tb_c899rw += tb_c899_4n; *tb_c899rw = 0; (const char *)tb_c899r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 824))->q_kids, 1LL, sizeof(tb_o_Node *), 824)), 3LL);
    } else if ((v_is_each && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 825))->q_kids, 0LL, sizeof(tb_o_Node *), 825)), 825))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c900_0 = "            _Bool const v_"; long long tb_c900_0n = sizeof("            _Bool const v_") - 1; const char *tb_c900_1 = ((tb_o_Node *)tb_nn(v_n, 826))->q_name; long long tb_c900_1n = (long long)strlen(tb_c900_1); const char *tb_c900_2 = " = TB_BIT(tb_j->tb_l, tb_i);\n"; long long tb_c900_2n = sizeof(" = TB_BIT(tb_j->tb_l, tb_i);\n") - 1; char *tb_c900r = tb_mem(tb_c900_0n + tb_c900_1n + tb_c900_2n + 1), *tb_c900rw = tb_c900r; memcpy(tb_c900rw, tb_c900_0, tb_c900_0n); tb_c900rw += tb_c900_0n; memcpy(tb_c900rw, tb_c900_1, tb_c900_1n); tb_c900rw += tb_c900_1n; memcpy(tb_c900rw, tb_c900_2, tb_c900_2n); tb_c900rw += tb_c900_2n; *tb_c900rw = 0; (const char *)tb_c900r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 827))->q_kids, 1LL, sizeof(tb_o_Node *), 827)), 3LL);
    } else if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c901_0 = "            "; long long tb_c901_0n = sizeof("            ") - 1; const char *tb_c901_1 = v_elem; long long tb_c901_1n = (long long)strlen(tb_c901_1); const char *tb_c901_2 = " const v_"; long long tb_c901_2n = sizeof(" const v_") - 1; const char *tb_c901_3 = ((tb_o_Node *)tb_nn(v_n, 829))->q_name; long long tb_c901_3n = (long long)strlen(tb_c901_3); const char *tb_c901_4 = " = (("; long long tb_c901_4n = sizeof(" = ((") - 1; const char *tb_c901_5 = v_elem; long long tb_c901_5n = (long long)strlen(tb_c901_5); const char *tb_c901_6 = " *)tb_j->tb_l->data)[tb_i];\n"; long long tb_c901_6n = sizeof(" *)tb_j->tb_l->data)[tb_i];\n") - 1; char *tb_c901r = tb_mem(tb_c901_0n + tb_c901_1n + tb_c901_2n + tb_c901_3n + tb_c901_4n + tb_c901_5n + tb_c901_6n + 1), *tb_c901rw = tb_c901r; memcpy(tb_c901rw, tb_c901_0, tb_c901_0n); tb_c901rw += tb_c901_0n; memcpy(tb_c901rw, tb_c901_1, tb_c901_1n); tb_c901rw += tb_c901_1n; memcpy(tb_c901rw, tb_c901_2, tb_c901_2n); tb_c901rw += tb_c901_2n; memcpy(tb_c901rw, tb_c901_3, tb_c901_3n); tb_c901rw += tb_c901_3n; memcpy(tb_c901rw, tb_c901_4, tb_c901_4n); tb_c901rw += tb_c901_4n; memcpy(tb_c901rw, tb_c901_5, tb_c901_5n); tb_c901rw += tb_c901_5n; memcpy(tb_c901rw, tb_c901_6, tb_c901_6n); tb_c901rw += tb_c901_6n; *tb_c901rw = 0; (const char *)tb_c901r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 830))->q_kids, 1LL, sizeof(tb_o_Node *), 830)), 3LL);
    } else {
        f_emit__put(v_c, ({ const char *tb_c902_0 = "            long long const v_"; long long tb_c902_0n = sizeof("            long long const v_") - 1; const char *tb_c902_1 = ((tb_o_Node *)tb_nn(v_n, 832))->q_name; long long tb_c902_1n = (long long)strlen(tb_c902_1); const char *tb_c902_2 = " = tb_j->tb_lo + tb_i;\n"; long long tb_c902_2n = sizeof(" = tb_j->tb_lo + tb_i;\n") - 1; char *tb_c902r = tb_mem(tb_c902_0n + tb_c902_1n + tb_c902_2n + 1), *tb_c902rw = tb_c902r; memcpy(tb_c902rw, tb_c902_0, tb_c902_0n); tb_c902rw += tb_c902_0n; memcpy(tb_c902rw, tb_c902_1, tb_c902_1n); tb_c902rw += tb_c902_1n; memcpy(tb_c902rw, tb_c902_2, tb_c902_2n); tb_c902rw += tb_c902_2n; *tb_c902rw = 0; (const char *)tb_c902r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 833))->q_kids, 2LL, sizeof(tb_o_Node *), 833)), 3LL);
    }
    f_emit__put(v_c, "        }\n    }\n");
    { tb_list * tb_v903 = v_saved_builders; tb_list * *tb_p903 = &(((tb_o_Comp *)tb_nn(v_c, 835))->q_builders); *tb_p903 = tb_v903; }
    if (v_has_shared) {
        f_emit__put(v_c, "    pthread_mutex_lock(&tb_par_lock);\n");
        {
            tb_list * tb_l904 = ((tb_o_Node *)tb_nn(v_n, 839))->q_caps;
            for (long long tb_i904 = 0; tb_i904 < tb_l904->len; tb_i904++) {
                tb_o_Var * const v_v = ((tb_o_Var * *)tb_l904->data)[tb_i904];
                if ((((tb_o_Var *)tb_nn(v_v, 840))->q_is_shared && ({ const char *tb_s905 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 840))->q_type); ((unsigned char)tb_s905[0] == 116 && (unsigned char)tb_s905[1] == 101 && (unsigned char)tb_s905[2] == 120 && (unsigned char)tb_s905[3] == 116 && tb_s905[4] == 0); }))) {
                    f_emit__put(v_c, ({ const char *tb_c906_0 = "    tb_app_take(&tb_j->m_"; long long tb_c906_0n = sizeof("    tb_app_take(&tb_j->m_") - 1; const char *tb_c906_1 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_1n = (long long)strlen(tb_c906_1); const char *tb_c906_2 = ", &tb_j->m_rl_"; long long tb_c906_2n = sizeof(", &tb_j->m_rl_") - 1; const char *tb_c906_3 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_3n = (long long)strlen(tb_c906_3); const char *tb_c906_4 = ", &tb_j->m_rc_"; long long tb_c906_4n = sizeof(", &tb_j->m_rc_") - 1; const char *tb_c906_5 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_5n = (long long)strlen(tb_c906_5); const char *tb_c906_6 = ", v_"; long long tb_c906_6n = sizeof(", v_") - 1; const char *tb_c906_7 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_7n = (long long)strlen(tb_c906_7); const char *tb_c906_8 = ", tb_rl_"; long long tb_c906_8n = sizeof(", tb_rl_") - 1; const char *tb_c906_9 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_9n = (long long)strlen(tb_c906_9); const char *tb_c906_10 = ", tb_rc_"; long long tb_c906_10n = sizeof(", tb_rc_") - 1; const char *tb_c906_11 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c906_11n = (long long)strlen(tb_c906_11); const char *tb_c906_12 = ");\n"; long long tb_c906_12n = sizeof(");\n") - 1; char *tb_c906r = tb_mem(tb_c906_0n + tb_c906_1n + tb_c906_2n + tb_c906_3n + tb_c906_4n + tb_c906_5n + tb_c906_6n + tb_c906_7n + tb_c906_8n + tb_c906_9n + tb_c906_10n + tb_c906_11n + tb_c906_12n + 1), *tb_c906rw = tb_c906r; memcpy(tb_c906rw, tb_c906_0, tb_c906_0n); tb_c906rw += tb_c906_0n; memcpy(tb_c906rw, tb_c906_1, tb_c906_1n); tb_c906rw += tb_c906_1n; memcpy(tb_c906rw, tb_c906_2, tb_c906_2n); tb_c906rw += tb_c906_2n; memcpy(tb_c906rw, tb_c906_3, tb_c906_3n); tb_c906rw += tb_c906_3n; memcpy(tb_c906rw, tb_c906_4, tb_c906_4n); tb_c906rw += tb_c906_4n; memcpy(tb_c906rw, tb_c906_5, tb_c906_5n); tb_c906rw += tb_c906_5n; memcpy(tb_c906rw, tb_c906_6, tb_c906_6n); tb_c906rw += tb_c906_6n; memcpy(tb_c906rw, tb_c906_7, tb_c906_7n); tb_c906rw += tb_c906_7n; memcpy(tb_c906rw, tb_c906_8, tb_c906_8n); tb_c906rw += tb_c906_8n; memcpy(tb_c906rw, tb_c906_9, tb_c906_9n); tb_c906rw += tb_c906_9n; memcpy(tb_c906rw, tb_c906_10, tb_c906_10n); tb_c906rw += tb_c906_10n; memcpy(tb_c906rw, tb_c906_11, tb_c906_11n); tb_c906rw += tb_c906_11n; memcpy(tb_c906rw, tb_c906_12, tb_c906_12n); tb_c906rw += tb_c906_12n; *tb_c906rw = 0; (const char *)tb_c906r; }));
                } else if (((tb_o_Var *)tb_nn(v_v, 842))->q_is_shared) {
                    f_emit__put(v_c, ({ const char *tb_c907_0 = "    *tb_j->p_"; long long tb_c907_0n = sizeof("    *tb_j->p_") - 1; const char *tb_c907_1 = ((tb_o_Var *)tb_nn(v_v, 843))->q_name; long long tb_c907_1n = (long long)strlen(tb_c907_1); const char *tb_c907_2 = " += v_"; long long tb_c907_2n = sizeof(" += v_") - 1; const char *tb_c907_3 = ((tb_o_Var *)tb_nn(v_v, 843))->q_name; long long tb_c907_3n = (long long)strlen(tb_c907_3); const char *tb_c907_4 = ";\n"; long long tb_c907_4n = sizeof(";\n") - 1; char *tb_c907r = tb_mem(tb_c907_0n + tb_c907_1n + tb_c907_2n + tb_c907_3n + tb_c907_4n + 1), *tb_c907rw = tb_c907r; memcpy(tb_c907rw, tb_c907_0, tb_c907_0n); tb_c907rw += tb_c907_0n; memcpy(tb_c907rw, tb_c907_1, tb_c907_1n); tb_c907rw += tb_c907_1n; memcpy(tb_c907rw, tb_c907_2, tb_c907_2n); tb_c907rw += tb_c907_2n; memcpy(tb_c907rw, tb_c907_3, tb_c907_3n); tb_c907rw += tb_c907_3n; memcpy(tb_c907rw, tb_c907_4, tb_c907_4n); tb_c907rw += tb_c907_4n; *tb_c907rw = 0; (const char *)tb_c907r; }));
                }
            }
        }
        f_emit__put(v_c, "    pthread_mutex_unlock(&tb_par_lock);\n");
    }
    f_emit__put(v_c, "    return 0;\n}\n");
    { const char * tb_v908 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 846))->q_workers) = tb_v908; }
    { tb_list * tb_v909 = v_saved; tb_list * *tb_p909 = &(((tb_o_Comp *)tb_nn(v_c, 847))->q_out); *tb_p909 = tb_v909; }
    const char * v_j = ({ const char *tb_c910_0 = "tb_j"; long long tb_c910_0n = sizeof("tb_j") - 1; const char *tb_c910_1 = v_id; long long tb_c910_1n = (long long)strlen(tb_c910_1); char *tb_c910r = tb_mem(tb_c910_0n + tb_c910_1n + 1), *tb_c910rw = tb_c910r; memcpy(tb_c910rw, tb_c910_0, tb_c910_0n); tb_c910rw += tb_c910_0n; memcpy(tb_c910rw, tb_c910_1, tb_c910_1n); tb_c910rw += tb_c910_1n; *tb_c910rw = 0; (const char *)tb_c910r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c911_0 = "tb_job"; long long tb_c911_0n = sizeof("tb_job") - 1; const char *tb_c911_1 = v_id; long long tb_c911_1n = (long long)strlen(tb_c911_1); const char *tb_c911_2 = " "; long long tb_c911_2n = sizeof(" ") - 1; const char *tb_c911_3 = v_j; long long tb_c911_3n = (long long)strlen(tb_c911_3); const char *tb_c911_4 = " = {0};\n"; long long tb_c911_4n = sizeof(" = {0};\n") - 1; char *tb_c911r = tb_mem(tb_c911_0n + tb_c911_1n + tb_c911_2n + tb_c911_3n + tb_c911_4n + 1), *tb_c911rw = tb_c911r; memcpy(tb_c911rw, tb_c911_0, tb_c911_0n); tb_c911rw += tb_c911_0n; memcpy(tb_c911rw, tb_c911_1, tb_c911_1n); tb_c911rw += tb_c911_1n; memcpy(tb_c911rw, tb_c911_2, tb_c911_2n); tb_c911rw += tb_c911_2n; memcpy(tb_c911rw, tb_c911_3, tb_c911_3n); tb_c911rw += tb_c911_3n; memcpy(tb_c911rw, tb_c911_4, tb_c911_4n); tb_c911rw += tb_c911_4n; *tb_c911rw = 0; (const char *)tb_c911r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c912_0 = v_j; long long tb_c912_0n = (long long)strlen(tb_c912_0); const char *tb_c912_1 = ".tb_l = "; long long tb_c912_1n = sizeof(".tb_l = ") - 1; char *tb_c912r = tb_mem(tb_c912_0n + tb_c912_1n + 1), *tb_c912rw = tb_c912r; memcpy(tb_c912rw, tb_c912_0, tb_c912_0n); tb_c912rw += tb_c912_0n; memcpy(tb_c912rw, tb_c912_1, tb_c912_1n); tb_c912rw += tb_c912_1n; *tb_c912rw = 0; (const char *)tb_c912r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 857))->q_kids, 0LL, sizeof(tb_o_Node *), 857)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c913_0 = v_j; long long tb_c913_0n = (long long)strlen(tb_c913_0); const char *tb_c913_1 = ".tb_total = "; long long tb_c913_1n = sizeof(".tb_total = ") - 1; const char *tb_c913_2 = v_j; long long tb_c913_2n = (long long)strlen(tb_c913_2); const char *tb_c913_3 = ".tb_l->len;\n"; long long tb_c913_3n = sizeof(".tb_l->len;\n") - 1; char *tb_c913r = tb_mem(tb_c913_0n + tb_c913_1n + tb_c913_2n + tb_c913_3n + 1), *tb_c913rw = tb_c913r; memcpy(tb_c913rw, tb_c913_0, tb_c913_0n); tb_c913rw += tb_c913_0n; memcpy(tb_c913rw, tb_c913_1, tb_c913_1n); tb_c913rw += tb_c913_1n; memcpy(tb_c913rw, tb_c913_2, tb_c913_2n); tb_c913rw += tb_c913_2n; memcpy(tb_c913rw, tb_c913_3, tb_c913_3n); tb_c913rw += tb_c913_3n; *tb_c913rw = 0; (const char *)tb_c913r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c914_0 = "long long tb_end"; long long tb_c914_0n = sizeof("long long tb_end") - 1; const char *tb_c914_1 = v_id; long long tb_c914_1n = (long long)strlen(tb_c914_1); const char *tb_c914_2 = " = "; long long tb_c914_2n = sizeof(" = ") - 1; char *tb_c914r = tb_mem(tb_c914_0n + tb_c914_1n + tb_c914_2n + 1), *tb_c914rw = tb_c914r; memcpy(tb_c914rw, tb_c914_0, tb_c914_0n); tb_c914rw += tb_c914_0n; memcpy(tb_c914rw, tb_c914_1, tb_c914_1n); tb_c914rw += tb_c914_1n; memcpy(tb_c914rw, tb_c914_2, tb_c914_2n); tb_c914rw += tb_c914_2n; *tb_c914rw = 0; (const char *)tb_c914r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 863))->q_kids, 1LL, sizeof(tb_o_Node *), 863)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c915_0 = v_j; long long tb_c915_0n = (long long)strlen(tb_c915_0); const char *tb_c915_1 = ".tb_lo = "; long long tb_c915_1n = sizeof(".tb_lo = ") - 1; char *tb_c915r = tb_mem(tb_c915_0n + tb_c915_1n + 1), *tb_c915rw = tb_c915r; memcpy(tb_c915rw, tb_c915_0, tb_c915_0n); tb_c915rw += tb_c915_0n; memcpy(tb_c915rw, tb_c915_1, tb_c915_1n); tb_c915rw += tb_c915_1n; *tb_c915rw = 0; (const char *)tb_c915r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 867))->q_kids, 0LL, sizeof(tb_o_Node *), 867)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c916_0 = v_j; long long tb_c916_0n = (long long)strlen(tb_c916_0); const char *tb_c916_1 = ".tb_total = tb_end"; long long tb_c916_1n = sizeof(".tb_total = tb_end") - 1; const char *tb_c916_2 = v_id; long long tb_c916_2n = (long long)strlen(tb_c916_2); const char *tb_c916_3 = " < "; long long tb_c916_3n = sizeof(" < ") - 1; const char *tb_c916_4 = v_j; long long tb_c916_4n = (long long)strlen(tb_c916_4); const char *tb_c916_5 = ".tb_lo ? 0 : tb_end"; long long tb_c916_5n = sizeof(".tb_lo ? 0 : tb_end") - 1; const char *tb_c916_6 = v_id; long long tb_c916_6n = (long long)strlen(tb_c916_6); const char *tb_c916_7 = " - "; long long tb_c916_7n = sizeof(" - ") - 1; const char *tb_c916_8 = v_j; long long tb_c916_8n = (long long)strlen(tb_c916_8); const char *tb_c916_9 = ".tb_lo + 1;\n"; long long tb_c916_9n = sizeof(".tb_lo + 1;\n") - 1; char *tb_c916r = tb_mem(tb_c916_0n + tb_c916_1n + tb_c916_2n + tb_c916_3n + tb_c916_4n + tb_c916_5n + tb_c916_6n + tb_c916_7n + tb_c916_8n + tb_c916_9n + 1), *tb_c916rw = tb_c916r; memcpy(tb_c916rw, tb_c916_0, tb_c916_0n); tb_c916rw += tb_c916_0n; memcpy(tb_c916rw, tb_c916_1, tb_c916_1n); tb_c916rw += tb_c916_1n; memcpy(tb_c916rw, tb_c916_2, tb_c916_2n); tb_c916rw += tb_c916_2n; memcpy(tb_c916rw, tb_c916_3, tb_c916_3n); tb_c916rw += tb_c916_3n; memcpy(tb_c916rw, tb_c916_4, tb_c916_4n); tb_c916rw += tb_c916_4n; memcpy(tb_c916rw, tb_c916_5, tb_c916_5n); tb_c916rw += tb_c916_5n; memcpy(tb_c916rw, tb_c916_6, tb_c916_6n); tb_c916rw += tb_c916_6n; memcpy(tb_c916rw, tb_c916_7, tb_c916_7n); tb_c916rw += tb_c916_7n; memcpy(tb_c916rw, tb_c916_8, tb_c916_8n); tb_c916rw += tb_c916_8n; memcpy(tb_c916rw, tb_c916_9, tb_c916_9n); tb_c916rw += tb_c916_9n; *tb_c916rw = 0; (const char *)tb_c916r; }));
    }
    {
        tb_list * tb_l917 = ((tb_o_Node *)tb_nn(v_n, 871))->q_caps;
        for (long long tb_i917 = 0; tb_i917 < tb_l917->len; tb_i917++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l917->data)[tb_i917];
            f_emit__indent(v_c, (v_depth + 1LL));
            if ((((tb_o_Var *)tb_nn(v_v, 873))->q_is_shared && ({ const char *tb_s918 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 873))->q_type); ((unsigned char)tb_s918[0] == 116 && (unsigned char)tb_s918[1] == 101 && (unsigned char)tb_s918[2] == 120 && (unsigned char)tb_s918[3] == 116 && tb_s918[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c919_0 = v_j; long long tb_c919_0n = (long long)strlen(tb_c919_0); const char *tb_c919_1 = ".m_"; long long tb_c919_1n = sizeof(".m_") - 1; const char *tb_c919_2 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c919_2n = (long long)strlen(tb_c919_2); const char *tb_c919_3 = " = v_"; long long tb_c919_3n = sizeof(" = v_") - 1; const char *tb_c919_4 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c919_4n = (long long)strlen(tb_c919_4); const char *tb_c919_5 = "; "; long long tb_c919_5n = sizeof("; ") - 1; const char *tb_c919_6 = v_j; long long tb_c919_6n = (long long)strlen(tb_c919_6); const char *tb_c919_7 = ".m_rl_"; long long tb_c919_7n = sizeof(".m_rl_") - 1; const char *tb_c919_8 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c919_8n = (long long)strlen(tb_c919_8); const char *tb_c919_9 = " = -1;\n"; long long tb_c919_9n = sizeof(" = -1;\n") - 1; char *tb_c919r = tb_mem(tb_c919_0n + tb_c919_1n + tb_c919_2n + tb_c919_3n + tb_c919_4n + tb_c919_5n + tb_c919_6n + tb_c919_7n + tb_c919_8n + tb_c919_9n + 1), *tb_c919rw = tb_c919r; memcpy(tb_c919rw, tb_c919_0, tb_c919_0n); tb_c919rw += tb_c919_0n; memcpy(tb_c919rw, tb_c919_1, tb_c919_1n); tb_c919rw += tb_c919_1n; memcpy(tb_c919rw, tb_c919_2, tb_c919_2n); tb_c919rw += tb_c919_2n; memcpy(tb_c919rw, tb_c919_3, tb_c919_3n); tb_c919rw += tb_c919_3n; memcpy(tb_c919rw, tb_c919_4, tb_c919_4n); tb_c919rw += tb_c919_4n; memcpy(tb_c919rw, tb_c919_5, tb_c919_5n); tb_c919rw += tb_c919_5n; memcpy(tb_c919rw, tb_c919_6, tb_c919_6n); tb_c919rw += tb_c919_6n; memcpy(tb_c919rw, tb_c919_7, tb_c919_7n); tb_c919rw += tb_c919_7n; memcpy(tb_c919rw, tb_c919_8, tb_c919_8n); tb_c919rw += tb_c919_8n; memcpy(tb_c919rw, tb_c919_9, tb_c919_9n); tb_c919rw += tb_c919_9n; *tb_c919rw = 0; (const char *)tb_c919r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 875))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c920_0 = v_j; long long tb_c920_0n = (long long)strlen(tb_c920_0); const char *tb_c920_1 = ".p_"; long long tb_c920_1n = sizeof(".p_") - 1; const char *tb_c920_2 = ((tb_o_Var *)tb_nn(v_v, 876))->q_name; long long tb_c920_2n = (long long)strlen(tb_c920_2); const char *tb_c920_3 = " = &v_"; long long tb_c920_3n = sizeof(" = &v_") - 1; const char *tb_c920_4 = ((tb_o_Var *)tb_nn(v_v, 876))->q_name; long long tb_c920_4n = (long long)strlen(tb_c920_4); const char *tb_c920_5 = ";\n"; long long tb_c920_5n = sizeof(";\n") - 1; char *tb_c920r = tb_mem(tb_c920_0n + tb_c920_1n + tb_c920_2n + tb_c920_3n + tb_c920_4n + tb_c920_5n + 1), *tb_c920rw = tb_c920r; memcpy(tb_c920rw, tb_c920_0, tb_c920_0n); tb_c920rw += tb_c920_0n; memcpy(tb_c920rw, tb_c920_1, tb_c920_1n); tb_c920rw += tb_c920_1n; memcpy(tb_c920rw, tb_c920_2, tb_c920_2n); tb_c920rw += tb_c920_2n; memcpy(tb_c920rw, tb_c920_3, tb_c920_3n); tb_c920rw += tb_c920_3n; memcpy(tb_c920rw, tb_c920_4, tb_c920_4n); tb_c920rw += tb_c920_4n; memcpy(tb_c920rw, tb_c920_5, tb_c920_5n); tb_c920rw += tb_c920_5n; *tb_c920rw = 0; (const char *)tb_c920r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c921_0 = v_j; long long tb_c921_0n = (long long)strlen(tb_c921_0); const char *tb_c921_1 = ".v_"; long long tb_c921_1n = sizeof(".v_") - 1; const char *tb_c921_2 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c921_2n = (long long)strlen(tb_c921_2); const char *tb_c921_3 = " = v_"; long long tb_c921_3n = sizeof(" = v_") - 1; const char *tb_c921_4 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c921_4n = (long long)strlen(tb_c921_4); const char *tb_c921_5 = ";\n"; long long tb_c921_5n = sizeof(";\n") - 1; char *tb_c921r = tb_mem(tb_c921_0n + tb_c921_1n + tb_c921_2n + tb_c921_3n + tb_c921_4n + tb_c921_5n + 1), *tb_c921rw = tb_c921r; memcpy(tb_c921rw, tb_c921_0, tb_c921_0n); tb_c921rw += tb_c921_0n; memcpy(tb_c921rw, tb_c921_1, tb_c921_1n); tb_c921rw += tb_c921_1n; memcpy(tb_c921rw, tb_c921_2, tb_c921_2n); tb_c921rw += tb_c921_2n; memcpy(tb_c921rw, tb_c921_3, tb_c921_3n); tb_c921rw += tb_c921_3n; memcpy(tb_c921rw, tb_c921_4, tb_c921_4n); tb_c921rw += tb_c921_4n; memcpy(tb_c921rw, tb_c921_5, tb_c921_5n); tb_c921rw += tb_c921_5n; *tb_c921rw = 0; (const char *)tb_c921r; }));
            }
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c922_0 = v_j; long long tb_c922_0n = (long long)strlen(tb_c922_0); const char *tb_c922_1 = ".tb_step = tb_par_step("; long long tb_c922_1n = sizeof(".tb_step = tb_par_step(") - 1; const char *tb_c922_2 = v_j; long long tb_c922_2n = (long long)strlen(tb_c922_2); const char *tb_c922_3 = ".tb_total);\n"; long long tb_c922_3n = sizeof(".tb_total);\n") - 1; char *tb_c922r = tb_mem(tb_c922_0n + tb_c922_1n + tb_c922_2n + tb_c922_3n + 1), *tb_c922rw = tb_c922r; memcpy(tb_c922rw, tb_c922_0, tb_c922_0n); tb_c922rw += tb_c922_0n; memcpy(tb_c922rw, tb_c922_1, tb_c922_1n); tb_c922rw += tb_c922_1n; memcpy(tb_c922rw, tb_c922_2, tb_c922_2n); tb_c922rw += tb_c922_2n; memcpy(tb_c922rw, tb_c922_3, tb_c922_3n); tb_c922rw += tb_c922_3n; *tb_c922rw = 0; (const char *)tb_c922r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c923_0 = "tb_par_run(tb_work"; long long tb_c923_0n = sizeof("tb_par_run(tb_work") - 1; const char *tb_c923_1 = v_id; long long tb_c923_1n = (long long)strlen(tb_c923_1); const char *tb_c923_2 = ", &"; long long tb_c923_2n = sizeof(", &") - 1; const char *tb_c923_3 = v_j; long long tb_c923_3n = (long long)strlen(tb_c923_3); const char *tb_c923_4 = ", "; long long tb_c923_4n = sizeof(", ") - 1; const char *tb_c923_5 = v_j; long long tb_c923_5n = (long long)strlen(tb_c923_5); const char *tb_c923_6 = ".tb_total);\n"; long long tb_c923_6n = sizeof(".tb_total);\n") - 1; char *tb_c923r = tb_mem(tb_c923_0n + tb_c923_1n + tb_c923_2n + tb_c923_3n + tb_c923_4n + tb_c923_5n + tb_c923_6n + 1), *tb_c923rw = tb_c923r; memcpy(tb_c923rw, tb_c923_0, tb_c923_0n); tb_c923rw += tb_c923_0n; memcpy(tb_c923rw, tb_c923_1, tb_c923_1n); tb_c923rw += tb_c923_1n; memcpy(tb_c923rw, tb_c923_2, tb_c923_2n); tb_c923rw += tb_c923_2n; memcpy(tb_c923rw, tb_c923_3, tb_c923_3n); tb_c923rw += tb_c923_3n; memcpy(tb_c923rw, tb_c923_4, tb_c923_4n); tb_c923rw += tb_c923_4n; memcpy(tb_c923rw, tb_c923_5, tb_c923_5n); tb_c923rw += tb_c923_5n; memcpy(tb_c923rw, tb_c923_6, tb_c923_6n); tb_c923rw += tb_c923_6n; *tb_c923rw = 0; (const char *)tb_c923r; }));
    {
        tb_list * tb_l924 = ((tb_o_Node *)tb_nn(v_n, 883))->q_caps;
        for (long long tb_i924 = 0; tb_i924 < tb_l924->len; tb_i924++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l924->data)[tb_i924];
            if ((((tb_o_Var *)tb_nn(v_v, 884))->q_is_shared && ({ const char *tb_s925 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 884))->q_type); ((unsigned char)tb_s925[0] == 116 && (unsigned char)tb_s925[1] == 101 && (unsigned char)tb_s925[2] == 120 && (unsigned char)tb_s925[3] == 116 && tb_s925[4] == 0); }))) {
                f_emit__indent(v_c, (v_depth + 1LL));
                f_emit__put(v_c, ({ const char *tb_c926_0 = "v_"; long long tb_c926_0n = sizeof("v_") - 1; const char *tb_c926_1 = ((tb_o_Var *)tb_nn(v_v, 886))->q_name; long long tb_c926_1n = (long long)strlen(tb_c926_1); const char *tb_c926_2 = " = "; long long tb_c926_2n = sizeof(" = ") - 1; const char *tb_c926_3 = v_j; long long tb_c926_3n = (long long)strlen(tb_c926_3); const char *tb_c926_4 = ".m_"; long long tb_c926_4n = sizeof(".m_") - 1; const char *tb_c926_5 = ((tb_o_Var *)tb_nn(v_v, 886))->q_name; long long tb_c926_5n = (long long)strlen(tb_c926_5); const char *tb_c926_6 = ";\n"; long long tb_c926_6n = sizeof(";\n") - 1; char *tb_c926r = tb_mem(tb_c926_0n + tb_c926_1n + tb_c926_2n + tb_c926_3n + tb_c926_4n + tb_c926_5n + tb_c926_6n + 1), *tb_c926rw = tb_c926r; memcpy(tb_c926rw, tb_c926_0, tb_c926_0n); tb_c926rw += tb_c926_0n; memcpy(tb_c926rw, tb_c926_1, tb_c926_1n); tb_c926rw += tb_c926_1n; memcpy(tb_c926rw, tb_c926_2, tb_c926_2n); tb_c926rw += tb_c926_2n; memcpy(tb_c926rw, tb_c926_3, tb_c926_3n); tb_c926rw += tb_c926_3n; memcpy(tb_c926rw, tb_c926_4, tb_c926_4n); tb_c926rw += tb_c926_4n; memcpy(tb_c926rw, tb_c926_5, tb_c926_5n); tb_c926rw += tb_c926_5n; memcpy(tb_c926rw, tb_c926_6, tb_c926_6n); tb_c926rw += tb_c926_6n; *tb_c926rw = 0; (const char *)tb_c926r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end927 = ((((tb_o_Comp *)tb_nn(v_c, 891))->q_vec_names)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end927; v_i++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 892))->q_vec_names, v_i, sizeof(const char *), 892)), v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(890, "функция vec_index дошла до конца без return");
    return 0LL;
}

static void f_emit__emit_vec(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v928 = 1LL; long long *tb_p928 = &(((tb_o_Comp *)tb_nn(v_c, 899))->q_label); *tb_p928 += tb_v928; }
    const char * v_id = ({ const char *tb_c929_0 = ""; long long tb_c929_0n = sizeof("") - 1; char tb_c929_1d[24]; long long tb_c929_1n; const char *tb_c929_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 900))->q_label, tb_c929_1d, &tb_c929_1n); char *tb_c929r = tb_mem(tb_c929_0n + tb_c929_1n + 1), *tb_c929rw = tb_c929r; memcpy(tb_c929rw, tb_c929_0, tb_c929_0n); tb_c929rw += tb_c929_0n; memcpy(tb_c929rw, tb_c929_1, tb_c929_1n); tb_c929rw += tb_c929_1n; *tb_c929rw = 0; (const char *)tb_c929r; });
    const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 901))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 901))->q_kids, 0LL, sizeof(tb_o_Node *), 901)), 901))->q_ty, sizeof(tb_o_Type *), 901)), 901))->q_elem);
    long long v_d = (v_depth + 1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c930_0 = "tb_list *tb_l"; long long tb_c930_0n = sizeof("tb_list *tb_l") - 1; const char *tb_c930_1 = v_id; long long tb_c930_1n = (long long)strlen(tb_c930_1); const char *tb_c930_2 = " = "; long long tb_c930_2n = sizeof(" = ") - 1; char *tb_c930r = tb_mem(tb_c930_0n + tb_c930_1n + tb_c930_2n + 1), *tb_c930rw = tb_c930r; memcpy(tb_c930rw, tb_c930_0, tb_c930_0n); tb_c930rw += tb_c930_0n; memcpy(tb_c930rw, tb_c930_1, tb_c930_1n); tb_c930rw += tb_c930_1n; memcpy(tb_c930rw, tb_c930_2, tb_c930_2n); tb_c930rw += tb_c930_2n; *tb_c930rw = 0; (const char *)tb_c930r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 907))->q_kids, 0LL, sizeof(tb_o_Node *), 907)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c931_0 = v_elem; long long tb_c931_0n = (long long)strlen(tb_c931_0); const char *tb_c931_1 = " *tb_d"; long long tb_c931_1n = sizeof(" *tb_d") - 1; const char *tb_c931_2 = v_id; long long tb_c931_2n = (long long)strlen(tb_c931_2); const char *tb_c931_3 = " = ("; long long tb_c931_3n = sizeof(" = (") - 1; const char *tb_c931_4 = v_elem; long long tb_c931_4n = (long long)strlen(tb_c931_4); const char *tb_c931_5 = " *)tb_l"; long long tb_c931_5n = sizeof(" *)tb_l") - 1; const char *tb_c931_6 = v_id; long long tb_c931_6n = (long long)strlen(tb_c931_6); const char *tb_c931_7 = "->data;\n"; long long tb_c931_7n = sizeof("->data;\n") - 1; char *tb_c931r = tb_mem(tb_c931_0n + tb_c931_1n + tb_c931_2n + tb_c931_3n + tb_c931_4n + tb_c931_5n + tb_c931_6n + tb_c931_7n + 1), *tb_c931rw = tb_c931r; memcpy(tb_c931rw, tb_c931_0, tb_c931_0n); tb_c931rw += tb_c931_0n; memcpy(tb_c931rw, tb_c931_1, tb_c931_1n); tb_c931rw += tb_c931_1n; memcpy(tb_c931rw, tb_c931_2, tb_c931_2n); tb_c931rw += tb_c931_2n; memcpy(tb_c931rw, tb_c931_3, tb_c931_3n); tb_c931rw += tb_c931_3n; memcpy(tb_c931rw, tb_c931_4, tb_c931_4n); tb_c931rw += tb_c931_4n; memcpy(tb_c931rw, tb_c931_5, tb_c931_5n); tb_c931rw += tb_c931_5n; memcpy(tb_c931rw, tb_c931_6, tb_c931_6n); tb_c931rw += tb_c931_6n; memcpy(tb_c931rw, tb_c931_7, tb_c931_7n); tb_c931rw += tb_c931_7n; *tb_c931rw = 0; (const char *)tb_c931r; }));
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c932_0 = "long long tb_n"; long long tb_c932_0n = sizeof("long long tb_n") - 1; const char *tb_c932_1 = v_id; long long tb_c932_1n = (long long)strlen(tb_c932_1); const char *tb_c932_2 = " = tb_l"; long long tb_c932_2n = sizeof(" = tb_l") - 1; const char *tb_c932_3 = v_id; long long tb_c932_3n = (long long)strlen(tb_c932_3); const char *tb_c932_4 = "->len, tb_i"; long long tb_c932_4n = sizeof("->len, tb_i") - 1; const char *tb_c932_5 = v_id; long long tb_c932_5n = (long long)strlen(tb_c932_5); const char *tb_c932_6 = " = 0;\n"; long long tb_c932_6n = sizeof(" = 0;\n") - 1; char *tb_c932r = tb_mem(tb_c932_0n + tb_c932_1n + tb_c932_2n + tb_c932_3n + tb_c932_4n + tb_c932_5n + tb_c932_6n + 1), *tb_c932rw = tb_c932r; memcpy(tb_c932rw, tb_c932_0, tb_c932_0n); tb_c932rw += tb_c932_0n; memcpy(tb_c932rw, tb_c932_1, tb_c932_1n); tb_c932rw += tb_c932_1n; memcpy(tb_c932rw, tb_c932_2, tb_c932_2n); tb_c932rw += tb_c932_2n; memcpy(tb_c932rw, tb_c932_3, tb_c932_3n); tb_c932rw += tb_c932_3n; memcpy(tb_c932rw, tb_c932_4, tb_c932_4n); tb_c932rw += tb_c932_4n; memcpy(tb_c932rw, tb_c932_5, tb_c932_5n); tb_c932rw += tb_c932_5n; memcpy(tb_c932rw, tb_c932_6, tb_c932_6n); tb_c932rw += tb_c932_6n; *tb_c932rw = 0; (const char *)tb_c932r; }));
    {
        tb_list * tb_l933 = ((tb_o_Node *)tb_nn(v_n, 915))->q_caps;
        for (long long tb_i933 = 0; tb_i933 < tb_l933->len; tb_i933++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l933->data)[tb_i933];
            if (((tb_o_Var *)tb_nn(v_v, 916))->q_is_shared) {
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c934_0 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 918))->q_type); long long tb_c934_0n = (long long)strlen(tb_c934_0); const char *tb_c934_1 = " "; long long tb_c934_1n = sizeof(" ") - 1; char *tb_c934r = tb_mem(tb_c934_0n + tb_c934_1n + 1), *tb_c934rw = tb_c934r; memcpy(tb_c934rw, tb_c934_0, tb_c934_0n); tb_c934rw += tb_c934_0n; memcpy(tb_c934rw, tb_c934_1, tb_c934_1n); tb_c934rw += tb_c934_1n; *tb_c934rw = 0; (const char *)tb_c934r; }));
                {
                    long long tb_end935 = 7LL;
                    for (long long v_k = 0LL; v_k <= tb_end935; v_k++) {
                        if ((v_k > 0LL)) {
                            f_emit__put(v_c, ", ");
                        }
                        f_emit__put(v_c, ({ const char *tb_c936_0 = "tb_a"; long long tb_c936_0n = sizeof("tb_a") - 1; const char *tb_c936_1 = v_id; long long tb_c936_1n = (long long)strlen(tb_c936_1); const char *tb_c936_2 = "_"; long long tb_c936_2n = sizeof("_") - 1; const char *tb_c936_3 = ((tb_o_Var *)tb_nn(v_v, 922))->q_name; long long tb_c936_3n = (long long)strlen(tb_c936_3); const char *tb_c936_4 = "_"; long long tb_c936_4n = sizeof("_") - 1; char tb_c936_5d[24]; long long tb_c936_5n; const char *tb_c936_5 = tb_digits(v_k, tb_c936_5d, &tb_c936_5n); const char *tb_c936_6 = " = 0"; long long tb_c936_6n = sizeof(" = 0") - 1; char *tb_c936r = tb_mem(tb_c936_0n + tb_c936_1n + tb_c936_2n + tb_c936_3n + tb_c936_4n + tb_c936_5n + tb_c936_6n + 1), *tb_c936rw = tb_c936r; memcpy(tb_c936rw, tb_c936_0, tb_c936_0n); tb_c936rw += tb_c936_0n; memcpy(tb_c936rw, tb_c936_1, tb_c936_1n); tb_c936rw += tb_c936_1n; memcpy(tb_c936rw, tb_c936_2, tb_c936_2n); tb_c936rw += tb_c936_2n; memcpy(tb_c936rw, tb_c936_3, tb_c936_3n); tb_c936rw += tb_c936_3n; memcpy(tb_c936rw, tb_c936_4, tb_c936_4n); tb_c936rw += tb_c936_4n; memcpy(tb_c936rw, tb_c936_5, tb_c936_5n); tb_c936rw += tb_c936_5n; memcpy(tb_c936rw, tb_c936_6, tb_c936_6n); tb_c936rw += tb_c936_6n; *tb_c936rw = 0; (const char *)tb_c936r; }));
                    }
                }
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c937_0 = "for (; tb_i"; long long tb_c937_0n = sizeof("for (; tb_i") - 1; const char *tb_c937_1 = v_id; long long tb_c937_1n = (long long)strlen(tb_c937_1); const char *tb_c937_2 = " + 8 <= tb_n"; long long tb_c937_2n = sizeof(" + 8 <= tb_n") - 1; const char *tb_c937_3 = v_id; long long tb_c937_3n = (long long)strlen(tb_c937_3); const char *tb_c937_4 = "; tb_i"; long long tb_c937_4n = sizeof("; tb_i") - 1; const char *tb_c937_5 = v_id; long long tb_c937_5n = (long long)strlen(tb_c937_5); const char *tb_c937_6 = " += 8) {\n"; long long tb_c937_6n = sizeof(" += 8) {\n") - 1; char *tb_c937r = tb_mem(tb_c937_0n + tb_c937_1n + tb_c937_2n + tb_c937_3n + tb_c937_4n + tb_c937_5n + tb_c937_6n + 1), *tb_c937rw = tb_c937r; memcpy(tb_c937rw, tb_c937_0, tb_c937_0n); tb_c937rw += tb_c937_0n; memcpy(tb_c937rw, tb_c937_1, tb_c937_1n); tb_c937rw += tb_c937_1n; memcpy(tb_c937rw, tb_c937_2, tb_c937_2n); tb_c937rw += tb_c937_2n; memcpy(tb_c937rw, tb_c937_3, tb_c937_3n); tb_c937rw += tb_c937_3n; memcpy(tb_c937rw, tb_c937_4, tb_c937_4n); tb_c937rw += tb_c937_4n; memcpy(tb_c937rw, tb_c937_5, tb_c937_5n); tb_c937rw += tb_c937_5n; memcpy(tb_c937rw, tb_c937_6, tb_c937_6n); tb_c937rw += tb_c937_6n; *tb_c937rw = 0; (const char *)tb_c937r; }));
    {
        long long tb_end938 = 7LL;
        for (long long v_k = 0LL; v_k <= tb_end938; v_k++) {
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "do {\n");
            f_emit__indent(v_c, (v_d + 2LL));
            f_emit__put(v_c, ({ const char *tb_c939_0 = v_elem; long long tb_c939_0n = (long long)strlen(tb_c939_0); const char *tb_c939_1 = " const v_"; long long tb_c939_1n = sizeof(" const v_") - 1; const char *tb_c939_2 = ((tb_o_Node *)tb_nn(v_n, 932))->q_name; long long tb_c939_2n = (long long)strlen(tb_c939_2); const char *tb_c939_3 = " = tb_d"; long long tb_c939_3n = sizeof(" = tb_d") - 1; const char *tb_c939_4 = v_id; long long tb_c939_4n = (long long)strlen(tb_c939_4); const char *tb_c939_5 = "[tb_i"; long long tb_c939_5n = sizeof("[tb_i") - 1; const char *tb_c939_6 = v_id; long long tb_c939_6n = (long long)strlen(tb_c939_6); const char *tb_c939_7 = " + "; long long tb_c939_7n = sizeof(" + ") - 1; char tb_c939_8d[24]; long long tb_c939_8n; const char *tb_c939_8 = tb_digits(v_k, tb_c939_8d, &tb_c939_8n); const char *tb_c939_9 = "];\n"; long long tb_c939_9n = sizeof("];\n") - 1; char *tb_c939r = tb_mem(tb_c939_0n + tb_c939_1n + tb_c939_2n + tb_c939_3n + tb_c939_4n + tb_c939_5n + tb_c939_6n + tb_c939_7n + tb_c939_8n + tb_c939_9n + 1), *tb_c939rw = tb_c939r; memcpy(tb_c939rw, tb_c939_0, tb_c939_0n); tb_c939rw += tb_c939_0n; memcpy(tb_c939rw, tb_c939_1, tb_c939_1n); tb_c939rw += tb_c939_1n; memcpy(tb_c939rw, tb_c939_2, tb_c939_2n); tb_c939rw += tb_c939_2n; memcpy(tb_c939rw, tb_c939_3, tb_c939_3n); tb_c939rw += tb_c939_3n; memcpy(tb_c939rw, tb_c939_4, tb_c939_4n); tb_c939rw += tb_c939_4n; memcpy(tb_c939rw, tb_c939_5, tb_c939_5n); tb_c939rw += tb_c939_5n; memcpy(tb_c939rw, tb_c939_6, tb_c939_6n); tb_c939rw += tb_c939_6n; memcpy(tb_c939rw, tb_c939_7, tb_c939_7n); tb_c939rw += tb_c939_7n; memcpy(tb_c939rw, tb_c939_8, tb_c939_8n); tb_c939rw += tb_c939_8n; memcpy(tb_c939rw, tb_c939_9, tb_c939_9n); tb_c939rw += tb_c939_9n; *tb_c939rw = 0; (const char *)tb_c939r; }));
            {
                tb_list * tb_l940 = ((tb_o_Node *)tb_nn(v_n, 933))->q_caps;
                for (long long tb_i940 = 0; tb_i940 < tb_l940->len; tb_i940++) {
                    tb_o_Var * const v_v = ((tb_o_Var * *)tb_l940->data)[tb_i940];
                    if (((tb_o_Var *)tb_nn(v_v, 934))->q_is_shared) {
                        { const char * tb_v941 = ((tb_o_Var *)tb_nn(v_v, 935))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 935))->q_vec_names) = tb_v941; }
                        { const char * tb_v942 = ({ const char *tb_c943_0 = "tb_a"; long long tb_c943_0n = sizeof("tb_a") - 1; const char *tb_c943_1 = v_id; long long tb_c943_1n = (long long)strlen(tb_c943_1); const char *tb_c943_2 = "_"; long long tb_c943_2n = sizeof("_") - 1; const char *tb_c943_3 = ((tb_o_Var *)tb_nn(v_v, 936))->q_name; long long tb_c943_3n = (long long)strlen(tb_c943_3); const char *tb_c943_4 = "_"; long long tb_c943_4n = sizeof("_") - 1; char tb_c943_5d[24]; long long tb_c943_5n; const char *tb_c943_5 = tb_digits(v_k, tb_c943_5d, &tb_c943_5n); char *tb_c943r = tb_mem(tb_c943_0n + tb_c943_1n + tb_c943_2n + tb_c943_3n + tb_c943_4n + tb_c943_5n + 1), *tb_c943rw = tb_c943r; memcpy(tb_c943rw, tb_c943_0, tb_c943_0n); tb_c943rw += tb_c943_0n; memcpy(tb_c943rw, tb_c943_1, tb_c943_1n); tb_c943rw += tb_c943_1n; memcpy(tb_c943rw, tb_c943_2, tb_c943_2n); tb_c943rw += tb_c943_2n; memcpy(tb_c943rw, tb_c943_3, tb_c943_3n); tb_c943rw += tb_c943_3n; memcpy(tb_c943rw, tb_c943_4, tb_c943_4n); tb_c943rw += tb_c943_4n; memcpy(tb_c943rw, tb_c943_5, tb_c943_5n); tb_c943rw += tb_c943_5n; *tb_c943rw = 0; (const char *)tb_c943r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 936))->q_vec_repl) = tb_v942; }
                    }
                }
            }
            f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 937))->q_kids, 1LL, sizeof(tb_o_Node *), 937)), (v_d + 2LL));
            tb_list * v_fresh_names = tb_list_new(sizeof(const char *));
            tb_list * v_fresh_repl = tb_list_new(sizeof(const char *));
            { tb_list * tb_v944 = v_fresh_names; tb_list * *tb_p944 = &(((tb_o_Comp *)tb_nn(v_c, 940))->q_vec_names); *tb_p944 = tb_v944; }
            { tb_list * tb_v945 = v_fresh_repl; tb_list * *tb_p945 = &(((tb_o_Comp *)tb_nn(v_c, 941))->q_vec_repl); *tb_p945 = tb_v945; }
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "} while (0);\n");
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c946_0 = "for (; tb_i"; long long tb_c946_0n = sizeof("for (; tb_i") - 1; const char *tb_c946_1 = v_id; long long tb_c946_1n = (long long)strlen(tb_c946_1); const char *tb_c946_2 = " < tb_n"; long long tb_c946_2n = sizeof(" < tb_n") - 1; const char *tb_c946_3 = v_id; long long tb_c946_3n = (long long)strlen(tb_c946_3); const char *tb_c946_4 = "; tb_i"; long long tb_c946_4n = sizeof("; tb_i") - 1; const char *tb_c946_5 = v_id; long long tb_c946_5n = (long long)strlen(tb_c946_5); const char *tb_c946_6 = "++) {\n"; long long tb_c946_6n = sizeof("++) {\n") - 1; char *tb_c946r = tb_mem(tb_c946_0n + tb_c946_1n + tb_c946_2n + tb_c946_3n + tb_c946_4n + tb_c946_5n + tb_c946_6n + 1), *tb_c946rw = tb_c946r; memcpy(tb_c946rw, tb_c946_0, tb_c946_0n); tb_c946rw += tb_c946_0n; memcpy(tb_c946rw, tb_c946_1, tb_c946_1n); tb_c946rw += tb_c946_1n; memcpy(tb_c946rw, tb_c946_2, tb_c946_2n); tb_c946rw += tb_c946_2n; memcpy(tb_c946rw, tb_c946_3, tb_c946_3n); tb_c946rw += tb_c946_3n; memcpy(tb_c946rw, tb_c946_4, tb_c946_4n); tb_c946rw += tb_c946_4n; memcpy(tb_c946rw, tb_c946_5, tb_c946_5n); tb_c946rw += tb_c946_5n; memcpy(tb_c946rw, tb_c946_6, tb_c946_6n); tb_c946rw += tb_c946_6n; *tb_c946rw = 0; (const char *)tb_c946r; }));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, ({ const char *tb_c947_0 = v_elem; long long tb_c947_0n = (long long)strlen(tb_c947_0); const char *tb_c947_1 = " const v_"; long long tb_c947_1n = sizeof(" const v_") - 1; const char *tb_c947_2 = ((tb_o_Node *)tb_nn(v_n, 950))->q_name; long long tb_c947_2n = (long long)strlen(tb_c947_2); const char *tb_c947_3 = " = tb_d"; long long tb_c947_3n = sizeof(" = tb_d") - 1; const char *tb_c947_4 = v_id; long long tb_c947_4n = (long long)strlen(tb_c947_4); const char *tb_c947_5 = "[tb_i"; long long tb_c947_5n = sizeof("[tb_i") - 1; const char *tb_c947_6 = v_id; long long tb_c947_6n = (long long)strlen(tb_c947_6); const char *tb_c947_7 = "];\n"; long long tb_c947_7n = sizeof("];\n") - 1; char *tb_c947r = tb_mem(tb_c947_0n + tb_c947_1n + tb_c947_2n + tb_c947_3n + tb_c947_4n + tb_c947_5n + tb_c947_6n + tb_c947_7n + 1), *tb_c947rw = tb_c947r; memcpy(tb_c947rw, tb_c947_0, tb_c947_0n); tb_c947rw += tb_c947_0n; memcpy(tb_c947rw, tb_c947_1, tb_c947_1n); tb_c947rw += tb_c947_1n; memcpy(tb_c947rw, tb_c947_2, tb_c947_2n); tb_c947rw += tb_c947_2n; memcpy(tb_c947rw, tb_c947_3, tb_c947_3n); tb_c947rw += tb_c947_3n; memcpy(tb_c947rw, tb_c947_4, tb_c947_4n); tb_c947rw += tb_c947_4n; memcpy(tb_c947rw, tb_c947_5, tb_c947_5n); tb_c947rw += tb_c947_5n; memcpy(tb_c947rw, tb_c947_6, tb_c947_6n); tb_c947rw += tb_c947_6n; memcpy(tb_c947rw, tb_c947_7, tb_c947_7n); tb_c947rw += tb_c947_7n; *tb_c947rw = 0; (const char *)tb_c947r; }));
    {
        tb_list * tb_l948 = ((tb_o_Node *)tb_nn(v_n, 951))->q_caps;
        for (long long tb_i948 = 0; tb_i948 < tb_l948->len; tb_i948++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l948->data)[tb_i948];
            if (((tb_o_Var *)tb_nn(v_v, 952))->q_is_shared) {
                { const char * tb_v949 = ((tb_o_Var *)tb_nn(v_v, 953))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 953))->q_vec_names) = tb_v949; }
                { const char * tb_v950 = ({ const char *tb_c951_0 = "tb_a"; long long tb_c951_0n = sizeof("tb_a") - 1; const char *tb_c951_1 = v_id; long long tb_c951_1n = (long long)strlen(tb_c951_1); const char *tb_c951_2 = "_"; long long tb_c951_2n = sizeof("_") - 1; const char *tb_c951_3 = ((tb_o_Var *)tb_nn(v_v, 954))->q_name; long long tb_c951_3n = (long long)strlen(tb_c951_3); const char *tb_c951_4 = "_0"; long long tb_c951_4n = sizeof("_0") - 1; char *tb_c951r = tb_mem(tb_c951_0n + tb_c951_1n + tb_c951_2n + tb_c951_3n + tb_c951_4n + 1), *tb_c951rw = tb_c951r; memcpy(tb_c951rw, tb_c951_0, tb_c951_0n); tb_c951rw += tb_c951_0n; memcpy(tb_c951rw, tb_c951_1, tb_c951_1n); tb_c951rw += tb_c951_1n; memcpy(tb_c951rw, tb_c951_2, tb_c951_2n); tb_c951rw += tb_c951_2n; memcpy(tb_c951rw, tb_c951_3, tb_c951_3n); tb_c951rw += tb_c951_3n; memcpy(tb_c951rw, tb_c951_4, tb_c951_4n); tb_c951rw += tb_c951_4n; *tb_c951rw = 0; (const char *)tb_c951r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 954))->q_vec_repl) = tb_v950; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 955))->q_kids, 1LL, sizeof(tb_o_Node *), 955)), (v_d + 1LL));
    tb_list * v_fresh_names2 = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl2 = tb_list_new(sizeof(const char *));
    { tb_list * tb_v952 = v_fresh_names2; tb_list * *tb_p952 = &(((tb_o_Comp *)tb_nn(v_c, 958))->q_vec_names); *tb_p952 = tb_v952; }
    { tb_list * tb_v953 = v_fresh_repl2; tb_list * *tb_p953 = &(((tb_o_Comp *)tb_nn(v_c, 959))->q_vec_repl); *tb_p953 = tb_v953; }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    {
        tb_list * tb_l954 = ((tb_o_Node *)tb_nn(v_n, 963))->q_caps;
        for (long long tb_i954 = 0; tb_i954 < tb_l954->len; tb_i954++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l954->data)[tb_i954];
            if (((tb_o_Var *)tb_nn(v_v, 964))->q_is_shared) {
                const char * v_target = ({ const char *tb_c955_0 = "v_"; long long tb_c955_0n = sizeof("v_") - 1; const char *tb_c955_1 = ((tb_o_Var *)tb_nn(v_v, 965))->q_name; long long tb_c955_1n = (long long)strlen(tb_c955_1); char *tb_c955r = tb_mem(tb_c955_0n + tb_c955_1n + 1), *tb_c955rw = tb_c955r; memcpy(tb_c955rw, tb_c955_0, tb_c955_0n); tb_c955rw += tb_c955_0n; memcpy(tb_c955rw, tb_c955_1, tb_c955_1n); tb_c955rw += tb_c955_1n; *tb_c955rw = 0; (const char *)tb_c955r; });
                if (((tb_o_Var *)tb_nn(v_v, 966))->q_is_link) {
                    v_target = ({ const char *tb_c956_0 = "(*v_"; long long tb_c956_0n = sizeof("(*v_") - 1; const char *tb_c956_1 = ((tb_o_Var *)tb_nn(v_v, 967))->q_name; long long tb_c956_1n = (long long)strlen(tb_c956_1); const char *tb_c956_2 = ")"; long long tb_c956_2n = sizeof(")") - 1; char *tb_c956r = tb_mem(tb_c956_0n + tb_c956_1n + tb_c956_2n + 1), *tb_c956rw = tb_c956r; memcpy(tb_c956rw, tb_c956_0, tb_c956_0n); tb_c956rw += tb_c956_0n; memcpy(tb_c956rw, tb_c956_1, tb_c956_1n); tb_c956rw += tb_c956_1n; memcpy(tb_c956rw, tb_c956_2, tb_c956_2n); tb_c956rw += tb_c956_2n; *tb_c956rw = 0; (const char *)tb_c956r; });
                }
                const char * v_a = ({ const char *tb_c957_0 = "tb_a"; long long tb_c957_0n = sizeof("tb_a") - 1; const char *tb_c957_1 = v_id; long long tb_c957_1n = (long long)strlen(tb_c957_1); const char *tb_c957_2 = "_"; long long tb_c957_2n = sizeof("_") - 1; const char *tb_c957_3 = ((tb_o_Var *)tb_nn(v_v, 968))->q_name; long long tb_c957_3n = (long long)strlen(tb_c957_3); const char *tb_c957_4 = "_"; long long tb_c957_4n = sizeof("_") - 1; char *tb_c957r = tb_mem(tb_c957_0n + tb_c957_1n + tb_c957_2n + tb_c957_3n + tb_c957_4n + 1), *tb_c957rw = tb_c957r; memcpy(tb_c957rw, tb_c957_0, tb_c957_0n); tb_c957rw += tb_c957_0n; memcpy(tb_c957rw, tb_c957_1, tb_c957_1n); tb_c957rw += tb_c957_1n; memcpy(tb_c957rw, tb_c957_2, tb_c957_2n); tb_c957rw += tb_c957_2n; memcpy(tb_c957rw, tb_c957_3, tb_c957_3n); tb_c957rw += tb_c957_3n; memcpy(tb_c957rw, tb_c957_4, tb_c957_4n); tb_c957rw += tb_c957_4n; *tb_c957rw = 0; (const char *)tb_c957r; });
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c958_0 = v_target; long long tb_c958_0n = (long long)strlen(tb_c958_0); const char *tb_c958_1 = " += (("; long long tb_c958_1n = sizeof(" += ((") - 1; const char *tb_c958_2 = v_a; long long tb_c958_2n = (long long)strlen(tb_c958_2); const char *tb_c958_3 = "0 + "; long long tb_c958_3n = sizeof("0 + ") - 1; const char *tb_c958_4 = v_a; long long tb_c958_4n = (long long)strlen(tb_c958_4); const char *tb_c958_5 = "1) + ("; long long tb_c958_5n = sizeof("1) + (") - 1; const char *tb_c958_6 = v_a; long long tb_c958_6n = (long long)strlen(tb_c958_6); const char *tb_c958_7 = "2 + "; long long tb_c958_7n = sizeof("2 + ") - 1; const char *tb_c958_8 = v_a; long long tb_c958_8n = (long long)strlen(tb_c958_8); const char *tb_c958_9 = "3)) + (("; long long tb_c958_9n = sizeof("3)) + ((") - 1; const char *tb_c958_10 = v_a; long long tb_c958_10n = (long long)strlen(tb_c958_10); const char *tb_c958_11 = "4 + "; long long tb_c958_11n = sizeof("4 + ") - 1; const char *tb_c958_12 = v_a; long long tb_c958_12n = (long long)strlen(tb_c958_12); const char *tb_c958_13 = "5) + ("; long long tb_c958_13n = sizeof("5) + (") - 1; const char *tb_c958_14 = v_a; long long tb_c958_14n = (long long)strlen(tb_c958_14); const char *tb_c958_15 = "6 + "; long long tb_c958_15n = sizeof("6 + ") - 1; const char *tb_c958_16 = v_a; long long tb_c958_16n = (long long)strlen(tb_c958_16); const char *tb_c958_17 = "7));\n"; long long tb_c958_17n = sizeof("7));\n") - 1; char *tb_c958r = tb_mem(tb_c958_0n + tb_c958_1n + tb_c958_2n + tb_c958_3n + tb_c958_4n + tb_c958_5n + tb_c958_6n + tb_c958_7n + tb_c958_8n + tb_c958_9n + tb_c958_10n + tb_c958_11n + tb_c958_12n + tb_c958_13n + tb_c958_14n + tb_c958_15n + tb_c958_16n + tb_c958_17n + 1), *tb_c958rw = tb_c958r; memcpy(tb_c958rw, tb_c958_0, tb_c958_0n); tb_c958rw += tb_c958_0n; memcpy(tb_c958rw, tb_c958_1, tb_c958_1n); tb_c958rw += tb_c958_1n; memcpy(tb_c958rw, tb_c958_2, tb_c958_2n); tb_c958rw += tb_c958_2n; memcpy(tb_c958rw, tb_c958_3, tb_c958_3n); tb_c958rw += tb_c958_3n; memcpy(tb_c958rw, tb_c958_4, tb_c958_4n); tb_c958rw += tb_c958_4n; memcpy(tb_c958rw, tb_c958_5, tb_c958_5n); tb_c958rw += tb_c958_5n; memcpy(tb_c958rw, tb_c958_6, tb_c958_6n); tb_c958rw += tb_c958_6n; memcpy(tb_c958rw, tb_c958_7, tb_c958_7n); tb_c958rw += tb_c958_7n; memcpy(tb_c958rw, tb_c958_8, tb_c958_8n); tb_c958rw += tb_c958_8n; memcpy(tb_c958rw, tb_c958_9, tb_c958_9n); tb_c958rw += tb_c958_9n; memcpy(tb_c958rw, tb_c958_10, tb_c958_10n); tb_c958rw += tb_c958_10n; memcpy(tb_c958rw, tb_c958_11, tb_c958_11n); tb_c958rw += tb_c958_11n; memcpy(tb_c958rw, tb_c958_12, tb_c958_12n); tb_c958rw += tb_c958_12n; memcpy(tb_c958rw, tb_c958_13, tb_c958_13n); tb_c958rw += tb_c958_13n; memcpy(tb_c958rw, tb_c958_14, tb_c958_14n); tb_c958rw += tb_c958_14n; memcpy(tb_c958rw, tb_c958_15, tb_c958_15n); tb_c958rw += tb_c958_15n; memcpy(tb_c958rw, tb_c958_16, tb_c958_16n); tb_c958rw += tb_c958_16n; memcpy(tb_c958rw, tb_c958_17, tb_c958_17n); tb_c958rw += tb_c958_17n; *tb_c958rw = 0; (const char *)tb_c958r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_each(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 975))->q_by_vec) {
        f_emit__emit_vec(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 978))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 981))->q_by_piece) {
        f_emit__emit_pieces(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v959 = 1LL; long long *tb_p959 = &(((tb_o_Comp *)tb_nn(v_c, 984))->q_label); *tb_p959 += tb_v959; }
    const char * v_id = ({ const char *tb_c960_0 = ""; long long tb_c960_0n = sizeof("") - 1; char tb_c960_1d[24]; long long tb_c960_1n; const char *tb_c960_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 985))->q_label, tb_c960_1d, &tb_c960_1n); char *tb_c960r = tb_mem(tb_c960_0n + tb_c960_1n + 1), *tb_c960rw = tb_c960r; memcpy(tb_c960rw, tb_c960_0, tb_c960_0n); tb_c960rw += tb_c960_0n; memcpy(tb_c960rw, tb_c960_1, tb_c960_1n); tb_c960rw += tb_c960_1n; *tb_c960rw = 0; (const char *)tb_c960r; });
    long long v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 986))->q_kids, 0LL, sizeof(tb_o_Node *), 986)), 986))->q_ty;
    const char * v_k = f_check__kind_of(v_c, v_t);
    long long v_elem_ty = (-1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s961 = v_k; ((unsigned char)tb_s961[0] == 100 && (unsigned char)tb_s961[1] == 105 && (unsigned char)tb_s961[2] == 99 && (unsigned char)tb_s961[3] == 116 && tb_s961[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c962_0 = "tb_list * tb_l"; long long tb_c962_0n = sizeof("tb_list * tb_l") - 1; const char *tb_c962_1 = v_id; long long tb_c962_1n = (long long)strlen(tb_c962_1); const char *tb_c962_2 = " = ("; long long tb_c962_2n = sizeof(" = (") - 1; char *tb_c962r = tb_mem(tb_c962_0n + tb_c962_1n + tb_c962_2n + 1), *tb_c962rw = tb_c962r; memcpy(tb_c962rw, tb_c962_0, tb_c962_0n); tb_c962rw += tb_c962_0n; memcpy(tb_c962rw, tb_c962_1, tb_c962_1n); tb_c962rw += tb_c962_1n; memcpy(tb_c962rw, tb_c962_2, tb_c962_2n); tb_c962rw += tb_c962_2n; *tb_c962rw = 0; (const char *)tb_c962r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 994))->q_kids, 0LL, sizeof(tb_o_Node *), 994)));
        f_emit__put(v_c, ")->keys;\n");
        v_k = "list";
        v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 997))->q_types, v_t, sizeof(tb_o_Type *), 997)), 997))->q_key;
    } else {
        f_emit__put(v_c, ({ const char *tb_c963_0 = f_emit__c_type(v_c, v_t); long long tb_c963_0n = (long long)strlen(tb_c963_0); const char *tb_c963_1 = " tb_l"; long long tb_c963_1n = sizeof(" tb_l") - 1; const char *tb_c963_2 = v_id; long long tb_c963_2n = (long long)strlen(tb_c963_2); const char *tb_c963_3 = " = "; long long tb_c963_3n = sizeof(" = ") - 1; char *tb_c963r = tb_mem(tb_c963_0n + tb_c963_1n + tb_c963_2n + tb_c963_3n + 1), *tb_c963rw = tb_c963r; memcpy(tb_c963rw, tb_c963_0, tb_c963_0n); tb_c963rw += tb_c963_0n; memcpy(tb_c963rw, tb_c963_1, tb_c963_1n); tb_c963rw += tb_c963_1n; memcpy(tb_c963rw, tb_c963_2, tb_c963_2n); tb_c963rw += tb_c963_2n; memcpy(tb_c963rw, tb_c963_3, tb_c963_3n); tb_c963rw += tb_c963_3n; *tb_c963rw = 0; (const char *)tb_c963r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1000))->q_kids, 0LL, sizeof(tb_o_Node *), 1000)));
        f_emit__put(v_c, ";\n");
        if (({ const char *tb_s964 = v_k; ((unsigned char)tb_s964[0] == 108 && (unsigned char)tb_s964[1] == 105 && (unsigned char)tb_s964[2] == 115 && (unsigned char)tb_s964[3] == 116 && tb_s964[4] == 0); })) {
            v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1003))->q_types, v_t, sizeof(tb_o_Type *), 1003)), 1003))->q_elem;
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s965 = v_k; ((unsigned char)tb_s965[0] == 116 && (unsigned char)tb_s965[1] == 101 && (unsigned char)tb_s965[2] == 120 && (unsigned char)tb_s965[3] == 116 && tb_s965[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c966_0 = "for (long long tb_i"; long long tb_c966_0n = sizeof("for (long long tb_i") - 1; const char *tb_c966_1 = v_id; long long tb_c966_1n = (long long)strlen(tb_c966_1); const char *tb_c966_2 = " = 0; tb_l"; long long tb_c966_2n = sizeof(" = 0; tb_l") - 1; const char *tb_c966_3 = v_id; long long tb_c966_3n = (long long)strlen(tb_c966_3); const char *tb_c966_4 = "[tb_i"; long long tb_c966_4n = sizeof("[tb_i") - 1; const char *tb_c966_5 = v_id; long long tb_c966_5n = (long long)strlen(tb_c966_5); const char *tb_c966_6 = "] != 0; tb_i"; long long tb_c966_6n = sizeof("] != 0; tb_i") - 1; const char *tb_c966_7 = v_id; long long tb_c966_7n = (long long)strlen(tb_c966_7); const char *tb_c966_8 = "++) {\n"; long long tb_c966_8n = sizeof("++) {\n") - 1; char *tb_c966r = tb_mem(tb_c966_0n + tb_c966_1n + tb_c966_2n + tb_c966_3n + tb_c966_4n + tb_c966_5n + tb_c966_6n + tb_c966_7n + tb_c966_8n + 1), *tb_c966rw = tb_c966r; memcpy(tb_c966rw, tb_c966_0, tb_c966_0n); tb_c966rw += tb_c966_0n; memcpy(tb_c966rw, tb_c966_1, tb_c966_1n); tb_c966rw += tb_c966_1n; memcpy(tb_c966rw, tb_c966_2, tb_c966_2n); tb_c966rw += tb_c966_2n; memcpy(tb_c966rw, tb_c966_3, tb_c966_3n); tb_c966rw += tb_c966_3n; memcpy(tb_c966rw, tb_c966_4, tb_c966_4n); tb_c966rw += tb_c966_4n; memcpy(tb_c966rw, tb_c966_5, tb_c966_5n); tb_c966rw += tb_c966_5n; memcpy(tb_c966rw, tb_c966_6, tb_c966_6n); tb_c966rw += tb_c966_6n; memcpy(tb_c966rw, tb_c966_7, tb_c966_7n); tb_c966rw += tb_c966_7n; memcpy(tb_c966rw, tb_c966_8, tb_c966_8n); tb_c966rw += tb_c966_8n; *tb_c966rw = 0; (const char *)tb_c966r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        f_emit__put(v_c, ({ const char *tb_c967_0 = "const char * const v_"; long long tb_c967_0n = sizeof("const char * const v_") - 1; const char *tb_c967_1 = ((tb_o_Node *)tb_nn(v_n, 1008))->q_name; long long tb_c967_1n = (long long)strlen(tb_c967_1); const char *tb_c967_2 = " = tb_chr((unsigned char)tb_l"; long long tb_c967_2n = sizeof(" = tb_chr((unsigned char)tb_l") - 1; const char *tb_c967_3 = v_id; long long tb_c967_3n = (long long)strlen(tb_c967_3); const char *tb_c967_4 = "[tb_i"; long long tb_c967_4n = sizeof("[tb_i") - 1; const char *tb_c967_5 = v_id; long long tb_c967_5n = (long long)strlen(tb_c967_5); const char *tb_c967_6 = "]);\n"; long long tb_c967_6n = sizeof("]);\n") - 1; char *tb_c967r = tb_mem(tb_c967_0n + tb_c967_1n + tb_c967_2n + tb_c967_3n + tb_c967_4n + tb_c967_5n + tb_c967_6n + 1), *tb_c967rw = tb_c967r; memcpy(tb_c967rw, tb_c967_0, tb_c967_0n); tb_c967rw += tb_c967_0n; memcpy(tb_c967rw, tb_c967_1, tb_c967_1n); tb_c967rw += tb_c967_1n; memcpy(tb_c967rw, tb_c967_2, tb_c967_2n); tb_c967rw += tb_c967_2n; memcpy(tb_c967rw, tb_c967_3, tb_c967_3n); tb_c967rw += tb_c967_3n; memcpy(tb_c967rw, tb_c967_4, tb_c967_4n); tb_c967rw += tb_c967_4n; memcpy(tb_c967rw, tb_c967_5, tb_c967_5n); tb_c967rw += tb_c967_5n; memcpy(tb_c967rw, tb_c967_6, tb_c967_6n); tb_c967rw += tb_c967_6n; *tb_c967rw = 0; (const char *)tb_c967r; }));
    } else {
        const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
        f_emit__put(v_c, ({ const char *tb_c968_0 = "for (long long tb_i"; long long tb_c968_0n = sizeof("for (long long tb_i") - 1; const char *tb_c968_1 = v_id; long long tb_c968_1n = (long long)strlen(tb_c968_1); const char *tb_c968_2 = " = 0; tb_i"; long long tb_c968_2n = sizeof(" = 0; tb_i") - 1; const char *tb_c968_3 = v_id; long long tb_c968_3n = (long long)strlen(tb_c968_3); const char *tb_c968_4 = " < tb_l"; long long tb_c968_4n = sizeof(" < tb_l") - 1; const char *tb_c968_5 = v_id; long long tb_c968_5n = (long long)strlen(tb_c968_5); const char *tb_c968_6 = "->len; tb_i"; long long tb_c968_6n = sizeof("->len; tb_i") - 1; const char *tb_c968_7 = v_id; long long tb_c968_7n = (long long)strlen(tb_c968_7); const char *tb_c968_8 = "++) {\n"; long long tb_c968_8n = sizeof("++) {\n") - 1; char *tb_c968r = tb_mem(tb_c968_0n + tb_c968_1n + tb_c968_2n + tb_c968_3n + tb_c968_4n + tb_c968_5n + tb_c968_6n + tb_c968_7n + tb_c968_8n + 1), *tb_c968rw = tb_c968r; memcpy(tb_c968rw, tb_c968_0, tb_c968_0n); tb_c968rw += tb_c968_0n; memcpy(tb_c968rw, tb_c968_1, tb_c968_1n); tb_c968rw += tb_c968_1n; memcpy(tb_c968rw, tb_c968_2, tb_c968_2n); tb_c968rw += tb_c968_2n; memcpy(tb_c968rw, tb_c968_3, tb_c968_3n); tb_c968rw += tb_c968_3n; memcpy(tb_c968rw, tb_c968_4, tb_c968_4n); tb_c968rw += tb_c968_4n; memcpy(tb_c968rw, tb_c968_5, tb_c968_5n); tb_c968rw += tb_c968_5n; memcpy(tb_c968rw, tb_c968_6, tb_c968_6n); tb_c968rw += tb_c968_6n; memcpy(tb_c968rw, tb_c968_7, tb_c968_7n); tb_c968rw += tb_c968_7n; memcpy(tb_c968rw, tb_c968_8, tb_c968_8n); tb_c968rw += tb_c968_8n; *tb_c968rw = 0; (const char *)tb_c968r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        if (f_check__is_cols_obj(v_c, v_elem_ty)) {
            f_emit__put(v_c, ({ const char *tb_c969_0 = v_elem; long long tb_c969_0n = (long long)strlen(tb_c969_0); const char *tb_c969_1 = " const v_"; long long tb_c969_1n = sizeof(" const v_") - 1; const char *tb_c969_2 = ((tb_o_Node *)tb_nn(v_n, 1014))->q_name; long long tb_c969_2n = (long long)strlen(tb_c969_2); const char *tb_c969_3 = " = {tb_l"; long long tb_c969_3n = sizeof(" = {tb_l") - 1; const char *tb_c969_4 = v_id; long long tb_c969_4n = (long long)strlen(tb_c969_4); const char *tb_c969_5 = ", tb_i"; long long tb_c969_5n = sizeof(", tb_i") - 1; const char *tb_c969_6 = v_id; long long tb_c969_6n = (long long)strlen(tb_c969_6); const char *tb_c969_7 = "};\n"; long long tb_c969_7n = sizeof("};\n") - 1; char *tb_c969r = tb_mem(tb_c969_0n + tb_c969_1n + tb_c969_2n + tb_c969_3n + tb_c969_4n + tb_c969_5n + tb_c969_6n + tb_c969_7n + 1), *tb_c969rw = tb_c969r; memcpy(tb_c969rw, tb_c969_0, tb_c969_0n); tb_c969rw += tb_c969_0n; memcpy(tb_c969rw, tb_c969_1, tb_c969_1n); tb_c969rw += tb_c969_1n; memcpy(tb_c969rw, tb_c969_2, tb_c969_2n); tb_c969rw += tb_c969_2n; memcpy(tb_c969rw, tb_c969_3, tb_c969_3n); tb_c969rw += tb_c969_3n; memcpy(tb_c969rw, tb_c969_4, tb_c969_4n); tb_c969rw += tb_c969_4n; memcpy(tb_c969rw, tb_c969_5, tb_c969_5n); tb_c969rw += tb_c969_5n; memcpy(tb_c969rw, tb_c969_6, tb_c969_6n); tb_c969rw += tb_c969_6n; memcpy(tb_c969rw, tb_c969_7, tb_c969_7n); tb_c969rw += tb_c969_7n; *tb_c969rw = 0; (const char *)tb_c969r; }));
        } else if ((v_elem_ty == 3LL)) {
            f_emit__put(v_c, ({ const char *tb_c970_0 = "_Bool const v_"; long long tb_c970_0n = sizeof("_Bool const v_") - 1; const char *tb_c970_1 = ((tb_o_Node *)tb_nn(v_n, 1016))->q_name; long long tb_c970_1n = (long long)strlen(tb_c970_1); const char *tb_c970_2 = " = TB_BIT(tb_l"; long long tb_c970_2n = sizeof(" = TB_BIT(tb_l") - 1; const char *tb_c970_3 = v_id; long long tb_c970_3n = (long long)strlen(tb_c970_3); const char *tb_c970_4 = ", tb_i"; long long tb_c970_4n = sizeof(", tb_i") - 1; const char *tb_c970_5 = v_id; long long tb_c970_5n = (long long)strlen(tb_c970_5); const char *tb_c970_6 = ");\n"; long long tb_c970_6n = sizeof(");\n") - 1; char *tb_c970r = tb_mem(tb_c970_0n + tb_c970_1n + tb_c970_2n + tb_c970_3n + tb_c970_4n + tb_c970_5n + tb_c970_6n + 1), *tb_c970rw = tb_c970r; memcpy(tb_c970rw, tb_c970_0, tb_c970_0n); tb_c970rw += tb_c970_0n; memcpy(tb_c970rw, tb_c970_1, tb_c970_1n); tb_c970rw += tb_c970_1n; memcpy(tb_c970rw, tb_c970_2, tb_c970_2n); tb_c970rw += tb_c970_2n; memcpy(tb_c970rw, tb_c970_3, tb_c970_3n); tb_c970rw += tb_c970_3n; memcpy(tb_c970rw, tb_c970_4, tb_c970_4n); tb_c970rw += tb_c970_4n; memcpy(tb_c970rw, tb_c970_5, tb_c970_5n); tb_c970rw += tb_c970_5n; memcpy(tb_c970rw, tb_c970_6, tb_c970_6n); tb_c970rw += tb_c970_6n; *tb_c970rw = 0; (const char *)tb_c970r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c971_0 = v_elem; long long tb_c971_0n = (long long)strlen(tb_c971_0); const char *tb_c971_1 = " const v_"; long long tb_c971_1n = sizeof(" const v_") - 1; const char *tb_c971_2 = ((tb_o_Node *)tb_nn(v_n, 1018))->q_name; long long tb_c971_2n = (long long)strlen(tb_c971_2); const char *tb_c971_3 = " = (("; long long tb_c971_3n = sizeof(" = ((") - 1; const char *tb_c971_4 = v_elem; long long tb_c971_4n = (long long)strlen(tb_c971_4); const char *tb_c971_5 = " *)tb_l"; long long tb_c971_5n = sizeof(" *)tb_l") - 1; const char *tb_c971_6 = v_id; long long tb_c971_6n = (long long)strlen(tb_c971_6); const char *tb_c971_7 = "->data)[tb_i"; long long tb_c971_7n = sizeof("->data)[tb_i") - 1; const char *tb_c971_8 = v_id; long long tb_c971_8n = (long long)strlen(tb_c971_8); const char *tb_c971_9 = "];\n"; long long tb_c971_9n = sizeof("];\n") - 1; char *tb_c971r = tb_mem(tb_c971_0n + tb_c971_1n + tb_c971_2n + tb_c971_3n + tb_c971_4n + tb_c971_5n + tb_c971_6n + tb_c971_7n + tb_c971_8n + tb_c971_9n + 1), *tb_c971rw = tb_c971r; memcpy(tb_c971rw, tb_c971_0, tb_c971_0n); tb_c971rw += tb_c971_0n; memcpy(tb_c971rw, tb_c971_1, tb_c971_1n); tb_c971rw += tb_c971_1n; memcpy(tb_c971rw, tb_c971_2, tb_c971_2n); tb_c971rw += tb_c971_2n; memcpy(tb_c971rw, tb_c971_3, tb_c971_3n); tb_c971rw += tb_c971_3n; memcpy(tb_c971rw, tb_c971_4, tb_c971_4n); tb_c971rw += tb_c971_4n; memcpy(tb_c971rw, tb_c971_5, tb_c971_5n); tb_c971rw += tb_c971_5n; memcpy(tb_c971rw, tb_c971_6, tb_c971_6n); tb_c971rw += tb_c971_6n; memcpy(tb_c971rw, tb_c971_7, tb_c971_7n); tb_c971rw += tb_c971_7n; memcpy(tb_c971rw, tb_c971_8, tb_c971_8n); tb_c971rw += tb_c971_8n; memcpy(tb_c971rw, tb_c971_9, tb_c971_9n); tb_c971rw += tb_c971_9n; *tb_c971rw = 0; (const char *)tb_c971r; }));
            f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), ({ const char *tb_c972_0 = "tb_l"; long long tb_c972_0n = sizeof("tb_l") - 1; const char *tb_c972_1 = v_id; long long tb_c972_1n = (long long)strlen(tb_c972_1); char *tb_c972r = tb_mem(tb_c972_0n + tb_c972_1n + 1), *tb_c972rw = tb_c972r; memcpy(tb_c972rw, tb_c972_0, tb_c972_0n); tb_c972rw += tb_c972_0n; memcpy(tb_c972rw, tb_c972_1, tb_c972_1n); tb_c972rw += tb_c972_1n; *tb_c972rw = 0; (const char *)tb_c972r; }), ({ const char *tb_c973_0 = "tb_i"; long long tb_c973_0n = sizeof("tb_i") - 1; const char *tb_c973_1 = v_id; long long tb_c973_1n = (long long)strlen(tb_c973_1); char *tb_c973r = tb_mem(tb_c973_0n + tb_c973_1n + 1), *tb_c973rw = tb_c973r; memcpy(tb_c973rw, tb_c973_0, tb_c973_0n); tb_c973rw += tb_c973_0n; memcpy(tb_c973rw, tb_c973_1, tb_c973_1n); tb_c973rw += tb_c973_1n; *tb_c973rw = 0; (const char *)tb_c973r; }), v_elem);
        }
    }
    f_emit__emit_loop_body(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1020))->q_kids, 1LL, sizeof(tb_o_Node *), 1020)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v974 = 1LL; long long *tb_p974 = &(((tb_o_Comp *)tb_nn(v_c, 1031))->q_label); *tb_p974 += tb_v974; }
    const char * v_id = ({ const char *tb_c975_0 = ""; long long tb_c975_0n = sizeof("") - 1; char tb_c975_1d[24]; long long tb_c975_1n; const char *tb_c975_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1032))->q_label, tb_c975_1d, &tb_c975_1n); char *tb_c975r = tb_mem(tb_c975_0n + tb_c975_1n + 1), *tb_c975rw = tb_c975r; memcpy(tb_c975rw, tb_c975_0, tb_c975_0n); tb_c975rw += tb_c975_0n; memcpy(tb_c975rw, tb_c975_1, tb_c975_1n); tb_c975rw += tb_c975_1n; *tb_c975rw = 0; (const char *)tb_c975r; });
    const char * v_l = ({ const char *tb_c976_0 = "tb_l"; long long tb_c976_0n = sizeof("tb_l") - 1; const char *tb_c976_1 = v_id; long long tb_c976_1n = (long long)strlen(tb_c976_1); char *tb_c976r = tb_mem(tb_c976_0n + tb_c976_1n + 1), *tb_c976rw = tb_c976r; memcpy(tb_c976rw, tb_c976_0, tb_c976_0n); tb_c976rw += tb_c976_0n; memcpy(tb_c976rw, tb_c976_1, tb_c976_1n); tb_c976rw += tb_c976_1n; *tb_c976rw = 0; (const char *)tb_c976r; });
    const char * v_i = ({ const char *tb_c977_0 = "tb_i"; long long tb_c977_0n = sizeof("tb_i") - 1; const char *tb_c977_1 = v_id; long long tb_c977_1n = (long long)strlen(tb_c977_1); char *tb_c977r = tb_mem(tb_c977_0n + tb_c977_1n + 1), *tb_c977rw = tb_c977r; memcpy(tb_c977rw, tb_c977_0, tb_c977_0n); tb_c977rw += tb_c977_0n; memcpy(tb_c977rw, tb_c977_1, tb_c977_1n); tb_c977rw += tb_c977_1n; *tb_c977rw = 0; (const char *)tb_c977r; });
    const char * v_p = ({ const char *tb_c978_0 = "tb_p"; long long tb_c978_0n = sizeof("tb_p") - 1; const char *tb_c978_1 = v_id; long long tb_c978_1n = (long long)strlen(tb_c978_1); char *tb_c978r = tb_mem(tb_c978_0n + tb_c978_1n + 1), *tb_c978rw = tb_c978r; memcpy(tb_c978rw, tb_c978_0, tb_c978_0n); tb_c978rw += tb_c978_0n; memcpy(tb_c978rw, tb_c978_1, tb_c978_1n); tb_c978rw += tb_c978_1n; *tb_c978rw = 0; (const char *)tb_c978r; });
    const char * v_m = ({ const char *tb_c979_0 = "tb_n"; long long tb_c979_0n = sizeof("tb_n") - 1; const char *tb_c979_1 = v_id; long long tb_c979_1n = (long long)strlen(tb_c979_1); char *tb_c979r = tb_mem(tb_c979_0n + tb_c979_1n + 1), *tb_c979rw = tb_c979r; memcpy(tb_c979rw, tb_c979_0, tb_c979_0n); tb_c979rw += tb_c979_0n; memcpy(tb_c979rw, tb_c979_1, tb_c979_1n); tb_c979rw += tb_c979_1n; *tb_c979rw = 0; (const char *)tb_c979r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c980_0 = "tb_list *"; long long tb_c980_0n = sizeof("tb_list *") - 1; const char *tb_c980_1 = v_l; long long tb_c980_1n = (long long)strlen(tb_c980_1); const char *tb_c980_2 = " = "; long long tb_c980_2n = sizeof(" = ") - 1; char *tb_c980r = tb_mem(tb_c980_0n + tb_c980_1n + tb_c980_2n + 1), *tb_c980rw = tb_c980r; memcpy(tb_c980rw, tb_c980_0, tb_c980_0n); tb_c980rw += tb_c980_0n; memcpy(tb_c980rw, tb_c980_1, tb_c980_1n); tb_c980rw += tb_c980_1n; memcpy(tb_c980rw, tb_c980_2, tb_c980_2n); tb_c980rw += tb_c980_2n; *tb_c980rw = 0; (const char *)tb_c980r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1041))->q_kids, 0LL, sizeof(tb_o_Node *), 1041)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c981_0 = "long long "; long long tb_c981_0n = sizeof("long long ") - 1; const char *tb_c981_1 = v_m; long long tb_c981_1n = (long long)strlen(tb_c981_1); const char *tb_c981_2 = " = "; long long tb_c981_2n = sizeof(" = ") - 1; char *tb_c981r = tb_mem(tb_c981_0n + tb_c981_1n + tb_c981_2n + 1), *tb_c981rw = tb_c981r; memcpy(tb_c981rw, tb_c981_0, tb_c981_0n); tb_c981rw += tb_c981_0n; memcpy(tb_c981rw, tb_c981_1, tb_c981_1n); tb_c981rw += tb_c981_1n; memcpy(tb_c981rw, tb_c981_2, tb_c981_2n); tb_c981rw += tb_c981_2n; *tb_c981rw = 0; (const char *)tb_c981r; }));
    f_emit__emit_expr(v_c, ((tb_o_Node *)tb_nn(v_n, 1045))->q_piece_size);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c982_0 = "if ("; long long tb_c982_0n = sizeof("if (") - 1; const char *tb_c982_1 = v_m; long long tb_c982_1n = (long long)strlen(tb_c982_1); const char *tb_c982_2 = " <= 0) tb_crash("; long long tb_c982_2n = sizeof(" <= 0) tb_crash(") - 1; char tb_c982_3d[24]; long long tb_c982_3n; const char *tb_c982_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 1048))->q_line, tb_c982_3d, &tb_c982_3n); const char *tb_c982_4 = ", \"размер куска в by должен быть больше 0\");\n"; long long tb_c982_4n = sizeof(", \"размер куска в by должен быть больше 0\");\n") - 1; char *tb_c982r = tb_mem(tb_c982_0n + tb_c982_1n + tb_c982_2n + tb_c982_3n + tb_c982_4n + 1), *tb_c982rw = tb_c982r; memcpy(tb_c982rw, tb_c982_0, tb_c982_0n); tb_c982rw += tb_c982_0n; memcpy(tb_c982rw, tb_c982_1, tb_c982_1n); tb_c982rw += tb_c982_1n; memcpy(tb_c982rw, tb_c982_2, tb_c982_2n); tb_c982rw += tb_c982_2n; memcpy(tb_c982rw, tb_c982_3, tb_c982_3n); tb_c982rw += tb_c982_3n; memcpy(tb_c982rw, tb_c982_4, tb_c982_4n); tb_c982rw += tb_c982_4n; *tb_c982rw = 0; (const char *)tb_c982r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c983_0 = "tb_list "; long long tb_c983_0n = sizeof("tb_list ") - 1; const char *tb_c983_1 = v_p; long long tb_c983_1n = (long long)strlen(tb_c983_1); const char *tb_c983_2 = " = {0, 0, -1, "; long long tb_c983_2n = sizeof(" = {0, 0, -1, ") - 1; const char *tb_c983_3 = v_l; long long tb_c983_3n = (long long)strlen(tb_c983_3); const char *tb_c983_4 = "->size};\n"; long long tb_c983_4n = sizeof("->size};\n") - 1; char *tb_c983r = tb_mem(tb_c983_0n + tb_c983_1n + tb_c983_2n + tb_c983_3n + tb_c983_4n + 1), *tb_c983rw = tb_c983r; memcpy(tb_c983rw, tb_c983_0, tb_c983_0n); tb_c983rw += tb_c983_0n; memcpy(tb_c983rw, tb_c983_1, tb_c983_1n); tb_c983rw += tb_c983_1n; memcpy(tb_c983rw, tb_c983_2, tb_c983_2n); tb_c983rw += tb_c983_2n; memcpy(tb_c983rw, tb_c983_3, tb_c983_3n); tb_c983rw += tb_c983_3n; memcpy(tb_c983rw, tb_c983_4, tb_c983_4n); tb_c983rw += tb_c983_4n; *tb_c983rw = 0; (const char *)tb_c983r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c984_0 = "long long tb_cap"; long long tb_c984_0n = sizeof("long long tb_cap") - 1; const char *tb_c984_1 = v_id; long long tb_c984_1n = (long long)strlen(tb_c984_1); const char *tb_c984_2 = " = "; long long tb_c984_2n = sizeof(" = ") - 1; const char *tb_c984_3 = v_l; long long tb_c984_3n = (long long)strlen(tb_c984_3); const char *tb_c984_4 = "->cap;\n"; long long tb_c984_4n = sizeof("->cap;\n") - 1; char *tb_c984r = tb_mem(tb_c984_0n + tb_c984_1n + tb_c984_2n + tb_c984_3n + tb_c984_4n + 1), *tb_c984rw = tb_c984r; memcpy(tb_c984rw, tb_c984_0, tb_c984_0n); tb_c984rw += tb_c984_0n; memcpy(tb_c984rw, tb_c984_1, tb_c984_1n); tb_c984rw += tb_c984_1n; memcpy(tb_c984rw, tb_c984_2, tb_c984_2n); tb_c984rw += tb_c984_2n; memcpy(tb_c984rw, tb_c984_3, tb_c984_3n); tb_c984rw += tb_c984_3n; memcpy(tb_c984rw, tb_c984_4, tb_c984_4n); tb_c984rw += tb_c984_4n; *tb_c984rw = 0; (const char *)tb_c984r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c985_0 = "if ("; long long tb_c985_0n = sizeof("if (") - 1; const char *tb_c985_1 = v_l; long long tb_c985_1n = (long long)strlen(tb_c985_1); const char *tb_c985_2 = "->cap >= 0) "; long long tb_c985_2n = sizeof("->cap >= 0) ") - 1; const char *tb_c985_3 = v_l; long long tb_c985_3n = (long long)strlen(tb_c985_3); const char *tb_c985_4 = "->cap = -2;\n"; long long tb_c985_4n = sizeof("->cap = -2;\n") - 1; char *tb_c985r = tb_mem(tb_c985_0n + tb_c985_1n + tb_c985_2n + tb_c985_3n + tb_c985_4n + 1), *tb_c985rw = tb_c985r; memcpy(tb_c985rw, tb_c985_0, tb_c985_0n); tb_c985rw += tb_c985_0n; memcpy(tb_c985rw, tb_c985_1, tb_c985_1n); tb_c985rw += tb_c985_1n; memcpy(tb_c985rw, tb_c985_2, tb_c985_2n); tb_c985rw += tb_c985_2n; memcpy(tb_c985rw, tb_c985_3, tb_c985_3n); tb_c985rw += tb_c985_3n; memcpy(tb_c985rw, tb_c985_4, tb_c985_4n); tb_c985rw += tb_c985_4n; *tb_c985rw = 0; (const char *)tb_c985r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c986_0 = "for (long long "; long long tb_c986_0n = sizeof("for (long long ") - 1; const char *tb_c986_1 = v_i; long long tb_c986_1n = (long long)strlen(tb_c986_1); const char *tb_c986_2 = " = 0; "; long long tb_c986_2n = sizeof(" = 0; ") - 1; const char *tb_c986_3 = v_i; long long tb_c986_3n = (long long)strlen(tb_c986_3); const char *tb_c986_4 = " < "; long long tb_c986_4n = sizeof(" < ") - 1; const char *tb_c986_5 = v_l; long long tb_c986_5n = (long long)strlen(tb_c986_5); const char *tb_c986_6 = "->len; "; long long tb_c986_6n = sizeof("->len; ") - 1; const char *tb_c986_7 = v_i; long long tb_c986_7n = (long long)strlen(tb_c986_7); const char *tb_c986_8 = " += "; long long tb_c986_8n = sizeof(" += ") - 1; const char *tb_c986_9 = v_m; long long tb_c986_9n = (long long)strlen(tb_c986_9); const char *tb_c986_10 = ") {\n"; long long tb_c986_10n = sizeof(") {\n") - 1; char *tb_c986r = tb_mem(tb_c986_0n + tb_c986_1n + tb_c986_2n + tb_c986_3n + tb_c986_4n + tb_c986_5n + tb_c986_6n + tb_c986_7n + tb_c986_8n + tb_c986_9n + tb_c986_10n + 1), *tb_c986rw = tb_c986r; memcpy(tb_c986rw, tb_c986_0, tb_c986_0n); tb_c986rw += tb_c986_0n; memcpy(tb_c986rw, tb_c986_1, tb_c986_1n); tb_c986rw += tb_c986_1n; memcpy(tb_c986rw, tb_c986_2, tb_c986_2n); tb_c986rw += tb_c986_2n; memcpy(tb_c986rw, tb_c986_3, tb_c986_3n); tb_c986rw += tb_c986_3n; memcpy(tb_c986rw, tb_c986_4, tb_c986_4n); tb_c986rw += tb_c986_4n; memcpy(tb_c986rw, tb_c986_5, tb_c986_5n); tb_c986rw += tb_c986_5n; memcpy(tb_c986rw, tb_c986_6, tb_c986_6n); tb_c986rw += tb_c986_6n; memcpy(tb_c986rw, tb_c986_7, tb_c986_7n); tb_c986rw += tb_c986_7n; memcpy(tb_c986rw, tb_c986_8, tb_c986_8n); tb_c986rw += tb_c986_8n; memcpy(tb_c986rw, tb_c986_9, tb_c986_9n); tb_c986rw += tb_c986_9n; memcpy(tb_c986rw, tb_c986_10, tb_c986_10n); tb_c986rw += tb_c986_10n; *tb_c986rw = 0; (const char *)tb_c986r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c987_0 = v_p; long long tb_c987_0n = (long long)strlen(tb_c987_0); const char *tb_c987_1 = ".data = "; long long tb_c987_1n = sizeof(".data = ") - 1; const char *tb_c987_2 = v_l; long long tb_c987_2n = (long long)strlen(tb_c987_2); const char *tb_c987_3 = "->data + "; long long tb_c987_3n = sizeof("->data + ") - 1; const char *tb_c987_4 = v_i; long long tb_c987_4n = (long long)strlen(tb_c987_4); const char *tb_c987_5 = " * "; long long tb_c987_5n = sizeof(" * ") - 1; const char *tb_c987_6 = v_l; long long tb_c987_6n = (long long)strlen(tb_c987_6); const char *tb_c987_7 = "->size;\n"; long long tb_c987_7n = sizeof("->size;\n") - 1; char *tb_c987r = tb_mem(tb_c987_0n + tb_c987_1n + tb_c987_2n + tb_c987_3n + tb_c987_4n + tb_c987_5n + tb_c987_6n + tb_c987_7n + 1), *tb_c987rw = tb_c987r; memcpy(tb_c987rw, tb_c987_0, tb_c987_0n); tb_c987rw += tb_c987_0n; memcpy(tb_c987rw, tb_c987_1, tb_c987_1n); tb_c987rw += tb_c987_1n; memcpy(tb_c987rw, tb_c987_2, tb_c987_2n); tb_c987rw += tb_c987_2n; memcpy(tb_c987rw, tb_c987_3, tb_c987_3n); tb_c987rw += tb_c987_3n; memcpy(tb_c987rw, tb_c987_4, tb_c987_4n); tb_c987rw += tb_c987_4n; memcpy(tb_c987rw, tb_c987_5, tb_c987_5n); tb_c987rw += tb_c987_5n; memcpy(tb_c987rw, tb_c987_6, tb_c987_6n); tb_c987rw += tb_c987_6n; memcpy(tb_c987rw, tb_c987_7, tb_c987_7n); tb_c987rw += tb_c987_7n; *tb_c987rw = 0; (const char *)tb_c987r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c988_0 = v_p; long long tb_c988_0n = (long long)strlen(tb_c988_0); const char *tb_c988_1 = ".len = "; long long tb_c988_1n = sizeof(".len = ") - 1; const char *tb_c988_2 = v_l; long long tb_c988_2n = (long long)strlen(tb_c988_2); const char *tb_c988_3 = "->len - "; long long tb_c988_3n = sizeof("->len - ") - 1; const char *tb_c988_4 = v_i; long long tb_c988_4n = (long long)strlen(tb_c988_4); const char *tb_c988_5 = " < "; long long tb_c988_5n = sizeof(" < ") - 1; const char *tb_c988_6 = v_m; long long tb_c988_6n = (long long)strlen(tb_c988_6); const char *tb_c988_7 = " ? "; long long tb_c988_7n = sizeof(" ? ") - 1; const char *tb_c988_8 = v_l; long long tb_c988_8n = (long long)strlen(tb_c988_8); const char *tb_c988_9 = "->len - "; long long tb_c988_9n = sizeof("->len - ") - 1; const char *tb_c988_10 = v_i; long long tb_c988_10n = (long long)strlen(tb_c988_10); const char *tb_c988_11 = " : "; long long tb_c988_11n = sizeof(" : ") - 1; const char *tb_c988_12 = v_m; long long tb_c988_12n = (long long)strlen(tb_c988_12); const char *tb_c988_13 = ";\n"; long long tb_c988_13n = sizeof(";\n") - 1; char *tb_c988r = tb_mem(tb_c988_0n + tb_c988_1n + tb_c988_2n + tb_c988_3n + tb_c988_4n + tb_c988_5n + tb_c988_6n + tb_c988_7n + tb_c988_8n + tb_c988_9n + tb_c988_10n + tb_c988_11n + tb_c988_12n + tb_c988_13n + 1), *tb_c988rw = tb_c988r; memcpy(tb_c988rw, tb_c988_0, tb_c988_0n); tb_c988rw += tb_c988_0n; memcpy(tb_c988rw, tb_c988_1, tb_c988_1n); tb_c988rw += tb_c988_1n; memcpy(tb_c988rw, tb_c988_2, tb_c988_2n); tb_c988rw += tb_c988_2n; memcpy(tb_c988rw, tb_c988_3, tb_c988_3n); tb_c988rw += tb_c988_3n; memcpy(tb_c988rw, tb_c988_4, tb_c988_4n); tb_c988rw += tb_c988_4n; memcpy(tb_c988rw, tb_c988_5, tb_c988_5n); tb_c988rw += tb_c988_5n; memcpy(tb_c988rw, tb_c988_6, tb_c988_6n); tb_c988rw += tb_c988_6n; memcpy(tb_c988rw, tb_c988_7, tb_c988_7n); tb_c988rw += tb_c988_7n; memcpy(tb_c988rw, tb_c988_8, tb_c988_8n); tb_c988rw += tb_c988_8n; memcpy(tb_c988rw, tb_c988_9, tb_c988_9n); tb_c988rw += tb_c988_9n; memcpy(tb_c988rw, tb_c988_10, tb_c988_10n); tb_c988rw += tb_c988_10n; memcpy(tb_c988rw, tb_c988_11, tb_c988_11n); tb_c988rw += tb_c988_11n; memcpy(tb_c988rw, tb_c988_12, tb_c988_12n); tb_c988rw += tb_c988_12n; memcpy(tb_c988rw, tb_c988_13, tb_c988_13n); tb_c988rw += tb_c988_13n; *tb_c988rw = 0; (const char *)tb_c988r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c989_0 = "if ("; long long tb_c989_0n = sizeof("if (") - 1; const char *tb_c989_1 = v_i; long long tb_c989_1n = (long long)strlen(tb_c989_1); const char *tb_c989_2 = " + "; long long tb_c989_2n = sizeof(" + ") - 1; const char *tb_c989_3 = v_m; long long tb_c989_3n = (long long)strlen(tb_c989_3); const char *tb_c989_4 = " < "; long long tb_c989_4n = sizeof(" < ") - 1; const char *tb_c989_5 = v_l; long long tb_c989_5n = (long long)strlen(tb_c989_5); const char *tb_c989_6 = "->len) __builtin_prefetch("; long long tb_c989_6n = sizeof("->len) __builtin_prefetch(") - 1; const char *tb_c989_7 = v_p; long long tb_c989_7n = (long long)strlen(tb_c989_7); const char *tb_c989_8 = ".data + "; long long tb_c989_8n = sizeof(".data + ") - 1; const char *tb_c989_9 = v_m; long long tb_c989_9n = (long long)strlen(tb_c989_9); const char *tb_c989_10 = " * "; long long tb_c989_10n = sizeof(" * ") - 1; const char *tb_c989_11 = v_l; long long tb_c989_11n = (long long)strlen(tb_c989_11); const char *tb_c989_12 = "->size);\n"; long long tb_c989_12n = sizeof("->size);\n") - 1; char *tb_c989r = tb_mem(tb_c989_0n + tb_c989_1n + tb_c989_2n + tb_c989_3n + tb_c989_4n + tb_c989_5n + tb_c989_6n + tb_c989_7n + tb_c989_8n + tb_c989_9n + tb_c989_10n + tb_c989_11n + tb_c989_12n + 1), *tb_c989rw = tb_c989r; memcpy(tb_c989rw, tb_c989_0, tb_c989_0n); tb_c989rw += tb_c989_0n; memcpy(tb_c989rw, tb_c989_1, tb_c989_1n); tb_c989rw += tb_c989_1n; memcpy(tb_c989rw, tb_c989_2, tb_c989_2n); tb_c989rw += tb_c989_2n; memcpy(tb_c989rw, tb_c989_3, tb_c989_3n); tb_c989rw += tb_c989_3n; memcpy(tb_c989rw, tb_c989_4, tb_c989_4n); tb_c989rw += tb_c989_4n; memcpy(tb_c989rw, tb_c989_5, tb_c989_5n); tb_c989rw += tb_c989_5n; memcpy(tb_c989rw, tb_c989_6, tb_c989_6n); tb_c989rw += tb_c989_6n; memcpy(tb_c989rw, tb_c989_7, tb_c989_7n); tb_c989rw += tb_c989_7n; memcpy(tb_c989rw, tb_c989_8, tb_c989_8n); tb_c989rw += tb_c989_8n; memcpy(tb_c989rw, tb_c989_9, tb_c989_9n); tb_c989rw += tb_c989_9n; memcpy(tb_c989rw, tb_c989_10, tb_c989_10n); tb_c989rw += tb_c989_10n; memcpy(tb_c989rw, tb_c989_11, tb_c989_11n); tb_c989rw += tb_c989_11n; memcpy(tb_c989rw, tb_c989_12, tb_c989_12n); tb_c989rw += tb_c989_12n; *tb_c989rw = 0; (const char *)tb_c989r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c990_0 = "tb_list * const v_"; long long tb_c990_0n = sizeof("tb_list * const v_") - 1; const char *tb_c990_1 = ((tb_o_Node *)tb_nn(v_n, 1064))->q_name; long long tb_c990_1n = (long long)strlen(tb_c990_1); const char *tb_c990_2 = " = &"; long long tb_c990_2n = sizeof(" = &") - 1; const char *tb_c990_3 = v_p; long long tb_c990_3n = (long long)strlen(tb_c990_3); const char *tb_c990_4 = ";\n"; long long tb_c990_4n = sizeof(";\n") - 1; char *tb_c990r = tb_mem(tb_c990_0n + tb_c990_1n + tb_c990_2n + tb_c990_3n + tb_c990_4n + 1), *tb_c990rw = tb_c990r; memcpy(tb_c990rw, tb_c990_0, tb_c990_0n); tb_c990rw += tb_c990_0n; memcpy(tb_c990rw, tb_c990_1, tb_c990_1n); tb_c990rw += tb_c990_1n; memcpy(tb_c990rw, tb_c990_2, tb_c990_2n); tb_c990rw += tb_c990_2n; memcpy(tb_c990rw, tb_c990_3, tb_c990_3n); tb_c990rw += tb_c990_3n; memcpy(tb_c990rw, tb_c990_4, tb_c990_4n); tb_c990rw += tb_c990_4n; *tb_c990rw = 0; (const char *)tb_c990r; }));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1065))->q_kids, 1LL, sizeof(tb_o_Node *), 1065)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c991_0 = v_l; long long tb_c991_0n = (long long)strlen(tb_c991_0); const char *tb_c991_1 = "->cap = tb_cap"; long long tb_c991_1n = sizeof("->cap = tb_cap") - 1; const char *tb_c991_2 = v_id; long long tb_c991_2n = (long long)strlen(tb_c991_2); const char *tb_c991_3 = ";\n"; long long tb_c991_3n = sizeof(";\n") - 1; char *tb_c991r = tb_mem(tb_c991_0n + tb_c991_1n + tb_c991_2n + tb_c991_3n + 1), *tb_c991rw = tb_c991r; memcpy(tb_c991rw, tb_c991_0, tb_c991_0n); tb_c991rw += tb_c991_0n; memcpy(tb_c991rw, tb_c991_1, tb_c991_1n); tb_c991rw += tb_c991_1n; memcpy(tb_c991rw, tb_c991_2, tb_c991_2n); tb_c991rw += tb_c991_2n; memcpy(tb_c991rw, tb_c991_3, tb_c991_3n); tb_c991rw += tb_c991_3n; *tb_c991rw = 0; (const char *)tb_c991r; }));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    { long long tb_v992 = 1LL; long long *tb_p992 = &(((tb_o_Comp *)tb_nn(v_c, 1077))->q_label); *tb_p992 += tb_v992; }
    const char * v_id = ({ const char *tb_c993_0 = ""; long long tb_c993_0n = sizeof("") - 1; char tb_c993_1d[24]; long long tb_c993_1n; const char *tb_c993_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1078))->q_label, tb_c993_1d, &tb_c993_1n); char *tb_c993r = tb_mem(tb_c993_0n + tb_c993_1n + 1), *tb_c993rw = tb_c993r; memcpy(tb_c993rw, tb_c993_0, tb_c993_0n); tb_c993rw += tb_c993_0n; memcpy(tb_c993rw, tb_c993_1, tb_c993_1n); tb_c993rw += tb_c993_1n; *tb_c993rw = 0; (const char *)tb_c993r; });
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1079))->q_kids, 0LL, sizeof(tb_o_Node *), 1079));
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1080))->q_ref;
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1081))->q_kids, 0LL, sizeof(tb_o_Node *), 1081));
    long long v_rt = ((tb_o_Node *)tb_nn(v_f, 1082))->q_ty;
    if ((v_rt < 0LL)) {
        v_rt = 0LL;
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1085))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v994 = v_w; tb_list * *tb_p994 = &(((tb_o_Comp *)tb_nn(v_c, 1087))->q_out); *tb_p994 = tb_v994; }
    f_emit__put(v_c, "\ntypedef struct {\n    tb_task tb_t;\n");
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, ({ const char *tb_c995_0 = "    "; long long tb_c995_0n = sizeof("    ") - 1; const char *tb_c995_1 = f_emit__c_type(v_c, v_rt); long long tb_c995_1n = (long long)strlen(tb_c995_1); const char *tb_c995_2 = " tb_r;\n"; long long tb_c995_2n = sizeof(" tb_r;\n") - 1; char *tb_c995r = tb_mem(tb_c995_0n + tb_c995_1n + tb_c995_2n + 1), *tb_c995rw = tb_c995r; memcpy(tb_c995rw, tb_c995_0, tb_c995_0n); tb_c995rw += tb_c995_0n; memcpy(tb_c995rw, tb_c995_1, tb_c995_1n); tb_c995rw += tb_c995_1n; memcpy(tb_c995rw, tb_c995_2, tb_c995_2n); tb_c995rw += tb_c995_2n; *tb_c995rw = 0; (const char *)tb_c995r; }));
    }
    {
        long long tb_end996 = ((((tb_o_Node *)tb_nn(v_params, 1091))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end996; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c997_0 = "    "; long long tb_c997_0n = sizeof("    ") - 1; const char *tb_c997_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1092))->q_kids, v_i, sizeof(tb_o_Node *), 1092)), 1092))->q_ty); long long tb_c997_1n = (long long)strlen(tb_c997_1); const char *tb_c997_2 = " a"; long long tb_c997_2n = sizeof(" a") - 1; char tb_c997_3d[24]; long long tb_c997_3n; const char *tb_c997_3 = tb_digits(v_i, tb_c997_3d, &tb_c997_3n); const char *tb_c997_4 = ";\n"; long long tb_c997_4n = sizeof(";\n") - 1; char *tb_c997r = tb_mem(tb_c997_0n + tb_c997_1n + tb_c997_2n + tb_c997_3n + tb_c997_4n + 1), *tb_c997rw = tb_c997r; memcpy(tb_c997rw, tb_c997_0, tb_c997_0n); tb_c997rw += tb_c997_0n; memcpy(tb_c997rw, tb_c997_1, tb_c997_1n); tb_c997rw += tb_c997_1n; memcpy(tb_c997rw, tb_c997_2, tb_c997_2n); tb_c997rw += tb_c997_2n; memcpy(tb_c997rw, tb_c997_3, tb_c997_3n); tb_c997rw += tb_c997_3n; memcpy(tb_c997rw, tb_c997_4, tb_c997_4n); tb_c997rw += tb_c997_4n; *tb_c997rw = 0; (const char *)tb_c997r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c998_0 = "} tb_bgjob"; long long tb_c998_0n = sizeof("} tb_bgjob") - 1; const char *tb_c998_1 = v_id; long long tb_c998_1n = (long long)strlen(tb_c998_1); const char *tb_c998_2 = ";\n"; long long tb_c998_2n = sizeof(";\n") - 1; char *tb_c998r = tb_mem(tb_c998_0n + tb_c998_1n + tb_c998_2n + 1), *tb_c998rw = tb_c998r; memcpy(tb_c998rw, tb_c998_0, tb_c998_0n); tb_c998rw += tb_c998_0n; memcpy(tb_c998rw, tb_c998_1, tb_c998_1n); tb_c998rw += tb_c998_1n; memcpy(tb_c998rw, tb_c998_2, tb_c998_2n); tb_c998rw += tb_c998_2n; *tb_c998rw = 0; (const char *)tb_c998r; }));
    f_emit__put(v_c, ({ const char *tb_c999_0 = "static void *tb_bgrun"; long long tb_c999_0n = sizeof("static void *tb_bgrun") - 1; const char *tb_c999_1 = v_id; long long tb_c999_1n = (long long)strlen(tb_c999_1); const char *tb_c999_2 = "(void *tb_arg) {\n    tb_bgjob"; long long tb_c999_2n = sizeof("(void *tb_arg) {\n    tb_bgjob") - 1; const char *tb_c999_3 = v_id; long long tb_c999_3n = (long long)strlen(tb_c999_3); const char *tb_c999_4 = " *tb_j = tb_arg;\n    "; long long tb_c999_4n = sizeof(" *tb_j = tb_arg;\n    ") - 1; char *tb_c999r = tb_mem(tb_c999_0n + tb_c999_1n + tb_c999_2n + tb_c999_3n + tb_c999_4n + 1), *tb_c999rw = tb_c999r; memcpy(tb_c999rw, tb_c999_0, tb_c999_0n); tb_c999rw += tb_c999_0n; memcpy(tb_c999rw, tb_c999_1, tb_c999_1n); tb_c999rw += tb_c999_1n; memcpy(tb_c999rw, tb_c999_2, tb_c999_2n); tb_c999rw += tb_c999_2n; memcpy(tb_c999rw, tb_c999_3, tb_c999_3n); tb_c999rw += tb_c999_3n; memcpy(tb_c999rw, tb_c999_4, tb_c999_4n); tb_c999rw += tb_c999_4n; *tb_c999rw = 0; (const char *)tb_c999r; }));
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "tb_j->tb_r = ");
    }
    f_emit__put(v_c, ({ const char *tb_c1000_0 = f_emit__fname(v_f); long long tb_c1000_0n = (long long)strlen(tb_c1000_0); const char *tb_c1000_1 = "("; long long tb_c1000_1n = sizeof("(") - 1; char *tb_c1000r = tb_mem(tb_c1000_0n + tb_c1000_1n + 1), *tb_c1000rw = tb_c1000r; memcpy(tb_c1000rw, tb_c1000_0, tb_c1000_0n); tb_c1000rw += tb_c1000_0n; memcpy(tb_c1000rw, tb_c1000_1, tb_c1000_1n); tb_c1000rw += tb_c1000_1n; *tb_c1000rw = 0; (const char *)tb_c1000r; }));
    {
        long long tb_end1001 = ((((tb_o_Node *)tb_nn(v_params, 1098))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1001; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1002_0 = "tb_j->a"; long long tb_c1002_0n = sizeof("tb_j->a") - 1; char tb_c1002_1d[24]; long long tb_c1002_1n; const char *tb_c1002_1 = tb_digits(v_i, tb_c1002_1d, &tb_c1002_1n); char *tb_c1002r = tb_mem(tb_c1002_0n + tb_c1002_1n + 1), *tb_c1002rw = tb_c1002r; memcpy(tb_c1002rw, tb_c1002_0, tb_c1002_0n); tb_c1002rw += tb_c1002_0n; memcpy(tb_c1002rw, tb_c1002_1, tb_c1002_1n); tb_c1002rw += tb_c1002_1n; *tb_c1002rw = 0; (const char *)tb_c1002r; }));
        }
    }
    f_emit__put(v_c, ");\n    return 0;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1003_0 = "static tb_task *tb_bg"; long long tb_c1003_0n = sizeof("static tb_task *tb_bg") - 1; const char *tb_c1003_1 = v_id; long long tb_c1003_1n = (long long)strlen(tb_c1003_1); const char *tb_c1003_2 = "("; long long tb_c1003_2n = sizeof("(") - 1; char *tb_c1003r = tb_mem(tb_c1003_0n + tb_c1003_1n + tb_c1003_2n + 1), *tb_c1003rw = tb_c1003r; memcpy(tb_c1003rw, tb_c1003_0, tb_c1003_0n); tb_c1003rw += tb_c1003_0n; memcpy(tb_c1003rw, tb_c1003_1, tb_c1003_1n); tb_c1003rw += tb_c1003_1n; memcpy(tb_c1003rw, tb_c1003_2, tb_c1003_2n); tb_c1003rw += tb_c1003_2n; *tb_c1003rw = 0; (const char *)tb_c1003r; }));
    {
        long long tb_end1004 = ((((tb_o_Node *)tb_nn(v_params, 1104))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1004; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1005_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1107))->q_kids, v_i, sizeof(tb_o_Node *), 1107)), 1107))->q_ty); long long tb_c1005_0n = (long long)strlen(tb_c1005_0); const char *tb_c1005_1 = " a"; long long tb_c1005_1n = sizeof(" a") - 1; char tb_c1005_2d[24]; long long tb_c1005_2n; const char *tb_c1005_2 = tb_digits(v_i, tb_c1005_2d, &tb_c1005_2n); char *tb_c1005r = tb_mem(tb_c1005_0n + tb_c1005_1n + tb_c1005_2n + 1), *tb_c1005rw = tb_c1005r; memcpy(tb_c1005rw, tb_c1005_0, tb_c1005_0n); tb_c1005rw += tb_c1005_0n; memcpy(tb_c1005rw, tb_c1005_1, tb_c1005_1n); tb_c1005rw += tb_c1005_1n; memcpy(tb_c1005rw, tb_c1005_2, tb_c1005_2n); tb_c1005rw += tb_c1005_2n; *tb_c1005rw = 0; (const char *)tb_c1005r; }));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_params, 1108))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    f_emit__put(v_c, ({ const char *tb_c1006_0 = ") {\n    tb_bgjob"; long long tb_c1006_0n = sizeof(") {\n    tb_bgjob") - 1; const char *tb_c1006_1 = v_id; long long tb_c1006_1n = (long long)strlen(tb_c1006_1); const char *tb_c1006_2 = " *tb_j = malloc(sizeof(tb_bgjob"; long long tb_c1006_2n = sizeof(" *tb_j = malloc(sizeof(tb_bgjob") - 1; const char *tb_c1006_3 = v_id; long long tb_c1006_3n = (long long)strlen(tb_c1006_3); const char *tb_c1006_4 = "));\n"; long long tb_c1006_4n = sizeof("));\n") - 1; char *tb_c1006r = tb_mem(tb_c1006_0n + tb_c1006_1n + tb_c1006_2n + tb_c1006_3n + tb_c1006_4n + 1), *tb_c1006rw = tb_c1006r; memcpy(tb_c1006rw, tb_c1006_0, tb_c1006_0n); tb_c1006rw += tb_c1006_0n; memcpy(tb_c1006rw, tb_c1006_1, tb_c1006_1n); tb_c1006rw += tb_c1006_1n; memcpy(tb_c1006rw, tb_c1006_2, tb_c1006_2n); tb_c1006rw += tb_c1006_2n; memcpy(tb_c1006rw, tb_c1006_3, tb_c1006_3n); tb_c1006rw += tb_c1006_3n; memcpy(tb_c1006rw, tb_c1006_4, tb_c1006_4n); tb_c1006rw += tb_c1006_4n; *tb_c1006rw = 0; (const char *)tb_c1006r; }));
    f_emit__put(v_c, "    if (tb_j == 0) tb_crash(0, \"не хватило памяти\");\n");
    {
        long long tb_end1007 = ((((tb_o_Node *)tb_nn(v_params, 1112))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1007; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1008_0 = "    tb_j->a"; long long tb_c1008_0n = sizeof("    tb_j->a") - 1; char tb_c1008_1d[24]; long long tb_c1008_1n; const char *tb_c1008_1 = tb_digits(v_i, tb_c1008_1d, &tb_c1008_1n); const char *tb_c1008_2 = " = a"; long long tb_c1008_2n = sizeof(" = a") - 1; char tb_c1008_3d[24]; long long tb_c1008_3n; const char *tb_c1008_3 = tb_digits(v_i, tb_c1008_3d, &tb_c1008_3n); const char *tb_c1008_4 = ";\n"; long long tb_c1008_4n = sizeof(";\n") - 1; char *tb_c1008r = tb_mem(tb_c1008_0n + tb_c1008_1n + tb_c1008_2n + tb_c1008_3n + tb_c1008_4n + 1), *tb_c1008rw = tb_c1008r; memcpy(tb_c1008rw, tb_c1008_0, tb_c1008_0n); tb_c1008rw += tb_c1008_0n; memcpy(tb_c1008rw, tb_c1008_1, tb_c1008_1n); tb_c1008rw += tb_c1008_1n; memcpy(tb_c1008rw, tb_c1008_2, tb_c1008_2n); tb_c1008rw += tb_c1008_2n; memcpy(tb_c1008rw, tb_c1008_3, tb_c1008_3n); tb_c1008rw += tb_c1008_3n; memcpy(tb_c1008rw, tb_c1008_4, tb_c1008_4n); tb_c1008rw += tb_c1008_4n; *tb_c1008rw = 0; (const char *)tb_c1008r; }));
        }
    }
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "    tb_j->tb_t.res = &tb_j->tb_r;\n");
    } else {
        f_emit__put(v_c, "    tb_j->tb_t.res = 0;\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1009_0 = "    tb_task_start(&tb_j->tb_t, tb_bgrun"; long long tb_c1009_0n = sizeof("    tb_task_start(&tb_j->tb_t, tb_bgrun") - 1; const char *tb_c1009_1 = v_id; long long tb_c1009_1n = (long long)strlen(tb_c1009_1); const char *tb_c1009_2 = ", tb_j);\n    return &tb_j->tb_t;\n}\n"; long long tb_c1009_2n = sizeof(", tb_j);\n    return &tb_j->tb_t;\n}\n") - 1; char *tb_c1009r = tb_mem(tb_c1009_0n + tb_c1009_1n + tb_c1009_2n + 1), *tb_c1009rw = tb_c1009r; memcpy(tb_c1009rw, tb_c1009_0, tb_c1009_0n); tb_c1009rw += tb_c1009_0n; memcpy(tb_c1009rw, tb_c1009_1, tb_c1009_1n); tb_c1009rw += tb_c1009_1n; memcpy(tb_c1009rw, tb_c1009_2, tb_c1009_2n); tb_c1009rw += tb_c1009_2n; *tb_c1009rw = 0; (const char *)tb_c1009r; }));
    { tb_list * tb_v1010 = v_saved; tb_list * *tb_p1010 = &(((tb_o_Comp *)tb_nn(v_c, 1119))->q_out); *tb_p1010 = tb_v1010; }
    { const char * tb_v1011 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1120))->q_workers) = tb_v1011; }
    f_emit__put(v_c, ({ const char *tb_c1012_0 = "tb_bg"; long long tb_c1012_0n = sizeof("tb_bg") - 1; const char *tb_c1012_1 = v_id; long long tb_c1012_1n = (long long)strlen(tb_c1012_1); const char *tb_c1012_2 = "("; long long tb_c1012_2n = sizeof("(") - 1; char *tb_c1012r = tb_mem(tb_c1012_0n + tb_c1012_1n + tb_c1012_2n + 1), *tb_c1012rw = tb_c1012r; memcpy(tb_c1012rw, tb_c1012_0, tb_c1012_0n); tb_c1012rw += tb_c1012_0n; memcpy(tb_c1012rw, tb_c1012_1, tb_c1012_1n); tb_c1012rw += tb_c1012_1n; memcpy(tb_c1012rw, tb_c1012_2, tb_c1012_2n); tb_c1012rw += tb_c1012_2n; *tb_c1012rw = 0; (const char *)tb_c1012r; }));
    {
        long long tb_end1013 = ((((tb_o_Node *)tb_nn(v_call, 1122))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1013; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 1125))->q_kids, v_i, sizeof(tb_o_Node *), 1125)), 1125))->q_kids, 0LL, sizeof(tb_o_Node *), 1125)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1125))->q_kids, v_i, sizeof(tb_o_Node *), 1125)), 1125))->q_ty);
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__is_pure(tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1130))->q_kind;
    if (((((({ const char *tb_s1014 = v_k; ((unsigned char)tb_s1014[0] == 110 && (unsigned char)tb_s1014[1] == 117 && (unsigned char)tb_s1014[2] == 109 && (unsigned char)tb_s1014[3] == 98 && (unsigned char)tb_s1014[4] == 101 && (unsigned char)tb_s1014[5] == 114 && tb_s1014[6] == 0); }) || ({ const char *tb_s1015 = v_k; ((unsigned char)tb_s1015[0] == 100 && (unsigned char)tb_s1015[1] == 101 && (unsigned char)tb_s1015[2] == 99 && (unsigned char)tb_s1015[3] == 105 && (unsigned char)tb_s1015[4] == 109 && (unsigned char)tb_s1015[5] == 97 && (unsigned char)tb_s1015[6] == 108 && tb_s1015[7] == 0); })) || ({ const char *tb_s1016 = v_k; ((unsigned char)tb_s1016[0] == 115 && (unsigned char)tb_s1016[1] == 116 && (unsigned char)tb_s1016[2] == 114 && (unsigned char)tb_s1016[3] == 105 && (unsigned char)tb_s1016[4] == 110 && (unsigned char)tb_s1016[5] == 103 && tb_s1016[6] == 0); })) || ({ const char *tb_s1017 = v_k; ((unsigned char)tb_s1017[0] == 98 && (unsigned char)tb_s1017[1] == 111 && (unsigned char)tb_s1017[2] == 111 && (unsigned char)tb_s1017[3] == 108 && tb_s1017[4] == 0); })) || ({ const char *tb_s1018 = v_k; ((unsigned char)tb_s1018[0] == 110 && (unsigned char)tb_s1018[1] == 97 && (unsigned char)tb_s1018[2] == 109 && (unsigned char)tb_s1018[3] == 101 && tb_s1018[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s1019 = v_k; ((unsigned char)tb_s1019[0] == 102 && (unsigned char)tb_s1019[1] == 105 && (unsigned char)tb_s1019[2] == 101 && (unsigned char)tb_s1019[3] == 108 && (unsigned char)tb_s1019[4] == 100 && tb_s1019[5] == 0); }) || ({ const char *tb_s1020 = v_k; ((unsigned char)tb_s1020[0] == 117 && (unsigned char)tb_s1020[1] == 110 && (unsigned char)tb_s1020[2] == 97 && (unsigned char)tb_s1020[3] == 114 && (unsigned char)tb_s1020[4] == 121 && tb_s1020[5] == 0); }))) {
        return f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1134))->q_kids, 0LL, sizeof(tb_o_Node *), 1134)));
    }
    if (({ const char *tb_s1021 = v_k; ((unsigned char)tb_s1021[0] == 98 && (unsigned char)tb_s1021[1] == 105 && (unsigned char)tb_s1021[2] == 110 && (unsigned char)tb_s1021[3] == 111 && (unsigned char)tb_s1021[4] == 112 && tb_s1021[5] == 0); })) {
        if ((({ const char *tb_s1022 = ((tb_o_Node *)tb_nn(v_n, 1136))->q_value; ((unsigned char)tb_s1022[0] == 47 && tb_s1022[1] == 0); }) || ({ const char *tb_s1023 = ((tb_o_Node *)tb_nn(v_n, 1136))->q_value; ((unsigned char)tb_s1023[0] == 37 && tb_s1023[1] == 0); }))) {
            return 0;
        }
        return (f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1138))->q_kids, 0LL, sizeof(tb_o_Node *), 1138))) && f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1138))->q_kids, 1LL, sizeof(tb_o_Node *), 1138))));
    }
    return 0;
    tb_crash(1129, "функция is_pure дошла до конца без return");
    return 0;
}

static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v1024 = 1LL; long long *tb_p1024 = &(((tb_o_Comp *)tb_nn(v_c, 1147))->q_label); *tb_p1024 += tb_v1024; }
    const char * v_id = ({ const char *tb_c1025_0 = ""; long long tb_c1025_0n = sizeof("") - 1; char tb_c1025_1d[24]; long long tb_c1025_1n; const char *tb_c1025_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1148))->q_label, tb_c1025_1d, &tb_c1025_1n); char *tb_c1025r = tb_mem(tb_c1025_0n + tb_c1025_1n + 1), *tb_c1025rw = tb_c1025r; memcpy(tb_c1025rw, tb_c1025_0, tb_c1025_0n); tb_c1025rw += tb_c1025_0n; memcpy(tb_c1025rw, tb_c1025_1, tb_c1025_1n); tb_c1025rw += tb_c1025_1n; *tb_c1025rw = 0; (const char *)tb_c1025r; });
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1149))->q_kids, 0LL, sizeof(tb_o_Node *), 1149));
    {
        long long tb_end1026 = ((((tb_o_Node *)tb_nn(v_block, 1150))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1026; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1151))->q_kids, v_j, sizeof(tb_o_Node *), 1151)), 1151))->q_kids, 0LL, sizeof(tb_o_Node *), 1151));
            if (((({ const char *tb_s1027 = ((tb_o_Node *)tb_nn(v_v, 1152))->q_kind; !((unsigned char)tb_s1027[0] == 105 && (unsigned char)tb_s1027[1] == 110 && (unsigned char)tb_s1027[2] == 100 && (unsigned char)tb_s1027[3] == 101 && (unsigned char)tb_s1027[4] == 120 && tb_s1027[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1152))->q_kids, 0LL, sizeof(tb_o_Node *), 1152))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1152))->q_kids, 1LL, sizeof(tb_o_Node *), 1152)))))) {
                continue;
            }
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1028 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1155))->q_kids, 0LL, sizeof(tb_o_Node *), 1155)), 1155))->q_ty); ((unsigned char)tb_s1028[0] == 100 && (unsigned char)tb_s1028[1] == 105 && (unsigned char)tb_s1028[2] == 99 && (unsigned char)tb_s1028[3] == 116 && tb_s1028[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1029_0 = "unsigned long long tb_h"; long long tb_c1029_0n = sizeof("unsigned long long tb_h") - 1; const char *tb_c1029_1 = v_id; long long tb_c1029_1n = (long long)strlen(tb_c1029_1); const char *tb_c1029_2 = "_"; long long tb_c1029_2n = sizeof("_") - 1; char tb_c1029_3d[24]; long long tb_c1029_3n; const char *tb_c1029_3 = tb_digits(v_j, tb_c1029_3d, &tb_c1029_3n); const char *tb_c1029_4 = " = tb_dpre1("; long long tb_c1029_4n = sizeof(" = tb_dpre1(") - 1; char *tb_c1029r = tb_mem(tb_c1029_0n + tb_c1029_1n + tb_c1029_2n + tb_c1029_3n + tb_c1029_4n + 1), *tb_c1029rw = tb_c1029r; memcpy(tb_c1029rw, tb_c1029_0, tb_c1029_0n); tb_c1029rw += tb_c1029_0n; memcpy(tb_c1029rw, tb_c1029_1, tb_c1029_1n); tb_c1029rw += tb_c1029_1n; memcpy(tb_c1029rw, tb_c1029_2, tb_c1029_2n); tb_c1029rw += tb_c1029_2n; memcpy(tb_c1029rw, tb_c1029_3, tb_c1029_3n); tb_c1029rw += tb_c1029_3n; memcpy(tb_c1029rw, tb_c1029_4, tb_c1029_4n); tb_c1029rw += tb_c1029_4n; *tb_c1029rw = 0; (const char *)tb_c1029r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1157))->q_kids, 0LL, sizeof(tb_o_Node *), 1157)));
                f_emit__put(v_c, ", ");
                f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1159))->q_kids, 0LL, sizeof(tb_o_Node *), 1159)), 1159))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1159))->q_kids, 1LL, sizeof(tb_o_Node *), 1159)));
                f_emit__put(v_c, ");\n");
            } else if ((f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1161))->q_kids, 0LL, sizeof(tb_o_Node *), 1161)), 1161))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1161))->q_kids, 0LL, sizeof(tb_o_Node *), 1161)), 1161))->q_ty)))) {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1163))->q_kids, 0LL, sizeof(tb_o_Node *), 1163)));
                f_emit__put(v_c, ", ");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1165))->q_kids, 1LL, sizeof(tb_o_Node *), 1165)));
                f_emit__put(v_c, ");\n");
            } else {
                f_emit__put(v_c, ";\n");
            }
        }
    }
    {
        long long tb_end1030 = ((((tb_o_Node *)tb_nn(v_block, 1169))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1030; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1170))->q_kids, v_j, sizeof(tb_o_Node *), 1170)), 1170))->q_kids, 0LL, sizeof(tb_o_Node *), 1170));
            if (((({ const char *tb_s1031 = ((tb_o_Node *)tb_nn(v_v, 1171))->q_kind; !((unsigned char)tb_s1031[0] == 105 && (unsigned char)tb_s1031[1] == 110 && (unsigned char)tb_s1031[2] == 100 && (unsigned char)tb_s1031[3] == 101 && (unsigned char)tb_s1031[4] == 120 && tb_s1031[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1171))->q_kids, 0LL, sizeof(tb_o_Node *), 1171))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1171))->q_kids, 1LL, sizeof(tb_o_Node *), 1171)))))) {
                continue;
            }
            if (({ const char *tb_s1032 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1173))->q_kids, 0LL, sizeof(tb_o_Node *), 1173)), 1173))->q_ty); ((unsigned char)tb_s1032[0] == 100 && (unsigned char)tb_s1032[1] == 105 && (unsigned char)tb_s1032[2] == 99 && (unsigned char)tb_s1032[3] == 116 && tb_s1032[4] == 0); })) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "tb_dpre2(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1176))->q_kids, 0LL, sizeof(tb_o_Node *), 1176)));
                f_emit__put(v_c, ({ const char *tb_c1033_0 = ", tb_h"; long long tb_c1033_0n = sizeof(", tb_h") - 1; const char *tb_c1033_1 = v_id; long long tb_c1033_1n = (long long)strlen(tb_c1033_1); const char *tb_c1033_2 = "_"; long long tb_c1033_2n = sizeof("_") - 1; char tb_c1033_3d[24]; long long tb_c1033_3n; const char *tb_c1033_3 = tb_digits(v_j, tb_c1033_3d, &tb_c1033_3n); const char *tb_c1033_4 = ");\n"; long long tb_c1033_4n = sizeof(");\n") - 1; char *tb_c1033r = tb_mem(tb_c1033_0n + tb_c1033_1n + tb_c1033_2n + tb_c1033_3n + tb_c1033_4n + 1), *tb_c1033rw = tb_c1033r; memcpy(tb_c1033rw, tb_c1033_0, tb_c1033_0n); tb_c1033rw += tb_c1033_0n; memcpy(tb_c1033rw, tb_c1033_1, tb_c1033_1n); tb_c1033rw += tb_c1033_1n; memcpy(tb_c1033rw, tb_c1033_2, tb_c1033_2n); tb_c1033rw += tb_c1033_2n; memcpy(tb_c1033rw, tb_c1033_3, tb_c1033_3n); tb_c1033rw += tb_c1033_3n; memcpy(tb_c1033rw, tb_c1033_4, tb_c1033_4n); tb_c1033rw += tb_c1033_4n; *tb_c1033rw = 0; (const char *)tb_c1033r; }));
            }
        }
    }
    {
        tb_list * tb_l1034 = ((tb_o_Node *)tb_nn(v_block, 1178))->q_kids;
        for (long long tb_i1034 = 0; tb_i1034 < tb_l1034->len; tb_i1034++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1034->data)[tb_i1034];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth, const char * v_list_c, const char * v_i_c, const char * v_elem) {
    {
        long long tb_end1035 = ((((tb_o_Node *)tb_nn(v_n, 1185))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1035; v_j++) {
            { long long tb_v1036 = 1LL; long long *tb_p1036 = &(((tb_o_Comp *)tb_nn(v_c, 1186))->q_label); *tb_p1036 += tb_v1036; }
            const char * v_f = ({ const char *tb_c1037_0 = "tb_f"; long long tb_c1037_0n = sizeof("tb_f") - 1; char tb_c1037_1d[24]; long long tb_c1037_1n; const char *tb_c1037_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1187))->q_label, tb_c1037_1d, &tb_c1037_1n); char *tb_c1037r = tb_mem(tb_c1037_0n + tb_c1037_1n + 1), *tb_c1037rw = tb_c1037r; memcpy(tb_c1037rw, tb_c1037_0, tb_c1037_0n); tb_c1037rw += tb_c1037_0n; memcpy(tb_c1037rw, tb_c1037_1, tb_c1037_1n); tb_c1037rw += tb_c1037_1n; *tb_c1037rw = 0; (const char *)tb_c1037r; });
            const char * v_plan = v_list_c;
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1038 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1190))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1190)), 1190))->q_kind; ((unsigned char)tb_s1038[0] == 110 && (unsigned char)tb_s1038[1] == 97 && (unsigned char)tb_s1038[2] == 109 && (unsigned char)tb_s1038[3] == 101 && tb_s1038[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1039_0 = "{ tb_list *tb_q"; long long tb_c1039_0n = sizeof("{ tb_list *tb_q") - 1; char tb_c1039_1d[24]; long long tb_c1039_1n; const char *tb_c1039_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1191))->q_label, tb_c1039_1d, &tb_c1039_1n); const char *tb_c1039_2 = " = "; long long tb_c1039_2n = sizeof(" = ") - 1; char *tb_c1039r = tb_mem(tb_c1039_0n + tb_c1039_1n + tb_c1039_2n + 1), *tb_c1039rw = tb_c1039r; memcpy(tb_c1039rw, tb_c1039_0, tb_c1039_0n); tb_c1039rw += tb_c1039_0n; memcpy(tb_c1039rw, tb_c1039_1, tb_c1039_1n); tb_c1039rw += tb_c1039_1n; memcpy(tb_c1039rw, tb_c1039_2, tb_c1039_2n); tb_c1039rw += tb_c1039_2n; *tb_c1039rw = 0; (const char *)tb_c1039r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1192))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1192)));
                f_emit__put(v_c, "; ");
                v_plan = ({ const char *tb_c1040_0 = "tb_q"; long long tb_c1040_0n = sizeof("tb_q") - 1; char tb_c1040_1d[24]; long long tb_c1040_1n; const char *tb_c1040_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1194))->q_label, tb_c1040_1d, &tb_c1040_1n); char *tb_c1040r = tb_mem(tb_c1040_0n + tb_c1040_1n + 1), *tb_c1040rw = tb_c1040r; memcpy(tb_c1040rw, tb_c1040_0, tb_c1040_0n); tb_c1040rw += tb_c1040_0n; memcpy(tb_c1040rw, tb_c1040_1, tb_c1040_1n); tb_c1040rw += tb_c1040_1n; *tb_c1040rw = 0; (const char *)tb_c1040r; });
            } else {
                f_emit__put(v_c, "{ ");
            }
            f_emit__put(v_c, ({ const char *tb_c1041_0 = "if ((unsigned long long)("; long long tb_c1041_0n = sizeof("if ((unsigned long long)(") - 1; const char *tb_c1041_1 = v_i_c; long long tb_c1041_1n = (long long)strlen(tb_c1041_1); const char *tb_c1041_2 = " + 16) < (unsigned long long)"; long long tb_c1041_2n = sizeof(" + 16) < (unsigned long long)") - 1; const char *tb_c1041_3 = v_plan; long long tb_c1041_3n = (long long)strlen(tb_c1041_3); const char *tb_c1041_4 = "->len) { "; long long tb_c1041_4n = sizeof("->len) { ") - 1; char *tb_c1041r = tb_mem(tb_c1041_0n + tb_c1041_1n + tb_c1041_2n + tb_c1041_3n + tb_c1041_4n + 1), *tb_c1041rw = tb_c1041r; memcpy(tb_c1041rw, tb_c1041_0, tb_c1041_0n); tb_c1041rw += tb_c1041_0n; memcpy(tb_c1041rw, tb_c1041_1, tb_c1041_1n); tb_c1041rw += tb_c1041_1n; memcpy(tb_c1041rw, tb_c1041_2, tb_c1041_2n); tb_c1041rw += tb_c1041_2n; memcpy(tb_c1041rw, tb_c1041_3, tb_c1041_3n); tb_c1041rw += tb_c1041_3n; memcpy(tb_c1041rw, tb_c1041_4, tb_c1041_4n); tb_c1041rw += tb_c1041_4n; *tb_c1041rw = 0; (const char *)tb_c1041r; }));
            const char * v_el = v_elem;
            if (({ const char *tb_s1042 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1199))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1199)), 1199))->q_kind; ((unsigned char)tb_s1042[0] == 110 && (unsigned char)tb_s1042[1] == 97 && (unsigned char)tb_s1042[2] == 109 && (unsigned char)tb_s1042[3] == 101 && tb_s1042[4] == 0); })) {
                v_el = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1200))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1200))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1200)), 1200))->q_ty, sizeof(tb_o_Type *), 1200)), 1200))->q_elem);
            }
            f_emit__put(v_c, ({ const char *tb_c1043_0 = v_el; long long tb_c1043_0n = (long long)strlen(tb_c1043_0); const char *tb_c1043_1 = " "; long long tb_c1043_1n = sizeof(" ") - 1; const char *tb_c1043_2 = v_f; long long tb_c1043_2n = (long long)strlen(tb_c1043_2); const char *tb_c1043_3 = " = (("; long long tb_c1043_3n = sizeof(" = ((") - 1; const char *tb_c1043_4 = v_el; long long tb_c1043_4n = (long long)strlen(tb_c1043_4); const char *tb_c1043_5 = " *)"; long long tb_c1043_5n = sizeof(" *)") - 1; const char *tb_c1043_6 = v_plan; long long tb_c1043_6n = (long long)strlen(tb_c1043_6); const char *tb_c1043_7 = "->data)["; long long tb_c1043_7n = sizeof("->data)[") - 1; const char *tb_c1043_8 = v_i_c; long long tb_c1043_8n = (long long)strlen(tb_c1043_8); const char *tb_c1043_9 = " + 16]; "; long long tb_c1043_9n = sizeof(" + 16]; ") - 1; char *tb_c1043r = tb_mem(tb_c1043_0n + tb_c1043_1n + tb_c1043_2n + tb_c1043_3n + tb_c1043_4n + tb_c1043_5n + tb_c1043_6n + tb_c1043_7n + tb_c1043_8n + tb_c1043_9n + 1), *tb_c1043rw = tb_c1043r; memcpy(tb_c1043rw, tb_c1043_0, tb_c1043_0n); tb_c1043rw += tb_c1043_0n; memcpy(tb_c1043rw, tb_c1043_1, tb_c1043_1n); tb_c1043rw += tb_c1043_1n; memcpy(tb_c1043rw, tb_c1043_2, tb_c1043_2n); tb_c1043rw += tb_c1043_2n; memcpy(tb_c1043rw, tb_c1043_3, tb_c1043_3n); tb_c1043rw += tb_c1043_3n; memcpy(tb_c1043rw, tb_c1043_4, tb_c1043_4n); tb_c1043rw += tb_c1043_4n; memcpy(tb_c1043rw, tb_c1043_5, tb_c1043_5n); tb_c1043rw += tb_c1043_5n; memcpy(tb_c1043rw, tb_c1043_6, tb_c1043_6n); tb_c1043rw += tb_c1043_6n; memcpy(tb_c1043rw, tb_c1043_7, tb_c1043_7n); tb_c1043rw += tb_c1043_7n; memcpy(tb_c1043rw, tb_c1043_8, tb_c1043_8n); tb_c1043rw += tb_c1043_8n; memcpy(tb_c1043rw, tb_c1043_9, tb_c1043_9n); tb_c1043rw += tb_c1043_9n; *tb_c1043rw = 0; (const char *)tb_c1043r; }));
            tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1202))->q_pre_bases, v_j, sizeof(tb_o_Node *), 1202));
            if (({ const char *tb_s1044 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 1203))->q_ty); ((unsigned char)tb_s1044[0] == 100 && (unsigned char)tb_s1044[1] == 105 && (unsigned char)tb_s1044[2] == 99 && (unsigned char)tb_s1044[3] == 116 && tb_s1044[4] == 0); })) {
                f_emit__put(v_c, "(void)tb_dpre1(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1045_0 = ", &"; long long tb_c1045_0n = sizeof(", &") - 1; const char *tb_c1045_1 = v_f; long long tb_c1045_1n = (long long)strlen(tb_c1045_1); const char *tb_c1045_2 = "); } }\n"; long long tb_c1045_2n = sizeof("); } }\n") - 1; char *tb_c1045r = tb_mem(tb_c1045_0n + tb_c1045_1n + tb_c1045_2n + 1), *tb_c1045rw = tb_c1045r; memcpy(tb_c1045rw, tb_c1045_0, tb_c1045_0n); tb_c1045rw += tb_c1045_0n; memcpy(tb_c1045rw, tb_c1045_1, tb_c1045_1n); tb_c1045rw += tb_c1045_1n; memcpy(tb_c1045rw, tb_c1045_2, tb_c1045_2n); tb_c1045rw += tb_c1045_2n; *tb_c1045rw = 0; (const char *)tb_c1045r; }));
            } else {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1046_0 = ", "; long long tb_c1046_0n = sizeof(", ") - 1; const char *tb_c1046_1 = v_f; long long tb_c1046_1n = (long long)strlen(tb_c1046_1); const char *tb_c1046_2 = "); } }\n"; long long tb_c1046_2n = sizeof("); } }\n") - 1; char *tb_c1046r = tb_mem(tb_c1046_0n + tb_c1046_1n + tb_c1046_2n + 1), *tb_c1046rw = tb_c1046r; memcpy(tb_c1046rw, tb_c1046_0, tb_c1046_0n); tb_c1046rw += tb_c1046_0n; memcpy(tb_c1046rw, tb_c1046_1, tb_c1046_1n); tb_c1046rw += tb_c1046_1n; memcpy(tb_c1046rw, tb_c1046_2, tb_c1046_2n); tb_c1046rw += tb_c1046_2n; *tb_c1046rw = 0; (const char *)tb_c1046r; }));
            }
        }
    }
}

static _Bool f_emit__short_literal(tb_o_Node * v_n) {
    if (((({ const char *tb_s1047 = ((tb_o_Node *)tb_nn(v_n, 1214))->q_kind; !((unsigned char)tb_s1047[0] == 115 && (unsigned char)tb_s1047[1] == 116 && (unsigned char)tb_s1047[2] == 114 && (unsigned char)tb_s1047[3] == 105 && (unsigned char)tb_s1047[4] == 110 && (unsigned char)tb_s1047[5] == 103 && tb_s1047[6] == 0); }) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1214))->q_value) < 2LL)) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1214))->q_value) > 10LL))) {
        return 0;
    }
    {
        const char * tb_l1048 = ((tb_o_Node *)tb_nn(v_n, 1216))->q_value;
        for (long long tb_i1048 = 0; tb_l1048[tb_i1048] != 0; tb_i1048++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l1048[tb_i1048]);
            if ((strcmp(v_ch, "\\") == 0)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1213, "функция short_literal дошла до конца без return");
    return 0;
}

static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 1222))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v1049 = 1LL; long long *tb_p1049 = &(((tb_o_Comp *)tb_nn(v_c, 1225))->q_label); *tb_p1049 += tb_v1049; }
    const char * v_id = ({ const char *tb_c1050_0 = ""; long long tb_c1050_0n = sizeof("") - 1; char tb_c1050_1d[24]; long long tb_c1050_1n; const char *tb_c1050_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1226))->q_label, tb_c1050_1d, &tb_c1050_1n); char *tb_c1050r = tb_mem(tb_c1050_0n + tb_c1050_1n + 1), *tb_c1050rw = tb_c1050r; memcpy(tb_c1050rw, tb_c1050_0, tb_c1050_0n); tb_c1050rw += tb_c1050_0n; memcpy(tb_c1050rw, tb_c1050_1, tb_c1050_1n); tb_c1050rw += tb_c1050_1n; *tb_c1050rw = 0; (const char *)tb_c1050r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1051_0 = "long long tb_end"; long long tb_c1051_0n = sizeof("long long tb_end") - 1; const char *tb_c1051_1 = v_id; long long tb_c1051_1n = (long long)strlen(tb_c1051_1); const char *tb_c1051_2 = " = "; long long tb_c1051_2n = sizeof(" = ") - 1; char *tb_c1051r = tb_mem(tb_c1051_0n + tb_c1051_1n + tb_c1051_2n + 1), *tb_c1051rw = tb_c1051r; memcpy(tb_c1051rw, tb_c1051_0, tb_c1051_0n); tb_c1051rw += tb_c1051_0n; memcpy(tb_c1051rw, tb_c1051_1, tb_c1051_1n); tb_c1051rw += tb_c1051_1n; memcpy(tb_c1051rw, tb_c1051_2, tb_c1051_2n); tb_c1051rw += tb_c1051_2n; *tb_c1051rw = 0; (const char *)tb_c1051r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1231))->q_kids, 1LL, sizeof(tb_o_Node *), 1231)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1052_0 = "for (long long v_"; long long tb_c1052_0n = sizeof("for (long long v_") - 1; const char *tb_c1052_1 = ((tb_o_Node *)tb_nn(v_n, 1234))->q_name; long long tb_c1052_1n = (long long)strlen(tb_c1052_1); const char *tb_c1052_2 = " = "; long long tb_c1052_2n = sizeof(" = ") - 1; char *tb_c1052r = tb_mem(tb_c1052_0n + tb_c1052_1n + tb_c1052_2n + 1), *tb_c1052rw = tb_c1052r; memcpy(tb_c1052rw, tb_c1052_0, tb_c1052_0n); tb_c1052rw += tb_c1052_0n; memcpy(tb_c1052rw, tb_c1052_1, tb_c1052_1n); tb_c1052rw += tb_c1052_1n; memcpy(tb_c1052rw, tb_c1052_2, tb_c1052_2n); tb_c1052rw += tb_c1052_2n; *tb_c1052rw = 0; (const char *)tb_c1052r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1235))->q_kids, 0LL, sizeof(tb_o_Node *), 1235)));
    f_emit__put(v_c, ({ const char *tb_c1053_0 = "; v_"; long long tb_c1053_0n = sizeof("; v_") - 1; const char *tb_c1053_1 = ((tb_o_Node *)tb_nn(v_n, 1236))->q_name; long long tb_c1053_1n = (long long)strlen(tb_c1053_1); const char *tb_c1053_2 = " <= tb_end"; long long tb_c1053_2n = sizeof(" <= tb_end") - 1; const char *tb_c1053_3 = v_id; long long tb_c1053_3n = (long long)strlen(tb_c1053_3); const char *tb_c1053_4 = "; v_"; long long tb_c1053_4n = sizeof("; v_") - 1; const char *tb_c1053_5 = ((tb_o_Node *)tb_nn(v_n, 1236))->q_name; long long tb_c1053_5n = (long long)strlen(tb_c1053_5); const char *tb_c1053_6 = "++) {\n"; long long tb_c1053_6n = sizeof("++) {\n") - 1; char *tb_c1053r = tb_mem(tb_c1053_0n + tb_c1053_1n + tb_c1053_2n + tb_c1053_3n + tb_c1053_4n + tb_c1053_5n + tb_c1053_6n + 1), *tb_c1053rw = tb_c1053r; memcpy(tb_c1053rw, tb_c1053_0, tb_c1053_0n); tb_c1053rw += tb_c1053_0n; memcpy(tb_c1053rw, tb_c1053_1, tb_c1053_1n); tb_c1053rw += tb_c1053_1n; memcpy(tb_c1053rw, tb_c1053_2, tb_c1053_2n); tb_c1053rw += tb_c1053_2n; memcpy(tb_c1053rw, tb_c1053_3, tb_c1053_3n); tb_c1053rw += tb_c1053_3n; memcpy(tb_c1053rw, tb_c1053_4, tb_c1053_4n); tb_c1053rw += tb_c1053_4n; memcpy(tb_c1053rw, tb_c1053_5, tb_c1053_5n); tb_c1053rw += tb_c1053_5n; memcpy(tb_c1053rw, tb_c1053_6, tb_c1053_6n); tb_c1053rw += tb_c1053_6n; *tb_c1053rw = 0; (const char *)tb_c1053r; }));
    f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), "", ({ const char *tb_c1054_0 = "v_"; long long tb_c1054_0n = sizeof("v_") - 1; const char *tb_c1054_1 = ((tb_o_Node *)tb_nn(v_n, 1237))->q_name; long long tb_c1054_1n = (long long)strlen(tb_c1054_1); char *tb_c1054r = tb_mem(tb_c1054_0n + tb_c1054_1n + 1), *tb_c1054rw = tb_c1054r; memcpy(tb_c1054rw, tb_c1054_0, tb_c1054_0n); tb_c1054rw += tb_c1054_0n; memcpy(tb_c1054rw, tb_c1054_1, tb_c1054_1n); tb_c1054rw += tb_c1054_1n; *tb_c1054rw = 0; (const char *)tb_c1054r; }), "long long");
    f_emit__emit_loop_body(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1238))->q_kids, 2LL, sizeof(tb_o_Node *), 1238)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth) {
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1246))->q_cur_func, 1246))->q_kids, 2LL, sizeof(tb_o_Node *), 1246));
    f_emit__indent(v_c, v_depth);
    if (((((tb_o_Node *)tb_nn(v_outputs, 1248))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c1055_0 = "return v_"; long long tb_c1055_0n = sizeof("return v_") - 1; const char *tb_c1055_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1249))->q_kids, 0LL, sizeof(tb_o_Node *), 1249)), 1249))->q_name; long long tb_c1055_1n = (long long)strlen(tb_c1055_1); const char *tb_c1055_2 = ";\n"; long long tb_c1055_2n = sizeof(";\n") - 1; char *tb_c1055r = tb_mem(tb_c1055_0n + tb_c1055_1n + tb_c1055_2n + 1), *tb_c1055rw = tb_c1055r; memcpy(tb_c1055rw, tb_c1055_0, tb_c1055_0n); tb_c1055rw += tb_c1055_0n; memcpy(tb_c1055rw, tb_c1055_1, tb_c1055_1n); tb_c1055rw += tb_c1055_1n; memcpy(tb_c1055rw, tb_c1055_2, tb_c1055_2n); tb_c1055rw += tb_c1055_2n; *tb_c1055rw = 0; (const char *)tb_c1055r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1056_0 = "return ("; long long tb_c1056_0n = sizeof("return (") - 1; const char *tb_c1056_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1251))->q_cur_func, 1251))->q_ty); long long tb_c1056_1n = (long long)strlen(tb_c1056_1); const char *tb_c1056_2 = "){"; long long tb_c1056_2n = sizeof("){") - 1; char *tb_c1056r = tb_mem(tb_c1056_0n + tb_c1056_1n + tb_c1056_2n + 1), *tb_c1056rw = tb_c1056r; memcpy(tb_c1056rw, tb_c1056_0, tb_c1056_0n); tb_c1056rw += tb_c1056_0n; memcpy(tb_c1056rw, tb_c1056_1, tb_c1056_1n); tb_c1056rw += tb_c1056_1n; memcpy(tb_c1056rw, tb_c1056_2, tb_c1056_2n); tb_c1056rw += tb_c1056_2n; *tb_c1056rw = 0; (const char *)tb_c1056r; }));
    {
        long long tb_end1057 = ((((tb_o_Node *)tb_nn(v_outputs, 1252))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1057; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1058_0 = "v_"; long long tb_c1058_0n = sizeof("v_") - 1; const char *tb_c1058_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1255))->q_kids, v_i, sizeof(tb_o_Node *), 1255)), 1255))->q_name; long long tb_c1058_1n = (long long)strlen(tb_c1058_1); char *tb_c1058r = tb_mem(tb_c1058_0n + tb_c1058_1n + 1), *tb_c1058rw = tb_c1058r; memcpy(tb_c1058rw, tb_c1058_0, tb_c1058_0n); tb_c1058rw += tb_c1058_0n; memcpy(tb_c1058rw, tb_c1058_1, tb_c1058_1n); tb_c1058rw += tb_c1058_1n; *tb_c1058rw = 0; (const char *)tb_c1058r; }));
        }
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__fill_form(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_body = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1260))->q_kids, 1LL, sizeof(tb_o_Node *), 1260));
    if ((((((tb_o_Node *)tb_nn(v_body, 1261))->q_kids)->len != 1LL) || (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1261))->q_kids, 0LL, sizeof(tb_o_Node *), 1261)), 1261))->q_kind, "expr_stmt") != 0))) {
        return 0;
    }
    tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1263))->q_kids, 0LL, sizeof(tb_o_Node *), 1263)), 1263))->q_kids, 0LL, sizeof(tb_o_Node *), 1263));
    if ((((({ const char *tb_s1059 = ((tb_o_Node *)tb_nn(v_e, 1264))->q_kind; !((unsigned char)tb_s1059[0] == 109 && (unsigned char)tb_s1059[1] == 101 && (unsigned char)tb_s1059[2] == 116 && (unsigned char)tb_s1059[3] == 104 && (unsigned char)tb_s1059[4] == 111 && (unsigned char)tb_s1059[5] == 100 && tb_s1059[6] == 0); }) || ({ const char *tb_s1060 = ((tb_o_Node *)tb_nn(v_e, 1264))->q_name; !((unsigned char)tb_s1060[0] == 97 && (unsigned char)tb_s1060[1] == 100 && (unsigned char)tb_s1060[2] == 100 && tb_s1060[3] == 0); })) || ({ const char *tb_s1061 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1264))->q_kids, 0LL, sizeof(tb_o_Node *), 1264)), 1264))->q_kind; !((unsigned char)tb_s1061[0] == 110 && (unsigned char)tb_s1061[1] == 97 && (unsigned char)tb_s1061[2] == 109 && (unsigned char)tb_s1061[3] == 101 && tb_s1061[4] == 0); })) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1264))->q_kids, 0LL, sizeof(tb_o_Node *), 1264)), 1264))->q_ty))) {
        return 0;
    }
    tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1266))->q_kids, 1LL, sizeof(tb_o_Node *), 1266)), 1266))->q_kids, 0LL, sizeof(tb_o_Node *), 1266));
    const char * v_k = ((tb_o_Node *)tb_nn(v_v, 1267))->q_kind;
    return (((({ const char *tb_s1062 = v_k; ((unsigned char)tb_s1062[0] == 110 && (unsigned char)tb_s1062[1] == 117 && (unsigned char)tb_s1062[2] == 109 && (unsigned char)tb_s1062[3] == 98 && (unsigned char)tb_s1062[4] == 101 && (unsigned char)tb_s1062[5] == 114 && tb_s1062[6] == 0); }) || ({ const char *tb_s1063 = v_k; ((unsigned char)tb_s1063[0] == 100 && (unsigned char)tb_s1063[1] == 101 && (unsigned char)tb_s1063[2] == 99 && (unsigned char)tb_s1063[3] == 105 && (unsigned char)tb_s1063[4] == 109 && (unsigned char)tb_s1063[5] == 97 && (unsigned char)tb_s1063[6] == 108 && tb_s1063[7] == 0); })) || ({ const char *tb_s1064 = v_k; ((unsigned char)tb_s1064[0] == 115 && (unsigned char)tb_s1064[1] == 116 && (unsigned char)tb_s1064[2] == 114 && (unsigned char)tb_s1064[3] == 105 && (unsigned char)tb_s1064[4] == 110 && (unsigned char)tb_s1064[5] == 103 && tb_s1064[6] == 0); })) || ({ const char *tb_s1065 = v_k; ((unsigned char)tb_s1065[0] == 98 && (unsigned char)tb_s1065[1] == 111 && (unsigned char)tb_s1065[2] == 111 && (unsigned char)tb_s1065[3] == 108 && tb_s1065[4] == 0); }));
    tb_crash(1259, "функция fill_form дошла до конца без return");
    return 0;
}

static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1271))->q_kind;
    if (({ const char *tb_s1066 = v_k; ((unsigned char)tb_s1066[0] == 97 && (unsigned char)tb_s1066[1] == 115 && (unsigned char)tb_s1066[2] == 115 && (unsigned char)tb_s1066[3] == 105 && (unsigned char)tb_s1066[4] == 103 && (unsigned char)tb_s1066[5] == 110 && tb_s1066[6] == 0); })) {
        f_emit__emit_assign(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1067 = v_k; ((unsigned char)tb_s1067[0] == 115 && (unsigned char)tb_s1067[1] == 101 && (unsigned char)tb_s1067[2] == 116 && tb_s1067[3] == 0); })) {
        f_emit__emit_set(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1068 = v_k; ((unsigned char)tb_s1068[0] == 108 && (unsigned char)tb_s1068[1] == 105 && (unsigned char)tb_s1068[2] == 110 && (unsigned char)tb_s1068[3] == 107 && (unsigned char)tb_s1068[4] == 100 && (unsigned char)tb_s1068[5] == 101 && (unsigned char)tb_s1068[6] == 99 && (unsigned char)tb_s1068[7] == 108 && tb_s1068[8] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1069_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 1278))->q_ty); long long tb_c1069_0n = (long long)strlen(tb_c1069_0); const char *tb_c1069_1 = " *v_"; long long tb_c1069_1n = sizeof(" *v_") - 1; const char *tb_c1069_2 = ((tb_o_Node *)tb_nn(v_n, 1278))->q_name; long long tb_c1069_2n = (long long)strlen(tb_c1069_2); const char *tb_c1069_3 = " = &("; long long tb_c1069_3n = sizeof(" = &(") - 1; char *tb_c1069r = tb_mem(tb_c1069_0n + tb_c1069_1n + tb_c1069_2n + tb_c1069_3n + 1), *tb_c1069rw = tb_c1069r; memcpy(tb_c1069rw, tb_c1069_0, tb_c1069_0n); tb_c1069rw += tb_c1069_0n; memcpy(tb_c1069rw, tb_c1069_1, tb_c1069_1n); tb_c1069rw += tb_c1069_1n; memcpy(tb_c1069rw, tb_c1069_2, tb_c1069_2n); tb_c1069rw += tb_c1069_2n; memcpy(tb_c1069rw, tb_c1069_3, tb_c1069_3n); tb_c1069rw += tb_c1069_3n; *tb_c1069rw = 0; (const char *)tb_c1069r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1279))->q_kids, 0LL, sizeof(tb_o_Node *), 1279)));
        f_emit__put(v_c, ");\n");
    } else if (({ const char *tb_s1070 = v_k; ((unsigned char)tb_s1070[0] == 109 && (unsigned char)tb_s1070[1] == 117 && (unsigned char)tb_s1070[2] == 108 && (unsigned char)tb_s1070[3] == 116 && (unsigned char)tb_s1070[4] == 105 && tb_s1070[5] == 0); })) {
        f_emit__emit_multi(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1071 = v_k; ((unsigned char)tb_s1071[0] == 105 && (unsigned char)tb_s1071[1] == 102 && tb_s1071[2] == 0); })) {
        f_emit__emit_if(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1072 = v_k; ((unsigned char)tb_s1072[0] == 101 && (unsigned char)tb_s1072[1] == 97 && (unsigned char)tb_s1072[2] == 99 && (unsigned char)tb_s1072[3] == 104 && tb_s1072[4] == 0); })) {
        f_emit__emit_each(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1073 = v_k; ((unsigned char)tb_s1073[0] == 99 && (unsigned char)tb_s1073[1] == 111 && (unsigned char)tb_s1073[2] == 117 && (unsigned char)tb_s1073[3] == 110 && (unsigned char)tb_s1073[4] == 116 && tb_s1073[5] == 0); })) {
        f_emit__emit_count(v_c, v_n, v_depth);
    } else if (((strcmp(v_k, "repeat_times") == 0) && f_emit__fill_form(v_c, v_n))) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1291))->q_kids, 1LL, sizeof(tb_o_Node *), 1291)), 1291))->q_kids, 0LL, sizeof(tb_o_Node *), 1291)), 1291))->q_kids, 0LL, sizeof(tb_o_Node *), 1291));
        tb_o_Node * v_lst = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1292))->q_kids, 0LL, sizeof(tb_o_Node *), 1292));
        tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1293))->q_kids, 1LL, sizeof(tb_o_Node *), 1293)), 1293))->q_kids, 0LL, sizeof(tb_o_Node *), 1293));
        f_emit__indent(v_c, v_depth);
        if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_lst, 1295))->q_ty)) {
            f_emit__put(v_c, "tb_bfill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1299))->q_kids, 0LL, sizeof(tb_o_Node *), 1299)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, v_value);
            f_emit__put(v_c, ");\n");
        } else {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1304))->q_types, ((tb_o_Node *)tb_nn(v_lst, 1304))->q_ty, sizeof(tb_o_Type *), 1304)), 1304))->q_elem;
            f_emit__put(v_c, "tb_fill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1308))->q_kids, 0LL, sizeof(tb_o_Node *), 1308)));
            f_emit__put(v_c, ({ const char *tb_c1074_0 = ", &("; long long tb_c1074_0n = sizeof(", &(") - 1; const char *tb_c1074_1 = f_emit__c_type(v_c, v_elem); long long tb_c1074_1n = (long long)strlen(tb_c1074_1); const char *tb_c1074_2 = "){"; long long tb_c1074_2n = sizeof("){") - 1; char *tb_c1074r = tb_mem(tb_c1074_0n + tb_c1074_1n + tb_c1074_2n + 1), *tb_c1074rw = tb_c1074r; memcpy(tb_c1074rw, tb_c1074_0, tb_c1074_0n); tb_c1074rw += tb_c1074_0n; memcpy(tb_c1074rw, tb_c1074_1, tb_c1074_1n); tb_c1074rw += tb_c1074_1n; memcpy(tb_c1074rw, tb_c1074_2, tb_c1074_2n); tb_c1074rw += tb_c1074_2n; *tb_c1074rw = 0; (const char *)tb_c1074r; }));
            f_emit__emit_as(v_c, v_value, v_elem);
            f_emit__put(v_c, "});\n");
        }
    } else if ((strcmp(v_k, "repeat_times") == 0)) {
        { long long tb_v1075 = 1LL; long long *tb_p1075 = &(((tb_o_Comp *)tb_nn(v_c, 1313))->q_label); *tb_p1075 += tb_v1075; }
        const char * v_id = ({ const char *tb_c1076_0 = ""; long long tb_c1076_0n = sizeof("") - 1; char tb_c1076_1d[24]; long long tb_c1076_1n; const char *tb_c1076_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1314))->q_label, tb_c1076_1d, &tb_c1076_1n); char *tb_c1076r = tb_mem(tb_c1076_0n + tb_c1076_1n + 1), *tb_c1076rw = tb_c1076r; memcpy(tb_c1076rw, tb_c1076_0, tb_c1076_0n); tb_c1076rw += tb_c1076_0n; memcpy(tb_c1076rw, tb_c1076_1, tb_c1076_1n); tb_c1076rw += tb_c1076_1n; *tb_c1076rw = 0; (const char *)tb_c1076r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1077_0 = "for (long long tb_r"; long long tb_c1077_0n = sizeof("for (long long tb_r") - 1; const char *tb_c1077_1 = v_id; long long tb_c1077_1n = (long long)strlen(tb_c1077_1); const char *tb_c1077_2 = " = 0, tb_n"; long long tb_c1077_2n = sizeof(" = 0, tb_n") - 1; const char *tb_c1077_3 = v_id; long long tb_c1077_3n = (long long)strlen(tb_c1077_3); const char *tb_c1077_4 = " = "; long long tb_c1077_4n = sizeof(" = ") - 1; char *tb_c1077r = tb_mem(tb_c1077_0n + tb_c1077_1n + tb_c1077_2n + tb_c1077_3n + tb_c1077_4n + 1), *tb_c1077rw = tb_c1077r; memcpy(tb_c1077rw, tb_c1077_0, tb_c1077_0n); tb_c1077rw += tb_c1077_0n; memcpy(tb_c1077rw, tb_c1077_1, tb_c1077_1n); tb_c1077rw += tb_c1077_1n; memcpy(tb_c1077rw, tb_c1077_2, tb_c1077_2n); tb_c1077rw += tb_c1077_2n; memcpy(tb_c1077rw, tb_c1077_3, tb_c1077_3n); tb_c1077rw += tb_c1077_3n; memcpy(tb_c1077rw, tb_c1077_4, tb_c1077_4n); tb_c1077rw += tb_c1077_4n; *tb_c1077rw = 0; (const char *)tb_c1077r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1317))->q_kids, 0LL, sizeof(tb_o_Node *), 1317)));
        f_emit__put(v_c, ({ const char *tb_c1078_0 = "; tb_r"; long long tb_c1078_0n = sizeof("; tb_r") - 1; const char *tb_c1078_1 = v_id; long long tb_c1078_1n = (long long)strlen(tb_c1078_1); const char *tb_c1078_2 = " < tb_n"; long long tb_c1078_2n = sizeof(" < tb_n") - 1; const char *tb_c1078_3 = v_id; long long tb_c1078_3n = (long long)strlen(tb_c1078_3); const char *tb_c1078_4 = "; tb_r"; long long tb_c1078_4n = sizeof("; tb_r") - 1; const char *tb_c1078_5 = v_id; long long tb_c1078_5n = (long long)strlen(tb_c1078_5); const char *tb_c1078_6 = "++) {\n"; long long tb_c1078_6n = sizeof("++) {\n") - 1; char *tb_c1078r = tb_mem(tb_c1078_0n + tb_c1078_1n + tb_c1078_2n + tb_c1078_3n + tb_c1078_4n + tb_c1078_5n + tb_c1078_6n + 1), *tb_c1078rw = tb_c1078r; memcpy(tb_c1078rw, tb_c1078_0, tb_c1078_0n); tb_c1078rw += tb_c1078_0n; memcpy(tb_c1078rw, tb_c1078_1, tb_c1078_1n); tb_c1078rw += tb_c1078_1n; memcpy(tb_c1078rw, tb_c1078_2, tb_c1078_2n); tb_c1078rw += tb_c1078_2n; memcpy(tb_c1078rw, tb_c1078_3, tb_c1078_3n); tb_c1078rw += tb_c1078_3n; memcpy(tb_c1078rw, tb_c1078_4, tb_c1078_4n); tb_c1078rw += tb_c1078_4n; memcpy(tb_c1078rw, tb_c1078_5, tb_c1078_5n); tb_c1078rw += tb_c1078_5n; memcpy(tb_c1078rw, tb_c1078_6, tb_c1078_6n); tb_c1078rw += tb_c1078_6n; *tb_c1078rw = 0; (const char *)tb_c1078r; }));
        f_emit__emit_loop_body(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1319))->q_kids, 1LL, sizeof(tb_o_Node *), 1319)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "repeat_while") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "while (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1325))->q_kids, 0LL, sizeof(tb_o_Node *), 1325)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_loop_body(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1327))->q_kids, 1LL, sizeof(tb_o_Node *), 1327)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s1079 = v_k; ((unsigned char)tb_s1079[0] == 116 && (unsigned char)tb_s1079[1] == 111 && (unsigned char)tb_s1079[2] == 103 && (unsigned char)tb_s1079[3] == 101 && (unsigned char)tb_s1079[4] == 116 && (unsigned char)tb_s1079[5] == 104 && (unsigned char)tb_s1079[6] == 101 && (unsigned char)tb_s1079[7] == 114 && tb_s1079[8] == 0); })) {
        f_emit__emit_together(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1080 = v_k; ((unsigned char)tb_s1080[0] == 116 && (unsigned char)tb_s1080[1] == 101 && (unsigned char)tb_s1080[2] == 109 && (unsigned char)tb_s1080[3] == 112 && tb_s1080[4] == 0); })) {
        { long long tb_v1081 = 1LL; long long *tb_p1081 = &(((tb_o_Comp *)tb_nn(v_c, 1333))->q_label); *tb_p1081 += tb_v1081; }
        const char * v_id = ({ const char *tb_c1082_0 = ""; long long tb_c1082_0n = sizeof("") - 1; char tb_c1082_1d[24]; long long tb_c1082_1n; const char *tb_c1082_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1334))->q_label, tb_c1082_1d, &tb_c1082_1n); char *tb_c1082r = tb_mem(tb_c1082_0n + tb_c1082_1n + 1), *tb_c1082rw = tb_c1082r; memcpy(tb_c1082rw, tb_c1082_0, tb_c1082_0n); tb_c1082rw += tb_c1082_0n; memcpy(tb_c1082rw, tb_c1082_1, tb_c1082_1n); tb_c1082rw += tb_c1082_1n; *tb_c1082rw = 0; (const char *)tb_c1082r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1083_0 = "tb_mark tb_k"; long long tb_c1083_0n = sizeof("tb_mark tb_k") - 1; const char *tb_c1083_1 = v_id; long long tb_c1083_1n = (long long)strlen(tb_c1083_1); const char *tb_c1083_2 = " = tb_temp_begin();\n"; long long tb_c1083_2n = sizeof(" = tb_temp_begin();\n") - 1; char *tb_c1083r = tb_mem(tb_c1083_0n + tb_c1083_1n + tb_c1083_2n + 1), *tb_c1083rw = tb_c1083r; memcpy(tb_c1083rw, tb_c1083_0, tb_c1083_0n); tb_c1083rw += tb_c1083_0n; memcpy(tb_c1083rw, tb_c1083_1, tb_c1083_1n); tb_c1083rw += tb_c1083_1n; memcpy(tb_c1083rw, tb_c1083_2, tb_c1083_2n); tb_c1083rw += tb_c1083_2n; *tb_c1083rw = 0; (const char *)tb_c1083r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1339))->q_kids, 0LL, sizeof(tb_o_Node *), 1339)), (v_depth + 1LL));
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1084_0 = "tb_temp_end(tb_k"; long long tb_c1084_0n = sizeof("tb_temp_end(tb_k") - 1; const char *tb_c1084_1 = v_id; long long tb_c1084_1n = (long long)strlen(tb_c1084_1); const char *tb_c1084_2 = ");\n"; long long tb_c1084_2n = sizeof(");\n") - 1; char *tb_c1084r = tb_mem(tb_c1084_0n + tb_c1084_1n + tb_c1084_2n + 1), *tb_c1084rw = tb_c1084r; memcpy(tb_c1084rw, tb_c1084_0, tb_c1084_0n); tb_c1084rw += tb_c1084_0n; memcpy(tb_c1084rw, tb_c1084_1, tb_c1084_1n); tb_c1084rw += tb_c1084_1n; memcpy(tb_c1084rw, tb_c1084_2, tb_c1084_2n); tb_c1084rw += tb_c1084_2n; *tb_c1084rw = 0; (const char *)tb_c1084r; }));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s1085 = v_k; ((unsigned char)tb_s1085[0] == 115 && (unsigned char)tb_s1085[1] == 107 && (unsigned char)tb_s1085[2] == 105 && (unsigned char)tb_s1085[3] == 112 && tb_s1085[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "continue;\n");
    } else if (({ const char *tb_s1086 = v_k; ((unsigned char)tb_s1086[0] == 115 && (unsigned char)tb_s1086[1] == 116 && (unsigned char)tb_s1086[2] == 111 && (unsigned char)tb_s1086[3] == 112 && tb_s1086[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "break;\n");
    } else if (({ const char *tb_s1087 = v_k; ((unsigned char)tb_s1087[0] == 102 && (unsigned char)tb_s1087[1] == 97 && (unsigned char)tb_s1087[2] == 105 && (unsigned char)tb_s1087[3] == 108 && tb_s1087[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{ tb_err = ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1353))->q_kids, 0LL, sizeof(tb_o_Node *), 1353)));
        f_emit__put(v_c, "; ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
    } else if ((({ const char *tb_s1088 = v_k; ((unsigned char)tb_s1088[0] == 114 && (unsigned char)tb_s1088[1] == 101 && (unsigned char)tb_s1088[2] == 116 && (unsigned char)tb_s1088[3] == 117 && (unsigned char)tb_s1088[4] == 114 && (unsigned char)tb_s1088[5] == 110 && tb_s1088[6] == 0); }) && ({ const char *tb_s1089 = ((tb_o_Comp *)tb_nn(v_c, 1357))->q_fmark; !(tb_s1089[0] == 0); }))) {
        if (((((tb_o_Node *)tb_nn(v_n, 1359))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1360))->q_kids, 0LL, sizeof(tb_o_Node *), 1360)), v_depth);
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, ({ const char *tb_c1090_0 = "{ "; long long tb_c1090_0n = sizeof("{ ") - 1; const char *tb_c1090_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1362))->q_cur_func, 1362))->q_ty); long long tb_c1090_1n = (long long)strlen(tb_c1090_1); const char *tb_c1090_2 = " tb_rv = "; long long tb_c1090_2n = sizeof(" tb_rv = ") - 1; char *tb_c1090r = tb_mem(tb_c1090_0n + tb_c1090_1n + tb_c1090_2n + 1), *tb_c1090rw = tb_c1090r; memcpy(tb_c1090rw, tb_c1090_0, tb_c1090_0n); tb_c1090rw += tb_c1090_0n; memcpy(tb_c1090rw, tb_c1090_1, tb_c1090_1n); tb_c1090rw += tb_c1090_1n; memcpy(tb_c1090rw, tb_c1090_2, tb_c1090_2n); tb_c1090rw += tb_c1090_2n; *tb_c1090rw = 0; (const char *)tb_c1090r; }));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1363))->q_kids, 0LL, sizeof(tb_o_Node *), 1363)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1363))->q_cur_func, 1363))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1091_0 = "; if (tb_fa) tb_temp_end("; long long tb_c1091_0n = sizeof("; if (tb_fa) tb_temp_end(") - 1; const char *tb_c1091_1 = ((tb_o_Comp *)tb_nn(v_c, 1364))->q_fmark; long long tb_c1091_1n = (long long)strlen(tb_c1091_1); const char *tb_c1091_2 = "); return tb_rv; }\n"; long long tb_c1091_2n = sizeof("); return tb_rv; }\n") - 1; char *tb_c1091r = tb_mem(tb_c1091_0n + tb_c1091_1n + tb_c1091_2n + 1), *tb_c1091rw = tb_c1091r; memcpy(tb_c1091rw, tb_c1091_0, tb_c1091_0n); tb_c1091rw += tb_c1091_0n; memcpy(tb_c1091rw, tb_c1091_1, tb_c1091_1n); tb_c1091rw += tb_c1091_1n; memcpy(tb_c1091rw, tb_c1091_2, tb_c1091_2n); tb_c1091rw += tb_c1091_2n; *tb_c1091rw = 0; (const char *)tb_c1091r; }));
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, ({ const char *tb_c1092_0 = "if (tb_fa) tb_temp_end("; long long tb_c1092_0n = sizeof("if (tb_fa) tb_temp_end(") - 1; const char *tb_c1092_1 = ((tb_o_Comp *)tb_nn(v_c, 1367))->q_fmark; long long tb_c1092_1n = (long long)strlen(tb_c1092_1); const char *tb_c1092_2 = ");\n"; long long tb_c1092_2n = sizeof(");\n") - 1; char *tb_c1092r = tb_mem(tb_c1092_0n + tb_c1092_1n + tb_c1092_2n + 1), *tb_c1092rw = tb_c1092r; memcpy(tb_c1092rw, tb_c1092_0, tb_c1092_0n); tb_c1092rw += tb_c1092_0n; memcpy(tb_c1092rw, tb_c1092_1, tb_c1092_1n); tb_c1092rw += tb_c1092_1n; memcpy(tb_c1092rw, tb_c1092_2, tb_c1092_2n); tb_c1092rw += tb_c1092_2n; *tb_c1092rw = 0; (const char *)tb_c1092r; }));
            if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1368))->q_cur_func, 1368))->q_kids, 2LL, sizeof(tb_o_Node *), 1368)), 1368))->q_kids)->len > 0LL)) {
                f_emit__emit_gives_return(v_c, v_depth);
            } else {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "return;\n");
            }
        }
    } else if (({ const char *tb_s1093 = v_k; ((unsigned char)tb_s1093[0] == 114 && (unsigned char)tb_s1093[1] == 101 && (unsigned char)tb_s1093[2] == 116 && (unsigned char)tb_s1093[3] == 117 && (unsigned char)tb_s1093[4] == 114 && (unsigned char)tb_s1093[5] == 110 && tb_s1093[6] == 0); })) {
        if (((((tb_o_Node *)tb_nn(v_n, 1374))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1375))->q_kids, 0LL, sizeof(tb_o_Node *), 1375)), v_depth);
        }
        if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1376))->q_cur_func, 1376))->q_kids, 2LL, sizeof(tb_o_Node *), 1376)), 1376))->q_kids)->len > 0LL) && ((((tb_o_Node *)tb_nn(v_n, 1376))->q_kids)->len == 0LL))) {
            f_emit__emit_gives_return(v_c, v_depth);
        } else if (((((tb_o_Node *)tb_nn(v_n, 1378))->q_kids)->len == 1LL)) {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1381))->q_kids, 0LL, sizeof(tb_o_Node *), 1381)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1381))->q_cur_func, 1381))->q_ty);
            f_emit__put(v_c, ";\n");
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return;\n");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1387))->q_kids, 0LL, sizeof(tb_o_Node *), 1387));
        if ((({ const char *tb_s1094 = ((tb_o_Node *)tb_nn(v_e, 1388))->q_kind; ((unsigned char)tb_s1094[0] == 116 && (unsigned char)tb_s1094[1] == 114 && (unsigned char)tb_s1094[2] == 121 && tb_s1094[3] == 0); }) || ({ const char *tb_s1095 = ((tb_o_Node *)tb_nn(v_e, 1388))->q_kind; ((unsigned char)tb_s1095[0] == 101 && (unsigned char)tb_s1095[1] == 108 && (unsigned char)tb_s1095[2] == 115 && (unsigned char)tb_s1095[3] == 101 && (unsigned char)tb_s1095[4] == 95 && (unsigned char)tb_s1095[5] == 101 && (unsigned char)tb_s1095[6] == 114 && (unsigned char)tb_s1095[7] == 114 && tb_s1095[8] == 0); }))) {
            f_emit__emit_pre(v_c, v_e, v_depth);
            return;
        }
        f_emit__indent(v_c, v_depth);
        if ((({ const char *tb_s1096 = ((tb_o_Node *)tb_nn(v_e, 1392))->q_kind; ((unsigned char)tb_s1096[0] == 109 && (unsigned char)tb_s1096[1] == 101 && (unsigned char)tb_s1096[2] == 116 && (unsigned char)tb_s1096[3] == 104 && (unsigned char)tb_s1096[4] == 111 && (unsigned char)tb_s1096[5] == 100 && tb_s1096[6] == 0); }) && ({ const char *tb_s1097 = ((tb_o_Node *)tb_nn(v_e, 1392))->q_name; ((unsigned char)tb_s1097[0] == 97 && (unsigned char)tb_s1097[1] == 100 && (unsigned char)tb_s1097[2] == 100 && tb_s1097[3] == 0); }))) {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1394))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1394))->q_kids, 0LL, sizeof(tb_o_Node *), 1394)), 1394))->q_ty, sizeof(tb_o_Type *), 1394)), 1394))->q_elem;
            { long long tb_v1098 = 1LL; long long *tb_p1098 = &(((tb_o_Comp *)tb_nn(v_c, 1395))->q_label); *tb_p1098 += tb_v1098; }
            const char * v_id = ({ const char *tb_c1099_0 = ""; long long tb_c1099_0n = sizeof("") - 1; char tb_c1099_1d[24]; long long tb_c1099_1n; const char *tb_c1099_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1396))->q_label, tb_c1099_1d, &tb_c1099_1n); char *tb_c1099r = tb_mem(tb_c1099_0n + tb_c1099_1n + 1), *tb_c1099rw = tb_c1099r; memcpy(tb_c1099rw, tb_c1099_0, tb_c1099_0n); tb_c1099rw += tb_c1099_0n; memcpy(tb_c1099rw, tb_c1099_1, tb_c1099_1n); tb_c1099rw += tb_c1099_1n; *tb_c1099rw = 0; (const char *)tb_c1099r; });
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1397))->q_kids, 1LL, sizeof(tb_o_Node *), 1397)), 1397))->q_kids, 0LL, sizeof(tb_o_Node *), 1397));
            if ((((f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1398))->q_kids, 0LL, sizeof(tb_o_Node *), 1398)), 1398))->q_ty) && ({ const char *tb_s1100 = ((tb_o_Node *)tb_nn(v_value, 1398))->q_kind; ((unsigned char)tb_s1100[0] == 99 && (unsigned char)tb_s1100[1] == 97 && (unsigned char)tb_s1100[2] == 108 && (unsigned char)tb_s1100[3] == 108 && tb_s1100[4] == 0); })) && ((tb_o_Node *)tb_nn(v_value, 1398))->q_has_ref) && ({ const char *tb_s1101 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_value, 1398))->q_ref, 1398))->q_kind; ((unsigned char)tb_s1101[0] == 111 && (unsigned char)tb_s1101[1] == 98 && (unsigned char)tb_s1101[2] == 106 && tb_s1101[3] == 0); }))) {
                tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_value, 1400))->q_ref;
                f_emit__put(v_c, ({ const char *tb_c1102_0 = "tb_colpush_"; long long tb_c1102_0n = sizeof("tb_colpush_") - 1; const char *tb_c1102_1 = ((tb_o_Node *)tb_nn(v_o, 1401))->q_name; long long tb_c1102_1n = (long long)strlen(tb_c1102_1); const char *tb_c1102_2 = "("; long long tb_c1102_2n = sizeof("(") - 1; char *tb_c1102r = tb_mem(tb_c1102_0n + tb_c1102_1n + tb_c1102_2n + 1), *tb_c1102rw = tb_c1102r; memcpy(tb_c1102rw, tb_c1102_0, tb_c1102_0n); tb_c1102rw += tb_c1102_0n; memcpy(tb_c1102rw, tb_c1102_1, tb_c1102_1n); tb_c1102rw += tb_c1102_1n; memcpy(tb_c1102rw, tb_c1102_2, tb_c1102_2n); tb_c1102rw += tb_c1102_2n; *tb_c1102rw = 0; (const char *)tb_c1102r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1402))->q_kids, 0LL, sizeof(tb_o_Node *), 1402)));
                {
                    long long tb_end1103 = ((((tb_o_Node *)tb_nn(v_o, 1403))->q_kids)->len - 1LL);
                    for (long long v_i = 0LL; v_i <= tb_end1103; v_i++) {
                        f_emit__put(v_c, ", ");
                        if (({ const char *tb_s1104 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1405))->q_kids, v_i, sizeof(tb_o_Node *), 1405)), 1405))->q_kind; ((unsigned char)tb_s1104[0] == 109 && (unsigned char)tb_s1104[1] == 105 && (unsigned char)tb_s1104[2] == 115 && (unsigned char)tb_s1104[3] == 115 && (unsigned char)tb_s1104[4] == 105 && (unsigned char)tb_s1104[5] == 110 && (unsigned char)tb_s1104[6] == 103 && tb_s1104[7] == 0); })) {
                            f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1406))->q_kids, v_i, sizeof(tb_o_Node *), 1406)), 1406))->q_ty);
                        } else {
                            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1408))->q_kids, v_i, sizeof(tb_o_Node *), 1408)), 1408))->q_kids, 0LL, sizeof(tb_o_Node *), 1408)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1408))->q_kids, v_i, sizeof(tb_o_Node *), 1408)), 1408))->q_ty);
                        }
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1105_0 = ", "; long long tb_c1105_0n = sizeof(", ") - 1; char tb_c1105_1d[24]; long long tb_c1105_1n; const char *tb_c1105_1 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1409))->q_line, tb_c1105_1d, &tb_c1105_1n); const char *tb_c1105_2 = ");\n"; long long tb_c1105_2n = sizeof(");\n") - 1; char *tb_c1105r = tb_mem(tb_c1105_0n + tb_c1105_1n + tb_c1105_2n + 1), *tb_c1105rw = tb_c1105r; memcpy(tb_c1105rw, tb_c1105_0, tb_c1105_0n); tb_c1105rw += tb_c1105_0n; memcpy(tb_c1105rw, tb_c1105_1, tb_c1105_1n); tb_c1105rw += tb_c1105_1n; memcpy(tb_c1105rw, tb_c1105_2, tb_c1105_2n); tb_c1105rw += tb_c1105_2n; *tb_c1105rw = 0; (const char *)tb_c1105r; }));
                return;
            }
            if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1411))->q_kids, 0LL, sizeof(tb_o_Node *), 1411)), 1411))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1106_0 = "{ "; long long tb_c1106_0n = sizeof("{ ") - 1; const char *tb_c1106_1 = f_emit__c_type(v_c, v_elem); long long tb_c1106_1n = (long long)strlen(tb_c1106_1); const char *tb_c1106_2 = " tb_v"; long long tb_c1106_2n = sizeof(" tb_v") - 1; const char *tb_c1106_3 = v_id; long long tb_c1106_3n = (long long)strlen(tb_c1106_3); const char *tb_c1106_4 = " = "; long long tb_c1106_4n = sizeof(" = ") - 1; char *tb_c1106r = tb_mem(tb_c1106_0n + tb_c1106_1n + tb_c1106_2n + tb_c1106_3n + tb_c1106_4n + 1), *tb_c1106rw = tb_c1106r; memcpy(tb_c1106rw, tb_c1106_0, tb_c1106_0n); tb_c1106rw += tb_c1106_0n; memcpy(tb_c1106rw, tb_c1106_1, tb_c1106_1n); tb_c1106rw += tb_c1106_1n; memcpy(tb_c1106rw, tb_c1106_2, tb_c1106_2n); tb_c1106rw += tb_c1106_2n; memcpy(tb_c1106rw, tb_c1106_3, tb_c1106_3n); tb_c1106rw += tb_c1106_3n; memcpy(tb_c1106rw, tb_c1106_4, tb_c1106_4n); tb_c1106rw += tb_c1106_4n; *tb_c1106rw = 0; (const char *)tb_c1106r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1413))->q_kids, 1LL, sizeof(tb_o_Node *), 1413)), 1413))->q_kids, 0LL, sizeof(tb_o_Node *), 1413)));
                f_emit__put(v_c, ({ const char *tb_c1107_0 = "; tb_coladd_"; long long tb_c1107_0n = sizeof("; tb_coladd_") - 1; const char *tb_c1107_1 = f_emit__cols_name(v_c, v_elem); long long tb_c1107_1n = (long long)strlen(tb_c1107_1); const char *tb_c1107_2 = "("; long long tb_c1107_2n = sizeof("(") - 1; char *tb_c1107r = tb_mem(tb_c1107_0n + tb_c1107_1n + tb_c1107_2n + 1), *tb_c1107rw = tb_c1107r; memcpy(tb_c1107rw, tb_c1107_0, tb_c1107_0n); tb_c1107rw += tb_c1107_0n; memcpy(tb_c1107rw, tb_c1107_1, tb_c1107_1n); tb_c1107rw += tb_c1107_1n; memcpy(tb_c1107rw, tb_c1107_2, tb_c1107_2n); tb_c1107rw += tb_c1107_2n; *tb_c1107rw = 0; (const char *)tb_c1107r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1415))->q_kids, 0LL, sizeof(tb_o_Node *), 1415)));
                f_emit__put(v_c, ({ const char *tb_c1108_0 = ", tb_v"; long long tb_c1108_0n = sizeof(", tb_v") - 1; const char *tb_c1108_1 = v_id; long long tb_c1108_1n = (long long)strlen(tb_c1108_1); const char *tb_c1108_2 = ", "; long long tb_c1108_2n = sizeof(", ") - 1; char tb_c1108_3d[24]; long long tb_c1108_3n; const char *tb_c1108_3 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1416))->q_line, tb_c1108_3d, &tb_c1108_3n); const char *tb_c1108_4 = "); }\n"; long long tb_c1108_4n = sizeof("); }\n") - 1; char *tb_c1108r = tb_mem(tb_c1108_0n + tb_c1108_1n + tb_c1108_2n + tb_c1108_3n + tb_c1108_4n + 1), *tb_c1108rw = tb_c1108r; memcpy(tb_c1108rw, tb_c1108_0, tb_c1108_0n); tb_c1108rw += tb_c1108_0n; memcpy(tb_c1108rw, tb_c1108_1, tb_c1108_1n); tb_c1108rw += tb_c1108_1n; memcpy(tb_c1108rw, tb_c1108_2, tb_c1108_2n); tb_c1108rw += tb_c1108_2n; memcpy(tb_c1108rw, tb_c1108_3, tb_c1108_3n); tb_c1108rw += tb_c1108_3n; memcpy(tb_c1108rw, tb_c1108_4, tb_c1108_4n); tb_c1108rw += tb_c1108_4n; *tb_c1108rw = 0; (const char *)tb_c1108r; }));
                return;
            }
            if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1418))->q_kids, 0LL, sizeof(tb_o_Node *), 1418)), 1418))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1109_0 = "{ _Bool tb_v"; long long tb_c1109_0n = sizeof("{ _Bool tb_v") - 1; const char *tb_c1109_1 = v_id; long long tb_c1109_1n = (long long)strlen(tb_c1109_1); const char *tb_c1109_2 = " = "; long long tb_c1109_2n = sizeof(" = ") - 1; char *tb_c1109r = tb_mem(tb_c1109_0n + tb_c1109_1n + tb_c1109_2n + 1), *tb_c1109rw = tb_c1109r; memcpy(tb_c1109rw, tb_c1109_0, tb_c1109_0n); tb_c1109rw += tb_c1109_0n; memcpy(tb_c1109rw, tb_c1109_1, tb_c1109_1n); tb_c1109rw += tb_c1109_1n; memcpy(tb_c1109rw, tb_c1109_2, tb_c1109_2n); tb_c1109rw += tb_c1109_2n; *tb_c1109rw = 0; (const char *)tb_c1109r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1420))->q_kids, 1LL, sizeof(tb_o_Node *), 1420)), 1420))->q_kids, 0LL, sizeof(tb_o_Node *), 1420)));
                f_emit__put(v_c, "; tb_badd(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1422))->q_kids, 0LL, sizeof(tb_o_Node *), 1422)));
                f_emit__put(v_c, ({ const char *tb_c1110_0 = ", tb_v"; long long tb_c1110_0n = sizeof(", tb_v") - 1; const char *tb_c1110_1 = v_id; long long tb_c1110_1n = (long long)strlen(tb_c1110_1); const char *tb_c1110_2 = "); }\n"; long long tb_c1110_2n = sizeof("); }\n") - 1; char *tb_c1110r = tb_mem(tb_c1110_0n + tb_c1110_1n + tb_c1110_2n + 1), *tb_c1110rw = tb_c1110r; memcpy(tb_c1110rw, tb_c1110_0, tb_c1110_0n); tb_c1110rw += tb_c1110_0n; memcpy(tb_c1110rw, tb_c1110_1, tb_c1110_1n); tb_c1110rw += tb_c1110_1n; memcpy(tb_c1110rw, tb_c1110_2, tb_c1110_2n); tb_c1110rw += tb_c1110_2n; *tb_c1110rw = 0; (const char *)tb_c1110r; }));
                return;
            }
            f_emit__put(v_c, ({ const char *tb_c1111_0 = "{ "; long long tb_c1111_0n = sizeof("{ ") - 1; const char *tb_c1111_1 = f_emit__c_type(v_c, v_elem); long long tb_c1111_1n = (long long)strlen(tb_c1111_1); const char *tb_c1111_2 = " tb_v"; long long tb_c1111_2n = sizeof(" tb_v") - 1; const char *tb_c1111_3 = v_id; long long tb_c1111_3n = (long long)strlen(tb_c1111_3); const char *tb_c1111_4 = " = "; long long tb_c1111_4n = sizeof(" = ") - 1; char *tb_c1111r = tb_mem(tb_c1111_0n + tb_c1111_1n + tb_c1111_2n + tb_c1111_3n + tb_c1111_4n + 1), *tb_c1111rw = tb_c1111r; memcpy(tb_c1111rw, tb_c1111_0, tb_c1111_0n); tb_c1111rw += tb_c1111_0n; memcpy(tb_c1111rw, tb_c1111_1, tb_c1111_1n); tb_c1111rw += tb_c1111_1n; memcpy(tb_c1111rw, tb_c1111_2, tb_c1111_2n); tb_c1111rw += tb_c1111_2n; memcpy(tb_c1111rw, tb_c1111_3, tb_c1111_3n); tb_c1111rw += tb_c1111_3n; memcpy(tb_c1111rw, tb_c1111_4, tb_c1111_4n); tb_c1111rw += tb_c1111_4n; *tb_c1111rw = 0; (const char *)tb_c1111r; }));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1426))->q_kids, 1LL, sizeof(tb_o_Node *), 1426)), 1426))->q_kids, 0LL, sizeof(tb_o_Node *), 1426)), v_elem);
            f_emit__put(v_c, ({ const char *tb_c1112_0 = "; *("; long long tb_c1112_0n = sizeof("; *(") - 1; const char *tb_c1112_1 = f_emit__c_type(v_c, v_elem); long long tb_c1112_1n = (long long)strlen(tb_c1112_1); const char *tb_c1112_2 = " *)tb_grow("; long long tb_c1112_2n = sizeof(" *)tb_grow(") - 1; char *tb_c1112r = tb_mem(tb_c1112_0n + tb_c1112_1n + tb_c1112_2n + 1), *tb_c1112rw = tb_c1112r; memcpy(tb_c1112rw, tb_c1112_0, tb_c1112_0n); tb_c1112rw += tb_c1112_0n; memcpy(tb_c1112rw, tb_c1112_1, tb_c1112_1n); tb_c1112rw += tb_c1112_1n; memcpy(tb_c1112rw, tb_c1112_2, tb_c1112_2n); tb_c1112rw += tb_c1112_2n; *tb_c1112rw = 0; (const char *)tb_c1112r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1428))->q_kids, 0LL, sizeof(tb_o_Node *), 1428)));
            f_emit__put(v_c, ({ const char *tb_c1113_0 = ") = tb_v"; long long tb_c1113_0n = sizeof(") = tb_v") - 1; const char *tb_c1113_1 = v_id; long long tb_c1113_1n = (long long)strlen(tb_c1113_1); const char *tb_c1113_2 = "; }\n"; long long tb_c1113_2n = sizeof("; }\n") - 1; char *tb_c1113r = tb_mem(tb_c1113_0n + tb_c1113_1n + tb_c1113_2n + 1), *tb_c1113rw = tb_c1113r; memcpy(tb_c1113rw, tb_c1113_0, tb_c1113_0n); tb_c1113rw += tb_c1113_0n; memcpy(tb_c1113rw, tb_c1113_1, tb_c1113_1n); tb_c1113rw += tb_c1113_1n; memcpy(tb_c1113rw, tb_c1113_2, tb_c1113_2n); tb_c1113rw += tb_c1113_2n; *tb_c1113rw = 0; (const char *)tb_c1113r; }));
        } else {
            f_emit__emit_expr(v_c, v_e);
            f_emit__put(v_c, ";\n");
        }
    }
}

static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth) {
    {
        tb_list * tb_l1114 = ((tb_o_Node *)tb_nn(v_block, 1435))->q_kids;
        for (long long tb_i1114 = 0; tb_i1114 < tb_l1114->len; tb_i1114++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1114->data)[tb_i1114];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1441))->q_kids, 0LL, sizeof(tb_o_Node *), 1441));
    f_emit__put(v_c, ({ const char *tb_c1115_0 = "static "; long long tb_c1115_0n = sizeof("static ") - 1; const char *tb_c1115_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1442))->q_ty); long long tb_c1115_1n = (long long)strlen(tb_c1115_1); const char *tb_c1115_2 = " "; long long tb_c1115_2n = sizeof(" ") - 1; const char *tb_c1115_3 = f_emit__fname(v_f); long long tb_c1115_3n = (long long)strlen(tb_c1115_3); const char *tb_c1115_4 = "("; long long tb_c1115_4n = sizeof("(") - 1; char *tb_c1115r = tb_mem(tb_c1115_0n + tb_c1115_1n + tb_c1115_2n + tb_c1115_3n + tb_c1115_4n + 1), *tb_c1115rw = tb_c1115r; memcpy(tb_c1115rw, tb_c1115_0, tb_c1115_0n); tb_c1115rw += tb_c1115_0n; memcpy(tb_c1115rw, tb_c1115_1, tb_c1115_1n); tb_c1115rw += tb_c1115_1n; memcpy(tb_c1115rw, tb_c1115_2, tb_c1115_2n); tb_c1115rw += tb_c1115_2n; memcpy(tb_c1115rw, tb_c1115_3, tb_c1115_3n); tb_c1115rw += tb_c1115_3n; memcpy(tb_c1115rw, tb_c1115_4, tb_c1115_4n); tb_c1115rw += tb_c1115_4n; *tb_c1115rw = 0; (const char *)tb_c1115r; }));
    if (((((tb_o_Node *)tb_nn(v_params, 1443))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1116 = ((((tb_o_Node *)tb_nn(v_params, 1445))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1116; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1446))->q_kids, v_i, sizeof(tb_o_Node *), 1446));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn(v_p, 1449))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c1117_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1450))->q_ty); long long tb_c1117_0n = (long long)strlen(tb_c1117_0); const char *tb_c1117_1 = " *v_"; long long tb_c1117_1n = sizeof(" *v_") - 1; const char *tb_c1117_2 = ((tb_o_Node *)tb_nn(v_p, 1450))->q_name; long long tb_c1117_2n = (long long)strlen(tb_c1117_2); char *tb_c1117r = tb_mem(tb_c1117_0n + tb_c1117_1n + tb_c1117_2n + 1), *tb_c1117rw = tb_c1117r; memcpy(tb_c1117rw, tb_c1117_0, tb_c1117_0n); tb_c1117rw += tb_c1117_0n; memcpy(tb_c1117rw, tb_c1117_1, tb_c1117_1n); tb_c1117rw += tb_c1117_1n; memcpy(tb_c1117rw, tb_c1117_2, tb_c1117_2n); tb_c1117rw += tb_c1117_2n; *tb_c1117rw = 0; (const char *)tb_c1117r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c1118_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1452))->q_ty); long long tb_c1118_0n = (long long)strlen(tb_c1118_0); const char *tb_c1118_1 = " v_"; long long tb_c1118_1n = sizeof(" v_") - 1; const char *tb_c1118_2 = ((tb_o_Node *)tb_nn(v_p, 1452))->q_name; long long tb_c1118_2n = (long long)strlen(tb_c1118_2); char *tb_c1118r = tb_mem(tb_c1118_0n + tb_c1118_1n + tb_c1118_2n + 1), *tb_c1118rw = tb_c1118r; memcpy(tb_c1118rw, tb_c1118_0, tb_c1118_0n); tb_c1118rw += tb_c1118_0n; memcpy(tb_c1118rw, tb_c1118_1, tb_c1118_1n); tb_c1118rw += tb_c1118_1n; memcpy(tb_c1118rw, tb_c1118_2, tb_c1118_2n); tb_c1118rw += tb_c1118_2n; *tb_c1118rw = 0; (const char *)tb_c1118r; }));
            }
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__bld_has(tb_list * v_names, const char * v_name) {
    {
        tb_list * tb_l1119 = v_names;
        for (long long tb_i1119 = 0; tb_i1119 < tb_l1119->len; tb_i1119++) {
            const char * const v_x = ((const char * *)tb_l1119->data)[tb_i1119];
            if ((strcmp(v_x, v_name) == 0)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1462, "функция bld_has дошла до конца без return");
    return 0;
}

static _Bool f_emit__bld_uses(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s1120 = ((tb_o_Node *)tb_nn(v_n, 1469))->q_kind; ((unsigned char)tb_s1120[0] == 110 && (unsigned char)tb_s1120[1] == 97 && (unsigned char)tb_s1120[2] == 109 && (unsigned char)tb_s1120[3] == 101 && tb_s1120[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1469))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l1121 = ((tb_o_Node *)tb_nn(v_n, 1471))->q_kids;
        for (long long tb_i1121 = 0; tb_i1121 < tb_l1121->len; tb_i1121++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1121->data)[tb_i1121];
            if (f_emit__bld_uses(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1468, "функция bld_uses дошла до конца без return");
    return 0;
}

static void f_emit__bld_bad_all(tb_o_Node * v_n, tb_list * v_bad) {
    if ((({ const char *tb_s1122 = ((tb_o_Node *)tb_nn(v_n, 1478))->q_kind; ((unsigned char)tb_s1122[0] == 110 && (unsigned char)tb_s1122[1] == 97 && (unsigned char)tb_s1122[2] == 109 && (unsigned char)tb_s1122[3] == 101 && tb_s1122[4] == 0); }) || ({ const char *tb_s1123 = ((tb_o_Node *)tb_nn(v_n, 1478))->q_kind; ((unsigned char)tb_s1123[0] == 97 && (unsigned char)tb_s1123[1] == 115 && (unsigned char)tb_s1123[2] == 115 && (unsigned char)tb_s1123[3] == 105 && (unsigned char)tb_s1123[4] == 103 && (unsigned char)tb_s1123[5] == 110 && tb_s1123[6] == 0); }))) {
        { const char * tb_v1124 = ((tb_o_Node *)tb_nn(v_n, 1479))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1124; }
    }
    {
        tb_list * tb_l1125 = ((tb_o_Node *)tb_nn(v_n, 1480))->q_kids;
        for (long long tb_i1125 = 0; tb_i1125 < tb_l1125->len; tb_i1125++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1125->data)[tb_i1125];
            f_emit__bld_bad_all(v_k, v_bad);
        }
    }
}

static _Bool f_emit__is_text_plus(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((({ const char *tb_s1126 = ((tb_o_Node *)tb_nn(v_n, 1484))->q_kind; ((unsigned char)tb_s1126[0] == 98 && (unsigned char)tb_s1126[1] == 105 && (unsigned char)tb_s1126[2] == 110 && (unsigned char)tb_s1126[3] == 111 && (unsigned char)tb_s1126[4] == 112 && tb_s1126[5] == 0); }) && ({ const char *tb_s1127 = ((tb_o_Node *)tb_nn(v_n, 1484))->q_value; ((unsigned char)tb_s1127[0] == 43 && tb_s1127[1] == 0); })) && ({ const char *tb_s1128 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1484))->q_ty); ((unsigned char)tb_s1128[0] == 116 && (unsigned char)tb_s1128[1] == 101 && (unsigned char)tb_s1128[2] == 120 && (unsigned char)tb_s1128[3] == 116 && tb_s1128[4] == 0); }));
    tb_crash(1483, "функция is_text_plus дошла до конца без return");
    return 0;
}

static _Bool f_emit__app_form(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_v) {
    if ((!f_emit__is_text_plus(v_c, v_v))) {
        return 0;
    }
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        if (f_emit__bld_uses((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1492))->q_kids, 1LL, sizeof(tb_o_Node *), 1492)), v_name)) {
            return 0;
        }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1494))->q_kids, 0LL, sizeof(tb_o_Node *), 1494));
    }
    return ((({ const char *tb_s1129 = ((tb_o_Node *)tb_nn(v_r, 1495))->q_kind; ((unsigned char)tb_s1129[0] == 110 && (unsigned char)tb_s1129[1] == 97 && (unsigned char)tb_s1129[2] == 109 && (unsigned char)tb_s1129[3] == 101 && tb_s1129[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_r, 1495))->q_name, v_name) == 0)) && (!((tb_o_Node *)tb_nn(v_r, 1495))->q_is_link));
    tb_crash(1487, "функция app_form дошла до конца без return");
    return 0;
}

static tb_list * f_emit__app_parts(tb_o_Comp * v_c, tb_o_Node * v_v) {
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    tb_list * v_back = tb_list_new(sizeof(tb_o_Node *));
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        { tb_o_Node * tb_v1130 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1502))->q_kids, 1LL, sizeof(tb_o_Node *), 1502)); *(tb_o_Node * *)tb_grow(v_back) = tb_v1130; }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1503))->q_kids, 0LL, sizeof(tb_o_Node *), 1503));
    }
    long long v_i = ((v_back)->len - 1LL);
    while ((v_i >= 0LL)) {
        { tb_o_Node * tb_v1131 = (*(tb_o_Node * *)tb_at(v_back, v_i, sizeof(tb_o_Node *), 1506)); *(tb_o_Node * *)tb_grow(v_parts) = tb_v1131; }
        { long long tb_v1132 = 1LL; long long *tb_p1132 = &(v_i); *tb_p1132 -= tb_v1132; }
    }
    return v_parts;
}

static void f_emit__bld_scan(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_in_par, tb_list * v_cand, tb_list * v_bad) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1510))->q_kind;
    _Bool v_par = (((v_in_par || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s1133 = v_k; ((unsigned char)tb_s1133[0] == 116 && (unsigned char)tb_s1133[1] == 111 && (unsigned char)tb_s1133[2] == 103 && (unsigned char)tb_s1133[3] == 101 && (unsigned char)tb_s1133[4] == 116 && (unsigned char)tb_s1133[5] == 104 && (unsigned char)tb_s1133[6] == 101 && (unsigned char)tb_s1133[7] == 114 && tb_s1133[8] == 0); })) || ((({ const char *tb_s1134 = v_k; ((unsigned char)tb_s1134[0] == 101 && (unsigned char)tb_s1134[1] == 97 && (unsigned char)tb_s1134[2] == 99 && (unsigned char)tb_s1134[3] == 104 && tb_s1134[4] == 0); }) || ({ const char *tb_s1135 = v_k; ((unsigned char)tb_s1135[0] == 99 && (unsigned char)tb_s1135[1] == 111 && (unsigned char)tb_s1135[2] == 117 && (unsigned char)tb_s1135[3] == 110 && (unsigned char)tb_s1135[4] == 116 && tb_s1135[5] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1511))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1511))->q_by_vec)));
    if (v_par) {
        f_emit__bld_bad_all(v_n, v_bad);
        return;
    }
    if ((({ const char *tb_s1136 = v_k; ((unsigned char)tb_s1136[0] == 108 && (unsigned char)tb_s1136[1] == 105 && (unsigned char)tb_s1136[2] == 110 && (unsigned char)tb_s1136[3] == 107 && (unsigned char)tb_s1136[4] == 100 && (unsigned char)tb_s1136[5] == 101 && (unsigned char)tb_s1136[6] == 99 && (unsigned char)tb_s1136[7] == 108 && tb_s1136[8] == 0); }) || ({ const char *tb_s1137 = v_k; ((unsigned char)tb_s1137[0] == 109 && (unsigned char)tb_s1137[1] == 117 && (unsigned char)tb_s1137[2] == 108 && (unsigned char)tb_s1137[3] == 116 && (unsigned char)tb_s1137[4] == 105 && tb_s1137[5] == 0); }))) {
        f_emit__bld_bad_all(v_n, v_bad);
    }
    if (((({ const char *tb_s1138 = v_k; ((unsigned char)tb_s1138[0] == 101 && (unsigned char)tb_s1138[1] == 97 && (unsigned char)tb_s1138[2] == 99 && (unsigned char)tb_s1138[3] == 104 && tb_s1138[4] == 0); }) || ({ const char *tb_s1139 = v_k; ((unsigned char)tb_s1139[0] == 99 && (unsigned char)tb_s1139[1] == 111 && (unsigned char)tb_s1139[2] == 117 && (unsigned char)tb_s1139[3] == 110 && (unsigned char)tb_s1139[4] == 116 && tb_s1139[5] == 0); })) || ({ const char *tb_s1140 = v_k; ((unsigned char)tb_s1140[0] == 101 && (unsigned char)tb_s1140[1] == 108 && (unsigned char)tb_s1140[2] == 115 && (unsigned char)tb_s1140[3] == 101 && (unsigned char)tb_s1140[4] == 95 && (unsigned char)tb_s1140[5] == 101 && (unsigned char)tb_s1140[6] == 114 && (unsigned char)tb_s1140[7] == 114 && tb_s1140[8] == 0); }))) {
        { const char * tb_v1141 = ((tb_o_Node *)tb_nn(v_n, 1518))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1141; }
    }
    if (((({ const char *tb_s1142 = v_k; ((unsigned char)tb_s1142[0] == 99 && (unsigned char)tb_s1142[1] == 97 && (unsigned char)tb_s1142[2] == 108 && (unsigned char)tb_s1142[3] == 108 && tb_s1142[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1519))->q_has_ref) && ({ const char *tb_s1143 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1519))->q_ref, 1519))->q_kind; ((unsigned char)tb_s1143[0] == 102 && (unsigned char)tb_s1143[1] == 117 && (unsigned char)tb_s1143[2] == 110 && (unsigned char)tb_s1143[3] == 99 && tb_s1143[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1520))->q_ref, 1520))->q_kids, 0LL, sizeof(tb_o_Node *), 1520));
        {
            long long tb_end1144 = ((((tb_o_Node *)tb_nn(v_n, 1521))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1144; v_i++) {
                if (((v_i < (((tb_o_Node *)tb_nn(v_params, 1522))->q_kids)->len) && ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1522))->q_kids, v_i, sizeof(tb_o_Node *), 1522)), 1522))->q_is_link)) {
                    f_emit__bld_bad_all((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1523))->q_kids, v_i, sizeof(tb_o_Node *), 1523)), v_bad);
                }
            }
        }
    }
    if (({ const char *tb_s1145 = v_k; ((unsigned char)tb_s1145[0] == 97 && (unsigned char)tb_s1145[1] == 115 && (unsigned char)tb_s1145[2] == 115 && (unsigned char)tb_s1145[3] == 105 && (unsigned char)tb_s1145[4] == 103 && (unsigned char)tb_s1145[5] == 110 && tb_s1145[6] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 1525))->q_is_link) {
            { const char * tb_v1146 = ((tb_o_Node *)tb_nn(v_n, 1526))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1146; }
        } else if ((((({ const char *tb_s1147 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1527))->q_ty); ((unsigned char)tb_s1147[0] == 116 && (unsigned char)tb_s1147[1] == 101 && (unsigned char)tb_s1147[2] == 120 && (unsigned char)tb_s1147[3] == 116 && tb_s1147[4] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1527))->q_kids)->len == 1LL)) && (!((tb_o_Node *)tb_nn(v_n, 1527))->q_creates)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 1527))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1527))->q_kids, 0LL, sizeof(tb_o_Node *), 1527))))) {
            { const char * tb_v1148 = ((tb_o_Node *)tb_nn(v_n, 1528))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1148; }
        }
    }
    if ((((({ const char *tb_s1149 = v_k; ((unsigned char)tb_s1149[0] == 115 && (unsigned char)tb_s1149[1] == 101 && (unsigned char)tb_s1149[2] == 116 && tb_s1149[3] == 0); }) && ({ const char *tb_s1150 = ((tb_o_Node *)tb_nn(v_n, 1529))->q_value; ((unsigned char)tb_s1150[0] == 43 && (unsigned char)tb_s1150[1] == 61 && tb_s1150[2] == 0); })) && ({ const char *tb_s1151 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1529))->q_kids, 0LL, sizeof(tb_o_Node *), 1529)), 1529))->q_kind; ((unsigned char)tb_s1151[0] == 110 && (unsigned char)tb_s1151[1] == 97 && (unsigned char)tb_s1151[2] == 109 && (unsigned char)tb_s1151[3] == 101 && tb_s1151[4] == 0); })) && ({ const char *tb_s1152 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1529))->q_kids, 0LL, sizeof(tb_o_Node *), 1529)), 1529))->q_ty); ((unsigned char)tb_s1152[0] == 116 && (unsigned char)tb_s1152[1] == 101 && (unsigned char)tb_s1152[2] == 120 && (unsigned char)tb_s1152[3] == 116 && tb_s1152[4] == 0); }))) {
        if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1530))->q_kids, 0LL, sizeof(tb_o_Node *), 1530)), 1530))->q_is_link) {
            { const char * tb_v1153 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1531))->q_kids, 0LL, sizeof(tb_o_Node *), 1531)), 1531))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1153; }
        } else {
            { const char * tb_v1154 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1533))->q_kids, 0LL, sizeof(tb_o_Node *), 1533)), 1533))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1154; }
        }
    }
    {
        tb_list * tb_l1155 = ((tb_o_Node *)tb_nn(v_n, 1534))->q_kids;
        for (long long tb_i1155 = 0; tb_i1155 < tb_l1155->len; tb_i1155++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1155->data)[tb_i1155];
            f_emit__bld_scan(v_c, v_kid, 0, v_cand, v_bad);
        }
    }
}

static void f_emit__find_builders(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1156 = v_fresh; tb_list * *tb_p1156 = &(((tb_o_Comp *)tb_nn(v_c, 1539))->q_builders); *tb_p1156 = tb_v1156; }
    tb_list * v_cand = tb_list_new(sizeof(const char *));
    tb_list * v_bad = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1157 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1542))->q_kids, 0LL, sizeof(tb_o_Node *), 1542)), 1542))->q_kids;
        for (long long tb_i1157 = 0; tb_i1157 < tb_l1157->len; tb_i1157++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l1157->data)[tb_i1157];
            if (((tb_o_Node *)tb_nn(v_p, 1543))->q_is_link) {
                { const char * tb_v1158 = ((tb_o_Node *)tb_nn(v_p, 1544))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1158; }
            }
        }
    }
    f_emit__bld_scan(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1545))->q_kids, 1LL, sizeof(tb_o_Node *), 1545)), 0, v_cand, v_bad);
    {
        tb_list * tb_l1159 = v_cand;
        for (long long tb_i1159 = 0; tb_i1159 < tb_l1159->len; tb_i1159++) {
            const char * const v_name = ((const char * *)tb_l1159->data)[tb_i1159];
            if (((!f_emit__bld_has(v_bad, v_name)) && (!f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1547))->q_builders, v_name)))) {
                { const char * tb_v1160 = v_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1548))->q_builders) = tb_v1160; }
            }
        }
    }
}

static _Bool f_emit__is_bld(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((((({ const char *tb_s1161 = ((tb_o_Node *)tb_nn(v_n, 1552))->q_kind; ((unsigned char)tb_s1161[0] == 110 && (unsigned char)tb_s1161[1] == 97 && (unsigned char)tb_s1161[2] == 109 && (unsigned char)tb_s1161[3] == 101 && tb_s1161[4] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1552))->q_is_link)) && ((((tb_o_Comp *)tb_nn(v_c, 1552))->q_builders)->len > 0LL)) && ({ const char *tb_s1162 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1552))->q_ty); ((unsigned char)tb_s1162[0] == 116 && (unsigned char)tb_s1162[1] == 101 && (unsigned char)tb_s1162[2] == 120 && (unsigned char)tb_s1162[3] == 116 && tb_s1162[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1552))->q_builders, ((tb_o_Node *)tb_nn(v_n, 1552))->q_name));
    tb_crash(1551, "функция is_bld дошла до конца без return");
    return 0;
}

static void f_emit__emit_q(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (f_emit__is_bld(v_c, v_n)) {
        f_emit__put(v_c, ({ const char *tb_c1163_0 = "v_"; long long tb_c1163_0n = sizeof("v_") - 1; const char *tb_c1163_1 = ((tb_o_Node *)tb_nn(v_n, 1557))->q_name; long long tb_c1163_1n = (long long)strlen(tb_c1163_1); char *tb_c1163r = tb_mem(tb_c1163_0n + tb_c1163_1n + 1), *tb_c1163rw = tb_c1163r; memcpy(tb_c1163rw, tb_c1163_0, tb_c1163_0n); tb_c1163rw += tb_c1163_0n; memcpy(tb_c1163rw, tb_c1163_1, tb_c1163_1n); tb_c1163rw += tb_c1163_1n; *tb_c1163rw = 0; (const char *)tb_c1163r; }));
    } else {
        f_emit__emit_expr(v_c, v_n);
    }
}

static void f_emit__emit_app(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_part) {
    const char * v_vars = ({ const char *tb_c1164_0 = "&v_"; long long tb_c1164_0n = sizeof("&v_") - 1; const char *tb_c1164_1 = v_name; long long tb_c1164_1n = (long long)strlen(tb_c1164_1); const char *tb_c1164_2 = ", &tb_rl_"; long long tb_c1164_2n = sizeof(", &tb_rl_") - 1; const char *tb_c1164_3 = v_name; long long tb_c1164_3n = (long long)strlen(tb_c1164_3); const char *tb_c1164_4 = ", &tb_rc_"; long long tb_c1164_4n = sizeof(", &tb_rc_") - 1; const char *tb_c1164_5 = v_name; long long tb_c1164_5n = (long long)strlen(tb_c1164_5); const char *tb_c1164_6 = ", "; long long tb_c1164_6n = sizeof(", ") - 1; char *tb_c1164r = tb_mem(tb_c1164_0n + tb_c1164_1n + tb_c1164_2n + tb_c1164_3n + tb_c1164_4n + tb_c1164_5n + tb_c1164_6n + 1), *tb_c1164rw = tb_c1164r; memcpy(tb_c1164rw, tb_c1164_0, tb_c1164_0n); tb_c1164rw += tb_c1164_0n; memcpy(tb_c1164rw, tb_c1164_1, tb_c1164_1n); tb_c1164rw += tb_c1164_1n; memcpy(tb_c1164rw, tb_c1164_2, tb_c1164_2n); tb_c1164rw += tb_c1164_2n; memcpy(tb_c1164rw, tb_c1164_3, tb_c1164_3n); tb_c1164rw += tb_c1164_3n; memcpy(tb_c1164rw, tb_c1164_4, tb_c1164_4n); tb_c1164rw += tb_c1164_4n; memcpy(tb_c1164rw, tb_c1164_5, tb_c1164_5n); tb_c1164rw += tb_c1164_5n; memcpy(tb_c1164rw, tb_c1164_6, tb_c1164_6n); tb_c1164rw += tb_c1164_6n; *tb_c1164rw = 0; (const char *)tb_c1164r; });
    if (({ const char *tb_s1165 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 1563))->q_ty); ((unsigned char)tb_s1165[0] == 105 && (unsigned char)tb_s1165[1] == 110 && (unsigned char)tb_s1165[2] == 116 && tb_s1165[3] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1166_0 = "tb_app_i("; long long tb_c1166_0n = sizeof("tb_app_i(") - 1; const char *tb_c1166_1 = v_vars; long long tb_c1166_1n = (long long)strlen(tb_c1166_1); char *tb_c1166r = tb_mem(tb_c1166_0n + tb_c1166_1n + 1), *tb_c1166rw = tb_c1166r; memcpy(tb_c1166rw, tb_c1166_0, tb_c1166_0n); tb_c1166rw += tb_c1166_0n; memcpy(tb_c1166rw, tb_c1166_1, tb_c1166_1n); tb_c1166rw += tb_c1166_1n; *tb_c1166rw = 0; (const char *)tb_c1166r; }));
        f_emit__emit_expr(v_c, v_part);
        f_emit__put(v_c, ");");
    } else if (({ const char *tb_s1167 = ((tb_o_Node *)tb_nn(v_part, 1568))->q_kind; ((unsigned char)tb_s1167[0] == 115 && (unsigned char)tb_s1167[1] == 116 && (unsigned char)tb_s1167[2] == 114 && (unsigned char)tb_s1167[3] == 105 && (unsigned char)tb_s1167[4] == 110 && (unsigned char)tb_s1167[5] == 103 && tb_s1167[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1168_0 = "tb_app_n("; long long tb_c1168_0n = sizeof("tb_app_n(") - 1; const char *tb_c1168_1 = v_vars; long long tb_c1168_1n = (long long)strlen(tb_c1168_1); const char *tb_c1168_2 = ((tb_o_Node *)tb_nn(v_part, 1570))->q_value; long long tb_c1168_2n = (long long)strlen(tb_c1168_2); const char *tb_c1168_3 = ", sizeof("; long long tb_c1168_3n = sizeof(", sizeof(") - 1; const char *tb_c1168_4 = ((tb_o_Node *)tb_nn(v_part, 1570))->q_value; long long tb_c1168_4n = (long long)strlen(tb_c1168_4); const char *tb_c1168_5 = ") - 1);"; long long tb_c1168_5n = sizeof(") - 1);") - 1; char *tb_c1168r = tb_mem(tb_c1168_0n + tb_c1168_1n + tb_c1168_2n + tb_c1168_3n + tb_c1168_4n + tb_c1168_5n + 1), *tb_c1168rw = tb_c1168r; memcpy(tb_c1168rw, tb_c1168_0, tb_c1168_0n); tb_c1168rw += tb_c1168_0n; memcpy(tb_c1168rw, tb_c1168_1, tb_c1168_1n); tb_c1168rw += tb_c1168_1n; memcpy(tb_c1168rw, tb_c1168_2, tb_c1168_2n); tb_c1168rw += tb_c1168_2n; memcpy(tb_c1168rw, tb_c1168_3, tb_c1168_3n); tb_c1168rw += tb_c1168_3n; memcpy(tb_c1168rw, tb_c1168_4, tb_c1168_4n); tb_c1168rw += tb_c1168_4n; memcpy(tb_c1168rw, tb_c1168_5, tb_c1168_5n); tb_c1168rw += tb_c1168_5n; *tb_c1168rw = 0; (const char *)tb_c1168r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c1169_0 = "tb_app("; long long tb_c1169_0n = sizeof("tb_app(") - 1; const char *tb_c1169_1 = v_vars; long long tb_c1169_1n = (long long)strlen(tb_c1169_1); char *tb_c1169r = tb_mem(tb_c1169_0n + tb_c1169_1n + 1), *tb_c1169rw = tb_c1169r; memcpy(tb_c1169rw, tb_c1169_0, tb_c1169_0n); tb_c1169rw += tb_c1169_0n; memcpy(tb_c1169rw, tb_c1169_1, tb_c1169_1n); tb_c1169rw += tb_c1169_1n; *tb_c1169rw = 0; (const char *)tb_c1169r; }));
        f_emit__emit_as_text(v_c, v_part);
        f_emit__put(v_c, ");");
    }
}

static _Bool f_emit__is_plain_ty(long long v_t) {
    return (((v_t == 1LL) || (v_t == 2LL)) || (v_t == 3LL));
    tb_crash(1581, "функция is_plain_ty дошла до конца без return");
    return 0;
}

static void f_emit__at_inner(tb_o_Node * v_n, tb_list * v_inner) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1586))->q_kind;
    if (((((({ const char *tb_s1170 = v_k; ((unsigned char)tb_s1170[0] == 97 && (unsigned char)tb_s1170[1] == 115 && (unsigned char)tb_s1170[2] == 115 && (unsigned char)tb_s1170[3] == 105 && (unsigned char)tb_s1170[4] == 103 && (unsigned char)tb_s1170[5] == 110 && tb_s1170[6] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1587))->q_creates) || ({ const char *tb_s1171 = v_k; ((unsigned char)tb_s1171[0] == 101 && (unsigned char)tb_s1171[1] == 97 && (unsigned char)tb_s1171[2] == 99 && (unsigned char)tb_s1171[3] == 104 && tb_s1171[4] == 0); })) || ({ const char *tb_s1172 = v_k; ((unsigned char)tb_s1172[0] == 99 && (unsigned char)tb_s1172[1] == 111 && (unsigned char)tb_s1172[2] == 117 && (unsigned char)tb_s1172[3] == 110 && (unsigned char)tb_s1172[4] == 116 && tb_s1172[5] == 0); })) || ({ const char *tb_s1173 = v_k; ((unsigned char)tb_s1173[0] == 101 && (unsigned char)tb_s1173[1] == 108 && (unsigned char)tb_s1173[2] == 115 && (unsigned char)tb_s1173[3] == 101 && (unsigned char)tb_s1173[4] == 95 && (unsigned char)tb_s1173[5] == 101 && (unsigned char)tb_s1173[6] == 114 && (unsigned char)tb_s1173[7] == 114 && tb_s1173[8] == 0); }))) {
        { const char * tb_v1174 = ((tb_o_Node *)tb_nn(v_n, 1588))->q_name; *(const char * *)tb_grow(v_inner) = tb_v1174; }
    }
    if (({ const char *tb_s1175 = v_k; ((unsigned char)tb_s1175[0] == 109 && (unsigned char)tb_s1175[1] == 117 && (unsigned char)tb_s1175[2] == 108 && (unsigned char)tb_s1175[3] == 116 && (unsigned char)tb_s1175[4] == 105 && tb_s1175[5] == 0); })) {
        {
            long long tb_end1176 = ((((tb_o_Node *)tb_nn(v_n, 1590))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1176; v_i++) {
                if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1591))->q_kids, v_i, sizeof(tb_o_Node *), 1591)), 1591))->q_creates) {
                    { const char * tb_v1177 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1592))->q_kids, v_i, sizeof(tb_o_Node *), 1592)), 1592))->q_name; *(const char * *)tb_grow(v_inner) = tb_v1177; }
                }
            }
        }
    }
    {
        tb_list * tb_l1178 = ((tb_o_Node *)tb_nn(v_n, 1593))->q_kids;
        for (long long tb_i1178 = 0; tb_i1178 < tb_l1178->len; tb_i1178++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1178->data)[tb_i1178];
            f_emit__at_inner(v_kid, v_inner);
        }
    }
}

static _Bool f_emit__is_box(long long v_t) {
    return (v_t > 4LL);
    tb_crash(1597, "функция is_box дошла до конца без return");
    return 0;
}

static const char * f_emit__root_name(tb_o_Node * v_n) {
    tb_o_Node * v_r = v_n;
    while ((({ const char *tb_s1179 = ((tb_o_Node *)tb_nn(v_r, 1603))->q_kind; ((unsigned char)tb_s1179[0] == 105 && (unsigned char)tb_s1179[1] == 110 && (unsigned char)tb_s1179[2] == 100 && (unsigned char)tb_s1179[3] == 101 && (unsigned char)tb_s1179[4] == 120 && tb_s1179[5] == 0); }) || ({ const char *tb_s1180 = ((tb_o_Node *)tb_nn(v_r, 1603))->q_kind; ((unsigned char)tb_s1180[0] == 102 && (unsigned char)tb_s1180[1] == 105 && (unsigned char)tb_s1180[2] == 101 && (unsigned char)tb_s1180[3] == 108 && (unsigned char)tb_s1180[4] == 100 && tb_s1180[5] == 0); }))) {
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1604))->q_kids, 0LL, sizeof(tb_o_Node *), 1604));
    }
    if (({ const char *tb_s1181 = ((tb_o_Node *)tb_nn(v_r, 1605))->q_kind; ((unsigned char)tb_s1181[0] == 110 && (unsigned char)tb_s1181[1] == 97 && (unsigned char)tb_s1181[2] == 109 && (unsigned char)tb_s1181[3] == 101 && tb_s1181[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(v_r, 1606))->q_name;
    }
    return "";
    tb_crash(1601, "функция root_name дошла до конца без return");
    return "";
}

static _Bool f_emit__at_fresh_expr(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh) {
    if (((({ const char *tb_s1182 = ((tb_o_Node *)tb_nn(v_n, 1611))->q_kind; ((unsigned char)tb_s1182[0] == 110 && (unsigned char)tb_s1182[1] == 97 && (unsigned char)tb_s1182[2] == 109 && (unsigned char)tb_s1182[3] == 101 && tb_s1182[4] == 0); }) && f_emit__is_box(((tb_o_Node *)tb_nn(v_n, 1611))->q_ty)) && (!(f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_n, 1611))->q_name) && f_emit__bld_has(v_fresh, ((tb_o_Node *)tb_nn(v_n, 1611))->q_name))))) {
        return 0;
    }
    {
        tb_list * tb_l1183 = ((tb_o_Node *)tb_nn(v_n, 1613))->q_kids;
        for (long long tb_i1183 = 0; tb_i1183 < tb_l1183->len; tb_i1183++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1183->data)[tb_i1183];
            if ((!f_emit__at_fresh_expr(v_k, v_inner, v_fresh))) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1610, "функция at_fresh_expr дошла до конца без return");
    return 0;
}

static _Bool f_emit__at_unfresh(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh) {
    _Bool v_changed = 0;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1620))->q_kind;
    const char * v_lose = "";
    if ((((({ const char *tb_s1184 = v_k; ((unsigned char)tb_s1184[0] == 97 && (unsigned char)tb_s1184[1] == 115 && (unsigned char)tb_s1184[2] == 115 && (unsigned char)tb_s1184[3] == 105 && (unsigned char)tb_s1184[4] == 103 && (unsigned char)tb_s1184[5] == 110 && tb_s1184[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1622))->q_kids)->len == 1LL)) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1622))->q_kids, 0LL, sizeof(tb_o_Node *), 1622)), 1622))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1622))->q_kids, 0LL, sizeof(tb_o_Node *), 1622)), v_inner, v_fresh)))) {
        v_lose = ((tb_o_Node *)tb_nn(v_n, 1623))->q_name;
    }
    if ((({ const char *tb_s1185 = v_k; ((unsigned char)tb_s1185[0] == 101 && (unsigned char)tb_s1185[1] == 97 && (unsigned char)tb_s1185[2] == 99 && (unsigned char)tb_s1185[3] == 104 && tb_s1185[4] == 0); }) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1624))->q_kids, 0LL, sizeof(tb_o_Node *), 1624)), v_inner, v_fresh)))) {
        v_lose = ((tb_o_Node *)tb_nn(v_n, 1625))->q_name;
    }
    if ((((({ const char *tb_s1186 = v_k; ((unsigned char)tb_s1186[0] == 115 && (unsigned char)tb_s1186[1] == 101 && (unsigned char)tb_s1186[2] == 116 && tb_s1186[3] == 0); }) && ({ const char *tb_s1187 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 0LL, sizeof(tb_o_Node *), 1626)), 1626))->q_kind; !((unsigned char)tb_s1187[0] == 110 && (unsigned char)tb_s1187[1] == 97 && (unsigned char)tb_s1187[2] == 109 && (unsigned char)tb_s1187[3] == 101 && tb_s1187[4] == 0); })) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 1LL, sizeof(tb_o_Node *), 1626)), 1626))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 1LL, sizeof(tb_o_Node *), 1626)), v_inner, v_fresh)))) {
        v_lose = f_emit__root_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1627))->q_kids, 0LL, sizeof(tb_o_Node *), 1627)));
    }
    if ((((({ const char *tb_s1188 = v_k; ((unsigned char)tb_s1188[0] == 109 && (unsigned char)tb_s1188[1] == 101 && (unsigned char)tb_s1188[2] == 116 && (unsigned char)tb_s1188[3] == 104 && (unsigned char)tb_s1188[4] == 111 && (unsigned char)tb_s1188[5] == 100 && tb_s1188[6] == 0); }) && ({ const char *tb_s1189 = ((tb_o_Node *)tb_nn(v_n, 1628))->q_name; ((unsigned char)tb_s1189[0] == 97 && (unsigned char)tb_s1189[1] == 100 && (unsigned char)tb_s1189[2] == 100 && tb_s1189[3] == 0); })) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1628))->q_kids, 1LL, sizeof(tb_o_Node *), 1628)), 1628))->q_kids, 0LL, sizeof(tb_o_Node *), 1628)), 1628))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1628))->q_kids, 1LL, sizeof(tb_o_Node *), 1628)), 1628))->q_kids, 0LL, sizeof(tb_o_Node *), 1628)), v_inner, v_fresh)))) {
        v_lose = f_emit__root_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1629))->q_kids, 0LL, sizeof(tb_o_Node *), 1629)));
    }
    if (({ const char *tb_s1190 = v_k; ((unsigned char)tb_s1190[0] == 109 && (unsigned char)tb_s1190[1] == 117 && (unsigned char)tb_s1190[2] == 108 && (unsigned char)tb_s1190[3] == 116 && (unsigned char)tb_s1190[4] == 105 && tb_s1190[5] == 0); })) {
        {
            long long tb_end1191 = ((((tb_o_Node *)tb_nn(v_n, 1631))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1191; v_i++) {
                if (f_emit__bld_has(v_fresh, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1632))->q_kids, v_i, sizeof(tb_o_Node *), 1632)), 1632))->q_name)) {
                    f_emit__drop_name(v_fresh, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1633))->q_kids, v_i, sizeof(tb_o_Node *), 1633)), 1633))->q_name);
                    v_changed = 1;
                }
            }
        }
    }
    if ((({ const char *tb_s1192 = v_lose; !(tb_s1192[0] == 0); }) && f_emit__bld_has(v_fresh, v_lose))) {
        f_emit__drop_name(v_fresh, v_lose);
        v_changed = 1;
    }
    {
        tb_list * tb_l1193 = ((tb_o_Node *)tb_nn(v_n, 1638))->q_kids;
        for (long long tb_i1193 = 0; tb_i1193 < tb_l1193->len; tb_i1193++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1193->data)[tb_i1193];
            if (f_emit__at_unfresh(v_c, v_kid, v_inner, v_fresh)) {
                v_changed = 1;
            }
        }
    }
    return v_changed;
}

static void f_emit__drop_name(tb_list * v_names, const char * v_name) {
    tb_list * v_keep = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1194 = v_names;
        for (long long tb_i1194 = 0; tb_i1194 < tb_l1194->len; tb_i1194++) {
            const char * const v_x = ((const char * *)tb_l1194->data)[tb_i1194];
            if ((strcmp(v_x, v_name) != 0)) {
                { const char * tb_v1195 = v_x; *(const char * *)tb_grow(v_keep) = tb_v1195; }
            }
        }
    }
    while (((v_names)->len > 0LL)) {
        tb_mark tb_am1196 = tb_temp_begin();
        do {
            const char * v_gone = (*(const char * *)tb_pop(v_names, 1648));
        } while (0);
        tb_temp_end(tb_am1196);
    }
    {
        tb_list * tb_l1197 = v_keep;
        for (long long tb_i1197 = 0; tb_i1197 < tb_l1197->len; tb_i1197++) {
            const char * const v_x = ((const char * *)tb_l1197->data)[tb_i1197];
            { const char * tb_v1198 = v_x; *(const char * *)tb_grow(v_names) = tb_v1198; }
        }
    }
}

static _Bool f_emit__at_own(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh) {
    const char * v_r = f_emit__root_name(v_n);
    return ((({ const char *tb_s1199 = v_r; !(tb_s1199[0] == 0); }) && f_emit__bld_has(v_inner, v_r)) && f_emit__bld_has(v_fresh, v_r));
    tb_crash(1653, "функция at_own дошла до конца без return");
    return 0;
}

static _Bool f_emit__at_bad(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh, long long v_level) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1659))->q_kind;
    if ((({ const char *tb_s1200 = v_k; ((unsigned char)tb_s1200[0] == 114 && (unsigned char)tb_s1200[1] == 101 && (unsigned char)tb_s1200[2] == 116 && (unsigned char)tb_s1200[3] == 117 && (unsigned char)tb_s1200[4] == 114 && (unsigned char)tb_s1200[5] == 110 && tb_s1200[6] == 0); }) && (!((tb_o_Comp *)tb_nn(v_c, 1660))->q_at_ret_ok))) {
        return 1;
    }
    if (((((({ const char *tb_s1201 = v_k; ((unsigned char)tb_s1201[0] == 102 && (unsigned char)tb_s1201[1] == 97 && (unsigned char)tb_s1201[2] == 105 && (unsigned char)tb_s1201[3] == 108 && tb_s1201[4] == 0); }) || ({ const char *tb_s1202 = v_k; ((unsigned char)tb_s1202[0] == 116 && (unsigned char)tb_s1202[1] == 114 && (unsigned char)tb_s1202[2] == 121 && tb_s1202[3] == 0); })) || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s1203 = v_k; ((unsigned char)tb_s1203[0] == 116 && (unsigned char)tb_s1203[1] == 111 && (unsigned char)tb_s1203[2] == 103 && (unsigned char)tb_s1203[3] == 101 && (unsigned char)tb_s1203[4] == 116 && (unsigned char)tb_s1203[5] == 104 && (unsigned char)tb_s1203[6] == 101 && (unsigned char)tb_s1203[7] == 114 && tb_s1203[8] == 0); })) || ({ const char *tb_s1204 = v_k; ((unsigned char)tb_s1204[0] == 116 && (unsigned char)tb_s1204[1] == 101 && (unsigned char)tb_s1204[2] == 109 && (unsigned char)tb_s1204[3] == 112 && tb_s1204[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s1205 = v_k; ((unsigned char)tb_s1205[0] == 115 && (unsigned char)tb_s1205[1] == 116 && (unsigned char)tb_s1205[2] == 111 && (unsigned char)tb_s1205[3] == 112 && tb_s1205[4] == 0); }) && (v_level == 0LL))) {
        return 1;
    }
    if (((({ const char *tb_s1206 = v_k; ((unsigned char)tb_s1206[0] == 101 && (unsigned char)tb_s1206[1] == 97 && (unsigned char)tb_s1206[2] == 99 && (unsigned char)tb_s1206[3] == 104 && tb_s1206[4] == 0); }) || ({ const char *tb_s1207 = v_k; ((unsigned char)tb_s1207[0] == 99 && (unsigned char)tb_s1207[1] == 111 && (unsigned char)tb_s1207[2] == 117 && (unsigned char)tb_s1207[3] == 110 && (unsigned char)tb_s1207[4] == 116 && tb_s1207[5] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1666))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1666))->q_by_vec) || ((tb_o_Node *)tb_nn(v_n, 1666))->q_by_piece))) {
        return 1;
    }
    if ((((({ const char *tb_s1208 = v_k; ((unsigned char)tb_s1208[0] == 97 && (unsigned char)tb_s1208[1] == 115 && (unsigned char)tb_s1208[2] == 115 && (unsigned char)tb_s1208[3] == 105 && (unsigned char)tb_s1208[4] == 103 && (unsigned char)tb_s1208[5] == 110 && tb_s1208[6] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1668))->q_creates)) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1668))->q_ty))) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_n, 1668))->q_name)))) {
        return 1;
    }
    if (({ const char *tb_s1209 = v_k; ((unsigned char)tb_s1209[0] == 115 && (unsigned char)tb_s1209[1] == 101 && (unsigned char)tb_s1209[2] == 116 && tb_s1209[3] == 0); })) {
        tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1671))->q_kids, 0LL, sizeof(tb_o_Node *), 1671));
        if (((({ const char *tb_s1210 = ((tb_o_Node *)tb_nn(v_target, 1672))->q_kind; ((unsigned char)tb_s1210[0] == 110 && (unsigned char)tb_s1210[1] == 97 && (unsigned char)tb_s1210[2] == 109 && (unsigned char)tb_s1210[3] == 101 && tb_s1210[4] == 0); }) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_target, 1672))->q_ty))) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_target, 1672))->q_name)))) {
            return 1;
        }
        if ((({ const char *tb_s1211 = ((tb_o_Node *)tb_nn(v_target, 1674))->q_kind; !((unsigned char)tb_s1211[0] == 110 && (unsigned char)tb_s1211[1] == 97 && (unsigned char)tb_s1211[2] == 109 && (unsigned char)tb_s1211[3] == 101 && tb_s1211[4] == 0); }) && (!f_emit__at_own(v_target, v_inner, v_fresh)))) {
            if ((!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1675))->q_kids, 1LL, sizeof(tb_o_Node *), 1675)), 1675))->q_ty))) {
                return 1;
            }
            if (((({ const char *tb_s1212 = ((tb_o_Node *)tb_nn(v_target, 1677))->q_kind; ((unsigned char)tb_s1212[0] == 105 && (unsigned char)tb_s1212[1] == 110 && (unsigned char)tb_s1212[2] == 100 && (unsigned char)tb_s1212[3] == 101 && (unsigned char)tb_s1212[4] == 120 && tb_s1212[5] == 0); }) && ({ const char *tb_s1213 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 1677))->q_kids, 0LL, sizeof(tb_o_Node *), 1677)), 1677))->q_ty); ((unsigned char)tb_s1213[0] == 100 && (unsigned char)tb_s1213[1] == 105 && (unsigned char)tb_s1213[2] == 99 && (unsigned char)tb_s1213[3] == 116 && tb_s1213[4] == 0); })) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 1677))->q_kids, 1LL, sizeof(tb_o_Node *), 1677)), 1677))->q_ty)))) {
                return 1;
            }
        }
    }
    if ((((({ const char *tb_s1214 = v_k; ((unsigned char)tb_s1214[0] == 109 && (unsigned char)tb_s1214[1] == 101 && (unsigned char)tb_s1214[2] == 116 && (unsigned char)tb_s1214[3] == 104 && (unsigned char)tb_s1214[4] == 111 && (unsigned char)tb_s1214[5] == 100 && tb_s1214[6] == 0); }) && ({ const char *tb_s1215 = ((tb_o_Node *)tb_nn(v_n, 1679))->q_name; ((unsigned char)tb_s1215[0] == 97 && (unsigned char)tb_s1215[1] == 100 && (unsigned char)tb_s1215[2] == 100 && tb_s1215[3] == 0); })) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1679))->q_kids, 1LL, sizeof(tb_o_Node *), 1679)), 1679))->q_kids, 0LL, sizeof(tb_o_Node *), 1679)), 1679))->q_ty))) && (!f_emit__at_own((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1679))->q_kids, 0LL, sizeof(tb_o_Node *), 1679)), v_inner, v_fresh)))) {
        return 1;
    }
    if (({ const char *tb_s1216 = v_k; ((unsigned char)tb_s1216[0] == 109 && (unsigned char)tb_s1216[1] == 117 && (unsigned char)tb_s1216[2] == 108 && (unsigned char)tb_s1216[3] == 116 && (unsigned char)tb_s1216[4] == 105 && tb_s1216[5] == 0); })) {
        {
            long long tb_end1217 = ((((tb_o_Node *)tb_nn(v_n, 1682))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1217; v_i++) {
                if (((!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1683))->q_kids, v_i, sizeof(tb_o_Node *), 1683)), 1683))->q_ty)) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1683))->q_kids, v_i, sizeof(tb_o_Node *), 1683)), 1683))->q_name)))) {
                    return 1;
                }
            }
        }
    }
    if ((({ const char *tb_s1218 = v_k; ((unsigned char)tb_s1218[0] == 108 && (unsigned char)tb_s1218[1] == 105 && (unsigned char)tb_s1218[2] == 110 && (unsigned char)tb_s1218[3] == 107 && (unsigned char)tb_s1218[4] == 100 && (unsigned char)tb_s1218[5] == 101 && (unsigned char)tb_s1218[6] == 99 && (unsigned char)tb_s1218[7] == 108 && tb_s1218[8] == 0); }) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1685))->q_ty)))) {
        return 1;
    }
    if (((({ const char *tb_s1219 = v_k; ((unsigned char)tb_s1219[0] == 99 && (unsigned char)tb_s1219[1] == 97 && (unsigned char)tb_s1219[2] == 108 && (unsigned char)tb_s1219[3] == 108 && tb_s1219[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1687))->q_has_ref) && ({ const char *tb_s1220 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1687))->q_ref, 1687))->q_kind; ((unsigned char)tb_s1220[0] == 102 && (unsigned char)tb_s1220[1] == 117 && (unsigned char)tb_s1220[2] == 110 && (unsigned char)tb_s1220[3] == 99 && tb_s1220[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1688))->q_ref, 1688))->q_kids, 0LL, sizeof(tb_o_Node *), 1688));
        {
            long long tb_end1221 = ((((tb_o_Node *)tb_nn(v_n, 1689))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1221; v_i++) {
                tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1690))->q_kids, v_i, sizeof(tb_o_Node *), 1690)), 1690))->q_kids, 0LL, sizeof(tb_o_Node *), 1690));
                if ((f_emit__is_box(((tb_o_Node *)tb_nn(v_a, 1692))->q_ty) && (!f_emit__at_fresh_expr(v_a, v_inner, v_fresh)))) {
                    return 1;
                }
                if (((((v_i < (((tb_o_Node *)tb_nn(v_params, 1694))->q_kids)->len) && ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1694))->q_kids, v_i, sizeof(tb_o_Node *), 1694)), 1694))->q_is_link) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_a, 1694))->q_ty))) && (!f_emit__bld_has(v_inner, f_emit__root_name(v_a))))) {
                    return 1;
                }
            }
        }
    }
    long long v_next_level = v_level;
    if ((((({ const char *tb_s1222 = v_k; ((unsigned char)tb_s1222[0] == 101 && (unsigned char)tb_s1222[1] == 97 && (unsigned char)tb_s1222[2] == 99 && (unsigned char)tb_s1222[3] == 104 && tb_s1222[4] == 0); }) || ({ const char *tb_s1223 = v_k; ((unsigned char)tb_s1223[0] == 99 && (unsigned char)tb_s1223[1] == 111 && (unsigned char)tb_s1223[2] == 117 && (unsigned char)tb_s1223[3] == 110 && (unsigned char)tb_s1223[4] == 116 && tb_s1223[5] == 0); })) || (strcmp(v_k, "repeat_times") == 0)) || (strcmp(v_k, "repeat_while") == 0))) {
        v_next_level = (v_level + 1LL);
    }
    {
        tb_list * tb_l1224 = ((tb_o_Node *)tb_nn(v_n, 1699))->q_kids;
        for (long long tb_i1224 = 0; tb_i1224 < tb_l1224->len; tb_i1224++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1224->data)[tb_i1224];
            if (f_emit__at_bad(v_c, v_kid, v_inner, v_fresh, v_next_level)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1658, "функция at_bad дошла до конца без return");
    return 0;
}

static _Bool f_emit__at_allocates(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1706))->q_kind;
    if (((((((({ const char *tb_s1225 = v_k; ((unsigned char)tb_s1225[0] == 99 && (unsigned char)tb_s1225[1] == 97 && (unsigned char)tb_s1225[2] == 108 && (unsigned char)tb_s1225[3] == 108 && tb_s1225[4] == 0); }) || ({ const char *tb_s1226 = v_k; ((unsigned char)tb_s1226[0] == 98 && (unsigned char)tb_s1226[1] == 105 && (unsigned char)tb_s1226[2] == 110 && (unsigned char)tb_s1226[3] == 111 && (unsigned char)tb_s1226[4] == 112 && tb_s1226[5] == 0); })) || ({ const char *tb_s1227 = v_k; ((unsigned char)tb_s1227[0] == 108 && (unsigned char)tb_s1227[1] == 105 && (unsigned char)tb_s1227[2] == 115 && (unsigned char)tb_s1227[3] == 116 && tb_s1227[4] == 0); })) || ({ const char *tb_s1228 = v_k; ((unsigned char)tb_s1228[0] == 100 && (unsigned char)tb_s1228[1] == 105 && (unsigned char)tb_s1228[2] == 99 && (unsigned char)tb_s1228[3] == 116 && tb_s1228[4] == 0); })) || ({ const char *tb_s1229 = v_k; ((unsigned char)tb_s1229[0] == 109 && (unsigned char)tb_s1229[1] == 101 && (unsigned char)tb_s1229[2] == 116 && (unsigned char)tb_s1229[3] == 104 && (unsigned char)tb_s1229[4] == 111 && (unsigned char)tb_s1229[5] == 100 && tb_s1229[6] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1707))->q_ty > 0LL)) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1707))->q_ty)))) {
        return 1;
    }
    if ((({ const char *tb_s1230 = v_k; ((unsigned char)tb_s1230[0] == 99 && (unsigned char)tb_s1230[1] == 97 && (unsigned char)tb_s1230[2] == 108 && (unsigned char)tb_s1230[3] == 108 && tb_s1230[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1709))->q_has_ref)) {
        return 1;
    }
    {
        tb_list * tb_l1231 = ((tb_o_Node *)tb_nn(v_n, 1711))->q_kids;
        for (long long tb_i1231 = 0; tb_i1231 < tb_l1231->len; tb_i1231++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1231->data)[tb_i1231];
            if (f_emit__at_allocates(v_c, v_kid)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1705, "функция at_allocates дошла до конца без return");
    return 0;
}

static _Bool f_emit__auto_temp_ok(tb_o_Comp * v_c, tb_o_Node * v_body) {
    if ((!f_emit__at_allocates(v_c, v_body))) {
        return 0;
    }
    tb_list * v_inner = tb_list_new(sizeof(const char *));
    f_emit__at_inner(v_body, v_inner);
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1232 = v_inner;
        for (long long tb_i1232 = 0; tb_i1232 < tb_l1232->len; tb_i1232++) {
            const char * const v_x = ((const char * *)tb_l1232->data)[tb_i1232];
            { const char * tb_v1233 = v_x; *(const char * *)tb_grow(v_fresh) = tb_v1233; }
        }
    }
    while (f_emit__at_unfresh(v_c, v_body, v_inner, v_fresh)) {
        v_fresh = v_fresh;
    }
    return (!f_emit__at_bad(v_c, v_body, v_inner, v_fresh, 0LL));
    tb_crash(1716, "функция auto_temp_ok дошла до конца без return");
    return 0;
}

static _Bool f_emit__func_arena_ok(tb_o_Comp * v_c, tb_o_Node * v_f) {
    if ((({ const char *tb_s1234 = ((tb_o_Node *)tb_nn(v_f, 1731))->q_name; ((unsigned char)tb_s1234[0] == 109 && (unsigned char)tb_s1234[1] == 97 && (unsigned char)tb_s1234[2] == 105 && (unsigned char)tb_s1234[3] == 110 && tb_s1234[4] == 0); }) || (!((({ const char *tb_s1235 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1731))->q_ty); ((unsigned char)tb_s1235[0] == 110 && (unsigned char)tb_s1235[1] == 111 && (unsigned char)tb_s1235[2] == 110 && (unsigned char)tb_s1235[3] == 101 && tb_s1235[4] == 0); }) || ({ const char *tb_s1236 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1731))->q_ty); ((unsigned char)tb_s1236[0] == 109 && (unsigned char)tb_s1236[1] == 117 && (unsigned char)tb_s1236[2] == 108 && (unsigned char)tb_s1236[3] == 116 && (unsigned char)tb_s1236[4] == 105 && tb_s1236[5] == 0); })) || f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_f, 1731))->q_ty))))) {
        return 0;
    }
    {
        tb_list * tb_l1237 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1733))->q_kids, 2LL, sizeof(tb_o_Node *), 1733)), 1733))->q_kids;
        for (long long tb_i1237 = 0; tb_i1237 < tb_l1237->len; tb_i1237++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l1237->data)[tb_i1237];
            if ((!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_o, 1734))->q_ty))) {
                return 0;
            }
        }
    }
    tb_o_Node * v_body = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1736))->q_kids, 1LL, sizeof(tb_o_Node *), 1736));
    if ((!f_emit__at_allocates(v_c, v_body))) {
        return 0;
    }
    tb_list * v_inner = tb_list_new(sizeof(const char *));
    f_emit__at_inner(v_body, v_inner);
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1238 = v_inner;
        for (long long tb_i1238 = 0; tb_i1238 < tb_l1238->len; tb_i1238++) {
            const char * const v_x = ((const char * *)tb_l1238->data)[tb_i1238];
            { const char * tb_v1239 = v_x; *(const char * *)tb_grow(v_fresh) = tb_v1239; }
        }
    }
    while (f_emit__at_unfresh(v_c, v_body, v_inner, v_fresh)) {
        v_fresh = v_fresh;
    }
    { _Bool tb_v1240 = 1; _Bool *tb_p1240 = &(((tb_o_Comp *)tb_nn(v_c, 1746))->q_at_ret_ok); *tb_p1240 = tb_v1240; }
    _Bool v_bad = f_emit__at_bad(v_c, v_body, v_inner, v_fresh, 0LL);
    { _Bool tb_v1241 = 0; _Bool *tb_p1241 = &(((tb_o_Comp *)tb_nn(v_c, 1748))->q_at_ret_ok); *tb_p1241 = tb_v1241; }
    return (!v_bad);
    tb_crash(1730, "функция func_arena_ok дошла до конца без return");
    return 0;
}

static void f_emit__emit_loop_body(tb_o_Comp * v_c, tb_o_Node * v_body, long long v_depth) {
    if ((!f_emit__auto_temp_ok(v_c, v_body))) {
        f_emit__emit_block(v_c, v_body, v_depth);
        return;
    }
    { long long tb_v1242 = 1LL; long long *tb_p1242 = &(((tb_o_Comp *)tb_nn(v_c, 1756))->q_label); *tb_p1242 += tb_v1242; }
    const char * v_id = ({ const char *tb_c1243_0 = ""; long long tb_c1243_0n = sizeof("") - 1; char tb_c1243_1d[24]; long long tb_c1243_1n; const char *tb_c1243_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1757))->q_label, tb_c1243_1d, &tb_c1243_1n); char *tb_c1243r = tb_mem(tb_c1243_0n + tb_c1243_1n + 1), *tb_c1243rw = tb_c1243r; memcpy(tb_c1243rw, tb_c1243_0, tb_c1243_0n); tb_c1243rw += tb_c1243_0n; memcpy(tb_c1243rw, tb_c1243_1, tb_c1243_1n); tb_c1243rw += tb_c1243_1n; *tb_c1243rw = 0; (const char *)tb_c1243r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, ({ const char *tb_c1244_0 = "tb_mark tb_am"; long long tb_c1244_0n = sizeof("tb_mark tb_am") - 1; const char *tb_c1244_1 = v_id; long long tb_c1244_1n = (long long)strlen(tb_c1244_1); const char *tb_c1244_2 = " = tb_temp_begin();\n"; long long tb_c1244_2n = sizeof(" = tb_temp_begin();\n") - 1; char *tb_c1244r = tb_mem(tb_c1244_0n + tb_c1244_1n + tb_c1244_2n + 1), *tb_c1244rw = tb_c1244r; memcpy(tb_c1244rw, tb_c1244_0, tb_c1244_0n); tb_c1244rw += tb_c1244_0n; memcpy(tb_c1244rw, tb_c1244_1, tb_c1244_1n); tb_c1244rw += tb_c1244_1n; memcpy(tb_c1244rw, tb_c1244_2, tb_c1244_2n); tb_c1244rw += tb_c1244_2n; *tb_c1244rw = 0; (const char *)tb_c1244r; }));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "do {\n");
    f_emit__emit_block(v_c, v_body, (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "} while (0);\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, ({ const char *tb_c1245_0 = "tb_temp_end(tb_am"; long long tb_c1245_0n = sizeof("tb_temp_end(tb_am") - 1; const char *tb_c1245_1 = v_id; long long tb_c1245_1n = (long long)strlen(tb_c1245_1); const char *tb_c1245_2 = ");\n"; long long tb_c1245_2n = sizeof(");\n") - 1; char *tb_c1245r = tb_mem(tb_c1245_0n + tb_c1245_1n + tb_c1245_2n + 1), *tb_c1245rw = tb_c1245r; memcpy(tb_c1245rw, tb_c1245_0, tb_c1245_0n); tb_c1245rw += tb_c1245_0n; memcpy(tb_c1245rw, tb_c1245_1, tb_c1245_1n); tb_c1245rw += tb_c1245_1n; memcpy(tb_c1245rw, tb_c1245_2, tb_c1245_2n); tb_c1245rw += tb_c1245_2n; *tb_c1245rw = 0; (const char *)tb_c1245r; }));
}

static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f) {
    { tb_o_Node * tb_v1246 = v_f; tb_o_Node * *tb_p1246 = &(((tb_o_Comp *)tb_nn(v_c, 1770))->q_cur_func); *tb_p1246 = tb_v1246; }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1771))->q_kids, 2LL, sizeof(tb_o_Node *), 1771));
    f_emit__put(v_c, "\n");
    f_emit__emit_signature(v_c, v_f);
    f_emit__put(v_c, " {\n");
    {
        tb_list * tb_l1247 = ((tb_o_Node *)tb_nn(v_outputs, 1775))->q_kids;
        for (long long tb_i1247 = 0; tb_i1247 < tb_l1247->len; tb_i1247++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l1247->data)[tb_i1247];
            if ((!((tb_o_Node *)tb_nn(v_o, 1776))->q_is_const)) {
                f_emit__put(v_c, ({ const char *tb_c1248_0 = "    "; long long tb_c1248_0n = sizeof("    ") - 1; const char *tb_c1248_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_o, 1777))->q_ty); long long tb_c1248_1n = (long long)strlen(tb_c1248_1); const char *tb_c1248_2 = " v_"; long long tb_c1248_2n = sizeof(" v_") - 1; const char *tb_c1248_3 = ((tb_o_Node *)tb_nn(v_o, 1777))->q_name; long long tb_c1248_3n = (long long)strlen(tb_c1248_3); const char *tb_c1248_4 = " = "; long long tb_c1248_4n = sizeof(" = ") - 1; char *tb_c1248r = tb_mem(tb_c1248_0n + tb_c1248_1n + tb_c1248_2n + tb_c1248_3n + tb_c1248_4n + 1), *tb_c1248rw = tb_c1248r; memcpy(tb_c1248rw, tb_c1248_0, tb_c1248_0n); tb_c1248rw += tb_c1248_0n; memcpy(tb_c1248rw, tb_c1248_1, tb_c1248_1n); tb_c1248rw += tb_c1248_1n; memcpy(tb_c1248rw, tb_c1248_2, tb_c1248_2n); tb_c1248rw += tb_c1248_2n; memcpy(tb_c1248rw, tb_c1248_3, tb_c1248_3n); tb_c1248rw += tb_c1248_3n; memcpy(tb_c1248rw, tb_c1248_4, tb_c1248_4n); tb_c1248rw += tb_c1248_4n; *tb_c1248rw = 0; (const char *)tb_c1248r; }));
                f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_o, 1778))->q_ty);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__find_builders(v_c, v_f);
    {
        tb_list * tb_l1249 = ((tb_o_Comp *)tb_nn(v_c, 1781))->q_builders;
        for (long long tb_i1249 = 0; tb_i1249 < tb_l1249->len; tb_i1249++) {
            const char * const v_b = ((const char * *)tb_l1249->data)[tb_i1249];
            f_emit__put(v_c, ({ const char *tb_c1250_0 = "    long long tb_rl_"; long long tb_c1250_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c1250_1 = v_b; long long tb_c1250_1n = (long long)strlen(tb_c1250_1); const char *tb_c1250_2 = " = -1, tb_rc_"; long long tb_c1250_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c1250_3 = v_b; long long tb_c1250_3n = (long long)strlen(tb_c1250_3); const char *tb_c1250_4 = " = 0; (void)tb_rl_"; long long tb_c1250_4n = sizeof(" = 0; (void)tb_rl_") - 1; const char *tb_c1250_5 = v_b; long long tb_c1250_5n = (long long)strlen(tb_c1250_5); const char *tb_c1250_6 = "; (void)tb_rc_"; long long tb_c1250_6n = sizeof("; (void)tb_rc_") - 1; const char *tb_c1250_7 = v_b; long long tb_c1250_7n = (long long)strlen(tb_c1250_7); const char *tb_c1250_8 = ";\n"; long long tb_c1250_8n = sizeof(";\n") - 1; char *tb_c1250r = tb_mem(tb_c1250_0n + tb_c1250_1n + tb_c1250_2n + tb_c1250_3n + tb_c1250_4n + tb_c1250_5n + tb_c1250_6n + tb_c1250_7n + tb_c1250_8n + 1), *tb_c1250rw = tb_c1250r; memcpy(tb_c1250rw, tb_c1250_0, tb_c1250_0n); tb_c1250rw += tb_c1250_0n; memcpy(tb_c1250rw, tb_c1250_1, tb_c1250_1n); tb_c1250rw += tb_c1250_1n; memcpy(tb_c1250rw, tb_c1250_2, tb_c1250_2n); tb_c1250rw += tb_c1250_2n; memcpy(tb_c1250rw, tb_c1250_3, tb_c1250_3n); tb_c1250rw += tb_c1250_3n; memcpy(tb_c1250rw, tb_c1250_4, tb_c1250_4n); tb_c1250rw += tb_c1250_4n; memcpy(tb_c1250rw, tb_c1250_5, tb_c1250_5n); tb_c1250rw += tb_c1250_5n; memcpy(tb_c1250rw, tb_c1250_6, tb_c1250_6n); tb_c1250rw += tb_c1250_6n; memcpy(tb_c1250rw, tb_c1250_7, tb_c1250_7n); tb_c1250rw += tb_c1250_7n; memcpy(tb_c1250rw, tb_c1250_8, tb_c1250_8n); tb_c1250rw += tb_c1250_8n; *tb_c1250rw = 0; (const char *)tb_c1250r; }));
        }
    }
    { const char * tb_v1251 = ""; const char * *tb_p1251 = &(((tb_o_Comp *)tb_nn(v_c, 1783))->q_fmark); *tb_p1251 = tb_v1251; }
    if (f_emit__func_arena_ok(v_c, v_f)) {
        { const char * tb_v1252 = "tb_fm"; const char * *tb_p1252 = &(((tb_o_Comp *)tb_nn(v_c, 1787))->q_fmark); *tb_p1252 = tb_v1252; }
        f_emit__put(v_c, "    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();\n");
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1789))->q_kids, 1LL, sizeof(tb_o_Node *), 1789)), 1LL);
    tb_list * v_no_builders = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1253 = v_no_builders; tb_list * *tb_p1253 = &(((tb_o_Comp *)tb_nn(v_c, 1791))->q_builders); *tb_p1253 = tb_v1253; }
    if (({ const char *tb_s1254 = ((tb_o_Comp *)tb_nn(v_c, 1792))->q_fmark; !(tb_s1254[0] == 0); })) {
        f_emit__put(v_c, "    if (tb_fa) tb_temp_end(tb_fm);\n");
    }
    { const char * tb_v1255 = ""; const char * *tb_p1255 = &(((tb_o_Comp *)tb_nn(v_c, 1794))->q_fmark); *tb_p1255 = tb_v1255; }
    if (((((tb_o_Node *)tb_nn(v_outputs, 1795))->q_kids)->len > 0LL)) {
        f_emit__emit_gives_return(v_c, 1LL);
    } else if (({ const char *tb_s1256 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1797))->q_ty); !((unsigned char)tb_s1256[0] == 110 && (unsigned char)tb_s1256[1] == 111 && (unsigned char)tb_s1256[2] == 110 && (unsigned char)tb_s1256[3] == 101 && tb_s1256[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1257_0 = "    tb_crash("; long long tb_c1257_0n = sizeof("    tb_crash(") - 1; char tb_c1257_1d[24]; long long tb_c1257_1n; const char *tb_c1257_1 = tb_digits(((tb_o_Node *)tb_nn(v_f, 1798))->q_line, tb_c1257_1d, &tb_c1257_1n); const char *tb_c1257_2 = ", \"функция "; long long tb_c1257_2n = sizeof(", \"функция ") - 1; const char *tb_c1257_3 = ((tb_o_Node *)tb_nn(v_f, 1798))->q_name; long long tb_c1257_3n = (long long)strlen(tb_c1257_3); const char *tb_c1257_4 = " дошла до конца без return\");\n"; long long tb_c1257_4n = sizeof(" дошла до конца без return\");\n") - 1; char *tb_c1257r = tb_mem(tb_c1257_0n + tb_c1257_1n + tb_c1257_2n + tb_c1257_3n + tb_c1257_4n + 1), *tb_c1257rw = tb_c1257r; memcpy(tb_c1257rw, tb_c1257_0, tb_c1257_0n); tb_c1257rw += tb_c1257_0n; memcpy(tb_c1257rw, tb_c1257_1, tb_c1257_1n); tb_c1257rw += tb_c1257_1n; memcpy(tb_c1257rw, tb_c1257_2, tb_c1257_2n); tb_c1257rw += tb_c1257_2n; memcpy(tb_c1257rw, tb_c1257_3, tb_c1257_3n); tb_c1257rw += tb_c1257_3n; memcpy(tb_c1257rw, tb_c1257_4, tb_c1257_4n); tb_c1257rw += tb_c1257_4n; *tb_c1257rw = 0; (const char *)tb_c1257r; }));
        f_emit__put(v_c, "    return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 1800))->q_ty);
        f_emit__put(v_c, ";\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t) {
    const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1806))->q_kind;
    if (((({ const char *tb_s1258 = v_k; ((unsigned char)tb_s1258[0] == 110 && (unsigned char)tb_s1258[1] == 111 && (unsigned char)tb_s1258[2] == 110 && (unsigned char)tb_s1258[3] == 101 && tb_s1258[4] == 0); }) || ({ const char *tb_s1259 = v_k; ((unsigned char)tb_s1259[0] == 109 && (unsigned char)tb_s1259[1] == 117 && (unsigned char)tb_s1259[2] == 108 && (unsigned char)tb_s1259[3] == 116 && (unsigned char)tb_s1259[4] == 105 && tb_s1259[5] == 0); })) || ({ const char *tb_s1260 = v_k; ((unsigned char)tb_s1260[0] == 117 && (unsigned char)tb_s1260[1] == 110 && (unsigned char)tb_s1260[2] == 107 && (unsigned char)tb_s1260[3] == 110 && (unsigned char)tb_s1260[4] == 111 && (unsigned char)tb_s1260[5] == 119 && (unsigned char)tb_s1260[6] == 110 && tb_s1260[7] == 0); }))) {
        return;
    }
    if (({ const char *tb_s1261 = v_k; ((unsigned char)tb_s1261[0] == 116 && (unsigned char)tb_s1261[1] == 97 && (unsigned char)tb_s1261[2] == 115 && (unsigned char)tb_s1261[3] == 107 && tb_s1261[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1262_0 = "static void tb_show_"; long long tb_c1262_0n = sizeof("static void tb_show_") - 1; char tb_c1262_1d[24]; long long tb_c1262_1n; const char *tb_c1262_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1810))->q_id, tb_c1262_1d, &tb_c1262_1n); const char *tb_c1262_2 = "(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n"; long long tb_c1262_2n = sizeof("(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n") - 1; char *tb_c1262r = tb_mem(tb_c1262_0n + tb_c1262_1n + tb_c1262_2n + 1), *tb_c1262rw = tb_c1262r; memcpy(tb_c1262rw, tb_c1262_0, tb_c1262_0n); tb_c1262rw += tb_c1262_0n; memcpy(tb_c1262rw, tb_c1262_1, tb_c1262_1n); tb_c1262rw += tb_c1262_1n; memcpy(tb_c1262rw, tb_c1262_2, tb_c1262_2n); tb_c1262rw += tb_c1262_2n; *tb_c1262rw = 0; (const char *)tb_c1262r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1263_0 = "static void tb_show_"; long long tb_c1263_0n = sizeof("static void tb_show_") - 1; char tb_c1263_1d[24]; long long tb_c1263_1n; const char *tb_c1263_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1812))->q_id, tb_c1263_1d, &tb_c1263_1n); const char *tb_c1263_2 = "("; long long tb_c1263_2n = sizeof("(") - 1; const char *tb_c1263_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1812))->q_id); long long tb_c1263_3n = (long long)strlen(tb_c1263_3); const char *tb_c1263_4 = " x) {\n"; long long tb_c1263_4n = sizeof(" x) {\n") - 1; char *tb_c1263r = tb_mem(tb_c1263_0n + tb_c1263_1n + tb_c1263_2n + tb_c1263_3n + tb_c1263_4n + 1), *tb_c1263rw = tb_c1263r; memcpy(tb_c1263rw, tb_c1263_0, tb_c1263_0n); tb_c1263rw += tb_c1263_0n; memcpy(tb_c1263rw, tb_c1263_1, tb_c1263_1n); tb_c1263rw += tb_c1263_1n; memcpy(tb_c1263rw, tb_c1263_2, tb_c1263_2n); tb_c1263rw += tb_c1263_2n; memcpy(tb_c1263rw, tb_c1263_3, tb_c1263_3n); tb_c1263rw += tb_c1263_3n; memcpy(tb_c1263rw, tb_c1263_4, tb_c1263_4n); tb_c1263rw += tb_c1263_4n; *tb_c1263rw = 0; (const char *)tb_c1263r; }));
    if (({ const char *tb_s1264 = v_k; ((unsigned char)tb_s1264[0] == 105 && (unsigned char)tb_s1264[1] == 110 && (unsigned char)tb_s1264[2] == 116 && tb_s1264[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%lld\", x);\n");
    } else if (({ const char *tb_s1265 = v_k; ((unsigned char)tb_s1265[0] == 100 && (unsigned char)tb_s1265[1] == 101 && (unsigned char)tb_s1265[2] == 99 && tb_s1265[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%g\", x);\n");
    } else if (({ const char *tb_s1266 = v_k; ((unsigned char)tb_s1266[0] == 98 && (unsigned char)tb_s1266[1] == 111 && (unsigned char)tb_s1266[2] == 111 && (unsigned char)tb_s1266[3] == 108 && tb_s1266[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if (({ const char *tb_s1267 = v_k; ((unsigned char)tb_s1267[0] == 116 && (unsigned char)tb_s1267[1] == 101 && (unsigned char)tb_s1267[2] == 120 && (unsigned char)tb_s1267[3] == 116 && tb_s1267[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x);\n");
    } else if (({ const char *tb_s1268 = v_k; ((unsigned char)tb_s1268[0] == 108 && (unsigned char)tb_s1268[1] == 105 && (unsigned char)tb_s1268[2] == 115 && (unsigned char)tb_s1268[3] == 116 && tb_s1268[4] == 0); })) {
        const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1822))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"[\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn(v_t, 1826))->q_elem)) {
            f_emit__put(v_c, ({ const char *tb_c1269_0 = "        tb_show_"; long long tb_c1269_0n = sizeof("        tb_show_") - 1; char tb_c1269_1d[24]; long long tb_c1269_1n; const char *tb_c1269_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1827))->q_elem, tb_c1269_1d, &tb_c1269_1n); const char *tb_c1269_2 = "(("; long long tb_c1269_2n = sizeof("((") - 1; const char *tb_c1269_3 = v_elem; long long tb_c1269_3n = (long long)strlen(tb_c1269_3); const char *tb_c1269_4 = "){x, i});\n"; long long tb_c1269_4n = sizeof("){x, i});\n") - 1; char *tb_c1269r = tb_mem(tb_c1269_0n + tb_c1269_1n + tb_c1269_2n + tb_c1269_3n + tb_c1269_4n + 1), *tb_c1269rw = tb_c1269r; memcpy(tb_c1269rw, tb_c1269_0, tb_c1269_0n); tb_c1269rw += tb_c1269_0n; memcpy(tb_c1269rw, tb_c1269_1, tb_c1269_1n); tb_c1269rw += tb_c1269_1n; memcpy(tb_c1269rw, tb_c1269_2, tb_c1269_2n); tb_c1269rw += tb_c1269_2n; memcpy(tb_c1269rw, tb_c1269_3, tb_c1269_3n); tb_c1269rw += tb_c1269_3n; memcpy(tb_c1269rw, tb_c1269_4, tb_c1269_4n); tb_c1269rw += tb_c1269_4n; *tb_c1269rw = 0; (const char *)tb_c1269r; }));
        } else if ((((tb_o_Type *)tb_nn(v_t, 1828))->q_elem == 3LL)) {
            f_emit__put(v_c, "        tb_show_3(TB_BIT(x, i));\n");
        } else {
            f_emit__put(v_c, ({ const char *tb_c1270_0 = "        tb_show_"; long long tb_c1270_0n = sizeof("        tb_show_") - 1; char tb_c1270_1d[24]; long long tb_c1270_1n; const char *tb_c1270_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1831))->q_elem, tb_c1270_1d, &tb_c1270_1n); const char *tb_c1270_2 = "((("; long long tb_c1270_2n = sizeof("(((") - 1; const char *tb_c1270_3 = v_elem; long long tb_c1270_3n = (long long)strlen(tb_c1270_3); const char *tb_c1270_4 = " *)x->data)[i]);\n"; long long tb_c1270_4n = sizeof(" *)x->data)[i]);\n") - 1; char *tb_c1270r = tb_mem(tb_c1270_0n + tb_c1270_1n + tb_c1270_2n + tb_c1270_3n + tb_c1270_4n + 1), *tb_c1270rw = tb_c1270r; memcpy(tb_c1270rw, tb_c1270_0, tb_c1270_0n); tb_c1270rw += tb_c1270_0n; memcpy(tb_c1270rw, tb_c1270_1, tb_c1270_1n); tb_c1270rw += tb_c1270_1n; memcpy(tb_c1270rw, tb_c1270_2, tb_c1270_2n); tb_c1270rw += tb_c1270_2n; memcpy(tb_c1270rw, tb_c1270_3, tb_c1270_3n); tb_c1270rw += tb_c1270_3n; memcpy(tb_c1270rw, tb_c1270_4, tb_c1270_4n); tb_c1270rw += tb_c1270_4n; *tb_c1270rw = 0; (const char *)tb_c1270r; }));
        }
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"]\");\n");
    } else if (({ const char *tb_s1271 = v_k; ((unsigned char)tb_s1271[0] == 100 && (unsigned char)tb_s1271[1] == 105 && (unsigned char)tb_s1271[2] == 99 && (unsigned char)tb_s1271[3] == 116 && tb_s1271[4] == 0); })) {
        const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1835))->q_key);
        const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1836))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"{\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        f_emit__put(v_c, ({ const char *tb_c1272_0 = "        tb_show_"; long long tb_c1272_0n = sizeof("        tb_show_") - 1; char tb_c1272_1d[24]; long long tb_c1272_1n; const char *tb_c1272_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1840))->q_key, tb_c1272_1d, &tb_c1272_1n); const char *tb_c1272_2 = "((("; long long tb_c1272_2n = sizeof("(((") - 1; const char *tb_c1272_3 = v_kt; long long tb_c1272_3n = (long long)strlen(tb_c1272_3); const char *tb_c1272_4 = " *)x->keys->data)[i]);\n"; long long tb_c1272_4n = sizeof(" *)x->keys->data)[i]);\n") - 1; char *tb_c1272r = tb_mem(tb_c1272_0n + tb_c1272_1n + tb_c1272_2n + tb_c1272_3n + tb_c1272_4n + 1), *tb_c1272rw = tb_c1272r; memcpy(tb_c1272rw, tb_c1272_0, tb_c1272_0n); tb_c1272rw += tb_c1272_0n; memcpy(tb_c1272rw, tb_c1272_1, tb_c1272_1n); tb_c1272rw += tb_c1272_1n; memcpy(tb_c1272rw, tb_c1272_2, tb_c1272_2n); tb_c1272rw += tb_c1272_2n; memcpy(tb_c1272rw, tb_c1272_3, tb_c1272_3n); tb_c1272rw += tb_c1272_3n; memcpy(tb_c1272rw, tb_c1272_4, tb_c1272_4n); tb_c1272rw += tb_c1272_4n; *tb_c1272rw = 0; (const char *)tb_c1272r; }));
        f_emit__put(v_c, "        tb_ps(\": \");\n");
        f_emit__put(v_c, ({ const char *tb_c1273_0 = "        tb_show_"; long long tb_c1273_0n = sizeof("        tb_show_") - 1; char tb_c1273_1d[24]; long long tb_c1273_1n; const char *tb_c1273_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1842))->q_elem, tb_c1273_1d, &tb_c1273_1n); const char *tb_c1273_2 = "((("; long long tb_c1273_2n = sizeof("(((") - 1; const char *tb_c1273_3 = v_vt; long long tb_c1273_3n = (long long)strlen(tb_c1273_3); const char *tb_c1273_4 = " *)x->vals->data)[i]);\n"; long long tb_c1273_4n = sizeof(" *)x->vals->data)[i]);\n") - 1; char *tb_c1273r = tb_mem(tb_c1273_0n + tb_c1273_1n + tb_c1273_2n + tb_c1273_3n + tb_c1273_4n + 1), *tb_c1273rw = tb_c1273r; memcpy(tb_c1273rw, tb_c1273_0, tb_c1273_0n); tb_c1273rw += tb_c1273_0n; memcpy(tb_c1273rw, tb_c1273_1, tb_c1273_1n); tb_c1273rw += tb_c1273_1n; memcpy(tb_c1273rw, tb_c1273_2, tb_c1273_2n); tb_c1273rw += tb_c1273_2n; memcpy(tb_c1273rw, tb_c1273_3, tb_c1273_3n); tb_c1273rw += tb_c1273_3n; memcpy(tb_c1273rw, tb_c1273_4, tb_c1273_4n); tb_c1273rw += tb_c1273_4n; *tb_c1273rw = 0; (const char *)tb_c1273r; }));
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"}\");\n");
    } else if (({ const char *tb_s1274 = v_k; ((unsigned char)tb_s1274[0] == 111 && (unsigned char)tb_s1274[1] == 98 && (unsigned char)tb_s1274[2] == 106 && tb_s1274[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn(v_t, 1846))->q_obj_node;
        if (((tb_o_Node *)tb_nn(v_o, 1847))->q_by_cols) {
            f_emit__put(v_c, "    if (x.c == 0) { tb_ps(\"пусто\"); return; }\n");
        } else {
            f_emit__put(v_c, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        }
        f_emit__put(v_c, ({ const char *tb_c1275_0 = "    tb_ps(\""; long long tb_c1275_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1275_1 = ((tb_o_Node *)tb_nn(v_o, 1851))->q_name; long long tb_c1275_1n = (long long)strlen(tb_c1275_1); const char *tb_c1275_2 = "(\");\n"; long long tb_c1275_2n = sizeof("(\");\n") - 1; char *tb_c1275r = tb_mem(tb_c1275_0n + tb_c1275_1n + tb_c1275_2n + 1), *tb_c1275rw = tb_c1275r; memcpy(tb_c1275rw, tb_c1275_0, tb_c1275_0n); tb_c1275rw += tb_c1275_0n; memcpy(tb_c1275rw, tb_c1275_1, tb_c1275_1n); tb_c1275rw += tb_c1275_1n; memcpy(tb_c1275rw, tb_c1275_2, tb_c1275_2n); tb_c1275rw += tb_c1275_2n; *tb_c1275rw = 0; (const char *)tb_c1275r; }));
        {
            long long tb_end1276 = ((((tb_o_Node *)tb_nn(v_o, 1852))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1276; v_i++) {
                const char * v_sep = "";
                if ((v_i > 0LL)) {
                    v_sep = ", ";
                }
                f_emit__put(v_c, ({ const char *tb_c1277_0 = "    tb_ps(\""; long long tb_c1277_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1277_1 = v_sep; long long tb_c1277_1n = (long long)strlen(tb_c1277_1); const char *tb_c1277_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1856))->q_kids, v_i, sizeof(tb_o_Node *), 1856)), 1856))->q_name; long long tb_c1277_2n = (long long)strlen(tb_c1277_2); const char *tb_c1277_3 = ": \");\n"; long long tb_c1277_3n = sizeof(": \");\n") - 1; char *tb_c1277r = tb_mem(tb_c1277_0n + tb_c1277_1n + tb_c1277_2n + tb_c1277_3n + 1), *tb_c1277rw = tb_c1277r; memcpy(tb_c1277rw, tb_c1277_0, tb_c1277_0n); tb_c1277rw += tb_c1277_0n; memcpy(tb_c1277rw, tb_c1277_1, tb_c1277_1n); tb_c1277rw += tb_c1277_1n; memcpy(tb_c1277rw, tb_c1277_2, tb_c1277_2n); tb_c1277rw += tb_c1277_2n; memcpy(tb_c1277rw, tb_c1277_3, tb_c1277_3n); tb_c1277rw += tb_c1277_3n; *tb_c1277rw = 0; (const char *)tb_c1277r; }));
                if (((tb_o_Node *)tb_nn(v_o, 1857))->q_by_cols) {
                    f_emit__put(v_c, ({ const char *tb_c1278_0 = "    tb_show_"; long long tb_c1278_0n = sizeof("    tb_show_") - 1; char tb_c1278_1d[24]; long long tb_c1278_1n; const char *tb_c1278_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1858))->q_kids, v_i, sizeof(tb_o_Node *), 1858)), 1858))->q_ty, tb_c1278_1d, &tb_c1278_1n); const char *tb_c1278_2 = "(x.c->q_"; long long tb_c1278_2n = sizeof("(x.c->q_") - 1; const char *tb_c1278_3 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1858))->q_kids, v_i, sizeof(tb_o_Node *), 1858)), 1858))->q_name; long long tb_c1278_3n = (long long)strlen(tb_c1278_3); const char *tb_c1278_4 = "[x.i]);\n"; long long tb_c1278_4n = sizeof("[x.i]);\n") - 1; char *tb_c1278r = tb_mem(tb_c1278_0n + tb_c1278_1n + tb_c1278_2n + tb_c1278_3n + tb_c1278_4n + 1), *tb_c1278rw = tb_c1278r; memcpy(tb_c1278rw, tb_c1278_0, tb_c1278_0n); tb_c1278rw += tb_c1278_0n; memcpy(tb_c1278rw, tb_c1278_1, tb_c1278_1n); tb_c1278rw += tb_c1278_1n; memcpy(tb_c1278rw, tb_c1278_2, tb_c1278_2n); tb_c1278rw += tb_c1278_2n; memcpy(tb_c1278rw, tb_c1278_3, tb_c1278_3n); tb_c1278rw += tb_c1278_3n; memcpy(tb_c1278rw, tb_c1278_4, tb_c1278_4n); tb_c1278rw += tb_c1278_4n; *tb_c1278rw = 0; (const char *)tb_c1278r; }));
                } else {
                    f_emit__put(v_c, ({ const char *tb_c1279_0 = "    tb_show_"; long long tb_c1279_0n = sizeof("    tb_show_") - 1; char tb_c1279_1d[24]; long long tb_c1279_1n; const char *tb_c1279_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1860))->q_kids, v_i, sizeof(tb_o_Node *), 1860)), 1860))->q_ty, tb_c1279_1d, &tb_c1279_1n); const char *tb_c1279_2 = "(x"; long long tb_c1279_2n = sizeof("(x") - 1; const char *tb_c1279_3 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1860))->q_kids, v_i, sizeof(tb_o_Node *), 1860)), 1860))->q_name); long long tb_c1279_3n = (long long)strlen(tb_c1279_3); const char *tb_c1279_4 = ");\n"; long long tb_c1279_4n = sizeof(");\n") - 1; char *tb_c1279r = tb_mem(tb_c1279_0n + tb_c1279_1n + tb_c1279_2n + tb_c1279_3n + tb_c1279_4n + 1), *tb_c1279rw = tb_c1279r; memcpy(tb_c1279rw, tb_c1279_0, tb_c1279_0n); tb_c1279rw += tb_c1279_0n; memcpy(tb_c1279rw, tb_c1279_1, tb_c1279_1n); tb_c1279rw += tb_c1279_1n; memcpy(tb_c1279rw, tb_c1279_2, tb_c1279_2n); tb_c1279rw += tb_c1279_2n; memcpy(tb_c1279rw, tb_c1279_3, tb_c1279_3n); tb_c1279rw += tb_c1279_3n; memcpy(tb_c1279rw, tb_c1279_4, tb_c1279_4n); tb_c1279rw += tb_c1279_4n; *tb_c1279rw = 0; (const char *)tb_c1279r; }));
                }
            }
        }
        f_emit__put(v_c, "    tb_ps(\")\");\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    f_emit__put(v_c, ({ const char *tb_c1280_0 = "struct tb_cols_"; long long tb_c1280_0n = sizeof("struct tb_cols_") - 1; const char *tb_c1280_1 = ((tb_o_Node *)tb_nn(v_o, 1868))->q_name; long long tb_c1280_1n = (long long)strlen(tb_c1280_1); const char *tb_c1280_2 = " {\n    long long len, cap;\n"; long long tb_c1280_2n = sizeof(" {\n    long long len, cap;\n") - 1; char *tb_c1280r = tb_mem(tb_c1280_0n + tb_c1280_1n + tb_c1280_2n + 1), *tb_c1280rw = tb_c1280r; memcpy(tb_c1280rw, tb_c1280_0, tb_c1280_0n); tb_c1280rw += tb_c1280_0n; memcpy(tb_c1280rw, tb_c1280_1, tb_c1280_1n); tb_c1280rw += tb_c1280_1n; memcpy(tb_c1280rw, tb_c1280_2, tb_c1280_2n); tb_c1280rw += tb_c1280_2n; *tb_c1280rw = 0; (const char *)tb_c1280r; }));
    {
        tb_list * tb_l1281 = ((tb_o_Node *)tb_nn(v_o, 1869))->q_kids;
        for (long long tb_i1281 = 0; tb_i1281 < tb_l1281->len; tb_i1281++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1281->data)[tb_i1281];
            f_emit__put(v_c, ({ const char *tb_c1282_0 = "    "; long long tb_c1282_0n = sizeof("    ") - 1; const char *tb_c1282_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1870))->q_ty); long long tb_c1282_1n = (long long)strlen(tb_c1282_1); const char *tb_c1282_2 = " *q_"; long long tb_c1282_2n = sizeof(" *q_") - 1; const char *tb_c1282_3 = ((tb_o_Node *)tb_nn(v_f, 1870))->q_name; long long tb_c1282_3n = (long long)strlen(tb_c1282_3); const char *tb_c1282_4 = ";\n"; long long tb_c1282_4n = sizeof(";\n") - 1; char *tb_c1282r = tb_mem(tb_c1282_0n + tb_c1282_1n + tb_c1282_2n + tb_c1282_3n + tb_c1282_4n + 1), *tb_c1282rw = tb_c1282r; memcpy(tb_c1282rw, tb_c1282_0, tb_c1282_0n); tb_c1282rw += tb_c1282_0n; memcpy(tb_c1282rw, tb_c1282_1, tb_c1282_1n); tb_c1282rw += tb_c1282_1n; memcpy(tb_c1282rw, tb_c1282_2, tb_c1282_2n); tb_c1282rw += tb_c1282_2n; memcpy(tb_c1282rw, tb_c1282_3, tb_c1282_3n); tb_c1282rw += tb_c1282_3n; memcpy(tb_c1282rw, tb_c1282_4, tb_c1282_4n); tb_c1282rw += tb_c1282_4n; *tb_c1282rw = 0; (const char *)tb_c1282r; }));
        }
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1874))->q_by_cols) {
        f_emit__emit_cols_obj(v_c, v_o);
        return;
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1283_0 = "struct tb_c_"; long long tb_c1283_0n = sizeof("struct tb_c_") - 1; const char *tb_c1283_1 = ((tb_o_Node *)tb_nn(v_o, 1879))->q_name; long long tb_c1283_1n = (long long)strlen(tb_c1283_1); const char *tb_c1283_2 = " {\n"; long long tb_c1283_2n = sizeof(" {\n") - 1; char *tb_c1283r = tb_mem(tb_c1283_0n + tb_c1283_1n + tb_c1283_2n + 1), *tb_c1283rw = tb_c1283r; memcpy(tb_c1283rw, tb_c1283_0, tb_c1283_0n); tb_c1283rw += tb_c1283_0n; memcpy(tb_c1283rw, tb_c1283_1, tb_c1283_1n); tb_c1283rw += tb_c1283_1n; memcpy(tb_c1283rw, tb_c1283_2, tb_c1283_2n); tb_c1283rw += tb_c1283_2n; *tb_c1283rw = 0; (const char *)tb_c1283r; }));
        {
            tb_list * tb_l1284 = ((tb_o_Node *)tb_nn(v_o, 1880))->q_kids;
            for (long long tb_i1284 = 0; tb_i1284 < tb_l1284->len; tb_i1284++) {
                tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1284->data)[tb_i1284];
                if (((tb_o_Node *)tb_nn(v_f, 1881))->q_is_cold) {
                    f_emit__put(v_c, ({ const char *tb_c1285_0 = "    "; long long tb_c1285_0n = sizeof("    ") - 1; const char *tb_c1285_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1882))->q_ty); long long tb_c1285_1n = (long long)strlen(tb_c1285_1); const char *tb_c1285_2 = " q_"; long long tb_c1285_2n = sizeof(" q_") - 1; const char *tb_c1285_3 = ((tb_o_Node *)tb_nn(v_f, 1882))->q_name; long long tb_c1285_3n = (long long)strlen(tb_c1285_3); const char *tb_c1285_4 = ";\n"; long long tb_c1285_4n = sizeof(";\n") - 1; char *tb_c1285r = tb_mem(tb_c1285_0n + tb_c1285_1n + tb_c1285_2n + tb_c1285_3n + tb_c1285_4n + 1), *tb_c1285rw = tb_c1285r; memcpy(tb_c1285rw, tb_c1285_0, tb_c1285_0n); tb_c1285rw += tb_c1285_0n; memcpy(tb_c1285rw, tb_c1285_1, tb_c1285_1n); tb_c1285rw += tb_c1285_1n; memcpy(tb_c1285rw, tb_c1285_2, tb_c1285_2n); tb_c1285rw += tb_c1285_2n; memcpy(tb_c1285rw, tb_c1285_3, tb_c1285_3n); tb_c1285rw += tb_c1285_3n; memcpy(tb_c1285rw, tb_c1285_4, tb_c1285_4n); tb_c1285rw += tb_c1285_4n; *tb_c1285rw = 0; (const char *)tb_c1285r; }));
                }
            }
        }
        f_emit__put(v_c, "};\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1286_0 = "struct tb_o_"; long long tb_c1286_0n = sizeof("struct tb_o_") - 1; const char *tb_c1286_1 = ((tb_o_Node *)tb_nn(v_o, 1884))->q_name; long long tb_c1286_1n = (long long)strlen(tb_c1286_1); const char *tb_c1286_2 = " {\n"; long long tb_c1286_2n = sizeof(" {\n") - 1; char *tb_c1286r = tb_mem(tb_c1286_0n + tb_c1286_1n + tb_c1286_2n + 1), *tb_c1286rw = tb_c1286r; memcpy(tb_c1286rw, tb_c1286_0, tb_c1286_0n); tb_c1286rw += tb_c1286_0n; memcpy(tb_c1286rw, tb_c1286_1, tb_c1286_1n); tb_c1286rw += tb_c1286_1n; memcpy(tb_c1286rw, tb_c1286_2, tb_c1286_2n); tb_c1286rw += tb_c1286_2n; *tb_c1286rw = 0; (const char *)tb_c1286r; }));
    {
        tb_list * tb_l1287 = ((tb_o_Node *)tb_nn(v_o, 1885))->q_kids;
        for (long long tb_i1287 = 0; tb_i1287 < tb_l1287->len; tb_i1287++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1287->data)[tb_i1287];
            if ((!((tb_o_Node *)tb_nn(v_f, 1886))->q_is_cold)) {
                f_emit__put(v_c, ({ const char *tb_c1288_0 = "    "; long long tb_c1288_0n = sizeof("    ") - 1; const char *tb_c1288_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1887))->q_ty); long long tb_c1288_1n = (long long)strlen(tb_c1288_1); const char *tb_c1288_2 = " q_"; long long tb_c1288_2n = sizeof(" q_") - 1; const char *tb_c1288_3 = ((tb_o_Node *)tb_nn(v_f, 1887))->q_name; long long tb_c1288_3n = (long long)strlen(tb_c1288_3); const char *tb_c1288_4 = ";\n"; long long tb_c1288_4n = sizeof(";\n") - 1; char *tb_c1288r = tb_mem(tb_c1288_0n + tb_c1288_1n + tb_c1288_2n + tb_c1288_3n + tb_c1288_4n + 1), *tb_c1288rw = tb_c1288r; memcpy(tb_c1288rw, tb_c1288_0, tb_c1288_0n); tb_c1288rw += tb_c1288_0n; memcpy(tb_c1288rw, tb_c1288_1, tb_c1288_1n); tb_c1288rw += tb_c1288_1n; memcpy(tb_c1288rw, tb_c1288_2, tb_c1288_2n); tb_c1288rw += tb_c1288_2n; memcpy(tb_c1288rw, tb_c1288_3, tb_c1288_3n); tb_c1288rw += tb_c1288_3n; memcpy(tb_c1288rw, tb_c1288_4, tb_c1288_4n); tb_c1288rw += tb_c1288_4n; *tb_c1288rw = 0; (const char *)tb_c1288r; }));
            }
        }
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1289_0 = "    struct tb_c_"; long long tb_c1289_0n = sizeof("    struct tb_c_") - 1; const char *tb_c1289_1 = ((tb_o_Node *)tb_nn(v_o, 1889))->q_name; long long tb_c1289_1n = (long long)strlen(tb_c1289_1); const char *tb_c1289_2 = " *tb_cold;\n"; long long tb_c1289_2n = sizeof(" *tb_cold;\n") - 1; char *tb_c1289r = tb_mem(tb_c1289_0n + tb_c1289_1n + tb_c1289_2n + 1), *tb_c1289rw = tb_c1289r; memcpy(tb_c1289rw, tb_c1289_0, tb_c1289_0n); tb_c1289rw += tb_c1289_0n; memcpy(tb_c1289rw, tb_c1289_1, tb_c1289_1n); tb_c1289rw += tb_c1289_1n; memcpy(tb_c1289rw, tb_c1289_2, tb_c1289_2n); tb_c1289rw += tb_c1289_2n; *tb_c1289rw = 0; (const char *)tb_c1289r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_o, 1890))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "    char empty;\n");
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__has_cold(tb_o_Node * v_o) {
    {
        tb_list * tb_l1290 = ((tb_o_Node *)tb_nn(v_o, 1895))->q_kids;
        for (long long tb_i1290 = 0; tb_i1290 < tb_l1290->len; tb_i1290++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1290->data)[tb_i1290];
            if (((tb_o_Node *)tb_nn(v_f, 1896))->q_is_cold) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1894, "функция has_cold дошла до конца без return");
    return 0;
}

static const char * f_emit__field_path(tb_o_Node * v_o, const char * v_name) {
    const char * v_r = "";
    {
        tb_list * tb_l1291 = ((tb_o_Node *)tb_nn(v_o, 1902))->q_kids;
        for (long long tb_i1291 = 0; tb_i1291 < tb_l1291->len; tb_i1291++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1291->data)[tb_i1291];
            if (((strcmp(((tb_o_Node *)tb_nn(v_f, 1903))->q_name, v_name) == 0) && ((tb_o_Node *)tb_nn(v_f, 1903))->q_is_cold)) {
                return ({ const char *tb_c1292_0 = "->tb_cold->q_"; long long tb_c1292_0n = sizeof("->tb_cold->q_") - 1; const char *tb_c1292_1 = v_name; long long tb_c1292_1n = (long long)strlen(tb_c1292_1); char *tb_c1292r = tb_mem(tb_c1292_0n + tb_c1292_1n + 1), *tb_c1292rw = tb_c1292r; memcpy(tb_c1292rw, tb_c1292_0, tb_c1292_0n); tb_c1292rw += tb_c1292_0n; memcpy(tb_c1292rw, tb_c1292_1, tb_c1292_1n); tb_c1292rw += tb_c1292_1n; *tb_c1292rw = 0; (const char *)tb_c1292r; });
            }
        }
    }
    return ({ const char *tb_c1293_0 = "->q_"; long long tb_c1293_0n = sizeof("->q_") - 1; const char *tb_c1293_1 = v_name; long long tb_c1293_1n = (long long)strlen(tb_c1293_1); char *tb_c1293r = tb_mem(tb_c1293_0n + tb_c1293_1n + 1), *tb_c1293rw = tb_c1293r; memcpy(tb_c1293rw, tb_c1293_0, tb_c1293_0n); tb_c1293rw += tb_c1293_0n; memcpy(tb_c1293rw, tb_c1293_1, tb_c1293_1n); tb_c1293rw += tb_c1293_1n; *tb_c1293rw = 0; (const char *)tb_c1293r; });
    return v_r;
}

static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o) {
    const char * v_nm = ((tb_o_Node *)tb_nn(v_o, 1909))->q_name;
    const char * v_cols = ({ const char *tb_c1294_0 = "tb_cols_"; long long tb_c1294_0n = sizeof("tb_cols_") - 1; const char *tb_c1294_1 = v_nm; long long tb_c1294_1n = (long long)strlen(tb_c1294_1); char *tb_c1294r = tb_mem(tb_c1294_0n + tb_c1294_1n + 1), *tb_c1294rw = tb_c1294r; memcpy(tb_c1294rw, tb_c1294_0, tb_c1294_0n); tb_c1294rw += tb_c1294_0n; memcpy(tb_c1294rw, tb_c1294_1, tb_c1294_1n); tb_c1294rw += tb_c1294_1n; *tb_c1294rw = 0; (const char *)tb_c1294r; });
    const char * v_row = ({ const char *tb_c1295_0 = "tb_row_"; long long tb_c1295_0n = sizeof("tb_row_") - 1; const char *tb_c1295_1 = v_nm; long long tb_c1295_1n = (long long)strlen(tb_c1295_1); char *tb_c1295r = tb_mem(tb_c1295_0n + tb_c1295_1n + 1), *tb_c1295rw = tb_c1295r; memcpy(tb_c1295rw, tb_c1295_0, tb_c1295_0n); tb_c1295rw += tb_c1295_0n; memcpy(tb_c1295rw, tb_c1295_1, tb_c1295_1n); tb_c1295rw += tb_c1295_1n; *tb_c1295rw = 0; (const char *)tb_c1295r; });
    f_emit__put(v_c, ({ const char *tb_c1296_0 = "static "; long long tb_c1296_0n = sizeof("static ") - 1; const char *tb_c1296_1 = v_cols; long long tb_c1296_1n = (long long)strlen(tb_c1296_1); const char *tb_c1296_2 = " *tb_cols_new_"; long long tb_c1296_2n = sizeof(" *tb_cols_new_") - 1; const char *tb_c1296_3 = v_nm; long long tb_c1296_3n = (long long)strlen(tb_c1296_3); const char *tb_c1296_4 = "(void) {\n"; long long tb_c1296_4n = sizeof("(void) {\n") - 1; char *tb_c1296r = tb_mem(tb_c1296_0n + tb_c1296_1n + tb_c1296_2n + tb_c1296_3n + tb_c1296_4n + 1), *tb_c1296rw = tb_c1296r; memcpy(tb_c1296rw, tb_c1296_0, tb_c1296_0n); tb_c1296rw += tb_c1296_0n; memcpy(tb_c1296rw, tb_c1296_1, tb_c1296_1n); tb_c1296rw += tb_c1296_1n; memcpy(tb_c1296rw, tb_c1296_2, tb_c1296_2n); tb_c1296rw += tb_c1296_2n; memcpy(tb_c1296rw, tb_c1296_3, tb_c1296_3n); tb_c1296rw += tb_c1296_3n; memcpy(tb_c1296rw, tb_c1296_4, tb_c1296_4n); tb_c1296rw += tb_c1296_4n; *tb_c1296rw = 0; (const char *)tb_c1296r; }));
    f_emit__put(v_c, ({ const char *tb_c1297_0 = "    "; long long tb_c1297_0n = sizeof("    ") - 1; const char *tb_c1297_1 = v_cols; long long tb_c1297_1n = (long long)strlen(tb_c1297_1); const char *tb_c1297_2 = " *l = tb_mem(sizeof("; long long tb_c1297_2n = sizeof(" *l = tb_mem(sizeof(") - 1; const char *tb_c1297_3 = v_cols; long long tb_c1297_3n = (long long)strlen(tb_c1297_3); const char *tb_c1297_4 = "));\n    l->len = 0; l->cap = 4;\n"; long long tb_c1297_4n = sizeof("));\n    l->len = 0; l->cap = 4;\n") - 1; char *tb_c1297r = tb_mem(tb_c1297_0n + tb_c1297_1n + tb_c1297_2n + tb_c1297_3n + tb_c1297_4n + 1), *tb_c1297rw = tb_c1297r; memcpy(tb_c1297rw, tb_c1297_0, tb_c1297_0n); tb_c1297rw += tb_c1297_0n; memcpy(tb_c1297rw, tb_c1297_1, tb_c1297_1n); tb_c1297rw += tb_c1297_1n; memcpy(tb_c1297rw, tb_c1297_2, tb_c1297_2n); tb_c1297rw += tb_c1297_2n; memcpy(tb_c1297rw, tb_c1297_3, tb_c1297_3n); tb_c1297rw += tb_c1297_3n; memcpy(tb_c1297rw, tb_c1297_4, tb_c1297_4n); tb_c1297rw += tb_c1297_4n; *tb_c1297rw = 0; (const char *)tb_c1297r; }));
    {
        tb_list * tb_l1298 = ((tb_o_Node *)tb_nn(v_o, 1914))->q_kids;
        for (long long tb_i1298 = 0; tb_i1298 < tb_l1298->len; tb_i1298++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1298->data)[tb_i1298];
            f_emit__put(v_c, ({ const char *tb_c1299_0 = "    l->q_"; long long tb_c1299_0n = sizeof("    l->q_") - 1; const char *tb_c1299_1 = ((tb_o_Node *)tb_nn(v_f, 1915))->q_name; long long tb_c1299_1n = (long long)strlen(tb_c1299_1); const char *tb_c1299_2 = " = tb_mem(sizeof("; long long tb_c1299_2n = sizeof(" = tb_mem(sizeof(") - 1; const char *tb_c1299_3 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1915))->q_ty); long long tb_c1299_3n = (long long)strlen(tb_c1299_3); const char *tb_c1299_4 = ") * 4);\n"; long long tb_c1299_4n = sizeof(") * 4);\n") - 1; char *tb_c1299r = tb_mem(tb_c1299_0n + tb_c1299_1n + tb_c1299_2n + tb_c1299_3n + tb_c1299_4n + 1), *tb_c1299rw = tb_c1299r; memcpy(tb_c1299rw, tb_c1299_0, tb_c1299_0n); tb_c1299rw += tb_c1299_0n; memcpy(tb_c1299rw, tb_c1299_1, tb_c1299_1n); tb_c1299rw += tb_c1299_1n; memcpy(tb_c1299rw, tb_c1299_2, tb_c1299_2n); tb_c1299rw += tb_c1299_2n; memcpy(tb_c1299rw, tb_c1299_3, tb_c1299_3n); tb_c1299rw += tb_c1299_3n; memcpy(tb_c1299rw, tb_c1299_4, tb_c1299_4n); tb_c1299rw += tb_c1299_4n; *tb_c1299rw = 0; (const char *)tb_c1299r; }));
        }
    }
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1300_0 = "static void tb_colgrow_"; long long tb_c1300_0n = sizeof("static void tb_colgrow_") - 1; const char *tb_c1300_1 = v_nm; long long tb_c1300_1n = (long long)strlen(tb_c1300_1); const char *tb_c1300_2 = "("; long long tb_c1300_2n = sizeof("(") - 1; const char *tb_c1300_3 = v_cols; long long tb_c1300_3n = (long long)strlen(tb_c1300_3); const char *tb_c1300_4 = " *l) {\n"; long long tb_c1300_4n = sizeof(" *l) {\n") - 1; char *tb_c1300r = tb_mem(tb_c1300_0n + tb_c1300_1n + tb_c1300_2n + tb_c1300_3n + tb_c1300_4n + 1), *tb_c1300rw = tb_c1300r; memcpy(tb_c1300rw, tb_c1300_0, tb_c1300_0n); tb_c1300rw += tb_c1300_0n; memcpy(tb_c1300rw, tb_c1300_1, tb_c1300_1n); tb_c1300rw += tb_c1300_1n; memcpy(tb_c1300rw, tb_c1300_2, tb_c1300_2n); tb_c1300rw += tb_c1300_2n; memcpy(tb_c1300rw, tb_c1300_3, tb_c1300_3n); tb_c1300rw += tb_c1300_3n; memcpy(tb_c1300rw, tb_c1300_4, tb_c1300_4n); tb_c1300rw += tb_c1300_4n; *tb_c1300rw = 0; (const char *)tb_c1300r; }));
    f_emit__put(v_c, "    if (l->len < l->cap) return;\n    l->cap *= 2;\n");
    {
        tb_list * tb_l1301 = ((tb_o_Node *)tb_nn(v_o, 1919))->q_kids;
        for (long long tb_i1301 = 0; tb_i1301 < tb_l1301->len; tb_i1301++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1301->data)[tb_i1301];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1920))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1302_0 = "    l->q_"; long long tb_c1302_0n = sizeof("    l->q_") - 1; const char *tb_c1302_1 = ((tb_o_Node *)tb_nn(v_f, 1921))->q_name; long long tb_c1302_1n = (long long)strlen(tb_c1302_1); const char *tb_c1302_2 = " = tb_resize(l->q_"; long long tb_c1302_2n = sizeof(" = tb_resize(l->q_") - 1; const char *tb_c1302_3 = ((tb_o_Node *)tb_nn(v_f, 1921))->q_name; long long tb_c1302_3n = (long long)strlen(tb_c1302_3); const char *tb_c1302_4 = ", sizeof("; long long tb_c1302_4n = sizeof(", sizeof(") - 1; const char *tb_c1302_5 = v_ft; long long tb_c1302_5n = (long long)strlen(tb_c1302_5); const char *tb_c1302_6 = ") * (l->cap / 2), sizeof("; long long tb_c1302_6n = sizeof(") * (l->cap / 2), sizeof(") - 1; const char *tb_c1302_7 = v_ft; long long tb_c1302_7n = (long long)strlen(tb_c1302_7); const char *tb_c1302_8 = ") * l->cap);\n"; long long tb_c1302_8n = sizeof(") * l->cap);\n") - 1; char *tb_c1302r = tb_mem(tb_c1302_0n + tb_c1302_1n + tb_c1302_2n + tb_c1302_3n + tb_c1302_4n + tb_c1302_5n + tb_c1302_6n + tb_c1302_7n + tb_c1302_8n + 1), *tb_c1302rw = tb_c1302r; memcpy(tb_c1302rw, tb_c1302_0, tb_c1302_0n); tb_c1302rw += tb_c1302_0n; memcpy(tb_c1302rw, tb_c1302_1, tb_c1302_1n); tb_c1302rw += tb_c1302_1n; memcpy(tb_c1302rw, tb_c1302_2, tb_c1302_2n); tb_c1302rw += tb_c1302_2n; memcpy(tb_c1302rw, tb_c1302_3, tb_c1302_3n); tb_c1302rw += tb_c1302_3n; memcpy(tb_c1302rw, tb_c1302_4, tb_c1302_4n); tb_c1302rw += tb_c1302_4n; memcpy(tb_c1302rw, tb_c1302_5, tb_c1302_5n); tb_c1302rw += tb_c1302_5n; memcpy(tb_c1302rw, tb_c1302_6, tb_c1302_6n); tb_c1302rw += tb_c1302_6n; memcpy(tb_c1302rw, tb_c1302_7, tb_c1302_7n); tb_c1302rw += tb_c1302_7n; memcpy(tb_c1302rw, tb_c1302_8, tb_c1302_8n); tb_c1302rw += tb_c1302_8n; *tb_c1302rw = 0; (const char *)tb_c1302r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1303_0 = "static "; long long tb_c1303_0n = sizeof("static ") - 1; const char *tb_c1303_1 = v_row; long long tb_c1303_1n = (long long)strlen(tb_c1303_1); const char *tb_c1303_2 = " tb_colat_"; long long tb_c1303_2n = sizeof(" tb_colat_") - 1; const char *tb_c1303_3 = v_nm; long long tb_c1303_3n = (long long)strlen(tb_c1303_3); const char *tb_c1303_4 = "("; long long tb_c1303_4n = sizeof("(") - 1; const char *tb_c1303_5 = v_cols; long long tb_c1303_5n = (long long)strlen(tb_c1303_5); const char *tb_c1303_6 = " *l, long long i, int line) {\n"; long long tb_c1303_6n = sizeof(" *l, long long i, int line) {\n") - 1; char *tb_c1303r = tb_mem(tb_c1303_0n + tb_c1303_1n + tb_c1303_2n + tb_c1303_3n + tb_c1303_4n + tb_c1303_5n + tb_c1303_6n + 1), *tb_c1303rw = tb_c1303r; memcpy(tb_c1303rw, tb_c1303_0, tb_c1303_0n); tb_c1303rw += tb_c1303_0n; memcpy(tb_c1303rw, tb_c1303_1, tb_c1303_1n); tb_c1303rw += tb_c1303_1n; memcpy(tb_c1303rw, tb_c1303_2, tb_c1303_2n); tb_c1303rw += tb_c1303_2n; memcpy(tb_c1303rw, tb_c1303_3, tb_c1303_3n); tb_c1303rw += tb_c1303_3n; memcpy(tb_c1303rw, tb_c1303_4, tb_c1303_4n); tb_c1303rw += tb_c1303_4n; memcpy(tb_c1303rw, tb_c1303_5, tb_c1303_5n); tb_c1303rw += tb_c1303_5n; memcpy(tb_c1303rw, tb_c1303_6, tb_c1303_6n); tb_c1303rw += tb_c1303_6n; *tb_c1303rw = 0; (const char *)tb_c1303r; }));
    f_emit__put(v_c, ({ const char *tb_c1304_0 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)(("; long long tb_c1304_0n = sizeof("    if (TB_RARE((unsigned long long)i >= (unsigned long long)((") - 1; const char *tb_c1304_1 = v_cols; long long tb_c1304_1n = (long long)strlen(tb_c1304_1); const char *tb_c1304_2 = " *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n"; long long tb_c1304_2n = sizeof(" *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n") - 1; char *tb_c1304r = tb_mem(tb_c1304_0n + tb_c1304_1n + tb_c1304_2n + 1), *tb_c1304rw = tb_c1304r; memcpy(tb_c1304rw, tb_c1304_0, tb_c1304_0n); tb_c1304rw += tb_c1304_0n; memcpy(tb_c1304rw, tb_c1304_1, tb_c1304_1n); tb_c1304rw += tb_c1304_1n; memcpy(tb_c1304rw, tb_c1304_2, tb_c1304_2n); tb_c1304rw += tb_c1304_2n; *tb_c1304rw = 0; (const char *)tb_c1304r; }));
    f_emit__put(v_c, ({ const char *tb_c1305_0 = "    return ("; long long tb_c1305_0n = sizeof("    return (") - 1; const char *tb_c1305_1 = v_row; long long tb_c1305_1n = (long long)strlen(tb_c1305_1); const char *tb_c1305_2 = "){l, i};\n}\n"; long long tb_c1305_2n = sizeof("){l, i};\n}\n") - 1; char *tb_c1305r = tb_mem(tb_c1305_0n + tb_c1305_1n + tb_c1305_2n + 1), *tb_c1305rw = tb_c1305r; memcpy(tb_c1305rw, tb_c1305_0, tb_c1305_0n); tb_c1305rw += tb_c1305_0n; memcpy(tb_c1305rw, tb_c1305_1, tb_c1305_1n); tb_c1305rw += tb_c1305_1n; memcpy(tb_c1305rw, tb_c1305_2, tb_c1305_2n); tb_c1305rw += tb_c1305_2n; *tb_c1305rw = 0; (const char *)tb_c1305r; }));
    {
        tb_list * tb_l1306 = ((tb_o_Node *)tb_nn(v_o, 1926))->q_kids;
        for (long long tb_i1306 = 0; tb_i1306 < tb_l1306->len; tb_i1306++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1306->data)[tb_i1306];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1927))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1307_0 = "static inline "; long long tb_c1307_0n = sizeof("static inline ") - 1; const char *tb_c1307_1 = v_ft; long long tb_c1307_1n = (long long)strlen(tb_c1307_1); const char *tb_c1307_2 = " *tb_colf_"; long long tb_c1307_2n = sizeof(" *tb_colf_") - 1; const char *tb_c1307_3 = v_nm; long long tb_c1307_3n = (long long)strlen(tb_c1307_3); const char *tb_c1307_4 = "_"; long long tb_c1307_4n = sizeof("_") - 1; const char *tb_c1307_5 = ((tb_o_Node *)tb_nn(v_f, 1928))->q_name; long long tb_c1307_5n = (long long)strlen(tb_c1307_5); const char *tb_c1307_6 = "("; long long tb_c1307_6n = sizeof("(") - 1; const char *tb_c1307_7 = v_row; long long tb_c1307_7n = (long long)strlen(tb_c1307_7); const char *tb_c1307_8 = " r, int line) {\n"; long long tb_c1307_8n = sizeof(" r, int line) {\n") - 1; char *tb_c1307r = tb_mem(tb_c1307_0n + tb_c1307_1n + tb_c1307_2n + tb_c1307_3n + tb_c1307_4n + tb_c1307_5n + tb_c1307_6n + tb_c1307_7n + tb_c1307_8n + 1), *tb_c1307rw = tb_c1307r; memcpy(tb_c1307rw, tb_c1307_0, tb_c1307_0n); tb_c1307rw += tb_c1307_0n; memcpy(tb_c1307rw, tb_c1307_1, tb_c1307_1n); tb_c1307rw += tb_c1307_1n; memcpy(tb_c1307rw, tb_c1307_2, tb_c1307_2n); tb_c1307rw += tb_c1307_2n; memcpy(tb_c1307rw, tb_c1307_3, tb_c1307_3n); tb_c1307rw += tb_c1307_3n; memcpy(tb_c1307rw, tb_c1307_4, tb_c1307_4n); tb_c1307rw += tb_c1307_4n; memcpy(tb_c1307rw, tb_c1307_5, tb_c1307_5n); tb_c1307rw += tb_c1307_5n; memcpy(tb_c1307rw, tb_c1307_6, tb_c1307_6n); tb_c1307rw += tb_c1307_6n; memcpy(tb_c1307rw, tb_c1307_7, tb_c1307_7n); tb_c1307rw += tb_c1307_7n; memcpy(tb_c1307rw, tb_c1307_8, tb_c1307_8n); tb_c1307rw += tb_c1307_8n; *tb_c1307rw = 0; (const char *)tb_c1307r; }));
            f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
            f_emit__put(v_c, ({ const char *tb_c1308_0 = "    return &r.c->q_"; long long tb_c1308_0n = sizeof("    return &r.c->q_") - 1; const char *tb_c1308_1 = ((tb_o_Node *)tb_nn(v_f, 1930))->q_name; long long tb_c1308_1n = (long long)strlen(tb_c1308_1); const char *tb_c1308_2 = "[r.i];\n}\n"; long long tb_c1308_2n = sizeof("[r.i];\n}\n") - 1; char *tb_c1308r = tb_mem(tb_c1308_0n + tb_c1308_1n + tb_c1308_2n + 1), *tb_c1308rw = tb_c1308r; memcpy(tb_c1308rw, tb_c1308_0, tb_c1308_0n); tb_c1308rw += tb_c1308_0n; memcpy(tb_c1308rw, tb_c1308_1, tb_c1308_1n); tb_c1308rw += tb_c1308_1n; memcpy(tb_c1308rw, tb_c1308_2, tb_c1308_2n); tb_c1308rw += tb_c1308_2n; *tb_c1308rw = 0; (const char *)tb_c1308r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1309_0 = "static void tb_coladd_"; long long tb_c1309_0n = sizeof("static void tb_coladd_") - 1; const char *tb_c1309_1 = v_nm; long long tb_c1309_1n = (long long)strlen(tb_c1309_1); const char *tb_c1309_2 = "("; long long tb_c1309_2n = sizeof("(") - 1; const char *tb_c1309_3 = v_cols; long long tb_c1309_3n = (long long)strlen(tb_c1309_3); const char *tb_c1309_4 = " *l, "; long long tb_c1309_4n = sizeof(" *l, ") - 1; const char *tb_c1309_5 = v_row; long long tb_c1309_5n = (long long)strlen(tb_c1309_5); const char *tb_c1309_6 = " r, int line) {\n"; long long tb_c1309_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1309r = tb_mem(tb_c1309_0n + tb_c1309_1n + tb_c1309_2n + tb_c1309_3n + tb_c1309_4n + tb_c1309_5n + tb_c1309_6n + 1), *tb_c1309rw = tb_c1309r; memcpy(tb_c1309rw, tb_c1309_0, tb_c1309_0n); tb_c1309rw += tb_c1309_0n; memcpy(tb_c1309rw, tb_c1309_1, tb_c1309_1n); tb_c1309rw += tb_c1309_1n; memcpy(tb_c1309rw, tb_c1309_2, tb_c1309_2n); tb_c1309rw += tb_c1309_2n; memcpy(tb_c1309rw, tb_c1309_3, tb_c1309_3n); tb_c1309rw += tb_c1309_3n; memcpy(tb_c1309rw, tb_c1309_4, tb_c1309_4n); tb_c1309rw += tb_c1309_4n; memcpy(tb_c1309rw, tb_c1309_5, tb_c1309_5n); tb_c1309rw += tb_c1309_5n; memcpy(tb_c1309rw, tb_c1309_6, tb_c1309_6n); tb_c1309rw += tb_c1309_6n; *tb_c1309rw = 0; (const char *)tb_c1309r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        long long tb_end1310 = ((((tb_o_Node *)tb_nn(v_o, 1933))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1310; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1311_0 = "    "; long long tb_c1311_0n = sizeof("    ") - 1; const char *tb_c1311_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1934))->q_kids, v_i, sizeof(tb_o_Node *), 1934)), 1934))->q_ty); long long tb_c1311_1n = (long long)strlen(tb_c1311_1); const char *tb_c1311_2 = " a"; long long tb_c1311_2n = sizeof(" a") - 1; char tb_c1311_3d[24]; long long tb_c1311_3n; const char *tb_c1311_3 = tb_digits(v_i, tb_c1311_3d, &tb_c1311_3n); const char *tb_c1311_4 = " = r.c->q_"; long long tb_c1311_4n = sizeof(" = r.c->q_") - 1; const char *tb_c1311_5 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1934))->q_kids, v_i, sizeof(tb_o_Node *), 1934)), 1934))->q_name; long long tb_c1311_5n = (long long)strlen(tb_c1311_5); const char *tb_c1311_6 = "[r.i];\n"; long long tb_c1311_6n = sizeof("[r.i];\n") - 1; char *tb_c1311r = tb_mem(tb_c1311_0n + tb_c1311_1n + tb_c1311_2n + tb_c1311_3n + tb_c1311_4n + tb_c1311_5n + tb_c1311_6n + 1), *tb_c1311rw = tb_c1311r; memcpy(tb_c1311rw, tb_c1311_0, tb_c1311_0n); tb_c1311rw += tb_c1311_0n; memcpy(tb_c1311rw, tb_c1311_1, tb_c1311_1n); tb_c1311rw += tb_c1311_1n; memcpy(tb_c1311rw, tb_c1311_2, tb_c1311_2n); tb_c1311rw += tb_c1311_2n; memcpy(tb_c1311rw, tb_c1311_3, tb_c1311_3n); tb_c1311rw += tb_c1311_3n; memcpy(tb_c1311rw, tb_c1311_4, tb_c1311_4n); tb_c1311rw += tb_c1311_4n; memcpy(tb_c1311rw, tb_c1311_5, tb_c1311_5n); tb_c1311rw += tb_c1311_5n; memcpy(tb_c1311rw, tb_c1311_6, tb_c1311_6n); tb_c1311rw += tb_c1311_6n; *tb_c1311rw = 0; (const char *)tb_c1311r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1312_0 = "    tb_colgrow_"; long long tb_c1312_0n = sizeof("    tb_colgrow_") - 1; const char *tb_c1312_1 = v_nm; long long tb_c1312_1n = (long long)strlen(tb_c1312_1); const char *tb_c1312_2 = "(tb_nn(l, line));\n"; long long tb_c1312_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1312r = tb_mem(tb_c1312_0n + tb_c1312_1n + tb_c1312_2n + 1), *tb_c1312rw = tb_c1312r; memcpy(tb_c1312rw, tb_c1312_0, tb_c1312_0n); tb_c1312rw += tb_c1312_0n; memcpy(tb_c1312rw, tb_c1312_1, tb_c1312_1n); tb_c1312rw += tb_c1312_1n; memcpy(tb_c1312rw, tb_c1312_2, tb_c1312_2n); tb_c1312rw += tb_c1312_2n; *tb_c1312rw = 0; (const char *)tb_c1312r; }));
    {
        long long tb_end1313 = ((((tb_o_Node *)tb_nn(v_o, 1936))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1313; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1314_0 = "    l->q_"; long long tb_c1314_0n = sizeof("    l->q_") - 1; const char *tb_c1314_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1937))->q_kids, v_i, sizeof(tb_o_Node *), 1937)), 1937))->q_name; long long tb_c1314_1n = (long long)strlen(tb_c1314_1); const char *tb_c1314_2 = "[l->len] = a"; long long tb_c1314_2n = sizeof("[l->len] = a") - 1; char tb_c1314_3d[24]; long long tb_c1314_3n; const char *tb_c1314_3 = tb_digits(v_i, tb_c1314_3d, &tb_c1314_3n); const char *tb_c1314_4 = ";\n"; long long tb_c1314_4n = sizeof(";\n") - 1; char *tb_c1314r = tb_mem(tb_c1314_0n + tb_c1314_1n + tb_c1314_2n + tb_c1314_3n + tb_c1314_4n + 1), *tb_c1314rw = tb_c1314r; memcpy(tb_c1314rw, tb_c1314_0, tb_c1314_0n); tb_c1314rw += tb_c1314_0n; memcpy(tb_c1314rw, tb_c1314_1, tb_c1314_1n); tb_c1314rw += tb_c1314_1n; memcpy(tb_c1314rw, tb_c1314_2, tb_c1314_2n); tb_c1314rw += tb_c1314_2n; memcpy(tb_c1314rw, tb_c1314_3, tb_c1314_3n); tb_c1314rw += tb_c1314_3n; memcpy(tb_c1314rw, tb_c1314_4, tb_c1314_4n); tb_c1314rw += tb_c1314_4n; *tb_c1314rw = 0; (const char *)tb_c1314r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1315_0 = "static void tb_colpush_"; long long tb_c1315_0n = sizeof("static void tb_colpush_") - 1; const char *tb_c1315_1 = v_nm; long long tb_c1315_1n = (long long)strlen(tb_c1315_1); const char *tb_c1315_2 = "("; long long tb_c1315_2n = sizeof("(") - 1; const char *tb_c1315_3 = v_cols; long long tb_c1315_3n = (long long)strlen(tb_c1315_3); const char *tb_c1315_4 = " *l"; long long tb_c1315_4n = sizeof(" *l") - 1; char *tb_c1315r = tb_mem(tb_c1315_0n + tb_c1315_1n + tb_c1315_2n + tb_c1315_3n + tb_c1315_4n + 1), *tb_c1315rw = tb_c1315r; memcpy(tb_c1315rw, tb_c1315_0, tb_c1315_0n); tb_c1315rw += tb_c1315_0n; memcpy(tb_c1315rw, tb_c1315_1, tb_c1315_1n); tb_c1315rw += tb_c1315_1n; memcpy(tb_c1315rw, tb_c1315_2, tb_c1315_2n); tb_c1315rw += tb_c1315_2n; memcpy(tb_c1315rw, tb_c1315_3, tb_c1315_3n); tb_c1315rw += tb_c1315_3n; memcpy(tb_c1315rw, tb_c1315_4, tb_c1315_4n); tb_c1315rw += tb_c1315_4n; *tb_c1315rw = 0; (const char *)tb_c1315r; }));
    {
        long long tb_end1316 = ((((tb_o_Node *)tb_nn(v_o, 1940))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1316; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1317_0 = ", "; long long tb_c1317_0n = sizeof(", ") - 1; const char *tb_c1317_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1941))->q_kids, v_i, sizeof(tb_o_Node *), 1941)), 1941))->q_ty); long long tb_c1317_1n = (long long)strlen(tb_c1317_1); const char *tb_c1317_2 = " a"; long long tb_c1317_2n = sizeof(" a") - 1; char tb_c1317_3d[24]; long long tb_c1317_3n; const char *tb_c1317_3 = tb_digits(v_i, tb_c1317_3d, &tb_c1317_3n); char *tb_c1317r = tb_mem(tb_c1317_0n + tb_c1317_1n + tb_c1317_2n + tb_c1317_3n + 1), *tb_c1317rw = tb_c1317r; memcpy(tb_c1317rw, tb_c1317_0, tb_c1317_0n); tb_c1317rw += tb_c1317_0n; memcpy(tb_c1317rw, tb_c1317_1, tb_c1317_1n); tb_c1317rw += tb_c1317_1n; memcpy(tb_c1317rw, tb_c1317_2, tb_c1317_2n); tb_c1317rw += tb_c1317_2n; memcpy(tb_c1317rw, tb_c1317_3, tb_c1317_3n); tb_c1317rw += tb_c1317_3n; *tb_c1317rw = 0; (const char *)tb_c1317r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1318_0 = ", int line) {\n    tb_colgrow_"; long long tb_c1318_0n = sizeof(", int line) {\n    tb_colgrow_") - 1; const char *tb_c1318_1 = v_nm; long long tb_c1318_1n = (long long)strlen(tb_c1318_1); const char *tb_c1318_2 = "(tb_nn(l, line));\n"; long long tb_c1318_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1318r = tb_mem(tb_c1318_0n + tb_c1318_1n + tb_c1318_2n + 1), *tb_c1318rw = tb_c1318r; memcpy(tb_c1318rw, tb_c1318_0, tb_c1318_0n); tb_c1318rw += tb_c1318_0n; memcpy(tb_c1318rw, tb_c1318_1, tb_c1318_1n); tb_c1318rw += tb_c1318_1n; memcpy(tb_c1318rw, tb_c1318_2, tb_c1318_2n); tb_c1318rw += tb_c1318_2n; *tb_c1318rw = 0; (const char *)tb_c1318r; }));
    {
        long long tb_end1319 = ((((tb_o_Node *)tb_nn(v_o, 1943))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1319; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1320_0 = "    l->q_"; long long tb_c1320_0n = sizeof("    l->q_") - 1; const char *tb_c1320_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1944))->q_kids, v_i, sizeof(tb_o_Node *), 1944)), 1944))->q_name; long long tb_c1320_1n = (long long)strlen(tb_c1320_1); const char *tb_c1320_2 = "[l->len] = a"; long long tb_c1320_2n = sizeof("[l->len] = a") - 1; char tb_c1320_3d[24]; long long tb_c1320_3n; const char *tb_c1320_3 = tb_digits(v_i, tb_c1320_3d, &tb_c1320_3n); const char *tb_c1320_4 = ";\n"; long long tb_c1320_4n = sizeof(";\n") - 1; char *tb_c1320r = tb_mem(tb_c1320_0n + tb_c1320_1n + tb_c1320_2n + tb_c1320_3n + tb_c1320_4n + 1), *tb_c1320rw = tb_c1320r; memcpy(tb_c1320rw, tb_c1320_0, tb_c1320_0n); tb_c1320rw += tb_c1320_0n; memcpy(tb_c1320rw, tb_c1320_1, tb_c1320_1n); tb_c1320rw += tb_c1320_1n; memcpy(tb_c1320rw, tb_c1320_2, tb_c1320_2n); tb_c1320rw += tb_c1320_2n; memcpy(tb_c1320rw, tb_c1320_3, tb_c1320_3n); tb_c1320rw += tb_c1320_3n; memcpy(tb_c1320rw, tb_c1320_4, tb_c1320_4n); tb_c1320rw += tb_c1320_4n; *tb_c1320rw = 0; (const char *)tb_c1320r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1321_0 = "static void tb_colset_"; long long tb_c1321_0n = sizeof("static void tb_colset_") - 1; const char *tb_c1321_1 = v_nm; long long tb_c1321_1n = (long long)strlen(tb_c1321_1); const char *tb_c1321_2 = "("; long long tb_c1321_2n = sizeof("(") - 1; const char *tb_c1321_3 = v_cols; long long tb_c1321_3n = (long long)strlen(tb_c1321_3); const char *tb_c1321_4 = " *l, long long i, "; long long tb_c1321_4n = sizeof(" *l, long long i, ") - 1; const char *tb_c1321_5 = v_row; long long tb_c1321_5n = (long long)strlen(tb_c1321_5); const char *tb_c1321_6 = " r, int line) {\n"; long long tb_c1321_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1321r = tb_mem(tb_c1321_0n + tb_c1321_1n + tb_c1321_2n + tb_c1321_3n + tb_c1321_4n + tb_c1321_5n + tb_c1321_6n + 1), *tb_c1321rw = tb_c1321r; memcpy(tb_c1321rw, tb_c1321_0, tb_c1321_0n); tb_c1321rw += tb_c1321_0n; memcpy(tb_c1321rw, tb_c1321_1, tb_c1321_1n); tb_c1321rw += tb_c1321_1n; memcpy(tb_c1321rw, tb_c1321_2, tb_c1321_2n); tb_c1321rw += tb_c1321_2n; memcpy(tb_c1321rw, tb_c1321_3, tb_c1321_3n); tb_c1321rw += tb_c1321_3n; memcpy(tb_c1321rw, tb_c1321_4, tb_c1321_4n); tb_c1321rw += tb_c1321_4n; memcpy(tb_c1321rw, tb_c1321_5, tb_c1321_5n); tb_c1321rw += tb_c1321_5n; memcpy(tb_c1321rw, tb_c1321_6, tb_c1321_6n); tb_c1321rw += tb_c1321_6n; *tb_c1321rw = 0; (const char *)tb_c1321r; }));
    f_emit__put(v_c, ({ const char *tb_c1322_0 = "    tb_colat_"; long long tb_c1322_0n = sizeof("    tb_colat_") - 1; const char *tb_c1322_1 = v_nm; long long tb_c1322_1n = (long long)strlen(tb_c1322_1); const char *tb_c1322_2 = "(l, i, line);\n"; long long tb_c1322_2n = sizeof("(l, i, line);\n") - 1; char *tb_c1322r = tb_mem(tb_c1322_0n + tb_c1322_1n + tb_c1322_2n + 1), *tb_c1322rw = tb_c1322r; memcpy(tb_c1322rw, tb_c1322_0, tb_c1322_0n); tb_c1322rw += tb_c1322_0n; memcpy(tb_c1322rw, tb_c1322_1, tb_c1322_1n); tb_c1322rw += tb_c1322_1n; memcpy(tb_c1322rw, tb_c1322_2, tb_c1322_2n); tb_c1322rw += tb_c1322_2n; *tb_c1322rw = 0; (const char *)tb_c1322r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        tb_list * tb_l1323 = ((tb_o_Node *)tb_nn(v_o, 1949))->q_kids;
        for (long long tb_i1323 = 0; tb_i1323 < tb_l1323->len; tb_i1323++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1323->data)[tb_i1323];
            f_emit__put(v_c, ({ const char *tb_c1324_0 = "    l->q_"; long long tb_c1324_0n = sizeof("    l->q_") - 1; const char *tb_c1324_1 = ((tb_o_Node *)tb_nn(v_f, 1950))->q_name; long long tb_c1324_1n = (long long)strlen(tb_c1324_1); const char *tb_c1324_2 = "[i] = r.c->q_"; long long tb_c1324_2n = sizeof("[i] = r.c->q_") - 1; const char *tb_c1324_3 = ((tb_o_Node *)tb_nn(v_f, 1950))->q_name; long long tb_c1324_3n = (long long)strlen(tb_c1324_3); const char *tb_c1324_4 = "[r.i];\n"; long long tb_c1324_4n = sizeof("[r.i];\n") - 1; char *tb_c1324r = tb_mem(tb_c1324_0n + tb_c1324_1n + tb_c1324_2n + tb_c1324_3n + tb_c1324_4n + 1), *tb_c1324rw = tb_c1324r; memcpy(tb_c1324rw, tb_c1324_0, tb_c1324_0n); tb_c1324rw += tb_c1324_0n; memcpy(tb_c1324rw, tb_c1324_1, tb_c1324_1n); tb_c1324rw += tb_c1324_1n; memcpy(tb_c1324rw, tb_c1324_2, tb_c1324_2n); tb_c1324rw += tb_c1324_2n; memcpy(tb_c1324rw, tb_c1324_3, tb_c1324_3n); tb_c1324rw += tb_c1324_3n; memcpy(tb_c1324rw, tb_c1324_4, tb_c1324_4n); tb_c1324rw += tb_c1324_4n; *tb_c1324rw = 0; (const char *)tb_c1324r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1325_0 = "static "; long long tb_c1325_0n = sizeof("static ") - 1; const char *tb_c1325_1 = v_cols; long long tb_c1325_1n = (long long)strlen(tb_c1325_1); const char *tb_c1325_2 = " *tb_colsfrom_"; long long tb_c1325_2n = sizeof(" *tb_colsfrom_") - 1; const char *tb_c1325_3 = v_nm; long long tb_c1325_3n = (long long)strlen(tb_c1325_3); const char *tb_c1325_4 = "(long long n, "; long long tb_c1325_4n = sizeof("(long long n, ") - 1; const char *tb_c1325_5 = v_row; long long tb_c1325_5n = (long long)strlen(tb_c1325_5); const char *tb_c1325_6 = " *rows) {\n"; long long tb_c1325_6n = sizeof(" *rows) {\n") - 1; char *tb_c1325r = tb_mem(tb_c1325_0n + tb_c1325_1n + tb_c1325_2n + tb_c1325_3n + tb_c1325_4n + tb_c1325_5n + tb_c1325_6n + 1), *tb_c1325rw = tb_c1325r; memcpy(tb_c1325rw, tb_c1325_0, tb_c1325_0n); tb_c1325rw += tb_c1325_0n; memcpy(tb_c1325rw, tb_c1325_1, tb_c1325_1n); tb_c1325rw += tb_c1325_1n; memcpy(tb_c1325rw, tb_c1325_2, tb_c1325_2n); tb_c1325rw += tb_c1325_2n; memcpy(tb_c1325rw, tb_c1325_3, tb_c1325_3n); tb_c1325rw += tb_c1325_3n; memcpy(tb_c1325rw, tb_c1325_4, tb_c1325_4n); tb_c1325rw += tb_c1325_4n; memcpy(tb_c1325rw, tb_c1325_5, tb_c1325_5n); tb_c1325rw += tb_c1325_5n; memcpy(tb_c1325rw, tb_c1325_6, tb_c1325_6n); tb_c1325rw += tb_c1325_6n; *tb_c1325rw = 0; (const char *)tb_c1325r; }));
    f_emit__put(v_c, ({ const char *tb_c1326_0 = "    "; long long tb_c1326_0n = sizeof("    ") - 1; const char *tb_c1326_1 = v_cols; long long tb_c1326_1n = (long long)strlen(tb_c1326_1); const char *tb_c1326_2 = " *l = tb_cols_new_"; long long tb_c1326_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1326_3 = v_nm; long long tb_c1326_3n = (long long)strlen(tb_c1326_3); const char *tb_c1326_4 = "();\n"; long long tb_c1326_4n = sizeof("();\n") - 1; char *tb_c1326r = tb_mem(tb_c1326_0n + tb_c1326_1n + tb_c1326_2n + tb_c1326_3n + tb_c1326_4n + 1), *tb_c1326rw = tb_c1326r; memcpy(tb_c1326rw, tb_c1326_0, tb_c1326_0n); tb_c1326rw += tb_c1326_0n; memcpy(tb_c1326rw, tb_c1326_1, tb_c1326_1n); tb_c1326rw += tb_c1326_1n; memcpy(tb_c1326rw, tb_c1326_2, tb_c1326_2n); tb_c1326rw += tb_c1326_2n; memcpy(tb_c1326rw, tb_c1326_3, tb_c1326_3n); tb_c1326rw += tb_c1326_3n; memcpy(tb_c1326rw, tb_c1326_4, tb_c1326_4n); tb_c1326rw += tb_c1326_4n; *tb_c1326rw = 0; (const char *)tb_c1326r; }));
    f_emit__put(v_c, ({ const char *tb_c1327_0 = "    for (long long i = 0; i < n; i++) tb_coladd_"; long long tb_c1327_0n = sizeof("    for (long long i = 0; i < n; i++) tb_coladd_") - 1; const char *tb_c1327_1 = v_nm; long long tb_c1327_1n = (long long)strlen(tb_c1327_1); const char *tb_c1327_2 = "(l, rows[i], 0);\n"; long long tb_c1327_2n = sizeof("(l, rows[i], 0);\n") - 1; char *tb_c1327r = tb_mem(tb_c1327_0n + tb_c1327_1n + tb_c1327_2n + 1), *tb_c1327rw = tb_c1327r; memcpy(tb_c1327rw, tb_c1327_0, tb_c1327_0n); tb_c1327rw += tb_c1327_0n; memcpy(tb_c1327rw, tb_c1327_1, tb_c1327_1n); tb_c1327rw += tb_c1327_1n; memcpy(tb_c1327rw, tb_c1327_2, tb_c1327_2n); tb_c1327rw += tb_c1327_2n; *tb_c1327rw = 0; (const char *)tb_c1327r; }));
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1328_0 = "static "; long long tb_c1328_0n = sizeof("static ") - 1; const char *tb_c1328_1 = v_row; long long tb_c1328_1n = (long long)strlen(tb_c1328_1); const char *tb_c1328_2 = " tb_new_"; long long tb_c1328_2n = sizeof(" tb_new_") - 1; const char *tb_c1328_3 = v_nm; long long tb_c1328_3n = (long long)strlen(tb_c1328_3); const char *tb_c1328_4 = "("; long long tb_c1328_4n = sizeof("(") - 1; char *tb_c1328r = tb_mem(tb_c1328_0n + tb_c1328_1n + tb_c1328_2n + tb_c1328_3n + tb_c1328_4n + 1), *tb_c1328rw = tb_c1328r; memcpy(tb_c1328rw, tb_c1328_0, tb_c1328_0n); tb_c1328rw += tb_c1328_0n; memcpy(tb_c1328rw, tb_c1328_1, tb_c1328_1n); tb_c1328rw += tb_c1328_1n; memcpy(tb_c1328rw, tb_c1328_2, tb_c1328_2n); tb_c1328rw += tb_c1328_2n; memcpy(tb_c1328rw, tb_c1328_3, tb_c1328_3n); tb_c1328rw += tb_c1328_3n; memcpy(tb_c1328rw, tb_c1328_4, tb_c1328_4n); tb_c1328rw += tb_c1328_4n; *tb_c1328rw = 0; (const char *)tb_c1328r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1958))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1329 = ((((tb_o_Node *)tb_nn(v_o, 1960))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1329; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1330_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1963))->q_kids, v_i, sizeof(tb_o_Node *), 1963)), 1963))->q_ty); long long tb_c1330_0n = (long long)strlen(tb_c1330_0); const char *tb_c1330_1 = " a"; long long tb_c1330_1n = sizeof(" a") - 1; char tb_c1330_2d[24]; long long tb_c1330_2n; const char *tb_c1330_2 = tb_digits(v_i, tb_c1330_2d, &tb_c1330_2n); char *tb_c1330r = tb_mem(tb_c1330_0n + tb_c1330_1n + tb_c1330_2n + 1), *tb_c1330rw = tb_c1330r; memcpy(tb_c1330rw, tb_c1330_0, tb_c1330_0n); tb_c1330rw += tb_c1330_0n; memcpy(tb_c1330rw, tb_c1330_1, tb_c1330_1n); tb_c1330rw += tb_c1330_1n; memcpy(tb_c1330rw, tb_c1330_2, tb_c1330_2n); tb_c1330rw += tb_c1330_2n; *tb_c1330rw = 0; (const char *)tb_c1330r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1331_0 = ") {\n    "; long long tb_c1331_0n = sizeof(") {\n    ") - 1; const char *tb_c1331_1 = v_cols; long long tb_c1331_1n = (long long)strlen(tb_c1331_1); const char *tb_c1331_2 = " *l = tb_cols_new_"; long long tb_c1331_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1331_3 = v_nm; long long tb_c1331_3n = (long long)strlen(tb_c1331_3); const char *tb_c1331_4 = "();\n"; long long tb_c1331_4n = sizeof("();\n") - 1; char *tb_c1331r = tb_mem(tb_c1331_0n + tb_c1331_1n + tb_c1331_2n + tb_c1331_3n + tb_c1331_4n + 1), *tb_c1331rw = tb_c1331r; memcpy(tb_c1331rw, tb_c1331_0, tb_c1331_0n); tb_c1331rw += tb_c1331_0n; memcpy(tb_c1331rw, tb_c1331_1, tb_c1331_1n); tb_c1331rw += tb_c1331_1n; memcpy(tb_c1331rw, tb_c1331_2, tb_c1331_2n); tb_c1331rw += tb_c1331_2n; memcpy(tb_c1331rw, tb_c1331_3, tb_c1331_3n); tb_c1331rw += tb_c1331_3n; memcpy(tb_c1331rw, tb_c1331_4, tb_c1331_4n); tb_c1331rw += tb_c1331_4n; *tb_c1331rw = 0; (const char *)tb_c1331r; }));
    {
        long long tb_end1332 = ((((tb_o_Node *)tb_nn(v_o, 1965))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1332; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1333_0 = "    l->q_"; long long tb_c1333_0n = sizeof("    l->q_") - 1; const char *tb_c1333_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1966))->q_kids, v_i, sizeof(tb_o_Node *), 1966)), 1966))->q_name; long long tb_c1333_1n = (long long)strlen(tb_c1333_1); const char *tb_c1333_2 = "[0] = a"; long long tb_c1333_2n = sizeof("[0] = a") - 1; char tb_c1333_3d[24]; long long tb_c1333_3n; const char *tb_c1333_3 = tb_digits(v_i, tb_c1333_3d, &tb_c1333_3n); const char *tb_c1333_4 = ";\n"; long long tb_c1333_4n = sizeof(";\n") - 1; char *tb_c1333r = tb_mem(tb_c1333_0n + tb_c1333_1n + tb_c1333_2n + tb_c1333_3n + tb_c1333_4n + 1), *tb_c1333rw = tb_c1333r; memcpy(tb_c1333rw, tb_c1333_0, tb_c1333_0n); tb_c1333rw += tb_c1333_0n; memcpy(tb_c1333rw, tb_c1333_1, tb_c1333_1n); tb_c1333rw += tb_c1333_1n; memcpy(tb_c1333rw, tb_c1333_2, tb_c1333_2n); tb_c1333rw += tb_c1333_2n; memcpy(tb_c1333rw, tb_c1333_3, tb_c1333_3n); tb_c1333rw += tb_c1333_3n; memcpy(tb_c1333rw, tb_c1333_4, tb_c1333_4n); tb_c1333rw += tb_c1333_4n; *tb_c1333rw = 0; (const char *)tb_c1333r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1334_0 = "    l->len = 1;\n    return ("; long long tb_c1334_0n = sizeof("    l->len = 1;\n    return (") - 1; const char *tb_c1334_1 = v_row; long long tb_c1334_1n = (long long)strlen(tb_c1334_1); const char *tb_c1334_2 = "){l, 0};\n}\n"; long long tb_c1334_2n = sizeof("){l, 0};\n}\n") - 1; char *tb_c1334r = tb_mem(tb_c1334_0n + tb_c1334_1n + tb_c1334_2n + 1), *tb_c1334rw = tb_c1334r; memcpy(tb_c1334rw, tb_c1334_0, tb_c1334_0n); tb_c1334rw += tb_c1334_0n; memcpy(tb_c1334rw, tb_c1334_1, tb_c1334_1n); tb_c1334rw += tb_c1334_1n; memcpy(tb_c1334rw, tb_c1334_2, tb_c1334_2n); tb_c1334rw += tb_c1334_2n; *tb_c1334rw = 0; (const char *)tb_c1334r; }));
}

static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1970))->q_by_cols) {
        f_emit__emit_cols_helpers(v_c, v_o);
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1335_0 = "static tb_o_"; long long tb_c1335_0n = sizeof("static tb_o_") - 1; const char *tb_c1335_1 = ((tb_o_Node *)tb_nn(v_o, 1973))->q_name; long long tb_c1335_1n = (long long)strlen(tb_c1335_1); const char *tb_c1335_2 = " *tb_new_"; long long tb_c1335_2n = sizeof(" *tb_new_") - 1; const char *tb_c1335_3 = ((tb_o_Node *)tb_nn(v_o, 1973))->q_name; long long tb_c1335_3n = (long long)strlen(tb_c1335_3); const char *tb_c1335_4 = "("; long long tb_c1335_4n = sizeof("(") - 1; char *tb_c1335r = tb_mem(tb_c1335_0n + tb_c1335_1n + tb_c1335_2n + tb_c1335_3n + tb_c1335_4n + 1), *tb_c1335rw = tb_c1335r; memcpy(tb_c1335rw, tb_c1335_0, tb_c1335_0n); tb_c1335rw += tb_c1335_0n; memcpy(tb_c1335rw, tb_c1335_1, tb_c1335_1n); tb_c1335rw += tb_c1335_1n; memcpy(tb_c1335rw, tb_c1335_2, tb_c1335_2n); tb_c1335rw += tb_c1335_2n; memcpy(tb_c1335rw, tb_c1335_3, tb_c1335_3n); tb_c1335rw += tb_c1335_3n; memcpy(tb_c1335rw, tb_c1335_4, tb_c1335_4n); tb_c1335rw += tb_c1335_4n; *tb_c1335rw = 0; (const char *)tb_c1335r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1974))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1336 = ((((tb_o_Node *)tb_nn(v_o, 1976))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1336; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1337_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1979))->q_kids, v_i, sizeof(tb_o_Node *), 1979)), 1979))->q_ty); long long tb_c1337_0n = (long long)strlen(tb_c1337_0); const char *tb_c1337_1 = " a"; long long tb_c1337_1n = sizeof(" a") - 1; char tb_c1337_2d[24]; long long tb_c1337_2n; const char *tb_c1337_2 = tb_digits(v_i, tb_c1337_2d, &tb_c1337_2n); char *tb_c1337r = tb_mem(tb_c1337_0n + tb_c1337_1n + tb_c1337_2n + 1), *tb_c1337rw = tb_c1337r; memcpy(tb_c1337rw, tb_c1337_0, tb_c1337_0n); tb_c1337rw += tb_c1337_0n; memcpy(tb_c1337rw, tb_c1337_1, tb_c1337_1n); tb_c1337rw += tb_c1337_1n; memcpy(tb_c1337rw, tb_c1337_2, tb_c1337_2n); tb_c1337rw += tb_c1337_2n; *tb_c1337rw = 0; (const char *)tb_c1337r; }));
        }
    }
    f_emit__put(v_c, ") {\n");
    f_emit__put(v_c, ({ const char *tb_c1338_0 = "    tb_o_"; long long tb_c1338_0n = sizeof("    tb_o_") - 1; const char *tb_c1338_1 = ((tb_o_Node *)tb_nn(v_o, 1981))->q_name; long long tb_c1338_1n = (long long)strlen(tb_c1338_1); const char *tb_c1338_2 = " *o = tb_mem(sizeof(tb_o_"; long long tb_c1338_2n = sizeof(" *o = tb_mem(sizeof(tb_o_") - 1; const char *tb_c1338_3 = ((tb_o_Node *)tb_nn(v_o, 1981))->q_name; long long tb_c1338_3n = (long long)strlen(tb_c1338_3); const char *tb_c1338_4 = "));\n"; long long tb_c1338_4n = sizeof("));\n") - 1; char *tb_c1338r = tb_mem(tb_c1338_0n + tb_c1338_1n + tb_c1338_2n + tb_c1338_3n + tb_c1338_4n + 1), *tb_c1338rw = tb_c1338r; memcpy(tb_c1338rw, tb_c1338_0, tb_c1338_0n); tb_c1338rw += tb_c1338_0n; memcpy(tb_c1338rw, tb_c1338_1, tb_c1338_1n); tb_c1338rw += tb_c1338_1n; memcpy(tb_c1338rw, tb_c1338_2, tb_c1338_2n); tb_c1338rw += tb_c1338_2n; memcpy(tb_c1338rw, tb_c1338_3, tb_c1338_3n); tb_c1338rw += tb_c1338_3n; memcpy(tb_c1338rw, tb_c1338_4, tb_c1338_4n); tb_c1338rw += tb_c1338_4n; *tb_c1338rw = 0; (const char *)tb_c1338r; }));
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1339_0 = "    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_"; long long tb_c1339_0n = sizeof("    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_") - 1; const char *tb_c1339_1 = ((tb_o_Node *)tb_nn(v_o, 1983))->q_name; long long tb_c1339_1n = (long long)strlen(tb_c1339_1); const char *tb_c1339_2 = "));\n"; long long tb_c1339_2n = sizeof("));\n") - 1; char *tb_c1339r = tb_mem(tb_c1339_0n + tb_c1339_1n + tb_c1339_2n + 1), *tb_c1339rw = tb_c1339r; memcpy(tb_c1339rw, tb_c1339_0, tb_c1339_0n); tb_c1339rw += tb_c1339_0n; memcpy(tb_c1339rw, tb_c1339_1, tb_c1339_1n); tb_c1339rw += tb_c1339_1n; memcpy(tb_c1339rw, tb_c1339_2, tb_c1339_2n); tb_c1339rw += tb_c1339_2n; *tb_c1339rw = 0; (const char *)tb_c1339r; }));
    }
    {
        long long tb_end1340 = ((((tb_o_Node *)tb_nn(v_o, 1984))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1340; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1341_0 = "    o"; long long tb_c1341_0n = sizeof("    o") - 1; const char *tb_c1341_1 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1985))->q_kids, v_i, sizeof(tb_o_Node *), 1985)), 1985))->q_name); long long tb_c1341_1n = (long long)strlen(tb_c1341_1); const char *tb_c1341_2 = " = a"; long long tb_c1341_2n = sizeof(" = a") - 1; char tb_c1341_3d[24]; long long tb_c1341_3n; const char *tb_c1341_3 = tb_digits(v_i, tb_c1341_3d, &tb_c1341_3n); const char *tb_c1341_4 = ";\n"; long long tb_c1341_4n = sizeof(";\n") - 1; char *tb_c1341r = tb_mem(tb_c1341_0n + tb_c1341_1n + tb_c1341_2n + tb_c1341_3n + tb_c1341_4n + 1), *tb_c1341rw = tb_c1341r; memcpy(tb_c1341rw, tb_c1341_0, tb_c1341_0n); tb_c1341rw += tb_c1341_0n; memcpy(tb_c1341rw, tb_c1341_1, tb_c1341_1n); tb_c1341rw += tb_c1341_1n; memcpy(tb_c1341rw, tb_c1341_2, tb_c1341_2n); tb_c1341rw += tb_c1341_2n; memcpy(tb_c1341rw, tb_c1341_3, tb_c1341_3n); tb_c1341rw += tb_c1341_3n; memcpy(tb_c1341rw, tb_c1341_4, tb_c1341_4n); tb_c1341rw += tb_c1341_4n; *tb_c1341rw = 0; (const char *)tb_c1341r; }));
        }
    }
    f_emit__put(v_c, "    return o;\n}\n");
}

static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1342 = v_fresh; tb_list * *tb_p1342 = &(((tb_o_Comp *)tb_nn(v_c, 1990))->q_out); *tb_p1342 = tb_v1342; }
    { long long tb_v1343 = 0LL; long long *tb_p1343 = &(((tb_o_Comp *)tb_nn(v_c, 1991))->q_label); *tb_p1343 = tb_v1343; }
    f_emit__put(v_c, "// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb\n");
    f_emit__put(v_c, f_runtime_text__runtime_text());
    {
        tb_list * tb_l1344 = ((tb_o_Node *)tb_nn(v_program, 1996))->q_kids;
        for (long long tb_i1344 = 0; tb_i1344 < tb_l1344->len; tb_i1344++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1344->data)[tb_i1344];
            if ((({ const char *tb_s1345 = ((tb_o_Node *)tb_nn(v_top, 1997))->q_kind; ((unsigned char)tb_s1345[0] == 111 && (unsigned char)tb_s1345[1] == 98 && (unsigned char)tb_s1345[2] == 106 && tb_s1345[3] == 0); }) && ((tb_o_Node *)tb_nn(v_top, 1997))->q_by_cols)) {
                f_emit__put(v_c, ({ const char *tb_c1346_0 = "typedef struct tb_cols_"; long long tb_c1346_0n = sizeof("typedef struct tb_cols_") - 1; const char *tb_c1346_1 = ((tb_o_Node *)tb_nn(v_top, 1998))->q_name; long long tb_c1346_1n = (long long)strlen(tb_c1346_1); const char *tb_c1346_2 = " tb_cols_"; long long tb_c1346_2n = sizeof(" tb_cols_") - 1; const char *tb_c1346_3 = ((tb_o_Node *)tb_nn(v_top, 1998))->q_name; long long tb_c1346_3n = (long long)strlen(tb_c1346_3); const char *tb_c1346_4 = ";\n"; long long tb_c1346_4n = sizeof(";\n") - 1; char *tb_c1346r = tb_mem(tb_c1346_0n + tb_c1346_1n + tb_c1346_2n + tb_c1346_3n + tb_c1346_4n + 1), *tb_c1346rw = tb_c1346r; memcpy(tb_c1346rw, tb_c1346_0, tb_c1346_0n); tb_c1346rw += tb_c1346_0n; memcpy(tb_c1346rw, tb_c1346_1, tb_c1346_1n); tb_c1346rw += tb_c1346_1n; memcpy(tb_c1346rw, tb_c1346_2, tb_c1346_2n); tb_c1346rw += tb_c1346_2n; memcpy(tb_c1346rw, tb_c1346_3, tb_c1346_3n); tb_c1346rw += tb_c1346_3n; memcpy(tb_c1346rw, tb_c1346_4, tb_c1346_4n); tb_c1346rw += tb_c1346_4n; *tb_c1346rw = 0; (const char *)tb_c1346r; }));
                f_emit__put(v_c, ({ const char *tb_c1347_0 = "typedef struct { tb_cols_"; long long tb_c1347_0n = sizeof("typedef struct { tb_cols_") - 1; const char *tb_c1347_1 = ((tb_o_Node *)tb_nn(v_top, 1999))->q_name; long long tb_c1347_1n = (long long)strlen(tb_c1347_1); const char *tb_c1347_2 = " *c; long long i; } tb_row_"; long long tb_c1347_2n = sizeof(" *c; long long i; } tb_row_") - 1; const char *tb_c1347_3 = ((tb_o_Node *)tb_nn(v_top, 1999))->q_name; long long tb_c1347_3n = (long long)strlen(tb_c1347_3); const char *tb_c1347_4 = ";\n"; long long tb_c1347_4n = sizeof(";\n") - 1; char *tb_c1347r = tb_mem(tb_c1347_0n + tb_c1347_1n + tb_c1347_2n + tb_c1347_3n + tb_c1347_4n + 1), *tb_c1347rw = tb_c1347r; memcpy(tb_c1347rw, tb_c1347_0, tb_c1347_0n); tb_c1347rw += tb_c1347_0n; memcpy(tb_c1347rw, tb_c1347_1, tb_c1347_1n); tb_c1347rw += tb_c1347_1n; memcpy(tb_c1347rw, tb_c1347_2, tb_c1347_2n); tb_c1347rw += tb_c1347_2n; memcpy(tb_c1347rw, tb_c1347_3, tb_c1347_3n); tb_c1347rw += tb_c1347_3n; memcpy(tb_c1347rw, tb_c1347_4, tb_c1347_4n); tb_c1347rw += tb_c1347_4n; *tb_c1347rw = 0; (const char *)tb_c1347r; }));
            } else if (({ const char *tb_s1348 = ((tb_o_Node *)tb_nn(v_top, 2000))->q_kind; ((unsigned char)tb_s1348[0] == 111 && (unsigned char)tb_s1348[1] == 98 && (unsigned char)tb_s1348[2] == 106 && tb_s1348[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1349_0 = "typedef struct tb_o_"; long long tb_c1349_0n = sizeof("typedef struct tb_o_") - 1; const char *tb_c1349_1 = ((tb_o_Node *)tb_nn(v_top, 2001))->q_name; long long tb_c1349_1n = (long long)strlen(tb_c1349_1); const char *tb_c1349_2 = " tb_o_"; long long tb_c1349_2n = sizeof(" tb_o_") - 1; const char *tb_c1349_3 = ((tb_o_Node *)tb_nn(v_top, 2001))->q_name; long long tb_c1349_3n = (long long)strlen(tb_c1349_3); const char *tb_c1349_4 = ";\n"; long long tb_c1349_4n = sizeof(";\n") - 1; char *tb_c1349r = tb_mem(tb_c1349_0n + tb_c1349_1n + tb_c1349_2n + tb_c1349_3n + tb_c1349_4n + 1), *tb_c1349rw = tb_c1349r; memcpy(tb_c1349rw, tb_c1349_0, tb_c1349_0n); tb_c1349rw += tb_c1349_0n; memcpy(tb_c1349rw, tb_c1349_1, tb_c1349_1n); tb_c1349rw += tb_c1349_1n; memcpy(tb_c1349rw, tb_c1349_2, tb_c1349_2n); tb_c1349rw += tb_c1349_2n; memcpy(tb_c1349rw, tb_c1349_3, tb_c1349_3n); tb_c1349rw += tb_c1349_3n; memcpy(tb_c1349rw, tb_c1349_4, tb_c1349_4n); tb_c1349rw += tb_c1349_4n; *tb_c1349rw = 0; (const char *)tb_c1349r; }));
            }
        }
    }
    {
        tb_list * tb_l1350 = ((tb_o_Node *)tb_nn(v_program, 2002))->q_kids;
        for (long long tb_i1350 = 0; tb_i1350 < tb_l1350->len; tb_i1350++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1350->data)[tb_i1350];
            if (({ const char *tb_s1351 = ((tb_o_Node *)tb_nn(v_top, 2003))->q_kind; ((unsigned char)tb_s1351[0] == 111 && (unsigned char)tb_s1351[1] == 98 && (unsigned char)tb_s1351[2] == 106 && tb_s1351[3] == 0); })) {
                f_emit__emit_obj(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1352 = ((tb_o_Node *)tb_nn(v_program, 2006))->q_kids;
        for (long long tb_i1352 = 0; tb_i1352 < tb_l1352->len; tb_i1352++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1352->data)[tb_i1352];
            if ((({ const char *tb_s1353 = ((tb_o_Node *)tb_nn(v_top, 2007))->q_kind; ((unsigned char)tb_s1353[0] == 102 && (unsigned char)tb_s1353[1] == 117 && (unsigned char)tb_s1353[2] == 110 && (unsigned char)tb_s1353[3] == 99 && tb_s1353[4] == 0); }) && ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2007))->q_kids, 2LL, sizeof(tb_o_Node *), 2007)), 2007))->q_kids)->len > 1LL))) {
                f_emit__put(v_c, "typedef struct {");
                {
                    long long tb_end1354 = ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2009))->q_kids, 2LL, sizeof(tb_o_Node *), 2009)), 2009))->q_kids)->len - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end1354; v_j++) {
                        f_emit__put(v_c, ({ const char *tb_c1355_0 = " "; long long tb_c1355_0n = sizeof(" ") - 1; const char *tb_c1355_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2010))->q_kids, 2LL, sizeof(tb_o_Node *), 2010)), 2010))->q_kids, v_j, sizeof(tb_o_Node *), 2010)), 2010))->q_ty); long long tb_c1355_1n = (long long)strlen(tb_c1355_1); const char *tb_c1355_2 = " o"; long long tb_c1355_2n = sizeof(" o") - 1; char tb_c1355_3d[24]; long long tb_c1355_3n; const char *tb_c1355_3 = tb_digits(v_j, tb_c1355_3d, &tb_c1355_3n); const char *tb_c1355_4 = ";"; long long tb_c1355_4n = sizeof(";") - 1; char *tb_c1355r = tb_mem(tb_c1355_0n + tb_c1355_1n + tb_c1355_2n + tb_c1355_3n + tb_c1355_4n + 1), *tb_c1355rw = tb_c1355r; memcpy(tb_c1355rw, tb_c1355_0, tb_c1355_0n); tb_c1355rw += tb_c1355_0n; memcpy(tb_c1355rw, tb_c1355_1, tb_c1355_1n); tb_c1355rw += tb_c1355_1n; memcpy(tb_c1355rw, tb_c1355_2, tb_c1355_2n); tb_c1355rw += tb_c1355_2n; memcpy(tb_c1355rw, tb_c1355_3, tb_c1355_3n); tb_c1355rw += tb_c1355_3n; memcpy(tb_c1355rw, tb_c1355_4, tb_c1355_4n); tb_c1355rw += tb_c1355_4n; *tb_c1355rw = 0; (const char *)tb_c1355r; }));
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1356_0 = " } tb_r_"; long long tb_c1356_0n = sizeof(" } tb_r_") - 1; const char *tb_c1356_1 = f_emit__fname(v_top); long long tb_c1356_1n = (long long)strlen(tb_c1356_1); const char *tb_c1356_2 = ";\n"; long long tb_c1356_2n = sizeof(";\n") - 1; char *tb_c1356r = tb_mem(tb_c1356_0n + tb_c1356_1n + tb_c1356_2n + 1), *tb_c1356rw = tb_c1356r; memcpy(tb_c1356rw, tb_c1356_0, tb_c1356_0n); tb_c1356rw += tb_c1356_0n; memcpy(tb_c1356rw, tb_c1356_1, tb_c1356_1n); tb_c1356rw += tb_c1356_1n; memcpy(tb_c1356rw, tb_c1356_2, tb_c1356_2n); tb_c1356rw += tb_c1356_2n; *tb_c1356rw = 0; (const char *)tb_c1356r; }));
            }
        }
    }
    {
        tb_list * tb_l1357 = ((tb_o_Comp *)tb_nn(v_c, 2012))->q_types;
        for (long long tb_i1357 = 0; tb_i1357 < tb_l1357->len; tb_i1357++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1357->data)[tb_i1357];
            const char * v_k = ((tb_o_Type *)tb_nn(v_t, 2013))->q_kind;
            if (((({ const char *tb_s1358 = v_k; !((unsigned char)tb_s1358[0] == 110 && (unsigned char)tb_s1358[1] == 111 && (unsigned char)tb_s1358[2] == 110 && (unsigned char)tb_s1358[3] == 101 && tb_s1358[4] == 0); }) && ({ const char *tb_s1359 = v_k; !((unsigned char)tb_s1359[0] == 109 && (unsigned char)tb_s1359[1] == 117 && (unsigned char)tb_s1359[2] == 108 && (unsigned char)tb_s1359[3] == 116 && (unsigned char)tb_s1359[4] == 105 && tb_s1359[5] == 0); })) && ({ const char *tb_s1360 = v_k; !((unsigned char)tb_s1360[0] == 117 && (unsigned char)tb_s1360[1] == 110 && (unsigned char)tb_s1360[2] == 107 && (unsigned char)tb_s1360[3] == 110 && (unsigned char)tb_s1360[4] == 111 && (unsigned char)tb_s1360[5] == 119 && (unsigned char)tb_s1360[6] == 110 && tb_s1360[7] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c1361_0 = "static void tb_show_"; long long tb_c1361_0n = sizeof("static void tb_show_") - 1; char tb_c1361_1d[24]; long long tb_c1361_1n; const char *tb_c1361_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 2015))->q_id, tb_c1361_1d, &tb_c1361_1n); const char *tb_c1361_2 = "("; long long tb_c1361_2n = sizeof("(") - 1; const char *tb_c1361_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 2015))->q_id); long long tb_c1361_3n = (long long)strlen(tb_c1361_3); const char *tb_c1361_4 = " x);\n"; long long tb_c1361_4n = sizeof(" x);\n") - 1; char *tb_c1361r = tb_mem(tb_c1361_0n + tb_c1361_1n + tb_c1361_2n + tb_c1361_3n + tb_c1361_4n + 1), *tb_c1361rw = tb_c1361r; memcpy(tb_c1361rw, tb_c1361_0, tb_c1361_0n); tb_c1361rw += tb_c1361_0n; memcpy(tb_c1361rw, tb_c1361_1, tb_c1361_1n); tb_c1361rw += tb_c1361_1n; memcpy(tb_c1361rw, tb_c1361_2, tb_c1361_2n); tb_c1361rw += tb_c1361_2n; memcpy(tb_c1361rw, tb_c1361_3, tb_c1361_3n); tb_c1361rw += tb_c1361_3n; memcpy(tb_c1361rw, tb_c1361_4, tb_c1361_4n); tb_c1361rw += tb_c1361_4n; *tb_c1361rw = 0; (const char *)tb_c1361r; }));
            }
        }
    }
    {
        long long tb_end1362 = ((((tb_o_Comp *)tb_nn(v_c, 2016))->q_temp_types)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end1362; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1363_0 = "static _Thread_local "; long long tb_c1363_0n = sizeof("static _Thread_local ") - 1; const char *tb_c1363_1 = f_emit__c_type(v_c, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 2017))->q_temp_types, v_i, sizeof(long long), 2017))); long long tb_c1363_1n = (long long)strlen(tb_c1363_1); const char *tb_c1363_2 = " tb_t"; long long tb_c1363_2n = sizeof(" tb_t") - 1; char tb_c1363_3d[24]; long long tb_c1363_3n; const char *tb_c1363_3 = tb_digits(v_i, tb_c1363_3d, &tb_c1363_3n); const char *tb_c1363_4 = ";   // для  f() or запасное\n"; long long tb_c1363_4n = sizeof(";   // для  f() or запасное\n") - 1; char *tb_c1363r = tb_mem(tb_c1363_0n + tb_c1363_1n + tb_c1363_2n + tb_c1363_3n + tb_c1363_4n + 1), *tb_c1363rw = tb_c1363r; memcpy(tb_c1363rw, tb_c1363_0, tb_c1363_0n); tb_c1363rw += tb_c1363_0n; memcpy(tb_c1363rw, tb_c1363_1, tb_c1363_1n); tb_c1363rw += tb_c1363_1n; memcpy(tb_c1363rw, tb_c1363_2, tb_c1363_2n); tb_c1363rw += tb_c1363_2n; memcpy(tb_c1363rw, tb_c1363_3, tb_c1363_3n); tb_c1363rw += tb_c1363_3n; memcpy(tb_c1363rw, tb_c1363_4, tb_c1363_4n); tb_c1363rw += tb_c1363_4n; *tb_c1363rw = 0; (const char *)tb_c1363r; }));
        }
    }
    {
        tb_list * tb_l1364 = ((tb_o_Node *)tb_nn(v_program, 2018))->q_kids;
        for (long long tb_i1364 = 0; tb_i1364 < tb_l1364->len; tb_i1364++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1364->data)[tb_i1364];
            if (({ const char *tb_s1365 = ((tb_o_Node *)tb_nn(v_top, 2019))->q_kind; ((unsigned char)tb_s1365[0] == 111 && (unsigned char)tb_s1365[1] == 98 && (unsigned char)tb_s1365[2] == 106 && tb_s1365[3] == 0); })) {
                f_emit__emit_constructor(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1366 = ((tb_o_Node *)tb_nn(v_program, 2021))->q_kids;
        for (long long tb_i1366 = 0; tb_i1366 < tb_l1366->len; tb_i1366++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1366->data)[tb_i1366];
            if (({ const char *tb_s1367 = ((tb_o_Node *)tb_nn(v_top, 2022))->q_kind; ((unsigned char)tb_s1367[0] == 102 && (unsigned char)tb_s1367[1] == 117 && (unsigned char)tb_s1367[2] == 110 && (unsigned char)tb_s1367[3] == 99 && tb_s1367[4] == 0); })) {
                f_emit__emit_signature(v_c, v_top);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__put(v_c, "\n");
    {
        tb_list * tb_l1368 = ((tb_o_Comp *)tb_nn(v_c, 2026))->q_types;
        for (long long tb_i1368 = 0; tb_i1368 < tb_l1368->len; tb_i1368++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1368->data)[tb_i1368];
            f_emit__emit_show(v_c, v_t);
        }
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 2029))->q_out;
    tb_list * v_funcs_out = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1369 = v_funcs_out; tb_list * *tb_p1369 = &(((tb_o_Comp *)tb_nn(v_c, 2031))->q_out); *tb_p1369 = tb_v1369; }
    {
        tb_list * tb_l1370 = ((tb_o_Node *)tb_nn(v_program, 2032))->q_kids;
        for (long long tb_i1370 = 0; tb_i1370 < tb_l1370->len; tb_i1370++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1370->data)[tb_i1370];
            if (({ const char *tb_s1371 = ((tb_o_Node *)tb_nn(v_top, 2033))->q_kind; ((unsigned char)tb_s1371[0] == 102 && (unsigned char)tb_s1371[1] == 117 && (unsigned char)tb_s1371[2] == 110 && (unsigned char)tb_s1371[3] == 99 && tb_s1371[4] == 0); })) {
                f_emit__emit_func(v_c, v_top);
            }
        }
    }
    { tb_list * tb_v1372 = v_saved; tb_list * *tb_p1372 = &(((tb_o_Comp *)tb_nn(v_c, 2035))->q_out); *tb_p1372 = tb_v1372; }
    {
        tb_list * tb_l1373 = ((tb_o_Comp *)tb_nn(v_c, 2036))->q_workers;
        for (long long tb_i1373 = 0; tb_i1373 < tb_l1373->len; tb_i1373++) {
            const char * const v_w = ((const char * *)tb_l1373->data)[tb_i1373];
            f_emit__put(v_c, v_w);
        }
    }
    {
        tb_list * tb_l1374 = v_funcs_out;
        for (long long tb_i1374 = 0; tb_i1374 < tb_l1374->len; tb_i1374++) {
            const char * const v_f = ((const char * *)tb_l1374->data)[tb_i1374];
            f_emit__put(v_c, v_f);
        }
    }
    tb_o_Node * v_main_func = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 2040))->q_kids, 0LL, sizeof(tb_o_Node *), 2040));
    {
        tb_list * tb_l1375 = ((tb_o_Node *)tb_nn(v_program, 2041))->q_kids;
        for (long long tb_i1375 = 0; tb_i1375 < tb_l1375->len; tb_i1375++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1375->data)[tb_i1375];
            if (((({ const char *tb_s1376 = ((tb_o_Node *)tb_nn(v_top, 2042))->q_kind; ((unsigned char)tb_s1376[0] == 102 && (unsigned char)tb_s1376[1] == 117 && (unsigned char)tb_s1376[2] == 110 && (unsigned char)tb_s1376[3] == 99 && tb_s1376[4] == 0); }) && ({ const char *tb_s1377 = ((tb_o_Node *)tb_nn(v_top, 2042))->q_name; ((unsigned char)tb_s1377[0] == 109 && (unsigned char)tb_s1377[1] == 97 && (unsigned char)tb_s1377[2] == 105 && (unsigned char)tb_s1377[3] == 110 && tb_s1377[4] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_top, 2042))->q_mod, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 2042))->q_kids, 0LL, sizeof(tb_o_Node *), 2042)), 2042))->q_mod) == 0))) {
                v_main_func = v_top;
            }
        }
    }
    f_emit__put(v_c, "\nint main(int argc, char **argv) {\n");
    f_emit__put(v_c, "    tb_args = tb_list_new(sizeof(const char *));\n");
    f_emit__put(v_c, "    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];\n");
    f_emit__put(v_c, ({ const char *tb_c1378_0 = "    "; long long tb_c1378_0n = sizeof("    ") - 1; const char *tb_c1378_1 = f_emit__fname(v_main_func); long long tb_c1378_1n = (long long)strlen(tb_c1378_1); const char *tb_c1378_2 = "();\n    return 0;\n}\n"; long long tb_c1378_2n = sizeof("();\n    return 0;\n}\n") - 1; char *tb_c1378r = tb_mem(tb_c1378_0n + tb_c1378_1n + tb_c1378_2n + 1), *tb_c1378rw = tb_c1378r; memcpy(tb_c1378rw, tb_c1378_0, tb_c1378_0n); tb_c1378rw += tb_c1378_0n; memcpy(tb_c1378rw, tb_c1378_1, tb_c1378_1n); tb_c1378rw += tb_c1378_1n; memcpy(tb_c1378rw, tb_c1378_2, tb_c1378_2n); tb_c1378rw += tb_c1378_2n; *tb_c1378rw = 0; (const char *)tb_c1378r; }));
}

static const char * f_runtime_text__runtime_text(void) {
    tb_list * v_lines = tb_list_new(sizeof(const char *));
    { const char * tb_v1379 = "// Библиотека, которую Tachbit кладёт в начало каждой программы на C.\n"; *(const char * *)tb_grow(v_lines) = tb_v1379; }
    { const char * tb_v1380 = "// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,\n"; *(const char * *)tb_grow(v_lines) = tb_v1380; }
    { const char * tb_v1381 = "// который делает tools/embed_runtime.py. Поменял этот файл — запусти:\n"; *(const char * *)tb_grow(v_lines) = tb_v1381; }
    { const char * tb_v1382 = "//     python3 tools/embed_runtime.py\n"; *(const char * *)tb_grow(v_lines) = tb_v1382; }
    { const char * tb_v1383 = "#include <stdio.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1383; }
    { const char * tb_v1384 = "#include <stdlib.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1384; }
    { const char * tb_v1385 = "#include <string.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1385; }
    { const char * tb_v1386 = "#include <math.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1386; }
    { const char * tb_v1387 = "#include <pthread.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1387; }
    { const char * tb_v1388 = "#include <unistd.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1388; }
    { const char * tb_v1389 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1389; }
    { const char * tb_v1390 = "// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается\n"; *(const char * *)tb_grow(v_lines) = tb_v1390; }
    { const char * tb_v1391 = "#if defined(__GNUC__)\n"; *(const char * *)tb_grow(v_lines) = tb_v1391; }
    { const char * tb_v1392 = "#define TB_RARE(x) __builtin_expect(!!(x), 0)\n"; *(const char * *)tb_grow(v_lines) = tb_v1392; }
    { const char * tb_v1393 = "#define TB_COLD __attribute__((cold, noreturn, noinline))\n"; *(const char * *)tb_grow(v_lines) = tb_v1393; }
    { const char * tb_v1394 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v1394; }
    { const char * tb_v1395 = "#define TB_RARE(x) (x)\n"; *(const char * *)tb_grow(v_lines) = tb_v1395; }
    { const char * tb_v1396 = "#define TB_COLD\n"; *(const char * *)tb_grow(v_lines) = tb_v1396; }
    { const char * tb_v1397 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v1397; }
    { const char * tb_v1398 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1398; }
    { const char * tb_v1399 = "// список: кусок памяти, len элементов по size байт, место на cap\n"; *(const char * *)tb_grow(v_lines) = tb_v1399; }
    { const char * tb_v1400 = "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"; *(const char * *)tb_grow(v_lines) = tb_v1400; }
    { const char * tb_v1401 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1401; }
    { const char * tb_v1402 = "TB_COLD static void tb_crash(int line, const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1402; }
    { const char * tb_v1403 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1403; }
    { const char * tb_v1404 = "    // в терминале — красным (если не задан NO_COLOR)\n"; *(const char * *)tb_grow(v_lines) = tb_v1404; }
    { const char * tb_v1405 = "    int color = isatty(2) && getenv(\"NO_COLOR\") == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1405; }
    { const char * tb_v1406 = "    if (color) fputs(\"\\033[31m\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1406; }
    { const char * tb_v1407 = "    if (line > 0) fprintf(stderr, \"строка %d: %s\", line, msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1407; }
    { const char * tb_v1408 = "    else fprintf(stderr, \"%s\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1408; }
    { const char * tb_v1409 = "    fputs(color ? \"\\033[0m\\n\" : \"\\n\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1409; }
    { const char * tb_v1410 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1410; }
    { const char * tb_v1411 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1411; }
    { const char * tb_v1412 = "// ---------- temp: временная память ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1412; }
    { const char * tb_v1413 = "// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),\n"; *(const char * *)tb_grow(v_lines) = tb_v1413; }
    { const char * tb_v1414 = "// а в конце блока все куски отдаются разом. У каждого ядра свои куски.\n"; *(const char * *)tb_grow(v_lines) = tb_v1414; }
    { const char * tb_v1415 = "typedef struct { char *mem; long long size, used; } tb_chunk;\n"; *(const char * *)tb_grow(v_lines) = tb_v1415; }
    { const char * tb_v1416 = "typedef struct { int n; long long used; } tb_mark;\n"; *(const char * *)tb_grow(v_lines) = tb_v1416; }
    { const char * tb_v1417 = "static _Thread_local tb_chunk *tb_chunks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1417; }
    { const char * tb_v1418 = "static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1418; }
    { const char * tb_v1419 = "static int tb_in_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1419; }
    { const char * tb_v1420 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1420; }
    { const char * tb_v1421 = "        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1421; }
    { const char * tb_v1422 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1422; }
    { const char * tb_v1423 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1423; }
    { const char * tb_v1424 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1424; }
    { const char * tb_v1425 = "static void *tb_temp_alloc(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1425; }
    { const char * tb_v1426 = "    n = n > 0 ? (n + 15) & ~15LL : 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1426; }
    { const char * tb_v1427 = "    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1427; }
    { const char * tb_v1428 = "    if (c == 0 || c->used + n > c->size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1428; }
    { const char * tb_v1429 = "        long long size = c != 0 ? c->size * 2 : (1LL << 20);\n"; *(const char * *)tb_grow(v_lines) = tb_v1429; }
    { const char * tb_v1430 = "        if (size < n) size = n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1430; }
    { const char * tb_v1431 = "        if (tb_nchunks == tb_capchunks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1431; }
    { const char * tb_v1432 = "            tb_capchunks = tb_capchunks * 2 + 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1432; }
    { const char * tb_v1433 = "            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);\n"; *(const char * *)tb_grow(v_lines) = tb_v1433; }
    { const char * tb_v1434 = "            if (tb_chunks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1434; }
    { const char * tb_v1435 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1435; }
    { const char * tb_v1436 = "        char *mem = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1436; }
    { const char * tb_v1437 = "        if (mem == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1437; }
    { const char * tb_v1438 = "        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1438; }
    { const char * tb_v1439 = "        c = &tb_chunks[tb_nchunks];\n"; *(const char * *)tb_grow(v_lines) = tb_v1439; }
    { const char * tb_v1440 = "        tb_nchunks += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1440; }
    { const char * tb_v1441 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1441; }
    { const char * tb_v1442 = "    void *p = c->mem + c->used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1442; }
    { const char * tb_v1443 = "    c->used += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1443; }
    { const char * tb_v1444 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1444; }
    { const char * tb_v1445 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1445; }
    { const char * tb_v1446 = "// метки открытых temp (и арен шага / функции): самая последняя — текущая область\n"; *(const char * *)tb_grow(v_lines) = tb_v1446; }
    { const char * tb_v1447 = "static _Thread_local tb_mark *tb_marks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1447; }
    { const char * tb_v1448 = "static _Thread_local int tb_capmarks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1448; }
    { const char * tb_v1449 = "static inline tb_mark tb_temp_begin(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1449; }
    { const char * tb_v1450 = "    tb_mark m = {tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1450; }
    { const char * tb_v1451 = "    if (tb_temp_depth == tb_capmarks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1451; }
    { const char * tb_v1452 = "        tb_capmarks = tb_capmarks * 2 + 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1452; }
    { const char * tb_v1453 = "        tb_marks = realloc(tb_marks, sizeof(tb_mark) * tb_capmarks);\n"; *(const char * *)tb_grow(v_lines) = tb_v1453; }
    { const char * tb_v1454 = "        if (tb_marks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1454; }
    { const char * tb_v1455 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1455; }
    { const char * tb_v1456 = "    tb_marks[tb_temp_depth] = m;\n"; *(const char * *)tb_grow(v_lines) = tb_v1456; }
    { const char * tb_v1457 = "    tb_temp_depth += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1457; }
    { const char * tb_v1458 = "    return m;\n"; *(const char * *)tb_grow(v_lines) = tb_v1458; }
    { const char * tb_v1459 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1459; }
    { const char * tb_v1460 = "// p во временной памяти текущей (самой внутренней) области, а не во внешней\n"; *(const char * *)tb_grow(v_lines) = tb_v1460; }
    { const char * tb_v1461 = "static int tb_in_cur_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1461; }
    { const char * tb_v1462 = "    if (tb_temp_depth == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1462; }
    { const char * tb_v1463 = "    tb_mark t = tb_marks[tb_temp_depth - 1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1463; }
    { const char * tb_v1464 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1464; }
    { const char * tb_v1465 = "        const char *m = tb_chunks[i].mem;\n"; *(const char * *)tb_grow(v_lines) = tb_v1465; }
    { const char * tb_v1466 = "        if ((const char *)p >= m && (const char *)p < m + tb_chunks[i].size)\n"; *(const char * *)tb_grow(v_lines) = tb_v1466; }
    { const char * tb_v1467 = "            return i > t.n - 1 || (i == t.n - 1 && (const char *)p - m >= t.used);\n"; *(const char * *)tb_grow(v_lines) = tb_v1467; }
    { const char * tb_v1468 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1468; }
    { const char * tb_v1469 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1469; }
    { const char * tb_v1470 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1470; }
    { const char * tb_v1471 = "// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз\n"; *(const char * *)tb_grow(v_lines) = tb_v1471; }
    { const char * tb_v1472 = "static _Thread_local const char *tb_lc_p[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1472; }
    { const char * tb_v1473 = "static _Thread_local long long tb_lc_n[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1473; }
    { const char * tb_v1474 = "static _Thread_local int tb_lc_next;\n"; *(const char * *)tb_grow(v_lines) = tb_v1474; }
    { const char * tb_v1475 = "static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1475; }
    { const char * tb_v1476 = "static inline void tb_temp_end(tb_mark m) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1476; }
    { const char * tb_v1477 = "    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc\n"; *(const char * *)tb_grow(v_lines) = tb_v1477; }
    { const char * tb_v1478 = "    while (tb_nchunks > m.n && tb_nchunks > 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1478; }
    { const char * tb_v1479 = "        tb_nchunks -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1479; }
    { const char * tb_v1480 = "        free(tb_chunks[tb_nchunks].mem);\n"; *(const char * *)tb_grow(v_lines) = tb_v1480; }
    { const char * tb_v1481 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1481; }
    { const char * tb_v1482 = "    if (tb_nchunks > m.n) tb_chunks[0].used = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1482; }
    { const char * tb_v1483 = "    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1483; }
    { const char * tb_v1484 = "    tb_temp_depth -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1484; }
    { const char * tb_v1485 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1485; }
    { const char * tb_v1486 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1486; }
    { const char * tb_v1487 = "static void *tb_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1487; }
    { const char * tb_v1488 = "    if (tb_temp_depth > 0) return tb_temp_alloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1488; }
    { const char * tb_v1489 = "    void *p = malloc(n > 0 ? n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1489; }
    { const char * tb_v1490 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1490; }
    { const char * tb_v1491 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1491; }
    { const char * tb_v1492 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1492; }
    { const char * tb_v1493 = "// вырасти кусок: временный текущей области растёт там же, обычный — через realloc.\n"; *(const char * *)tb_grow(v_lines) = tb_v1493; }
    { const char * tb_v1494 = "// Кусок из внешней области (список создан снаружи, растёт внутри temp или шага цикла)\n"; *(const char * *)tb_grow(v_lines) = tb_v1494; }
    { const char * tb_v1495 = "// переезжает в обычную память: внутренняя область скоро отдаётся, а список живёт дольше.\n"; *(const char * *)tb_grow(v_lines) = tb_v1495; }
    { const char * tb_v1496 = "static void *tb_resize(void *p, long long old_n, long long new_n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1496; }
    { const char * tb_v1497 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1497; }
    { const char * tb_v1498 = "    if (tb_nchunks > 0 && tb_in_temp(p)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1498; }
    { const char * tb_v1499 = "        void *q = tb_in_cur_temp(p) ? tb_temp_alloc(new_n) : malloc(new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1499; }
    { const char * tb_v1500 = "        if (q == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1500; }
    { const char * tb_v1501 = "        memcpy(q, p, old_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1501; }
    { const char * tb_v1502 = "        return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1502; }
    { const char * tb_v1503 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1503; }
    { const char * tb_v1504 = "    p = realloc(p, new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1504; }
    { const char * tb_v1505 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1505; }
    { const char * tb_v1506 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1506; }
    { const char * tb_v1507 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1507; }
    { const char * tb_v1508 = "static tb_list *tb_list_new(long long size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1508; }
    { const char * tb_v1509 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1509; }
    { const char * tb_v1510 = "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"; *(const char * *)tb_grow(v_lines) = tb_v1510; }
    { const char * tb_v1511 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1511; }
    { const char * tb_v1512 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1512; }
    { const char * tb_v1513 = "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1513; }
    { const char * tb_v1514 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1514; }
    { const char * tb_v1515 = "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1515; }
    { const char * tb_v1516 = "    memcpy(l->data, src, size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1516; }
    { const char * tb_v1517 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1517; }
    { const char * tb_v1518 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1518; }
    { const char * tb_v1519 = "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1519; }
    { const char * tb_v1520 = "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1520; }
    { const char * tb_v1521 = "    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным\n"; *(const char * *)tb_grow(v_lines) = tb_v1521; }
    { const char * tb_v1522 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1522; }
    { const char * tb_v1523 = "    return l->data + i * size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1523; }
    { const char * tb_v1524 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1524; }
    { const char * tb_v1525 = "static inline void *tb_grow(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1525; }
    { const char * tb_v1526 = "    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски\n"; *(const char * *)tb_grow(v_lines) = tb_v1526; }
    { const char * tb_v1527 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1527; }
    { const char * tb_v1528 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1528; }
    { const char * tb_v1529 = "        long long old = l->cap * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1529; }
    { const char * tb_v1530 = "        l->cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1530; }
    { const char * tb_v1531 = "        // realloc часто растит большой кусок на месте, без копирования\n"; *(const char * *)tb_grow(v_lines) = tb_v1531; }
    { const char * tb_v1532 = "        l->data = tb_resize(l->data, old, l->cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1532; }
    { const char * tb_v1533 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1533; }
    { const char * tb_v1534 = "    l->len += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1534; }
    { const char * tb_v1535 = "    return l->data + (l->len - 1) * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1535; }
    { const char * tb_v1536 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1536; }
    { const char * tb_v1537 = "static void *tb_pop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1537; }
    { const char * tb_v1538 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1538; }
    { const char * tb_v1539 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1539; }
    { const char * tb_v1540 = "    return l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1540; }
    { const char * tb_v1541 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1541; }
    { const char * tb_v1542 = "// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1542; }
    { const char * tb_v1543 = "// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита\n"; *(const char * *)tb_grow(v_lines) = tb_v1543; }
    { const char * tb_v1544 = "static tb_list *tb_bits_new(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1544; }
    { const char * tb_v1545 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1545; }
    { const char * tb_v1546 = "    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem(8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1546; }
    { const char * tb_v1547 = "    memset(l->data, 0, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1547; }
    { const char * tb_v1548 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1548; }
    { const char * tb_v1549 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1549; }
    { const char * tb_v1550 = "static tb_list *tb_bits_from(long long n, const unsigned char *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1550; }
    { const char * tb_v1551 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1551; }
    { const char * tb_v1552 = "    long long words = (n + 63) / 64 + (n == 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1552; }
    { const char * tb_v1553 = "    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem(words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1553; }
    { const char * tb_v1554 = "    memset(l->data, 0, words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1554; }
    { const char * tb_v1555 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1555; }
    { const char * tb_v1556 = "    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1556; }
    { const char * tb_v1557 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1557; }
    { const char * tb_v1558 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1558; }
    { const char * tb_v1559 = "// бит без проверки номера (для each — номер всегда внутри)\n"; *(const char * *)tb_grow(v_lines) = tb_v1559; }
    { const char * tb_v1560 = "#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))\n"; *(const char * *)tb_grow(v_lines) = tb_v1560; }
    { const char * tb_v1561 = "static inline _Bool tb_bget(tb_list *l, long long i, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1561; }
    { const char * tb_v1562 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1562; }
    { const char * tb_v1563 = "    return TB_BIT(l, i);\n"; *(const char * *)tb_grow(v_lines) = tb_v1563; }
    { const char * tb_v1564 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1564; }
    { const char * tb_v1565 = "static inline void tb_bset(tb_list *l, long long i, _Bool v, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1565; }
    { const char * tb_v1566 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1566; }
    { const char * tb_v1567 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1567; }
    { const char * tb_v1568 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1568; }
    { const char * tb_v1569 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1569; }
    { const char * tb_v1570 = "static inline void tb_badd(tb_list *l, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1570; }
    { const char * tb_v1571 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1571; }
    { const char * tb_v1572 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1572; }
    { const char * tb_v1573 = "        long long old = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1573; }
    { const char * tb_v1574 = "        l->cap = l->cap * 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1574; }
    { const char * tb_v1575 = "        l->data = tb_resize(l->data, old, l->cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1575; }
    { const char * tb_v1576 = "        memset(l->data + old, 0, l->cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1576; }
    { const char * tb_v1577 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1577; }
    { const char * tb_v1578 = "    long long i = l->len++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1578; }
    { const char * tb_v1579 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1579; }
    { const char * tb_v1580 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1580; }
    { const char * tb_v1581 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1581; }
    { const char * tb_v1582 = "static inline _Bool tb_bpop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1582; }
    { const char * tb_v1583 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1583; }
    { const char * tb_v1584 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1584; }
    { const char * tb_v1585 = "    return TB_BIT(l, l->len);\n"; *(const char * *)tb_grow(v_lines) = tb_v1585; }
    { const char * tb_v1586 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1586; }
    { const char * tb_v1587 = "// N одинаковых битов в конец (repeat N times: flags.add(true))\n"; *(const char * *)tb_grow(v_lines) = tb_v1587; }
    { const char * tb_v1588 = "static void tb_bfill(tb_list *l, long long n, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1588; }
    { const char * tb_v1589 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1589; }
    { const char * tb_v1590 = "    if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1590; }
    { const char * tb_v1591 = "    long long need = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1591; }
    { const char * tb_v1592 = "    if (need > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1592; }
    { const char * tb_v1593 = "        long long old = l->cap / 8, cap = l->cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1593; }
    { const char * tb_v1594 = "        while (cap < need) cap *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1594; }
    { const char * tb_v1595 = "        l->data = tb_resize(l->data, old, cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1595; }
    { const char * tb_v1596 = "        memset(l->data + old, 0, cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1596; }
    { const char * tb_v1597 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1597; }
    { const char * tb_v1598 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1598; }
    { const char * tb_v1599 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1599; }
    { const char * tb_v1600 = "    long long i = l->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1600; }
    { const char * tb_v1601 = "    for (; i < need && (i & 63); i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1601; }
    { const char * tb_v1602 = "    if (need - i >= 64) { memset(w + (i >> 6), v ? 0xff : 0, ((need - i) >> 6) * 8); i += (need - i) & ~63LL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1602; }
    { const char * tb_v1603 = "    for (; i < need; i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1603; }
    { const char * tb_v1604 = "    l->len = need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1604; }
    { const char * tb_v1605 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1605; }
    { const char * tb_v1606 = "// значения dict bool лежат байтами — в list bool их надо переложить в биты\n"; *(const char * *)tb_grow(v_lines) = tb_v1606; }
    { const char * tb_v1607 = "static tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1607; }
    { const char * tb_v1608 = "// N одинаковых элементов в конец (repeat N times: nums.add(0))\n"; *(const char * *)tb_grow(v_lines) = tb_v1608; }
    { const char * tb_v1609 = "static void tb_fill(tb_list *l, long long n, const void *v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1609; }
    { const char * tb_v1610 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1610; }
    { const char * tb_v1611 = "    if (l->len + n > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1611; }
    { const char * tb_v1612 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1612; }
    { const char * tb_v1613 = "        long long cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1613; }
    { const char * tb_v1614 = "        if (cap < l->len + n) cap = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1614; }
    { const char * tb_v1615 = "        l->data = tb_resize(l->data, l->cap * l->size, cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1615; }
    { const char * tb_v1616 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1616; }
    { const char * tb_v1617 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1617; }
    { const char * tb_v1618 = "    char *p = l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1618; }
    { const char * tb_v1619 = "    int zero = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1619; }
    { const char * tb_v1620 = "    for (long long b = 0; b < l->size; b++) if (((const char *)v)[b]) zero = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1620; }
    { const char * tb_v1621 = "    if (zero) memset(p, 0, n * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1621; }
    { const char * tb_v1622 = "    else for (long long i = 0; i < n; i++) memcpy(p + i * l->size, v, l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1622; }
    { const char * tb_v1623 = "    l->len += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1623; }
    { const char * tb_v1624 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1624; }
    { const char * tb_v1625 = "static void *tb_nn(void *p, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1625; }
    { const char * tb_v1626 = "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1626; }
    { const char * tb_v1627 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1627; }
    { const char * tb_v1628 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1628; }
    { const char * tb_v1629 = "static const char *tb_cat(const char *a, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1629; }
    { const char * tb_v1630 = "    size_t la = strlen(a), lb = strlen(b);\n"; *(const char * *)tb_grow(v_lines) = tb_v1630; }
    { const char * tb_v1631 = "    char *s = tb_mem(la + lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1631; }
    { const char * tb_v1632 = "    memcpy(s, a, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1632; }
    { const char * tb_v1633 = "    memcpy(s + la, b, lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1633; }
    { const char * tb_v1634 = "    return s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1634; }
    { const char * tb_v1635 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1635; }
    { const char * tb_v1636 = "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1636; }
    { const char * tb_v1637 = "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1637; }
    { const char * tb_v1638 = "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1638; }
    { const char * tb_v1639 = "// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву\n"; *(const char * *)tb_grow(v_lines) = tb_v1639; }
    { const char * tb_v1640 = "// таблица постоянная: компилятор C видит, что второй байт всегда 0, и  ch == \",\"  становится одним сравнением байта\n"; *(const char * *)tb_grow(v_lines) = tb_v1640; }
    { const char * tb_v1641 = "static const char tb_chars[512] = {\n"; *(const char * *)tb_grow(v_lines) = tb_v1641; }
    { const char * tb_v1642 = "    0,0,1,0,2,0,3,0,4,0,5,0,6,0,7,0,8,0,9,0,10,0,11,0,12,0,13,0,14,0,15,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1642; }
    { const char * tb_v1643 = "    16,0,17,0,18,0,19,0,20,0,21,0,22,0,23,0,24,0,25,0,26,0,27,0,28,0,29,0,30,0,31,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1643; }
    { const char * tb_v1644 = "    32,0,33,0,34,0,35,0,36,0,37,0,38,0,39,0,40,0,41,0,42,0,43,0,44,0,45,0,46,0,47,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1644; }
    { const char * tb_v1645 = "    48,0,49,0,50,0,51,0,52,0,53,0,54,0,55,0,56,0,57,0,58,0,59,0,60,0,61,0,62,0,63,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1645; }
    { const char * tb_v1646 = "    64,0,65,0,66,0,67,0,68,0,69,0,70,0,71,0,72,0,73,0,74,0,75,0,76,0,77,0,78,0,79,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1646; }
    { const char * tb_v1647 = "    80,0,81,0,82,0,83,0,84,0,85,0,86,0,87,0,88,0,89,0,90,0,91,0,92,0,93,0,94,0,95,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1647; }
    { const char * tb_v1648 = "    96,0,97,0,98,0,99,0,100,0,101,0,102,0,103,0,104,0,105,0,106,0,107,0,108,0,109,0,110,0,111,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1648; }
    { const char * tb_v1649 = "    112,0,113,0,114,0,115,0,116,0,117,0,118,0,119,0,120,0,121,0,122,0,123,0,124,0,125,0,126,0,127,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1649; }
    { const char * tb_v1650 = "    128,0,129,0,130,0,131,0,132,0,133,0,134,0,135,0,136,0,137,0,138,0,139,0,140,0,141,0,142,0,143,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1650; }
    { const char * tb_v1651 = "    144,0,145,0,146,0,147,0,148,0,149,0,150,0,151,0,152,0,153,0,154,0,155,0,156,0,157,0,158,0,159,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1651; }
    { const char * tb_v1652 = "    160,0,161,0,162,0,163,0,164,0,165,0,166,0,167,0,168,0,169,0,170,0,171,0,172,0,173,0,174,0,175,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1652; }
    { const char * tb_v1653 = "    176,0,177,0,178,0,179,0,180,0,181,0,182,0,183,0,184,0,185,0,186,0,187,0,188,0,189,0,190,0,191,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1653; }
    { const char * tb_v1654 = "    192,0,193,0,194,0,195,0,196,0,197,0,198,0,199,0,200,0,201,0,202,0,203,0,204,0,205,0,206,0,207,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1654; }
    { const char * tb_v1655 = "    208,0,209,0,210,0,211,0,212,0,213,0,214,0,215,0,216,0,217,0,218,0,219,0,220,0,221,0,222,0,223,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1655; }
    { const char * tb_v1656 = "    224,0,225,0,226,0,227,0,228,0,229,0,230,0,231,0,232,0,233,0,234,0,235,0,236,0,237,0,238,0,239,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1656; }
    { const char * tb_v1657 = "    240,0,241,0,242,0,243,0,244,0,245,0,246,0,247,0,248,0,249,0,250,0,251,0,252,0,253,0,254,0,255,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1657; }
    { const char * tb_v1658 = "};\n"; *(const char * *)tb_grow(v_lines) = tb_v1658; }
    { const char * tb_v1659 = "static inline const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1659; }
    { const char * tb_v1660 = "static inline long long tb_tlen(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1660; }
    { const char * tb_v1661 = "    if (tb_lc_p[0] == s) return tb_lc_n[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1661; }
    { const char * tb_v1662 = "    if (tb_lc_p[1] == s) return tb_lc_n[1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1662; }
    { const char * tb_v1663 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1663; }
    { const char * tb_v1664 = "    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1664; }
    { const char * tb_v1665 = "    return n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1665; }
    { const char * tb_v1666 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1666; }
    { const char * tb_v1667 = "// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1\n"; *(const char * *)tb_grow(v_lines) = tb_v1667; }
    { const char * tb_v1668 = "static inline const char *tb_char_at(const char *s, long long i, long long n, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1668; }
    { const char * tb_v1669 = "    if (n < 0) n = tb_tlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1669; }
    { const char * tb_v1670 = "    if (i < 0 || i >= n) tb_crash(line, \"номер за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1670; }
    { const char * tb_v1671 = "    return tb_chr((unsigned char)s[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1671; }
    { const char * tb_v1672 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1672; }
    { const char * tb_v1673 = "// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.\n"; *(const char * *)tb_grow(v_lines) = tb_v1673; }
    { const char * tb_v1674 = "// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),\n"; *(const char * *)tb_grow(v_lines) = tb_v1674; }
    { const char * tb_v1675 = "// *rc — сколько там места.\n"; *(const char * *)tb_grow(v_lines) = tb_v1675; }
    { const char * tb_v1676 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1676; }
    { const char * tb_v1677 = "// число цифрами в конец буфера d[24]: вернёт начало цифр, длину — в *n\n"; *(const char * *)tb_grow(v_lines) = tb_v1677; }
    { const char * tb_v1678 = "static inline const char *tb_digits(long long x, char *d, long long *n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1678; }
    { const char * tb_v1679 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1679; }
    { const char * tb_v1680 = "    char *p = d + 24;\n"; *(const char * *)tb_grow(v_lines) = tb_v1680; }
    { const char * tb_v1681 = "    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1681; }
    { const char * tb_v1682 = "    if (x < 0) *--p = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1682; }
    { const char * tb_v1683 = "    *n = d + 24 - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1683; }
    { const char * tb_v1684 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1684; }
    { const char * tb_v1685 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1685; }
    { const char * tb_v1686 = "static inline void tb_app(const char **p, long long *rl, long long *rc, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1686; }
    { const char * tb_v1687 = "    tb_app_n(p, rl, rc, b, (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1687; }
    { const char * tb_v1688 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1688; }
    { const char * tb_v1689 = "// дописать число: цифры пишутся прямо в конец, без отдельного текста\n"; *(const char * *)tb_grow(v_lines) = tb_v1689; }
    { const char * tb_v1690 = "static inline void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1690; }
    { const char * tb_v1691 = "    char d[24]; int n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1691; }
    { const char * tb_v1692 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1692; }
    { const char * tb_v1693 = "    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1693; }
    { const char * tb_v1694 = "    if (x < 0) d[23 - n++] = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1694; }
    { const char * tb_v1695 = "    tb_app_n(p, rl, rc, d + 24 - n, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1695; }
    { const char * tb_v1696 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1696; }
    { const char * tb_v1697 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1697; }
    { const char * tb_v1698 = "// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии\n"; *(const char * *)tb_grow(v_lines) = tb_v1698; }
    { const char * tb_v1699 = "static void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1699; }
    { const char * tb_v1700 = "    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1700; }
    { const char * tb_v1701 = "    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1701; }
    { const char * tb_v1702 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1702; }
    { const char * tb_v1703 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1703; }
    { const char * tb_v1704 = "    char *s = (char *)*p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1704; }
    { const char * tb_v1705 = "    if (*rl < 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1705; }
    { const char * tb_v1706 = "        long long la = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1706; }
    { const char * tb_v1707 = "        long long cap = 2 * (la + lb) + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1707; }
    { const char * tb_v1708 = "        if (cap < 64) cap = 64;\n"; *(const char * *)tb_grow(v_lines) = tb_v1708; }
    { const char * tb_v1709 = "        char *r = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1709; }
    { const char * tb_v1710 = "        memcpy(r, s, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1710; }
    { const char * tb_v1711 = "        memcpy(r + la, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1711; }
    { const char * tb_v1712 = "        r[la + lb] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1712; }
    { const char * tb_v1713 = "        *p = r; *rl = la + lb; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1713; }
    { const char * tb_v1714 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1714; }
    { const char * tb_v1715 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1715; }
    { const char * tb_v1716 = "    long long need = *rl + lb + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1716; }
    { const char * tb_v1717 = "    if (need > *rc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1717; }
    { const char * tb_v1718 = "        long long cap = 2 * need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1718; }
    { const char * tb_v1719 = "        long long off = (b >= s && b < s + *rc) ? b - s : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1719; }
    { const char * tb_v1720 = "        s = tb_resize(s, *rl + 1, cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1720; }
    { const char * tb_v1721 = "        if (off >= 0) b = s + off;\n"; *(const char * *)tb_grow(v_lines) = tb_v1721; }
    { const char * tb_v1722 = "        *p = s; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1722; }
    { const char * tb_v1723 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1723; }
    { const char * tb_v1724 = "    memmove(s + *rl, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1724; }
    { const char * tb_v1725 = "    *rl += lb;\n"; *(const char * *)tb_grow(v_lines) = tb_v1725; }
    { const char * tb_v1726 = "    s[*rl] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1726; }
    { const char * tb_v1727 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1727; }
    { const char * tb_v1728 = "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1728; }
    { const char * tb_v1729 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1729; }
    { const char * tb_v1730 = "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1730; }
    { const char * tb_v1731 = "    char *r = tb_mem(to - from + 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v1731; }
    { const char * tb_v1732 = "    memcpy(r, s + from, to - from + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1732; }
    { const char * tb_v1733 = "    r[to - from + 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1733; }
    { const char * tb_v1734 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1734; }
    { const char * tb_v1735 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1735; }
    { const char * tb_v1736 = "static long long tb_code(const char *s, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1736; }
    { const char * tb_v1737 = "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1737; }
    { const char * tb_v1738 = "    return (unsigned char)s[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1738; }
    { const char * tb_v1739 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1739; }
    { const char * tb_v1740 = "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1740; }
    { const char * tb_v1741 = "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1741; }
    { const char * tb_v1742 = "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1742; }
    { const char * tb_v1743 = "static void tb_ps(const char *s) { fputs(s, stdout); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1743; }
    { const char * tb_v1744 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1744; }
    { const char * tb_v1745 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1745; }
    { const char * tb_v1746 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1746; }
    { const char * tb_v1747 = "// ---------- ошибки fail / try / or / else err ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1747; }
    { const char * tb_v1748 = "// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.\n"; *(const char * *)tb_grow(v_lines) = tb_v1748; }
    { const char * tb_v1749 = "// Тот, кто её вызвал, смотрит в tb_err.\n"; *(const char * *)tb_grow(v_lines) = tb_v1749; }
    { const char * tb_v1750 = "static _Thread_local const char *tb_err = 0;   // у каждого ядра своя\n"; *(const char * *)tb_grow(v_lines) = tb_v1750; }
    { const char * tb_v1751 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1751; }
    { const char * tb_v1752 = "TB_COLD static void tb_die(const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1752; }
    { const char * tb_v1753 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1753; }
    { const char * tb_v1754 = "    fprintf(stderr, \"ошибка: %s\\n\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1754; }
    { const char * tb_v1755 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1755; }
    { const char * tb_v1756 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1756; }
    { const char * tb_v1757 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1757; }
    { const char * tb_v1758 = "// ---------- текст ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1758; }
    { const char * tb_v1759 = "static const char *tb_upper(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1759; }
    { const char * tb_v1760 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1760; }
    { const char * tb_v1761 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1761; }
    { const char * tb_v1762 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1762; }
    { const char * tb_v1763 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1763; }
    { const char * tb_v1764 = "static const char *tb_lower(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1764; }
    { const char * tb_v1765 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1765; }
    { const char * tb_v1766 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1766; }
    { const char * tb_v1767 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1767; }
    { const char * tb_v1768 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1768; }
    { const char * tb_v1769 = "static int tb_space(char c) { return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1769; }
    { const char * tb_v1770 = "static const char *tb_trim(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1770; }
    { const char * tb_v1771 = "    size_t a = 0, b = strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1771; }
    { const char * tb_v1772 = "    while (a < b && tb_space(s[a])) a++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1772; }
    { const char * tb_v1773 = "    while (b > a && tb_space(s[b - 1])) b--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1773; }
    { const char * tb_v1774 = "    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1774; }
    { const char * tb_v1775 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1775; }
    { const char * tb_v1776 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1776; }
    { const char * tb_v1777 = "static long long tb_find(const char *s, const char *part) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1777; }
    { const char * tb_v1778 = "    const char *p = strstr(s, part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1778; }
    { const char * tb_v1779 = "    return p ? (long long)(p - s) : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1779; }
    { const char * tb_v1780 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1780; }
    { const char * tb_v1781 = "static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1781; }
    { const char * tb_v1782 = "static int tb_ends(const char *s, const char *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1782; }
    { const char * tb_v1783 = "    size_t a = strlen(s), b = strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1783; }
    { const char * tb_v1784 = "    return b <= a && strcmp(s + a - b, p) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1784; }
    { const char * tb_v1785 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1785; }
    { const char * tb_v1786 = "static const char *tb_replace(const char *s, const char *from, const char *to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1786; }
    { const char * tb_v1787 = "    size_t lf = strlen(from), lt = strlen(to), count = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1787; }
    { const char * tb_v1788 = "    if (lf == 0) tb_crash(line, \"replace: что заменять — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1788; }
    { const char * tb_v1789 = "    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1789; }
    { const char * tb_v1790 = "    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1790; }
    { const char * tb_v1791 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1791; }
    { const char * tb_v1792 = "    while ((q = strstr(p, from)) != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1792; }
    { const char * tb_v1793 = "        memcpy(w, p, q - p); w += q - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1793; }
    { const char * tb_v1794 = "        memcpy(w, to, lt); w += lt;\n"; *(const char * *)tb_grow(v_lines) = tb_v1794; }
    { const char * tb_v1795 = "        p = q + lf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1795; }
    { const char * tb_v1796 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1796; }
    { const char * tb_v1797 = "    strcpy(w, p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1797; }
    { const char * tb_v1798 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1798; }
    { const char * tb_v1799 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1799; }
    { const char * tb_v1800 = "static tb_list *tb_split(const char *s, const char *sep, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1800; }
    { const char * tb_v1801 = "    size_t ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1801; }
    { const char * tb_v1802 = "    if (ls == 0) tb_crash(line, \"split: разделитель — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1802; }
    { const char * tb_v1803 = "    tb_list *l = tb_list_new(sizeof(const char *));\n"; *(const char * *)tb_grow(v_lines) = tb_v1803; }
    { const char * tb_v1804 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1804; }
    { const char * tb_v1805 = "    while (1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1805; }
    { const char * tb_v1806 = "        q = strstr(p, sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1806; }
    { const char * tb_v1807 = "        size_t n = q ? (size_t)(q - p) : strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1807; }
    { const char * tb_v1808 = "        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1808; }
    { const char * tb_v1809 = "        *(const char **)tb_grow(l) = part;\n"; *(const char * *)tb_grow(v_lines) = tb_v1809; }
    { const char * tb_v1810 = "        if (!q) break;\n"; *(const char * *)tb_grow(v_lines) = tb_v1810; }
    { const char * tb_v1811 = "        p = q + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1811; }
    { const char * tb_v1812 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1812; }
    { const char * tb_v1813 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1813; }
    { const char * tb_v1814 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1814; }
    { const char * tb_v1815 = "static const char *tb_join(tb_list *l, const char *sep) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1815; }
    { const char * tb_v1816 = "    size_t n = 0, ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1816; }
    { const char * tb_v1817 = "    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1817; }
    { const char * tb_v1818 = "    char *r = tb_mem(n + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1818; }
    { const char * tb_v1819 = "    for (long long i = 0; i < l->len; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1819; }
    { const char * tb_v1820 = "        const char *part = ((const char **)l->data)[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1820; }
    { const char * tb_v1821 = "        if (i > 0) { memcpy(w, sep, ls); w += ls; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1821; }
    { const char * tb_v1822 = "        memcpy(w, part, strlen(part)); w += strlen(part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1822; }
    { const char * tb_v1823 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1823; }
    { const char * tb_v1824 = "    *w = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1824; }
    { const char * tb_v1825 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1825; }
    { const char * tb_v1826 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1826; }
    { const char * tb_v1827 = "static long long tb_to_int(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1827; }
    { const char * tb_v1828 = "    long long v = 0; int i = 0, minus = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1828; }
    { const char * tb_v1829 = "    if (s[0] == '-') { minus = 1; i = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1829; }
    { const char * tb_v1830 = "    if (s[i] == 0) { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1830; }
    { const char * tb_v1831 = "    for (; s[i]; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1831; }
    { const char * tb_v1832 = "        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1832; }
    { const char * tb_v1833 = "        v = v * 10 + (s[i] - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v1833; }
    { const char * tb_v1834 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1834; }
    { const char * tb_v1835 = "    return minus ? -v : v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1835; }
    { const char * tb_v1836 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1836; }
    { const char * tb_v1837 = "static double tb_to_dec(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1837; }
    { const char * tb_v1838 = "    char *end = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1838; }
    { const char * tb_v1839 = "    double v = strtod(s, &end);\n"; *(const char * *)tb_grow(v_lines) = tb_v1839; }
    { const char * tb_v1840 = "    if (s[0] == 0 || *end != 0) { tb_err = tb_cat(\"это не дробное число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1840; }
    { const char * tb_v1841 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1841; }
    { const char * tb_v1842 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1842; }
    { const char * tb_v1843 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1843; }
    { const char * tb_v1844 = "// ---------- файлы, ввод, слова запуска ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1844; }
    { const char * tb_v1845 = "static const char *tb_read_file(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1845; }
    { const char * tb_v1846 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1846; }
    { const char * tb_v1847 = "    if (f == 0) { tb_err = tb_cat(\"не могу открыть файл: \", path); return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1847; }
    { const char * tb_v1848 = "    size_t cap = 4096, n = 0, got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1848; }
    { const char * tb_v1849 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1849; }
    { const char * tb_v1850 = "    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1850; }
    { const char * tb_v1851 = "        n += got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1851; }
    { const char * tb_v1852 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1852; }
    { const char * tb_v1853 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1853; }
    { const char * tb_v1854 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1854; }
    { const char * tb_v1855 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1855; }
    { const char * tb_v1856 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1856; }
    { const char * tb_v1857 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1857; }
    { const char * tb_v1858 = "static void tb_write_file(const char *path, const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1858; }
    { const char * tb_v1859 = "    FILE *f = fopen(path, \"wb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1859; }
    { const char * tb_v1860 = "    if (f == 0) { tb_err = tb_cat(\"не могу записать файл: \", path); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1860; }
    { const char * tb_v1861 = "    fputs(s, f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1861; }
    { const char * tb_v1862 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1862; }
    { const char * tb_v1863 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1863; }
    { const char * tb_v1864 = "static const char *tb_input(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1864; }
    { const char * tb_v1865 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1865; }
    { const char * tb_v1866 = "    size_t cap = 256, n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1866; }
    { const char * tb_v1867 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1867; }
    { const char * tb_v1868 = "    int c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1868; }
    { const char * tb_v1869 = "    while ((c = getchar()) != EOF && c != '\\n') {\n"; *(const char * *)tb_grow(v_lines) = tb_v1869; }
    { const char * tb_v1870 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1870; }
    { const char * tb_v1871 = "        buf[n++] = (char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1871; }
    { const char * tb_v1872 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1872; }
    { const char * tb_v1873 = "    if (c == EOF && n == 0) { tb_err = \"ввод кончился\"; return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1873; }
    { const char * tb_v1874 = "    if (n > 0 && buf[n - 1] == '\\r') n--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1874; }
    { const char * tb_v1875 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1875; }
    { const char * tb_v1876 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1876; }
    { const char * tb_v1877 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1877; }
    { const char * tb_v1878 = "static tb_list *tb_args;\n"; *(const char * *)tb_grow(v_lines) = tb_v1878; }
    { const char * tb_v1879 = "static long long tb_run(const char *command) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1879; }
    { const char * tb_v1880 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1880; }
    { const char * tb_v1881 = "    int code = system(command);\n"; *(const char * *)tb_grow(v_lines) = tb_v1881; }
    { const char * tb_v1882 = "    if (code == -1) return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1882; }
    { const char * tb_v1883 = "    return code == 0 ? 0 : (code > 255 ? code / 256 : code);\n"; *(const char * *)tb_grow(v_lines) = tb_v1883; }
    { const char * tb_v1884 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1884; }
    { const char * tb_v1885 = "static void tb_exit(long long code) { fflush(stdout); exit((int)code); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1885; }
    { const char * tb_v1886 = "static int tb_file_exists(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1886; }
    { const char * tb_v1887 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1887; }
    { const char * tb_v1888 = "    if (f == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1888; }
    { const char * tb_v1889 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1889; }
    { const char * tb_v1890 = "    return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1890; }
    { const char * tb_v1891 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1891; }
    { const char * tb_v1892 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1892; }
    { const char * tb_v1893 = "// ---------- числа ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1893; }
    { const char * tb_v1894 = "static long long tb_absi(long long x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1894; }
    { const char * tb_v1895 = "static long long tb_mini(long long a, long long b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1895; }
    { const char * tb_v1896 = "static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1896; }
    { const char * tb_v1897 = "static double tb_mind(double a, double b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1897; }
    { const char * tb_v1898 = "static double tb_maxd(double a, double b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1898; }
    { const char * tb_v1899 = "static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, \"sqrt: корень из отрицательного числа\"); return sqrt(x); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1899; }
    { const char * tb_v1900 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1900; }
    { const char * tb_v1901 = "// ---------- sort ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1901; }
    { const char * tb_v1902 = "static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1902; }
    { const char * tb_v1903 = "static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1903; }
    { const char * tb_v1904 = "static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1904; }
    { const char * tb_v1905 = "static tb_list *tb_copy(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1905; }
    { const char * tb_v1906 = "    if (l->size == 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1906; }
    { const char * tb_v1907 = "        tb_list *r = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1907; }
    { const char * tb_v1908 = "        long long bytes = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1908; }
    { const char * tb_v1909 = "        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1909; }
    { const char * tb_v1910 = "        memcpy(r->data, l->data, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1910; }
    { const char * tb_v1911 = "        return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1911; }
    { const char * tb_v1912 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1912; }
    { const char * tb_v1913 = "    return tb_list_from(l->len, l->size, l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v1913; }
    { const char * tb_v1914 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1914; }
    { const char * tb_v1915 = "static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1915; }
    { const char * tb_v1916 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1916; }
    { const char * tb_v1917 = "// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,\n"; *(const char * *)tb_grow(v_lines) = tb_v1917; }
    { const char * tb_v1918 = "// а найти номер по ключу можно тремя способами (mode):\n"; *(const char * *)tb_grow(v_lines) = tb_v1918; }
    { const char * tb_v1919 = "//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;\n"; *(const char * *)tb_grow(v_lines) = tb_v1919; }
    { const char * tb_v1920 = "//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;\n"; *(const char * *)tb_grow(v_lines) = tb_v1920; }
    { const char * tb_v1921 = "//   0 — таблица slots с хешем (текст или разбросанные числа) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1921; }
    { const char * tb_v1922 = "typedef struct {\n"; *(const char * *)tb_grow(v_lines) = tb_v1922; }
    { const char * tb_v1923 = "    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;\n"; *(const char * *)tb_grow(v_lines) = tb_v1923; }
    { const char * tb_v1924 = "    int mode; long long base; long long *pos; long long npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1924; }
    { const char * tb_v1925 = "} tb_dict;\n"; *(const char * *)tb_grow(v_lines) = tb_v1925; }
    { const char * tb_v1926 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1926; }
    { const char * tb_v1927 = "static unsigned long long tb_hash(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1927; }
    { const char * tb_v1928 = "    unsigned long long h = 1469598103934665603ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1928; }
    { const char * tb_v1929 = "    if (d->text) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1929; }
    { const char * tb_v1930 = "        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1930; }
    { const char * tb_v1931 = "    } else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1931; }
    { const char * tb_v1932 = "        h ^= (unsigned long long)*(const long long *)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v1932; }
    { const char * tb_v1933 = "        h *= 0x9E3779B97F4A7C15ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1933; }
    { const char * tb_v1934 = "        h ^= h >> 32;\n"; *(const char * *)tb_grow(v_lines) = tb_v1934; }
    { const char * tb_v1935 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1935; }
    { const char * tb_v1936 = "    return h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1936; }
    { const char * tb_v1937 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1937; }
    { const char * tb_v1938 = "static int tb_keq(tb_dict *d, const void *a, const void *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1938; }
    { const char * tb_v1939 = "    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1939; }
    { const char * tb_v1940 = "    return *(const long long *)a == *(const long long *)b;\n"; *(const char * *)tb_grow(v_lines) = tb_v1940; }
    { const char * tb_v1941 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1941; }
    { const char * tb_v1942 = "static inline long long tb_dfind(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1942; }
    { const char * tb_v1943 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1943; }
    { const char * tb_v1944 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1944; }
    { const char * tb_v1945 = "        return at < (unsigned long long)d->keys->len ? (long long)at : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1945; }
    { const char * tb_v1946 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1946; }
    { const char * tb_v1947 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1947; }
    { const char * tb_v1948 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1948; }
    { const char * tb_v1949 = "        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1949; }
    { const char * tb_v1950 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1950; }
    { const char * tb_v1951 = "    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1951; }
    { const char * tb_v1952 = "    while (d->slots[i] != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1952; }
    { const char * tb_v1953 = "        long long at = d->slots[i] - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1953; }
    { const char * tb_v1954 = "        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;\n"; *(const char * *)tb_grow(v_lines) = tb_v1954; }
    { const char * tb_v1955 = "        i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1955; }
    { const char * tb_v1956 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1956; }
    { const char * tb_v1957 = "    return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1957; }
    { const char * tb_v1958 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1958; }
    { const char * tb_v1959 = "// память для таблиц dict — там же, где лежат его ключи (временная или обычная)\n"; *(const char * *)tb_grow(v_lines) = tb_v1959; }
    { const char * tb_v1960 = "static void *tb_dalloc(tb_dict *d, void *old, long long bytes) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1960; }
    { const char * tb_v1961 = "    if (old && !(tb_nchunks > 0 && tb_in_temp(old))) free(old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1961; }
    { const char * tb_v1962 = "    void *p = tb_in_cur_temp(d->keys->data) ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1962; }
    { const char * tb_v1963 = "    if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1963; }
    { const char * tb_v1964 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1964; }
    { const char * tb_v1965 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1965; }
    { const char * tb_v1966 = "static void tb_dreindex(tb_dict *d, long long nslots) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1966; }
    { const char * tb_v1967 = "    d->mode = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1967; }
    { const char * tb_v1968 = "    if (d->pos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1968; }
    { const char * tb_v1969 = "        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1969; }
    { const char * tb_v1970 = "        d->pos = 0; d->npos = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1970; }
    { const char * tb_v1971 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1971; }
    { const char * tb_v1972 = "    d->nslots = nslots;\n"; *(const char * *)tb_grow(v_lines) = tb_v1972; }
    { const char * tb_v1973 = "    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1973; }
    { const char * tb_v1974 = "    memset(d->slots, 0, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1974; }
    { const char * tb_v1975 = "    unsigned long long mask = nslots - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1975; }
    { const char * tb_v1976 = "    for (long long at = 0; at < d->keys->len; at++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1976; }
    { const char * tb_v1977 = "        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1977; }
    { const char * tb_v1978 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1978; }
    { const char * tb_v1979 = "        d->slots[i] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1979; }
    { const char * tb_v1980 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1980; }
    { const char * tb_v1981 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1981; }
    { const char * tb_v1982 = "// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)\n"; *(const char * *)tb_grow(v_lines) = tb_v1982; }
    { const char * tb_v1983 = "static void tb_dto_hash(tb_dict *d) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1983; }
    { const char * tb_v1984 = "    long long n = 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1984; }
    { const char * tb_v1985 = "    while (n < d->keys->len * 2 + 2) n *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1985; }
    { const char * tb_v1986 = "    tb_dreindex(d, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1986; }
    { const char * tb_v1987 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1987; }
    { const char * tb_v1988 = "// mode 2: таблица pos на ключи base .. base+npos-1\n"; *(const char * *)tb_grow(v_lines) = tb_v1988; }
    { const char * tb_v1989 = "static void tb_dto_pos(tb_dict *d, long long npos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1989; }
    { const char * tb_v1990 = "    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1990; }
    { const char * tb_v1991 = "    memset(p, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1991; }
    { const char * tb_v1992 = "    const long long *ks = (const long long *)d->keys->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1992; }
    { const char * tb_v1993 = "    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1993; }
    { const char * tb_v1994 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1994; }
    { const char * tb_v1995 = "    d->pos = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1995; }
    { const char * tb_v1996 = "    d->npos = npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1996; }
    { const char * tb_v1997 = "    d->mode = 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1997; }
    { const char * tb_v1998 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1998; }
    { const char * tb_v1999 = "static tb_dict *tb_dict_new(int text, long long vsize) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1999; }
    { const char * tb_v2000 = "    tb_dict *d = tb_mem(sizeof(tb_dict));\n"; *(const char * *)tb_grow(v_lines) = tb_v2000; }
    { const char * tb_v2001 = "    d->text = text;\n"; *(const char * *)tb_grow(v_lines) = tb_v2001; }
    { const char * tb_v2002 = "    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2002; }
    { const char * tb_v2003 = "    d->vals = tb_list_new(vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2003; }
    { const char * tb_v2004 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2004; }
    { const char * tb_v2005 = "    if (text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v2005; }
    { const char * tb_v2006 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2006; }
    { const char * tb_v2007 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v2007; }
    { const char * tb_v2008 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2008; }
    { const char * tb_v2009 = "// .clear(): список снова пустой; свой кусок памяти отдаётся (если он не временный).\n"; *(const char * *)tb_grow(v_lines) = tb_v2009; }
    { const char * tb_v2010 = "// Тексты и obj внутри не удаляются: на них могут смотреть другие переменные.\n"; *(const char * *)tb_grow(v_lines) = tb_v2010; }
    { const char * tb_v2011 = "static void tb_lclear(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2011; }
    { const char * tb_v2012 = "    if (l->cap < 0) tb_crash(line, \".clear() нельзя, пока по списку идёт each ... by N\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2012; }
    { const char * tb_v2013 = "    int temp = tb_nchunks > 0 && tb_in_temp(l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v2013; }
    { const char * tb_v2014 = "    if (!temp) free(l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v2014; }
    { const char * tb_v2015 = "    long long cap = l->size == 0 ? 64 : 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v2015; }
    { const char * tb_v2016 = "    long long bytes = l->size == 0 ? 8 : l->size * 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v2016; }
    { const char * tb_v2017 = "    l->data = tb_in_cur_temp(l->data) ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2017; }
    { const char * tb_v2018 = "    if (l->data == 0) tb_crash(line, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2018; }
    { const char * tb_v2019 = "    memset(l->data, 0, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2019; }
    { const char * tb_v2020 = "    l->len = 0; l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v2020; }
    { const char * tb_v2021 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2021; }
    { const char * tb_v2022 = "static void tb_dclear(tb_dict *d, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2022; }
    { const char * tb_v2023 = "    tb_lclear(d->keys, line);\n"; *(const char * *)tb_grow(v_lines) = tb_v2023; }
    { const char * tb_v2024 = "    tb_lclear(d->vals, line);\n"; *(const char * *)tb_grow(v_lines) = tb_v2024; }
    { const char * tb_v2025 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v2025; }
    { const char * tb_v2026 = "    if (d->slots && !(tb_nchunks > 0 && tb_in_temp(d->slots))) free(d->slots);\n"; *(const char * *)tb_grow(v_lines) = tb_v2026; }
    { const char * tb_v2027 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2027; }
    { const char * tb_v2028 = "    if (d->text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v2028; }
    { const char * tb_v2029 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2029; }
    { const char * tb_v2030 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2030; }
    { const char * tb_v2031 = "// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)\n"; *(const char * *)tb_grow(v_lines) = tb_v2031; }
    { const char * tb_v2032 = "static void tb_dplace_int(tb_dict *d, long long k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2032; }
    { const char * tb_v2033 = "    long long n = d->keys->len;          // уже вместе с новым ключом\n"; *(const char * *)tb_grow(v_lines) = tb_v2033; }
    { const char * tb_v2034 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2034; }
    { const char * tb_v2035 = "        if (n == 1) { d->base = k; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2035; }
    { const char * tb_v2036 = "        if (k == d->base + n - 1) return;  // подряд — остаёмся в mode 1\n"; *(const char * *)tb_grow(v_lines) = tb_v2036; }
    { const char * tb_v2037 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2037; }
    { const char * tb_v2038 = "    if (d->mode == 1 || d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2038; }
    { const char * tb_v2039 = "        // плотно ли: ключи не дальше 4 ячеек на один ключ\n"; *(const char * *)tb_grow(v_lines) = tb_v2039; }
    { const char * tb_v2040 = "        long long o = k - d->base;\n"; *(const char * *)tb_grow(v_lines) = tb_v2040; }
    { const char * tb_v2041 = "        if (o >= 0 && o < 4 * n + 64) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2041; }
    { const char * tb_v2042 = "            if (d->mode == 2 && o < d->npos) { d->pos[o] = n; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2042; }
    { const char * tb_v2043 = "            long long want = d->npos * 2 > o + 1 ? d->npos * 2 : o + 1 + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2043; }
    { const char * tb_v2044 = "            if (want < 16) want = 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v2044; }
    { const char * tb_v2045 = "            tb_dto_pos(d, want);\n"; *(const char * *)tb_grow(v_lines) = tb_v2045; }
    { const char * tb_v2046 = "            return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2046; }
    { const char * tb_v2047 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v2047; }
    { const char * tb_v2048 = "        tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v2048; }
    { const char * tb_v2049 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2049; }
    { const char * tb_v2050 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2050; }
    { const char * tb_v2051 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2051; }
    { const char * tb_v2052 = "// место под значение ключа k; если ключа нет — добавить со значением *def\n"; *(const char * *)tb_grow(v_lines) = tb_v2052; }
    { const char * tb_v2053 = "static void *tb_dput(tb_dict *d, const void *k, const void *def) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2053; }
    { const char * tb_v2054 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2054; }
    { const char * tb_v2055 = "    if (at >= 0) return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2055; }
    { const char * tb_v2056 = "    memcpy(tb_grow(d->keys), k, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2056; }
    { const char * tb_v2057 = "    void *v = tb_grow(d->vals);\n"; *(const char * *)tb_grow(v_lines) = tb_v2057; }
    { const char * tb_v2058 = "    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2058; }
    { const char * tb_v2059 = "    if (d->mode != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2059; }
    { const char * tb_v2060 = "        // без поиска; если ключ лёг далеко, tb_dplace_int сам перейдёт на хеш и впишет все ключи\n"; *(const char * *)tb_grow(v_lines) = tb_v2060; }
    { const char * tb_v2061 = "        tb_dplace_int(d, *(const long long *)k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2061; }
    { const char * tb_v2062 = "        return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2062; }
    { const char * tb_v2063 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2063; }
    { const char * tb_v2064 = "    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v2064; }
    { const char * tb_v2065 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v2065; }
    { const char * tb_v2066 = "        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2066; }
    { const char * tb_v2067 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2067; }
    { const char * tb_v2068 = "        d->slots[i] = d->keys->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v2068; }
    { const char * tb_v2069 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2069; }
    { const char * tb_v2070 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2070; }
    { const char * tb_v2071 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2071; }
    { const char * tb_v2072 = "static void *tb_dget(tb_dict *d, const void *k, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2072; }
    { const char * tb_v2073 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2073; }
    { const char * tb_v2074 = "    if (TB_RARE(at < 0)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2074; }
    { const char * tb_v2075 = "        if (d->text) tb_crash(line, tb_cat(\"в dict нет ключа: \", *(const char * const *)k));\n"; *(const char * *)tb_grow(v_lines) = tb_v2075; }
    { const char * tb_v2076 = "        tb_crash(line, tb_cat(\"в dict нет ключа: \", tb_itos(*(const long long *)k)));\n"; *(const char * *)tb_grow(v_lines) = tb_v2076; }
    { const char * tb_v2077 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2077; }
    { const char * tb_v2078 = "    return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2078; }
    { const char * tb_v2079 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2079; }
    { const char * tb_v2080 = "// together: шаг 1 — просим память принести место ключа заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v2080; }
    { const char * tb_v2081 = "static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2081; }
    { const char * tb_v2082 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2082; }
    { const char * tb_v2083 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2083; }
    { const char * tb_v2084 = "        if (at < (unsigned long long)d->vals->len) __builtin_prefetch(d->vals->data + at * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2084; }
    { const char * tb_v2085 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2085; }
    { const char * tb_v2086 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2086; }
    { const char * tb_v2087 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2087; }
    { const char * tb_v2088 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2088; }
    { const char * tb_v2089 = "        if (o < (unsigned long long)d->npos) { __builtin_prefetch(&d->pos[o]); return o; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2089; }
    { const char * tb_v2090 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2090; }
    { const char * tb_v2091 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2091; }
    { const char * tb_v2092 = "    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2092; }
    { const char * tb_v2093 = "    __builtin_prefetch(&d->slots[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v2093; }
    { const char * tb_v2094 = "    return i;\n"; *(const char * *)tb_grow(v_lines) = tb_v2094; }
    { const char * tb_v2095 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2095; }
    { const char * tb_v2096 = "// шаг 2 — место приехало: просим принести ключ и значение\n"; *(const char * *)tb_grow(v_lines) = tb_v2096; }
    { const char * tb_v2097 = "static inline void tb_dpre2(tb_dict *d, unsigned long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2097; }
    { const char * tb_v2098 = "    if (i == ~0ULL || d->mode == 1) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2098; }
    { const char * tb_v2099 = "    long long s = d->mode == 2 ? d->pos[i] : d->slots[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v2099; }
    { const char * tb_v2100 = "    if (s != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2100; }
    { const char * tb_v2101 = "        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2101; }
    { const char * tb_v2102 = "        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2102; }
    { const char * tb_v2103 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2103; }
    { const char * tb_v2104 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2104; }
    { const char * tb_v2105 = "// элемент списка заранее (prefetch за пределами списка ничего не ломает)\n"; *(const char * *)tb_grow(v_lines) = tb_v2105; }
    { const char * tb_v2106 = "static inline void tb_lpre(tb_list *l, long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2106; }
    { const char * tb_v2107 = "    __builtin_prefetch(l->data + i * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2107; }
    { const char * tb_v2108 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2108; }
    { const char * tb_v2109 = "// ---------- холодные качества obj: своя память, подряд, отдельно от горячих ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2109; }
    { const char * tb_v2110 = "static void *tb_cold_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2110; }
    { const char * tb_v2111 = "    if (tb_temp_depth > 0) return tb_mem(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2111; }
    { const char * tb_v2112 = "    static _Thread_local char *p = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2112; }
    { const char * tb_v2113 = "    static _Thread_local long long left = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2113; }
    { const char * tb_v2114 = "    n = (n + 15) & ~15LL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2114; }
    { const char * tb_v2115 = "    if (left < n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2115; }
    { const char * tb_v2116 = "        long long size = n > 65536 ? n : 65536;\n"; *(const char * *)tb_grow(v_lines) = tb_v2116; }
    { const char * tb_v2117 = "        p = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2117; }
    { const char * tb_v2118 = "        if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2118; }
    { const char * tb_v2119 = "        left = size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2119; }
    { const char * tb_v2120 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2120; }
    { const char * tb_v2121 = "    void *r = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v2121; }
    { const char * tb_v2122 = "    p += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2122; }
    { const char * tb_v2123 = "    left -= n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2123; }
    { const char * tb_v2124 = "    memset(r, 0, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2124; }
    { const char * tb_v2125 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2125; }
    { const char * tb_v2126 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2126; }
    { const char * tb_v2127 = "// ---------- background: работа в своём потоке ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2127; }
    { const char * tb_v2128 = "typedef struct { pthread_t th; int done; void *res; } tb_task;\n"; *(const char * *)tb_grow(v_lines) = tb_v2128; }
    { const char * tb_v2129 = "static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2129; }
    { const char * tb_v2130 = "    t->done = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2130; }
    { const char * tb_v2131 = "    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, \"не получилось запустить background\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2131; }
    { const char * tb_v2132 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2132; }
    { const char * tb_v2133 = "// a.wait: дождаться потока (один раз) и отдать адрес ответа\n"; *(const char * *)tb_grow(v_lines) = tb_v2133; }
    { const char * tb_v2134 = "static void *tb_wait(tb_task *t) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2134; }
    { const char * tb_v2135 = "    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2135; }
    { const char * tb_v2136 = "    return t->res;\n"; *(const char * *)tb_grow(v_lines) = tb_v2136; }
    { const char * tb_v2137 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2137; }
    { const char * tb_v2138 = "static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2138; }
    { const char * tb_v2139 = "static void tb_dremove(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2139; }
    { const char * tb_v2140 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2140; }
    { const char * tb_v2141 = "    if (at < 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2141; }
    { const char * tb_v2142 = "    if (d->mode != 0) tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v2142; }
    { const char * tb_v2143 = "    long long last = d->keys->len - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2143; }
    { const char * tb_v2144 = "    // на место удалённого ставим последний: порядок меняется только у него\n"; *(const char * *)tb_grow(v_lines) = tb_v2144; }
    { const char * tb_v2145 = "    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2145; }
    { const char * tb_v2146 = "    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2146; }
    { const char * tb_v2147 = "    d->keys->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2147; }
    { const char * tb_v2148 = "    d->vals->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2148; }
    { const char * tb_v2149 = "    tb_dreindex(d, d->nslots);\n"; *(const char * *)tb_grow(v_lines) = tb_v2149; }
    { const char * tb_v2150 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2150; }
    { const char * tb_v2151 = "static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2151; }
    { const char * tb_v2152 = "    tb_dict *d = tb_dict_new(text, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2152; }
    { const char * tb_v2153 = "    long long ksize = d->keys->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2153; }
    { const char * tb_v2154 = "    for (long long i = 0; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2154; }
    { const char * tb_v2155 = "        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2155; }
    { const char * tb_v2156 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2156; }
    { const char * tb_v2157 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v2157; }
    { const char * tb_v2158 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2158; }
    { const char * tb_v2159 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2159; }
    { const char * tb_v2160 = "// ---------- on all cores: цикл на всех ядрах ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2160; }
    { const char * tb_v2161 = "// Работа делится на куски; каждое ядро берёт следующий свободный кусок,\n"; *(const char * *)tb_grow(v_lines) = tb_v2161; }
    { const char * tb_v2162 = "// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).\n"; *(const char * *)tb_grow(v_lines) = tb_v2162; }
    { const char * tb_v2163 = "// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.\n"; *(const char * *)tb_grow(v_lines) = tb_v2163; }
    { const char * tb_v2164 = "static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;\n"; *(const char * *)tb_grow(v_lines) = tb_v2164; }
    { const char * tb_v2165 = "static int tb_cores(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2165; }
    { const char * tb_v2166 = "    const char *want = getenv(\"TB_CORES\");   // TB_CORES=1 — проверить на одном ядре\n"; *(const char * *)tb_grow(v_lines) = tb_v2166; }
    { const char * tb_v2167 = "    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);\n"; *(const char * *)tb_grow(v_lines) = tb_v2167; }
    { const char * tb_v2168 = "    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2168; }
    { const char * tb_v2169 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2169; }
    { const char * tb_v2170 = "static void tb_par_run(void *(*work)(void *), void *job, long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2170; }
    { const char * tb_v2171 = "    int n = tb_cores();\n"; *(const char * *)tb_grow(v_lines) = tb_v2171; }
    { const char * tb_v2172 = "    if (n > total) n = (int)total;\n"; *(const char * *)tb_grow(v_lines) = tb_v2172; }
    { const char * tb_v2173 = "    if (n < 1) n = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2173; }
    { const char * tb_v2174 = "    pthread_t t[256];\n"; *(const char * *)tb_grow(v_lines) = tb_v2174; }
    { const char * tb_v2175 = "    for (int i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2175; }
    { const char * tb_v2176 = "        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, \"не получилось запустить ядро\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2176; }
    { const char * tb_v2177 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2177; }
    { const char * tb_v2178 = "    work(job);\n"; *(const char * *)tb_grow(v_lines) = tb_v2178; }
    { const char * tb_v2179 = "    for (int i = 1; i < n; i++) pthread_join(t[i], 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v2179; }
    { const char * tb_v2180 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2180; }
    { const char * tb_v2181 = "static long long tb_par_step(long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2181; }
    { const char * tb_v2182 = "    long long s = total / ((long long)tb_cores() * 16);\n"; *(const char * *)tb_grow(v_lines) = tb_v2182; }
    { const char * tb_v2183 = "    return s < 1 ? 1 : s;\n"; *(const char * *)tb_grow(v_lines) = tb_v2183; }
    { const char * tb_v2184 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2184; }
    return tb_join(v_lines, "");
    tb_crash(2, "функция runtime_text дошла до конца без return");
    return "";
}

int main(int argc, char **argv) {
    tb_args = tb_list_new(sizeof(const char *));
    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];
    f_main__main();
    return 0;
}
